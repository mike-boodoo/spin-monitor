/* test_contention.c - regression gate on lock contention.
 *
 * Build:  gcc -std=c11 -O2 -pthread test_contention.c -o test_contention
 * Record: ./test_contention record      (on the baseline commit; writes baseline.txt)
 * Gate:   ./test_contention             (exit 0 pass, 1 fail, 2 no baseline, 77 skipped)
 */
#define SPINMON_NO_DEMO
#include "spinmon.c"          /* first include: defines _GNU_SOURCE before any libc header */
#include <stdlib.h>

#ifndef NTHREADS
#define NTHREADS 4
#endif
#ifndef OPS_PER_THREAD
#define OPS_PER_THREAD 300000
#endif
#ifndef TRIALS
#define TRIALS 7
#endif
#ifndef TOLERANCE
#define TOLERANCE 1.5         /* fail if median spins/acq exceeds 1.5x baseline ...   */
#endif
#ifndef ABS_FLOOR
#define ABS_FLOOR 0.5         /* ... plus this floor, so a ~0 baseline is not a hair trigger */
#endif

static sm_ticket lock;
static atomic_int stall_reports;
static volatile uint64_t protected_state;

static void on_stall(const sm_stall_report *r, void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&stall_reports, 1);
    fprintf(stderr, "stall: holder=%u site=%s held=%.1fms\n",
            r->holder_tid, r->holder_site ? r->holder_site : "?", r->held_ns / 1e6);
}

static void *worker(void *arg)
{
    sm_pin((int)(intptr_t)arg);
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        SM_LOCK(&lock);
        for (int k = 0; k < 8; k++) protected_state++;   /* the critical section under test */
        sm_unlock(&lock);
    }
    return NULL;
}

static double run_trial(long ncpu)
{
    sm_ticket_init(&lock, "test", 50000000ull /* 50 ms */, on_stall, NULL);  /* fresh counters */
    pthread_t t[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)(i % ncpu));
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);

    sm_stats s;
    sm_snapshot(&lock, &s);
    return s.acquisitions ? (double)s.spins / (double)s.acquisitions : 0.0;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < NTHREADS + 1 && !getenv("SPINMON_ALLOW_OVERSUBSCRIBE")) {
        printf("SKIP: need %d CPUs, have %ld\n", NTHREADS + 1, ncpu);
        return 77;
    }

    double spa[TRIALS];
    for (int i = 0; i < TRIALS; i++) spa[i] = run_trial(ncpu);
    qsort(spa, TRIALS, sizeof spa[0], cmp);
    double median = spa[TRIALS / 2];
    int stalls = atomic_load(&stall_reports);
    printf("median spins/acq=%.2f  watchdog reports=%d\n", median, stalls);

    if (argc > 1 && !strcmp(argv[1], "record")) {
        FILE *f = fopen("baseline.txt", "w");
        if (!f) return 2;
        fprintf(f, "%.4f\n", median);
        fclose(f);
        return 0;
    }

    double base;
    FILE *f = fopen("baseline.txt", "r");
    if (!f || fscanf(f, "%lf", &base) != 1) { puts("no baseline.txt; run with 'record' first"); return 2; }
    fclose(f);

    int fail = 0;
    if (stalls) { printf("FAIL: %d stall reports in steady state\n", stalls); fail = 1; }
    if (median > base * TOLERANCE + ABS_FLOOR) {
        printf("FAIL: spins/acq %.2f > %.2f (baseline %.2f x %.1f + %.1f)\n",
               median, base * TOLERANCE + ABS_FLOOR, base, TOLERANCE, ABS_FLOOR);
        fail = 1;
    }
    if (!fail) puts("PASS");
    return fail;
}
