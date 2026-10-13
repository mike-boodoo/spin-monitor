/*
 * spinmon.c - spin primitives that double as monitoring sensors.
 *
 *   1. Ticket lock           queue depth is next - serving; no extra instrumentation
 *   2. Spin accounting       per-thread-slot counters -> snapshot -> rates (leading indicator)
 *   3. Bounded-spin watchdog on stall, report holder tid / cpu / call site / hold time
 *   4. Lease word            shared-memory lease: heartbeat, takeover, fencing token
 *   5. Seqlock feed +        pinned busy-poll monitor core that trips a circuit breaker
 *
 * Build:  gcc -std=c11 -O2 -Wall -Wextra -pthread spinmon.c -o spinmon
 * Run:    ./spinmon
 * Flags:  -DSPINMON_NO_DEMO    omit main() so this file can be #included from a test/app
 *         -DSPINMON_NO_HOLDER  skip the holder stamp (no tid/cpu/site/hold time in stall
 *                              reports); removes most per-acquisition overhead, for A/B runs
 * Target: Linux, x86-64 or aarch64.
 *
 * Caveats (also true of any spin lock):
 *   - Only use SM_LOCK around very short critical sections that never block.
 *     A preempted holder makes every waiter burn CPU; ticket locks are FIFO,
 *     so they degrade worse than test-and-set under oversubscription.
 *   - The watchdog reports and keeps waiting. A ticket cannot be abandoned
 *     without breaking the queue, so a stall is surfaced, not escaped.
 *   - Holder info is best-effort diagnostics: fields are read independently
 *     and can briefly describe two different holders during a handoff.
 *   - Per-slot counters are exact up to SM_SLOTS threads; beyond that, slots
 *     are shared and counts become slightly lossy (still fine as a signal).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define SM_CL 64
typedef _Atomic uint64_t sm_u64;

/* ------------------------------------------------------------------ */
/* utilities                                                           */
/* ------------------------------------------------------------------ */

static inline uint64_t sm_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void sm_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

static inline uint32_t sm_tid(void)
{
    static _Thread_local uint32_t t;
    if (!t) t = (uint32_t)syscall(SYS_gettid);
    return t;
}

static int sm_pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

/* ------------------------------------------------------------------ */
/* 1-3. instrumented ticket lock                                       */
/* ------------------------------------------------------------------ */

#define SM_SLOTS 32

typedef struct {
    _Alignas(SM_CL) sm_u64 acquisitions;
    sm_u64 contended, spins, max_spins;
} sm_slot;

typedef struct {
    const char *lock_name;
    uint32_t waiter_tid, waiter_ahead;
    uint32_t holder_tid, holder_cpu;
    const char *holder_site;
    uint64_t held_ns, waited_ns;
    uint32_t depth;
} sm_stall_report;

typedef void (*sm_stall_cb)(const sm_stall_report *, void *ctx);

typedef struct {
    /* cold config */
    const char *name;
    uint64_t stall_ns;               /* 0 disables the watchdog          */
    sm_stall_cb on_stall;
    void *on_stall_ctx;
    sm_u64 last_alert_ns;            /* per-lock alert rate limit        */

    _Alignas(SM_CL) _Atomic uint32_t next;     /* ticket dispenser       */
    _Alignas(SM_CL) _Atomic uint32_t serving;  /* waiters spin here      */

    _Alignas(SM_CL) struct {                   /* written by the holder  */
        _Atomic uint32_t tid, cpu;
        sm_u64 since_ns;
        _Atomic(const char *) site;
    } holder;

    _Alignas(SM_CL) sm_slot slots[SM_SLOTS];
} sm_ticket;

#define SM_STR2(x) #x
#define SM_STR(x) SM_STR2(x)
#define SM_SITE (__FILE__ ":" SM_STR(__LINE__))
#define SM_LOCK(l) sm_lock_at((l), SM_SITE)

static void sm_ticket_init(sm_ticket *l, const char *name, uint64_t stall_ns,
                           sm_stall_cb cb, void *ctx)
{
    memset(l, 0, sizeof *l);
    l->name = name;
    l->stall_ns = stall_ns;
    l->on_stall = cb;
    l->on_stall_ctx = ctx;
}

