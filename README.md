# spinmon

Spin primitives that double as monitoring sensors. Single-file C11, Linux, x86-64 / aarch64.

A spin loop is the one place in a program where you *know* something is waiting. spinmon
instruments that loop, so the lock reports its own queue depth, contention, and stalls
as a side effect of working, instead of being observed from outside.

**Contents**

1. [What is in the box](#1-what-is-in-the-box)
2. [Quick start](#2-quick-start)
3. [Using it in your code](#3-using-it-in-your-code)
4. [Cost model](#4-cost-model)
5. [spinmon vs. regular monitoring in regression tests](#5-spinmon-vs-regular-monitoring-in-regression-tests)
6. [Writing regression tests with spinmon](#6-writing-regression-tests-with-spinmon)
7. [Calibrating thresholds](#7-calibrating-thresholds)
8. [Limitations](#8-limitations)
9. [Verification status](#9-verification-status)
10. [API reference](#10-api-reference)

---

## 1. What is in the box

| Component | Sensor it provides | Main functions |
|---|---|---|
| Ticket lock (`sm_ticket`) | Queue depth: `next - serving` is the number of holders plus waiters | `sm_lock_at`, `SM_LOCK`, `sm_unlock`, `sm_ticket_depth` |
| Spin accounting | Per-thread-slot counters: acquisitions, contended acquisitions, total spins, max spins | `sm_snapshot` |
| Bounded-spin watchdog | Stall report naming holder tid, CPU, call site, hold time | `sm_ticket_init(..., stall_ns, cb, ctx)` |
| Lease word (`sm_lease`) | Heartbeat, expiry detection, takeover, fencing token | `sm_lease_acquire/renew/release/peek` |
| Seqlock feed + monitor (`sm_feed`, `sm_monitor`) | A pinned busy-poll thread that trips a circuit breaker on published samples | `sm_feed_publish`, `sm_feed_read`, `sm_monitor_main` |

Files:

| File | Purpose |
|---|---|
| `spinmon.c` | The library, plus a demo `main()` (omit with `-DSPINMON_NO_DEMO`) |
| `test_contention.c` | Regression gate on lock contention (record a baseline, compare later) |
| `test_monitor_lease.c` | Deterministic tests for the monitor logic and the lease protocol |
| `test_rendezvous.c` | Deterministic tests for the rotating-destination rendezvous (section 3.6) |

---

## 2. Quick start

```sh
gcc -std=c11 -O2 -Wall -Wextra -pthread spinmon.c -o spinmon
./spinmon
```

The demo runs three phases against one ticket lock and prints a row every 200 ms:

```
t=0.4s light depth=0 acq/s=29315 contended= 0.14% spins/acq=  49.0 breaker=off shed=221
```

| Column | Meaning |
|---|---|
| `phase` | `light` (think time between locks), `heavy` (no think time), `stall` (one holder sleeps 60 ms *inside* the lock, on purpose) |
| `depth` | Holders + waiters at the instant of sampling |
| `contended` | Share of acquisitions in this window that had to spin at all |
| `spins/acq` | Total spin iterations divided by all acquisitions in the window |
| `breaker` | State of the monitor core's circuit breaker; workers shed load while it is `ON` |
| `shed` | Cumulative count of operations workers skipped because the breaker was on |

During the stall phase you will also see watchdog lines:

```
[watchdog] lock=orders waiter=259 holder=258 cpu=0 site=spinmon.c:462 held=20.5ms waited=20.5ms ahead=1 depth=2
```

After the lock section the demo runs the lease scenario: a holder heartbeats three times,
wedges, is detected as expired, is taken over, and is then refused when it wakes up.

Build flags:

| Flag | Effect |
|---|---|
| `-DSPINMON_NO_DEMO` | Omit `main()` and the demo so you can `#include "spinmon.c"` from your own code |
| `-DSPINMON_NO_HOLDER` | Skip the holder stamp. Stall reports then have no holder tid, CPU, site, or hold time. Removes most of the per-acquisition cost (see [section 4](#4-cost-model)) and is intended for A/B runs |

---

## 3. Using it in your code

spinmon is a single translation unit, stb-style: every function is `static`.

```c
#define SPINMON_NO_DEMO
#include "spinmon.c"   /* first include, or compile with -D_GNU_SOURCE */
```

`spinmon.c` needs `_GNU_SOURCE` (for `sched_getcpu`, `pthread_setaffinity_np`). It defines it
itself, but only if it is the first include; otherwise glibc headers have already been
processed and you will get implicit-declaration warnings. Include it first, or pass `-D_GNU_SOURCE`.

### 3.1 The ticket lock

```c
static sm_ticket orders;                      /* static or aligned_alloc(64, sizeof) */

sm_ticket_init(&orders, "orders",
               20 * 1000 * 1000ull,           /* stall_ns: 20 ms; 0 disables the watchdog */
               on_stall, NULL);

SM_LOCK(&orders);        /* records __FILE__:__LINE__ as the call site */
/* ... short critical section, never blocks ... */
sm_unlock(&orders);
```

Rules:

- `sm_ticket` contains cache-line-aligned members. Use a static/global, or allocate with
  `aligned_alloc(64, sizeof(sm_ticket))`. Plain `malloc` is not enough.
- Call `sm_ticket_init` before any thread uses the lock. It `memset`s the whole struct, which
  also makes it a convenient way to get fresh counters between test trials. Never re-init a lock in use.
- Not recursive. Locking twice from one thread deadlocks. Unlock only from the owning thread.
- `SM_LOCK(l)` is `sm_lock_at(l, __FILE__ ":" __LINE__)`. To label a site yourself, call
  `sm_lock_at(l, "flush path")`. The pointer is stored, so it must outlive the lock (use a string literal).
- Keep critical sections to nanoseconds-to-microseconds and never sleep, block, or do I/O inside one
  (the demo's stall phase does so deliberately, to trigger the watchdog).

### 3.2 Reading the sensors

```c
uint32_t d = sm_ticket_depth(&orders);        /* instantaneous queue depth */

sm_stats s;
sm_snapshot(&orders, &s);                     /* sums all per-thread slots */
```

`sm_stats` fields:

| Field | Meaning | Diffable? |
|---|---|---|
| `acquisitions` | Total successful locks | yes |
| `contended` | Acquisitions that spun at least once | yes |
| `spins` | Total spin iterations across all waits | yes |
| `max_spins` | Largest single wait seen by any slot (running max) | **no**, it is a running maximum |
| `depth` | Same as `sm_ticket_depth` at snapshot time | n/a, instantaneous |

Counters are cumulative since `sm_ticket_init`. To get a rate, take two snapshots and subtract.

**Depth reading.** `0` means free, `1` means held with no waiters, `>= 2` means a queue. It can never
exceed the number of threads using the lock. It is read as `serving` first, then `next`, so the
subtraction cannot go negative.

**Derived metrics worth computing:**

| Metric | Formula | What it tells you |
|---|---|---|
| Contended ratio | `Δcontended / Δacquisitions` | How often a locker finds the lock busy |
| Spins per acquisition | `Δspins / Δacquisitions` | Average wait cost across all lockers |
| Spins per contended acquisition | `Δspins / Δcontended` | How long a wait lasts when one happens |

The last two together separate two different problems:

- **High contended ratio, low spins per contended acquisition:** many brief collisions. The lock is
  hot but each hold is short. A throughput question, usually solved by sharding.
- **Low contended ratio, huge spins per contended acquisition:** rare, long waits. This is the
  signature of a preempted or slow holder. Look at the watchdog, not the average.

You can see the second pattern in the demo on a 1-vCPU machine: well under 1% of acquisitions are
contended, yet spins per acquisition reaches the tens or hundreds, because a handful of waiters spin
through a whole scheduler timeslice.

**Converting spins to time.** One spin is one `pause`/`yield` plus a load. Its duration differs
widely between CPU generations, so spin counts are not comparable across hardware. Calibrate per machine:

```c
uint64_t t0 = sm_now_ns();
for (int i = 0; i < 1000000; i++) sm_relax();
double ns_per_spin = (double)(sm_now_ns() - t0) / 1e6;   /* approximate */
```

### 3.3 The stall watchdog

Pass a nonzero `stall_ns` and a callback to `sm_ticket_init`.

```c
static void on_stall(const sm_stall_report *r, void *ctx)
{
    /* runs on the *waiter's* thread, inside its spin loop */
}
```

Behavior:

- A waiter checks the clock every 256 spins. Once it has waited at least `stall_ns`, it may fire.
- Alerts are rate-limited **per lock**: at most one per `stall_ns` across all waiters (a shared
  compare-and-swap on `last_alert_ns`). A persistent stall therefore re-reports every `stall_ns`.
- The waiter **keeps waiting** after reporting. A ticket cannot be abandoned without breaking the queue,
  so the watchdog surfaces a stall; it cannot escape one.
- With `stall_ns = 0` the waiter never reads the clock.

`sm_stall_report` fields:

| Field | Meaning |
|---|---|
| `lock_name` | Name given at init |
| `waiter_tid` | Kernel tid of the reporting waiter |
| `waiter_ahead` | Tickets ahead of this waiter, **including the current holder** (`1` means the waiter is next) |
| `holder_tid`, `holder_cpu` | Tid and CPU recorded by the holder when it acquired (0 if it just released) |
| `holder_site` | The `SM_LOCK` call site (or your label) of the current holder |
| `held_ns` | Time since the holder acquired |
| `waited_ns` | How long this waiter has waited |
| `depth` | Queue depth at report time |

Constraints on the callback:

- It runs while the thread is spin-waiting. Keep it fast. Do not block, and never take the same lock.
  Push the report into a ring buffer or an `eventfd` and let another thread log it. (The demo uses
  `printf` for readability only.)
- Holder fields are read independently and are best-effort. During a handoff they can briefly describe
  two different holders.

What the watchdog does **not** do:

- It does not capture the holder's stack. It gives you the holder's tid, CPU, and call site; for a
  stack you would signal that tid and `backtrace()` from a handler.
- It does not fire when nobody is waiting. A holder stuck with no contenders is silent. A periodic
  sampler can read `orders.holder` (tid, `since_ns`, site) directly if you need that case covered.

### 3.4 Publishing samples and running the monitor core

The monitor consumes a seqlock-protected feed. You run one publisher (single writer), typically a
sampler thread that snapshots the lock and publishes windowed rates:

```c
static sm_feed    feed;
static sm_monitor mon;

mon.feed       = &feed;
mon.cpu        = 7;          /* a core nothing else uses; -1 = don't pin */
mon.depth_trip = 4;          /* trip at depth >= 4 ... */
mon.spa_trip   = 1500;       /* ... or spins/acq >= 15.00 (value is x100) */

pthread_t t;
pthread_create(&t, NULL, sm_monitor_main, &mon);

/* sampler thread, every 5 ms */
sm_stats prev, cur;
sm_snapshot(&orders, &prev);
for (;;) {
    usleep(5000);
    sm_snapshot(&orders, &cur);
    uint64_t da = cur.acquisitions - prev.acquisitions;
    sm_sample x = {
        .pub_ns             = sm_now_ns(),
        .depth              = cur.depth,
        .spins_per_acq_x100 = da ? (cur.spins - prev.spins) * 100 / da : 0,
        .contended_pct      = da ? (cur.contended - prev.contended) * 100 / da : 0,
    };
    sm_feed_publish(&feed, &x);
    prev = cur;
}
```

Workers (or a load balancer, or an admission controller) consume the breaker:

```c
if (atomic_load_explicit(&mon.breaker, memory_order_acquire)) { /* shed or back off */ }
```

Semantics:

- **Trip:** the breaker goes on when `depth >= depth_trip` **or** `spins_per_acq_x100 >= spa_trip`.
- **Clear:** it goes off only when `depth <= depth_trip / 2` **and** `spins_per_acq_x100 <= spa_trip / 2`.
  The gap is hysteresis. A sample between half and full threshold leaves the state unchanged
  (`test_monitor_lease.c` checks exactly this).
- **The monitor spins at 100% on its core.** Give it a core that nothing else uses. This is the
  trade for reaction speed; if you do not need sub-millisecond reaction, a normal sleeping thread is better.
- **Reaction latency** (`last_react_ns`, `max_react_ns`) is measured from the sample's `pub_ns` to the
  trip. It does not include the publisher's window. Real time-to-detect is roughly the publish interval
  (5 ms above) plus the reaction latency.
- `sm_feed_read` can be called from any number of additional reader threads. It returns a consistent
  copy and the sequence number it read.
- A window with very few acquisitions gives a noisy ratio. If your lock can go quiet, add a minimum
  acquisition count before publishing a nonzero `spins_per_acq_x100`.
- `umwait`/`tpause` (power-efficient waiting on newer Intel CPUs) is not implemented. It needs a
  runtime CPU-feature check and a separate code path.

### 3.5 The lease word

A lock-word-as-heartbeat for detecting a dead or wedged owner without a separate health channel.
It is safe in shared memory across processes (`mmap` with `MAP_SHARED`, or `shm_open`). A zeroed
`sm_lease` is free.

```
word  = [ heartbeat_us : 48 bits | owner_id : 16 bits ]     owner_id 0 means free
epoch = fencing token, incremented on every acquire / takeover
```

```c
static sm_lease L;
uint64_t fence = 0; uint16_t prev = 0;

if (sm_lease_acquire(&L, /*me=*/1, /*lease_ns=*/100000000ull, &fence, &prev)) {
    /* owner; if prev != 0 we took over from a silent owner */
}

/* owner, periodically, well inside lease_ns (e.g. every lease_ns / 3) */
if (!sm_lease_renew(&L, 1, fence)) { /* lost the lease: stop acting as owner */ }

/* monitor, no side effects */
uint16_t owner; uint64_t age_ns;
if (sm_lease_peek(&L, &owner, &age_ns) && age_ns > lease_ns) { /* expired */ }

sm_lease_release(&L, 1);
```

Points that matter:

- **`me` must be nonzero** and unique among contenders.
- **Takeover is a compare-and-swap**, so exactly one contender wins. But the old owner may be alive and
  merely paused (GC pause, `SIGSTOP`, VM freeze). Takeover cannot tell the difference.
- **That is what the fence is for.** `sm_lease_renew` refuses an owner whose fence is stale, which is the
  "fenced out" line in the demo. Anything the owner *writes* to a downstream resource must carry the
  fence, and the resource must reject a token lower than the highest it has seen.
- The epoch is incremented just after the CAS. `lease_ns` must be vastly larger than that window,
  which it is (nanoseconds against milliseconds).
- Time source is `CLOCK_MONOTONIC`, shared by all processes on one host. This does **not** work across hosts.
- The 48-bit microsecond heartbeat wraps after about 8.9 years of uptime.
- Liveness is judged only by heartbeat age. An owner that is busy but not heartbeating looks dead.

### 3.6 The rendezvous (moving destination)

A destination that changes when authorized followers ping, so that only followers know the current one.
Built on the ticket lock (state guard) and the lease (the server role is fenceable).

```c
static sm_rendezvous R;                       /* static or aligned */
sm_rv_open(&R, /*server*/1, /*lease_ns*/1e9, /*every_n*/1, /*every_ns*/0, /*grace_ns*/50e6);

uint64_t key[2]; sm_rv_gen_key(key);          /* give to the follower out of band */
sm_rv_enroll(&R, /*id*/5, key);

/* follower */
sm_rv_reply r;
if (sm_rv_ping(&R, 5, known_epoch, sm_rv_proof(key, 5, known_epoch), &r) == SM_RV_OK) {
    known_epoch = r.epoch;
    dest = sm_rv_unseal(key, 5, &r);          /* the new reference */
}

/* resource side */
if (sm_rv_resolve(&R, presented_dest)) { /* serve */ }
```

- `dest(epoch) = SipHash-2-4(master_key, epoch)`; the reply is sealed with a per-follower keyed pad, so a captured reply is useless without that follower's key.
- Only a valid ping from an enrolled follower can rotate. Unknown ids (`SM_RV_DENIED`) and wrong keys (`SM_RV_BAD_PROOF`) get nothing and change nothing.
- Rotation policy: every `every_n` valid pings and/or when `every_ns` has elapsed since the last rotation.
- `grace_ns`: the previous destination still resolves for that long, so a follower with a ping in flight is not cut off. Two rotations back is always dead.
- `sm_rv_revoke` + `sm_rv_rotate` cuts a follower off. A server that loses its lease is fenced (`SM_RV_FENCED`).
- Scope: same-host shared memory, not a network protocol. A proof can be replayed within an epoch (replies stay sealed). Anything that can read the struct's memory can read the master key; the guarantee is against parties that use only the API.

---

## 4. Cost model

Measured with a single thread, uncontended, 5 M lock/unlock pairs, best of 5, `-O2`, x86-64, on a shared
1-vCPU VM. Treat these as orders of magnitude, not benchmarks:

| Variant | ns per lock+unlock |
|---|---|
| Bare ticket lock (no instrumentation) | 9.1 |
| `sm_ticket` with `-DSPINMON_NO_HOLDER` | 9.0 |
| `sm_ticket` default | 41.1 |
| `pthread_spin_lock` | 11.7 |
| `pthread_mutex_lock` | 6.4 |

What this says:

- **The counters are effectively free** when uncontended (relaxed load and store on a thread-private cache line).
- **The holder stamp is the cost**, about 30 ns: one `clock_gettime`, one `sched_getcpu`, four stores.
- **The stamp runs inside the critical section**, because you only know you hold the lock after acquiring
  it. It therefore lengthens *every hold*, which increases real contention on very short critical sections.
  If you are measuring contention on a hot lock, A/B with and without `-DSPINMON_NO_HOLDER` to bound
  that observer effect.
- Memory: `sizeof(sm_ticket)` is 2304 bytes (32 slots of 64 bytes plus the hot lines). `sm_lease` and
  `sm_feed` are one cache line each.
- Slots are assigned in thread registration order, modulo 32. Beyond 32 threads, slots are shared and
  counts become slightly lossy (updates are not atomic read-modify-write). Still fine as a signal; not
  exact accounting.

---

## 5. spinmon vs. regular monitoring in regression tests

"Regular" here means black-box: run a workload, measure outcomes (throughput, latency percentiles,
CPU%), compare against a baseline with a tolerance, and reach for a profiler (`perf`, `lock_stat`,
eBPF `klockstat`) only after something fails.

spinmon is a white-box sensor. It measures the *mechanism* (who is waiting on what, for how long)
rather than the *outcome*.

| | Regular regression monitoring | spinmon |
|---|---|---|
| **Measures** | Outcomes: ops/s, p50/p99 latency, CPU% | Mechanism: queue depth, spins per acquisition, stalled holders |
| **Vantage point** | Outside the code, end to end | Inside the lock's own wait loop |
| **Coverage** | Everything, including I/O, allocation, cache misses | Only locks you converted to `sm_ticket` |
| **Collection** | Latency histograms from sampled requests; profilers at ~100 Hz | Every acquisition is counted; the watchdog is event-driven on the waiter |
| **Dilution** | Lock wait is averaged in with all other work | Measures the queue directly, so less diluted by work outside the lock |
| **Attribution** | "p99 regressed"; a second tool run finds why | The stall report names holder tid, CPU, call site, and hold time at the moment it happens |
| **Typical noise** | CPU frequency, noisy neighbors, cache state, input mix | Preemption of holder or waiters, `pause` latency by CPU model, thread placement |
| **Baseline portability** | Per machine class | Per CPU model, and more strictly so (spin counts are not comparable across generations) |
| **Cost to the system under test** | None to low | ~0 ns for counters, ~30 ns holder stamp inside the critical section |
| **Blind spots** | Regressions that hide inside the tolerance | Any contention you did not instrument (mutexes, I/O, allocator, memory bandwidth) |
| **Testing the alert path** | Hard: you must wait for a real incident | Easy: inject a stall and assert the callback fired with the right site |
| **Determinism of the logic** | Statistical by nature | The monitor and lease logic can be tested with synthetic inputs (see below) |

### Where spinmon is the better tool

**It can notice a change before the outcome metrics do.** Under queueing, waiting time grows faster than
the hold time that causes it. When a refactor widens a critical section (a log call, a map insert, or an
allocation under the lock), spins per acquisition can move visibly while end-to-end throughput stays
inside its noise band, because the lock is not yet the saturated bottleneck. This is reasoning from
queueing behavior, not a measured result from this repository. Verify it on your workload before trusting it.

**It separates "often and brief" from "rare and long."** Throughput and mean latency conflate them. The
contended ratio against spins per contended acquisition (section 3.2) does not.

**It gives attribution with the symptom.** A black-box failure says *that* something got slower. A
watchdog report says which thread held which lock from which line for how long, at the time.

**It makes alerting testable.** A regression suite normally cannot prove that your stall alert works
without a real incident. Here you hold a lock for 60 ms on purpose and assert the callback ran and named
the expected `file:line`.

**Part of it is deterministic.** Throughput tests are statistical. The monitor's trip/hysteresis logic
and the lease protocol take synthetic inputs and give exact answers. `test_monitor_lease.c` does this with
no contending workload at all.

### Where regular monitoring is the better tool

- **It sees everything; spinmon sees the locks you converted.** If the regression is in the allocator,
  the network stack, or cache behavior, spinmon reports nothing.
- **Spin counts measure the scheduler as well as the lock.** If a holder is preempted, every waiter spins
  through the gap. On a shared or oversubscribed CI runner that inflates spins and can fail a gate for
  reasons unrelated to your change. In the demo on a 1-vCPU box, spins per acquisition was in the tens to
  hundreds with well under 1% of acquisitions contended, entirely from preemption.
- **It does not translate across hardware.** `pause` is a handful of cycles on some CPUs and over a
  hundred on others. A baseline recorded on one CPU model is not valid on another.
- **It perturbs what it measures.** The holder stamp lengthens each hold. Use `-DSPINMON_NO_HOLDER` to
  bound the effect.
- **It needs code changes.** Black-box monitoring works on any binary. spinmon requires swapping lock types.
- **Ticket locks are unforgiving when oversubscribed.** They are strictly FIFO, so a preempted waiter
  stalls everyone behind it. In testing here, an oversubscribed run (2 threads pinned to 1 CPU,
  300 000 operations each) did not complete within two minutes. A regression test must refuse to run
  without enough cores (section 6).

### Recommended layering

1. **Keep the black-box gate** (throughput/latency vs. baseline). It covers everything and is what users feel.
2. **Add spinmon counters as a diagnostic and early-warning layer** on the few locks that matter. Start
   as a non-blocking report. Promote to a hard gate only on dedicated runners after you have seen its variance.
3. **Keep the profilers** (`perf lock`, `lock_stat`, `klockstat`) for locks you do not own and for
   confirming a spinmon finding.
4. **Use spinmon's deterministic parts as plain unit tests**: monitor, hysteresis, lease takeover, fencing.

---

## 6. Writing regression tests with spinmon

### 6.1 Principles

- **Pin threads** (`sm_pin(cpu)`) and **require `workers + 1` CPUs**. Return an explicit skip (exit code
  77, Automake/CTest convention) instead of running oversubscribed.
- **Fixed work, not fixed time.** Count operations. A time-boxed run makes the denominator depend on speed.
- **Several trials, assert on the median.** Re-init the lock per trial for fresh counters.
- **Compare to a baseline from the same runner class.** Record on the baseline commit, compare on the
  candidate. Do not hard-code absolute spin numbers.
- **Use a ratio plus an absolute floor**, so a near-zero baseline is not a hair trigger.
- **Treat any watchdog report in steady state as a failure** and print the report; it already says who and where.

### 6.2 Recipes

| Recipe | Assert |
|---|---|
| **Contention budget** | Median spins per acquisition at N threads is within `baseline * 1.5 + 0.5` |
| **No steady-state stalls** | Zero watchdog callbacks during a normal run |
| **Alert path works** | Hold the lock past `stall_ns`; expect at least one callback and `holder_site` containing the expected `file:line` |
| **Scaling** | Repeat at 1, 2, 4, 8 threads; spins per acquisition should not blow up between steps |
| **Monitor logic** | Publish synthetic samples; breaker trips on hot, holds on mid, clears on cool |
| **Lease takeover** | Stop heartbeating; takeover is refused before `lease_ns`, succeeds after, fence advances by one, old owner's renew is refused |

### 6.3 The contention gate

`test_contention.c` implements the first two recipes. Core of it:

```c
#define SPINMON_NO_DEMO
#include "spinmon.c"

static void *worker(void *arg)
{
    sm_pin((int)(intptr_t)arg);
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        SM_LOCK(&lock);
        /* the critical section under test */
        sm_unlock(&lock);
    }
    return NULL;
}

static double run_trial(long ncpu)
{
    sm_ticket_init(&lock, "test", 50000000ull, on_stall, NULL);   /* fresh counters */
    /* start NTHREADS workers, join them */
    sm_stats s;
    sm_snapshot(&lock, &s);
    return s.acquisitions ? (double)s.spins / (double)s.acquisitions : 0.0;
}
```

Use it as:

```sh
gcc -std=c11 -O2 -pthread test_contention.c -o test_contention
./test_contention record     # on the baseline commit: writes baseline.txt
./test_contention            # on the candidate: exit 0 pass, 1 fail, 2 no baseline, 77 skipped
```

Tunables are macros (`NTHREADS`, `OPS_PER_THREAD`, `TRIALS`, `TOLERANCE`, `ABS_FLOOR`). Set
`SPINMON_ALLOW_OVERSUBSCRIBE=1` to bypass the CPU-count guard on a dev machine, with a small
`OPS_PER_THREAD` (see the convoy warning in section 5).

### 6.4 Deterministic tests

`test_monitor_lease.c` needs no contending workload.

Monitor logic, driven by synthetic samples:

```c
static sm_feed feed;  static sm_monitor m;     /* static: zeroed and aligned */
m.feed = &feed; m.cpu = -1; m.depth_trip = 8; m.spa_trip = 1000;
pthread_create(&t, NULL, sm_monitor_main, &m);

sm_sample hot = { .pub_ns = sm_now_ns(), .depth = 9 };
sm_feed_publish(&feed, &hot);
/* wait for m.breaker == true; then check m.last_react_ns against your bound */
```

Lease fault injection, where "the holder died" is just "stop calling renew":

```c
CHECK(sm_lease_acquire(&L, 1, LEASE, &f1, NULL));
CHECK(!sm_lease_acquire(&L, 2, LEASE, &f2, &prev));   /* fresh: refused */
usleep(/* well past LEASE */);
CHECK(sm_lease_acquire(&L, 2, LEASE, &f2, &prev));    /* takeover */
CHECK(prev == 1 && f2 == f1 + 1);                      /* fence advanced by one */
CHECK(!sm_lease_renew(&L, 1, f1));                     /* old owner fenced out */
```

The wall-clock assertions use generous margins (60 ms of slack on a 100 ms lease) because sleeps are
lower bounds. Reaction-latency bounds in the monitor test are deliberately loose (50 ms); on a
dedicated pinned core you can tighten them considerably.

### 6.5 CI environment checklist

- Dedicated or `cpuset`-isolated cores for the workers and the monitor; no SMT sibling of a busy core.
- Pin the CPU governor (`performance`) if you can; frequency changes shift spin cost.
- Check steal time on virtualized runners. High steal means preempted vCPUs, which means inflated spins.
- Record the CPU model next to the baseline, and invalidate the baseline when it changes.
- Start with the gate as a **warning**. Promote to a failure only after you have seen its run-to-run variance.

---

## 7. Calibrating thresholds

The defaults in the demo (`depth_trip = workers`, `spa_trip = 1500`) are placeholders. They tripped
during the light phase on the 1-vCPU test box, which is the preemption inflation described above.
To set real values:

1. Run a representative steady-state workload on the target hardware class with the sampler publishing
   (or just logging) windowed `depth` and `spins_per_acq_x100`.
2. Collect the distribution across many windows. Look at the tail (p99.9), not the mean.
3. Set the trip a safe margin above the observed steady-state tail. The clear level is half of it.
4. Re-do this per CPU model, and after any change to critical-section length.
5. Choose the sampling window deliberately: shorter windows detect faster but give noisier ratios when
   the lock is quiet.

---

## 8. Limitations

- **Spin locks fit only very short, non-blocking critical sections.** A preempted holder makes every
  waiter burn CPU. Ticket locks are FIFO and degrade worse than test-and-set under oversubscription.
- **The watchdog reports; it cannot abort a wait.** And it only fires when someone is waiting.
- **Holder information is best-effort**, no stack capture, and can mix two holders during a handoff.
- **Instrumentation shifts what it measures** (about 30 ns per hold from the stamp).
- **Counters are approximate beyond 32 threads** (shared slots, non-atomic read-modify-write).
- **Spin counts are hardware-specific** and inflate under preemption.
- **Lease:** heartbeat-only liveness, same-host `CLOCK_MONOTONIC`, 16-bit owner ids, ~8.9 year timestamp
  wrap, and downstream resources must actually enforce the fence.
- **Seqlock feed is single-writer.**
- **Monitor burns a core** by design. `umwait` is not implemented.
- **Linux only** (`gettid`, `sched_getcpu`, `pthread_setaffinity_np`).

---

## 9. Verification status

What was run, and where:

- Built clean with `gcc -std=c11 -O2 -Wall -Wextra -pthread` in three modes: demo, `-DSPINMON_NO_HOLDER`, and
  `-DSPINMON_NO_DEMO` (included from another file).
- ThreadSanitizer run over the demo and over `test_monitor_lease.c`: no data-race reports.
- Demo exercised end to end: the watchdog fired on the injected stall, the breaker tripped and cleared,
  the lease was taken over and the old owner fenced out.
- `test_monitor_lease.c` passes. `test_contention.c` was exercised through its skip (77), no-baseline (2),
  record (0), and pass (0) paths. The fail (1) path was forced with a deliberately negative baseline, because
  the box could not produce real contention (see below).
- Microbenchmark numbers in section 4.

What was **not** done: everything above ran on a single-vCPU x86-64 VM. There are no multi-core contention
numbers, no real reaction-latency measurements on a dedicated core, no aarch64 run, and no measurement of the
"counters move before throughput does" claim in section 5. Those need real hardware.

---

## 10. API reference

**Lock**

| Function | Notes |
|---|---|
| `sm_ticket_init(l, name, stall_ns, cb, ctx)` | Zeroes the struct. `stall_ns = 0` disables the watchdog |
| `SM_LOCK(l)` / `sm_lock_at(l, site)` | Acquire; `site` must have static lifetime |
| `sm_unlock(l)` | Owner only |
| `sm_ticket_depth(l)` | Holders + waiters |
| `sm_snapshot(l, &stats)` | Sum of per-slot counters plus current depth |

**Lease**

| Function | Notes |
|---|---|
| `sm_lease_acquire(L, me, lease_ns, &fence, &stolen_from)` | Acquire if free or expired; `stolen_from` may be `NULL` |
| `sm_lease_renew(L, me, fence)` | Heartbeat; false means lost or fenced |
| `sm_lease_release(L, me)` | False if not the owner |
| `sm_lease_peek(L, &owner, &age_ns)` | Monitor-side read; false if free |

**Rendezvous**

| Function | Notes |
|---|---|
| `sm_rv_open(R, server, lease_ns, every_n, every_ns, grace_ns)` | Take the server role; false if no entropy or lease |
| `sm_rv_gen_key(key)` / `sm_rv_enroll(R, id, key)` / `sm_rv_revoke(R, id)` | Follower management |
| `sm_rv_ping(R, id, known_epoch, proof, &reply)` | Follower ping; may rotate. Returns `sm_rv_status` |
| `sm_rv_proof(key, id, known_epoch)` / `sm_rv_unseal(key, id, &reply)` | Follower-side helpers |
| `sm_rv_resolve(R, dest)` | Resource side: is `dest` live (or in grace)? |
| `sm_rv_rotate(R)` | Force a rotation |

**Feed and monitor**

| Function | Notes |
|---|---|
| `sm_feed_publish(f, &sample)` | Single writer |
| `sm_feed_read(f, &sample)` | Any reader; returns the sequence number read |
| `sm_monitor_main(&mon)` | Thread entry; set `stop` to end it |
| `sm_pin(cpu)` | Pin the calling thread |

**Utilities:** `sm_now_ns()` (monotonic), `sm_relax()` (`pause`/`yield`), `sm_tid()` (cached `gettid`).
