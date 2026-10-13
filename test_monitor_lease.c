/* test_monitor_lease.c - deterministic tests for the monitor core and the lease word.
 * Build: gcc -std=c11 -O2 -pthread test_monitor_lease.c -o test_monitor_lease
 */
#define SPINMON_NO_DEMO
#include "spinmon.c"
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void wait_breaker(sm_monitor *m, bool want, uint64_t budget_ns)
{
    uint64_t deadline = sm_now_ns() + budget_ns;
    while (atomic_load(&m->breaker) != want && sm_now_ns() < deadline) sched_yield();
}

/* Drive the monitor with synthetic samples: no lock, no threads contending, fully deterministic. */
static void test_monitor(void)
{
    static sm_feed feed;               /* static: zeroed and cache-line aligned */
    static sm_monitor m;
    m.feed = &feed; m.cpu = -1; m.depth_trip = 8; m.spa_trip = 1000;
    pthread_t t;
    pthread_create(&t, NULL, sm_monitor_main, &m);

    sm_sample calm = { .pub_ns = sm_now_ns(), .depth = 1, .spins_per_acq_x100 = 10 };
    sm_feed_publish(&feed, &calm);
    usleep(20000);
    CHECK(!atomic_load(&m.breaker));

    sm_sample hot = { .pub_ns = sm_now_ns(), .depth = 9 };
    sm_feed_publish(&feed, &hot);
    wait_breaker(&m, true, 500000000ull);
    CHECK(atomic_load(&m.breaker));
    CHECK(atomic_load(&m.last_react_ns) < 50000000ull);   /* tighten (e.g. 20us) on a dedicated core */

    sm_sample mid = { .pub_ns = sm_now_ns(), .depth = 5 };  /* below trip, above half: hysteresis holds */
    sm_feed_publish(&feed, &mid);
    usleep(20000);
    CHECK(atomic_load(&m.breaker));

    sm_sample cool = { .pub_ns = sm_now_ns(), .depth = 2, .spins_per_acq_x100 = 100 };
    sm_feed_publish(&feed, &cool);
    wait_breaker(&m, false, 500000000ull);
    CHECK(!atomic_load(&m.breaker));

    atomic_store(&m.stop, true);
    pthread_join(t, NULL);
}

/* Fault injection: the holder just stops heartbeating. Margins are generous because sleeps are lower bounds. */
static void test_lease(void)
{
    static sm_lease L;
    const uint64_t LEASE = 100000000ull;               /* 100 ms */
    uint64_t f1 = 0, f2 = 0;
    uint16_t prev = 0, owner = 0;
    uint64_t age = 0;

    CHECK(sm_lease_acquire(&L, 1, LEASE, &f1, NULL));
    CHECK(!sm_lease_acquire(&L, 2, LEASE, &f2, &prev));   /* fresh: contender refused */

    usleep(60000);
    CHECK(sm_lease_renew(&L, 1, f1));                      /* heartbeat restarts the clock */
    usleep(60000);
    CHECK(!sm_lease_acquire(&L, 2, LEASE, &f2, &prev));   /* 60 ms since renew < 100 ms */

    usleep(120000);                                        /* holder goes silent */
    CHECK(sm_lease_peek(&L, &owner, &age));
    CHECK(owner == 1 && age >= LEASE);                     /* monitor sees it expired */
    CHECK(sm_lease_acquire(&L, 2, LEASE, &f2, &prev));    /* takeover */
    CHECK(prev == 1 && f2 == f1 + 1);                      /* fence advanced by exactly one */
    CHECK(!sm_lease_renew(&L, 1, f1));                     /* old owner is fenced out */
    CHECK(sm_lease_release(&L, 2));
}

int main(void)
{
    test_monitor();  puts("monitor: ok");
    test_lease();    puts("lease: ok");
    return 0;
}