/* Holders + waiters. Read serving first so the subtraction cannot go negative. */
static inline uint32_t sm_ticket_depth(sm_ticket *l)
{
    uint32_t s = atomic_load_explicit(&l->serving, memory_order_acquire);
    uint32_t n = atomic_load_explicit(&l->next, memory_order_acquire);
    return n - s;
}

static inline unsigned sm_slot_index(void)
{
    static atomic_uint next_slot;
    static _Thread_local int mine = -1;
    if (mine < 0)
        mine = (int)(atomic_fetch_add_explicit(&next_slot, 1, memory_order_relaxed) % SM_SLOTS);
    return (unsigned)mine;
}

static void sm_check_stall(sm_ticket *l, uint64_t t0, uint32_t my_ticket)
{
    uint64_t now = sm_now_ns();
    if (now - t0 < l->stall_ns) return;

    uint64_t last = atomic_load_explicit(&l->last_alert_ns, memory_order_relaxed);
    if (now - last < l->stall_ns) return;
    if (!atomic_compare_exchange_strong(&l->last_alert_ns, &last, now)) return;
    if (!l->on_stall) return;

    uint64_t since = atomic_load_explicit(&l->holder.since_ns, memory_order_relaxed);
    sm_stall_report r = {
        .lock_name   = l->name,
        .waiter_tid  = sm_tid(),
        .waiter_ahead = my_ticket - atomic_load_explicit(&l->serving, memory_order_relaxed),
        .holder_tid  = atomic_load_explicit(&l->holder.tid, memory_order_relaxed),
        .holder_cpu  = atomic_load_explicit(&l->holder.cpu, memory_order_relaxed),
        .holder_site = atomic_load_explicit(&l->holder.site, memory_order_relaxed),
        .held_ns     = since && now > since ? now - since : 0,
        .waited_ns   = now - t0,
        .depth       = sm_ticket_depth(l),
    };
    l->on_stall(&r, l->on_stall_ctx);
}

static void sm_lock_at(sm_ticket *l, const char *site)
{
    const uint32_t t = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    uint64_t spins = 0;

    if (atomic_load_explicit(&l->serving, memory_order_acquire) != t) {
        const uint64_t t0 = l->stall_ns ? sm_now_ns() : 0;
        do {
            sm_relax();
            spins++;
            /* amortise the clock read: check the deadline every 256 spins */
            if (l->stall_ns && (spins & 0xFF) == 0) sm_check_stall(l, t0, t);
        } while (atomic_load_explicit(&l->serving, memory_order_acquire) != t);
    }

    /* lock owned: account (relaxed load+store; slot is thread-private) */
    sm_slot *s = &l->slots[sm_slot_index()];
    atomic_store_explicit(&s->acquisitions,
        atomic_load_explicit(&s->acquisitions, memory_order_relaxed) + 1, memory_order_relaxed);
    if (spins) {
        atomic_store_explicit(&s->contended,
            atomic_load_explicit(&s->contended, memory_order_relaxed) + 1, memory_order_relaxed);
        atomic_store_explicit(&s->spins,
            atomic_load_explicit(&s->spins, memory_order_relaxed) + spins, memory_order_relaxed);
        if (spins > atomic_load_explicit(&s->max_spins, memory_order_relaxed))
            atomic_store_explicit(&s->max_spins, spins, memory_order_relaxed);
    }

#ifndef SPINMON_NO_HOLDER
    /* publish holder identity for the watchdog (a clock read + sched_getcpu + 4 stores,
     * executed inside the critical section, so it lengthens every hold) */
    atomic_store_explicit(&l->holder.tid, sm_tid(), memory_order_relaxed);
    atomic_store_explicit(&l->holder.cpu, (uint32_t)sched_getcpu(), memory_order_relaxed);
    atomic_store_explicit(&l->holder.since_ns, sm_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&l->holder.site, site, memory_order_relaxed);
#else
    (void)site;
#endif
}

static inline void sm_unlock(sm_ticket *l)
{
#ifndef SPINMON_NO_HOLDER
    atomic_store_explicit(&l->holder.tid, 0, memory_order_relaxed);
#endif
    uint32_t s = atomic_load_explicit(&l->serving, memory_order_relaxed); /* only holder writes */
    atomic_store_explicit(&l->serving, s + 1, memory_order_release);
}

typedef struct {
    uint64_t acquisitions, contended, spins, max_spins;
    uint32_t depth;
} sm_stats;

static void sm_snapshot(sm_ticket *l, sm_stats *o)
{
    memset(o, 0, sizeof *o);
    for (int i = 0; i < SM_SLOTS; i++) {
        sm_slot *s = &l->slots[i];
        o->acquisitions += atomic_load_explicit(&s->acquisitions, memory_order_relaxed);
        o->contended    += atomic_load_explicit(&s->contended, memory_order_relaxed);
        o->spins        += atomic_load_explicit(&s->spins, memory_order_relaxed);
        uint64_t m = atomic_load_explicit(&s->max_spins, memory_order_relaxed);
        if (m > o->max_spins) o->max_spins = m;
    }
    o->depth = sm_ticket_depth(l);
}

/* ------------------------------------------------------------------ */
/* 4. lease word (works in shared memory across processes)             */
/*                                                                     */
/*   word  = [ heartbeat_us : 48 | owner_id : 16 ]   owner 0 == free   */
/*   epoch = fencing token, bumped on every acquire / takeover         */
/*                                                                     */
/* The epoch is bumped just after the CAS, so lease_ns must be much    */
/* larger than that window (it is: nanoseconds vs. milliseconds).      */
/* Downstream resources should reject writes carrying a stale fence.   */
/* ------------------------------------------------------------------ */

typedef struct {
    _Alignas(SM_CL) sm_u64 word;
    sm_u64 epoch;
} sm_lease;

#define SM_LEASE_OWNER(w) ((uint16_t)((w) & 0xFFFFu))

static inline uint64_t sm_lease_pack(uint64_t now_ns, uint16_t owner)
{
    return ((now_ns / 1000u) << 16) | owner;
}

static inline uint64_t sm_lease_age_ns(uint64_t w, uint64_t now_ns)
{
    uint64_t now_us = now_ns / 1000u, ts = w >> 16;
    return now_us > ts ? (now_us - ts) * 1000u : 0;
}

/* Monitor side: true if held; reports owner and time since last heartbeat. */
static bool sm_lease_peek(sm_lease *L, uint16_t *owner, uint64_t *age_ns)
{
    uint64_t w = atomic_load_explicit(&L->word, memory_order_acquire);
    if (SM_LEASE_OWNER(w) == 0) return false;
    *owner = SM_LEASE_OWNER(w);
    *age_ns = sm_lease_age_ns(w, sm_now_ns());
    return true;
}

/* Acquire if free, or take over if the holder's heartbeat is older than lease_ns.
 * `me` must be nonzero. On success *fence is the new fencing token. */
static bool sm_lease_acquire(sm_lease *L, uint16_t me, uint64_t lease_ns,
                             uint64_t *fence, uint16_t *stolen_from)
{
    uint64_t now = sm_now_ns();
    uint64_t w = atomic_load_explicit(&L->word, memory_order_acquire);
    uint16_t cur = SM_LEASE_OWNER(w);
    if (cur != 0 && sm_lease_age_ns(w, now) < lease_ns) return false;
    if (!atomic_compare_exchange_strong_explicit(&L->word, &w, sm_lease_pack(now, me),
                                                 memory_order_acq_rel, memory_order_acquire))
        return false;                      /* someone renewed or beat us */
    if (stolen_from) *stolen_from = cur;
    *fence = atomic_fetch_add_explicit(&L->epoch, 1, memory_order_acq_rel) + 1;
    return true;
}

/* Heartbeat. False means the lease was lost (taken over): stop acting as owner. */
static bool sm_lease_renew(sm_lease *L, uint16_t me, uint64_t fence)
{
    uint64_t w = atomic_load_explicit(&L->word, memory_order_acquire);
    if (SM_LEASE_OWNER(w) != me) return false;
    if (atomic_load_explicit(&L->epoch, memory_order_acquire) != fence) return false;
    return atomic_compare_exchange_strong_explicit(&L->word, &w, sm_lease_pack(sm_now_ns(), me),
                                                   memory_order_acq_rel, memory_order_acquire);
}

static bool sm_lease_release(sm_lease *L, uint16_t me)
{
    uint64_t w = atomic_load_explicit(&L->word, memory_order_acquire);
    if (SM_LEASE_OWNER(w) != me) return false;
    return atomic_compare_exchange_strong_explicit(&L->word, &w, 0,
                                                   memory_order_acq_rel, memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* 5. seqlock feed + pinned busy-poll monitor                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t pub_ns;
    uint64_t depth;
    uint64_t spins_per_acq_x100;   /* spins per acquisition, x100 */
    uint64_t contended_pct;
} sm_sample;

typedef struct {
    _Alignas(SM_CL) sm_u64 seq;    /* odd while the writer is mid-update */
    sm_u64 pub_ns, depth, spa, cpct;
} sm_feed;

/* Single writer. */
static void sm_feed_publish(sm_feed *f, const sm_sample *x)
{
    uint64_t q = atomic_load_explicit(&f->seq, memory_order_relaxed);
    atomic_store_explicit(&f->seq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&f->pub_ns, x->pub_ns, memory_order_relaxed);
    atomic_store_explicit(&f->depth, x->depth, memory_order_relaxed);
    atomic_store_explicit(&f->spa, x->spins_per_acq_x100, memory_order_relaxed);
    atomic_store_explicit(&f->cpct, x->contended_pct, memory_order_relaxed);
    atomic_store_explicit(&f->seq, q + 2, memory_order_release);
}

/* Any number of readers. Returns the sequence of the consistent copy. */
static uint64_t sm_feed_read(sm_feed *f, sm_sample *out)
{
    for (;;) {
        uint64_t s1 = atomic_load_explicit(&f->seq, memory_order_acquire);
        if (s1 & 1) { sm_relax(); continue; }
        out->pub_ns = atomic_load_explicit(&f->pub_ns, memory_order_relaxed);
        out->depth = atomic_load_explicit(&f->depth, memory_order_relaxed);
        out->spins_per_acq_x100 = atomic_load_explicit(&f->spa, memory_order_relaxed);
        out->contended_pct = atomic_load_explicit(&f->cpct, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&f->seq, memory_order_relaxed) == s1) return s1;
        sm_relax();
    }
}

typedef struct {
    /* config */
    sm_feed *feed;
    int cpu;                        /* -1: do not pin */
    uint64_t depth_trip, spa_trip;  /* trip if either is reached; clear below half of both */
    /* control / output */
    atomic_bool breaker, stop;
    sm_u64 trips, last_react_ns, max_react_ns;
} sm_monitor;

static void *sm_monitor_main(void *arg)
{
    sm_monitor *m = arg;
    if (m->cpu >= 0) sm_pin(m->cpu);

    uint64_t last = 0;
    while (!atomic_load_explicit(&m->stop, memory_order_relaxed)) {
        uint64_t s = atomic_load_explicit(&m->feed->seq, memory_order_acquire);
        if (s == last || (s & 1)) { sm_relax(); continue; }

        sm_sample x;
        last = sm_feed_read(m->feed, &x);

        bool bad  = x.depth >= m->depth_trip || x.spins_per_acq_x100 >= m->spa_trip;
        bool calm = x.depth <= m->depth_trip / 2 && x.spins_per_acq_x100 <= m->spa_trip / 2;
        bool on = atomic_load_explicit(&m->breaker, memory_order_relaxed);

        if (!on && bad) {
            uint64_t lat = sm_now_ns() - x.pub_ns;
            atomic_store_explicit(&m->last_react_ns, lat, memory_order_relaxed);
            if (lat > atomic_load_explicit(&m->max_react_ns, memory_order_relaxed))
                atomic_store_explicit(&m->max_react_ns, lat, memory_order_relaxed);
            atomic_fetch_add_explicit(&m->trips, 1, memory_order_relaxed);
            atomic_store_explicit(&m->breaker, true, memory_order_release);
        } else if (on && calm) {
            atomic_store_explicit(&m->breaker, false, memory_order_release);
        }
    }
    return NULL;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifndef SPINMON_NO_DEMO

/* ------------------------------------------------------------------ */
/* demo                                                                */
/* ------------------------------------------------------------------ */

enum { PH_LIGHT, PH_HEAVY, PH_STALL };

static sm_ticket    g_orders;
static sm_feed      g_feed;
static sm_monitor   g_mon;
static atomic_int   g_phase;
static atomic_bool  g_stall_armed, g_stop;
static sm_u64       g_shed;

static void burn_ns(uint64_t ns)
{
    uint64_t end = sm_now_ns() + ns;
    while (sm_now_ns() < end) sm_relax();
}

static void on_stall(const sm_stall_report *r, void *ctx)
{
    (void)ctx;
    printf("  [watchdog] lock=%s waiter=%u holder=%u cpu=%u site=%s held=%.1fms "
           "waited=%.1fms ahead=%u depth=%u\n",
           r->lock_name, r->waiter_tid, r->holder_tid, r->holder_cpu,
           r->holder_site ? r->holder_site : "?", r->held_ns / 1e6, r->waited_ns / 1e6,
           r->waiter_ahead, r->depth);
}

static void *worker(void *arg)
{
    (void)arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        /* load shedding driven by the monitor core */
        if (atomic_load_explicit(&g_mon.breaker, memory_order_acquire)) {
            atomic_fetch_add_explicit(&g_shed, 1, memory_order_relaxed);
            usleep(200);
            continue;
        }
        int ph = atomic_load_explicit(&g_phase, memory_order_relaxed);

        SM_LOCK(&g_orders);
        burn_ns(ph == PH_HEAVY ? 300 : 200);              /* critical section */
        if (ph == PH_STALL && atomic_exchange(&g_stall_armed, false))
            usleep(60000);          /* deliberate bug: sleeping under a spin lock */
        sm_unlock(&g_orders);

        if (ph == PH_LIGHT) burn_ns(20000);               /* think time */
    }
    return NULL;
}

static const char *phase_name(int p)
{
    return p == PH_LIGHT ? "light" : p == PH_HEAVY ? "heavy" : "stall";
}

#define LEASE_NS 50000000ull   /* 50 ms */

static sm_lease g_lease;
static atomic_bool g_a_ready;

static void *lease_holder_a(void *arg)
{
    (void)arg;
    uint64_t fence;
    if (!sm_lease_acquire(&g_lease, 1, LEASE_NS, &fence, NULL)) return NULL;
    printf("  [lease] A acquired, fence=%" PRIu64 "\n", fence);
    atomic_store(&g_a_ready, true);

    for (int i = 0; i < 3; i++) {
        usleep(10000);
        sm_lease_renew(&g_lease, 1, fence);
    }
    printf("  [lease] A wedged: no heartbeats for 150 ms\n");
    usleep(150000);
    if (!sm_lease_renew(&g_lease, 1, fence))
        printf("  [lease] A woke up, renew refused: fenced out (token %" PRIu64 " is stale)\n", fence);
    return NULL;
}

static void demo_lease(void)
{
    printf("\n== lease word as heartbeat ==\n");
    pthread_t a;
    pthread_create(&a, NULL, lease_holder_a, NULL);
    while (!atomic_load(&g_a_ready)) usleep(100);

    bool flagged = false;
    uint64_t fence = 0;
    uint16_t prev = 0;
    for (;;) {
        uint16_t owner; uint64_t age;
        if (!flagged && sm_lease_peek(&g_lease, &owner, &age) && age > LEASE_NS) {
            printf("  [lease] monitor: owner %u silent for %.0f ms, lease expired\n", owner, age / 1e6);
            flagged = true;
        }
        if (sm_lease_acquire(&g_lease, 2, LEASE_NS, &fence, &prev)) break;
        usleep(2000);
    }
    printf("  [lease] B took over from %u, fence=%" PRIu64 "\n", prev, fence);
    pthread_join(a, NULL);
    sm_lease_release(&g_lease, 2);
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nworkers = (int)(ncpu - 1 < 2 ? 2 : ncpu - 1 > 4 ? 4 : ncpu - 1);

    sm_ticket_init(&g_orders, "orders", 20000000ull /* 20 ms */, on_stall, NULL);

    g_mon.feed = &g_feed;
    g_mon.cpu = ncpu > 1 ? (int)ncpu - 1 : -1;
    g_mon.depth_trip = (uint64_t)nworkers;
    g_mon.spa_trip = 1500;                  /* 15 spins per acquisition */
    atomic_store(&g_phase, PH_LIGHT);

    printf("== ticket lock: depth, spin accounting, watchdog, breaker ==\n");
    printf("   cpus=%ld workers=%d monitor_cpu=%d\n", ncpu, nworkers, g_mon.cpu);

    pthread_t mon, w[4];
    pthread_create(&mon, NULL, sm_monitor_main, &g_mon);
    for (int i = 0; i < nworkers; i++) pthread_create(&w[i], NULL, worker, NULL);

    const uint64_t t_start = sm_now_ns();
    uint64_t next_pub = t_start, next_print = t_start + 200000000ull;
    sm_stats pub_prev, print_prev;
    sm_snapshot(&g_orders, &pub_prev);
    print_prev = pub_prev;

    while (sm_now_ns() - t_start < 2000000000ull) {
        uint64_t now = sm_now_ns();
        double t = (now - t_start) / 1e9;

        if (t >= 1.2 && atomic_load(&g_phase) != PH_STALL) {
            atomic_store(&g_stall_armed, true);
            atomic_store(&g_phase, PH_STALL);
        } else if (t >= 0.6 && atomic_load(&g_phase) == PH_LIGHT) {
            atomic_store(&g_phase, PH_HEAVY);
        }

        if (now >= next_pub) {                          /* publish every 5 ms */
            sm_stats cur;
            sm_snapshot(&g_orders, &cur);
            uint64_t da = cur.acquisitions - pub_prev.acquisitions;
            sm_sample x = {
                .pub_ns = now,
                .depth = cur.depth,
                .spins_per_acq_x100 = da ? (cur.spins - pub_prev.spins) * 100 / da : 0,
                .contended_pct = da ? (cur.contended - pub_prev.contended) * 100 / da : 0,
            };
            sm_feed_publish(&g_feed, &x);
            pub_prev = cur;
            next_pub = now + 5000000ull;
        }

        if (now >= next_print) {                        /* report every 200 ms */
            sm_stats cur;
            sm_snapshot(&g_orders, &cur);
            uint64_t da = cur.acquisitions - print_prev.acquisitions;
            printf("t=%.1fs %-5s depth=%u acq/s=%-9.0f contended=%5.2f%% spins/acq=%6.1f breaker=%-3s shed=%" PRIu64 "\n",
                   t, phase_name(atomic_load(&g_phase)), cur.depth, da / 0.2,
                   da ? (double)(cur.contended - print_prev.contended) * 100.0 / da : 0.0,
                   da ? (double)(cur.spins - print_prev.spins) / da : 0.0,
                   atomic_load(&g_mon.breaker) ? "ON" : "off", (uint64_t)atomic_load(&g_shed));
            print_prev = cur;
            next_print += 200000000ull;
        }
        usleep(500);
    }

    atomic_store(&g_stop, true);
    for (int i = 0; i < nworkers; i++) pthread_join(w[i], NULL);
    atomic_store(&g_mon.stop, true);
    pthread_join(mon, NULL);

    printf("monitor: trips=%" PRIu64 " last_reaction=%.1fus max_reaction=%.1fus\n",
           (uint64_t)atomic_load(&g_mon.trips),
           atomic_load(&g_mon.last_react_ns) / 1e3, atomic_load(&g_mon.max_react_ns) / 1e3);

    demo_lease();
    return 0;
}

#endif /* SPINMON_NO_DEMO */
