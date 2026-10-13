/* test_rendezvous.c - deterministic tests for the moving-destination rendezvous.
 * Build: gcc -std=c11 -O2 -pthread test_rendezvous.c -o test_rendezvous
 */
#define SPINMON_NO_DEMO
#include "spinmon.c"
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Follower-side helper: ping and unseal. Returns the status; *dest/epoch updated on OK. */
static sm_rv_status follow(sm_rendezvous *R, uint16_t id, const uint64_t key[2],
                           uint64_t *epoch, uint64_t *dest)
{
    sm_rv_reply r;
    sm_rv_status st = sm_rv_ping(R, id, *epoch, sm_rv_proof(key, id, *epoch), &r);
    if (st == SM_RV_OK) { *epoch = r.epoch; *dest = sm_rv_unseal(key, id, &r); }
    return st;
}

static void test_siphash(void)
{
    /* Reference SipHash-2-4 vector: key 00..0f, input 00..0f. */
    uint64_t k[2] = { 0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull };
    CHECK(sm_siphash16(k, 0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull) == 0x3f2acc7f57c29bdbull);
}

static void test_followers_track_outsiders_do_not(void)
{
    static sm_rendezvous R;
    uint64_t ka[2], kb[2], kx[2];
    CHECK(sm_rv_gen_key(ka) && sm_rv_gen_key(kb) && sm_rv_gen_key(kx));
    CHECK(sm_rv_open(&R, 1, 1000000000ull, /*every_n*/1, 0, /*grace*/1000000000ull));
    CHECK(sm_rv_enroll(&R, 5, ka) == SM_RV_OK);
    CHECK(sm_rv_enroll(&R, 6, kb) == SM_RV_OK);
    CHECK(sm_rv_enroll(&R, 5, ka) == SM_RV_FULL);          /* duplicate id */

    uint64_t ea = 0, da = 0, eb = 0, db = 0;
    CHECK(follow(&R, 5, ka, &ea, &da) == SM_RV_OK);
    CHECK(sm_rv_resolve(&R, da));                           /* A's reference is live */
    CHECK(follow(&R, 6, kb, &eb, &db) == SM_RV_OK);        /* B's ping rotates it */
    CHECK(db != da && eb == ea + 1);
    CHECK(sm_rv_resolve(&R, db));
    CHECK(sm_rv_resolve(&R, da));                           /* grace: previous still resolves */

    CHECK(follow(&R, 5, ka, &ea, &da) == SM_RV_OK);        /* A re-pings with its stale epoch... */
    CHECK(sm_rv_resolve(&R, da));                           /* ...and its reference follows */
    CHECK(sm_rv_resolve(&R, db));                           /* B's is one back: still in grace */
    uint64_t older = db;
    CHECK(follow(&R, 6, kb, &eb, &db) == SM_RV_OK);
    CHECK(!sm_rv_resolve(&R, older));                       /* two rotations back: dead */

    /* Outsiders. */
    uint64_t epoch_before;
    sm_rv_reply r;
    SM_LOCK(&R.lock); epoch_before = R.epoch; sm_unlock(&R.lock);
    CHECK(sm_rv_ping(&R, 9, 0, sm_rv_proof(kx, 9, 0), &r) == SM_RV_DENIED);        /* not enrolled */
    CHECK(sm_rv_ping(&R, 5, 0, sm_rv_proof(kx, 5, 0), &r) == SM_RV_BAD_PROOF);     /* spoofing A */
    CHECK(sm_rv_ping(&R, 0, 0, 0, &r) == SM_RV_DENIED);
    SM_LOCK(&R.lock); CHECK(R.epoch == epoch_before); sm_unlock(&R.lock);         /* none rotated it */

    /* An outsider who captures A's reply cannot unseal it with another key. */
    CHECK(sm_rv_ping(&R, 5, ea, sm_rv_proof(ka, 5, ea), &r) == SM_RV_OK);
    CHECK(sm_rv_unseal(kx, 5, &r) != R.dest && sm_rv_unseal(kb, 5, &r) != R.dest);
    CHECK(sm_rv_unseal(ka, 5, &r) == R.dest);
    /* Guessing does not work either. */
    for (uint64_t g = 1; g < 1000; g++) CHECK(!sm_rv_resolve(&R, g * 0x9e3779b97f4a7c15ull));

    /* Revocation + forced rotation cuts a follower off. */
    CHECK(sm_rv_revoke(&R, 6));
    sm_rv_rotate(&R);
    CHECK(follow(&R, 6, kb, &eb, &db) == SM_RV_DENIED);
    CHECK(!sm_rv_resolve(&R, db));
}

static void test_grace_and_time_rotation(void)
{
    static sm_rendezvous R;
    uint64_t k[2];
    CHECK(sm_rv_gen_key(k));
    /* Rotate by time only (50 ms); previous destination survives 60 ms. */
    CHECK(sm_rv_open(&R, 1, 1000000000ull, 0, 50000000ull, 60000000ull));
    CHECK(sm_rv_enroll(&R, 3, k) == SM_RV_OK);
    uint64_t e = 0, d1 = 0, d2 = 0;
    CHECK(follow(&R, 3, k, &e, &d1) == SM_RV_OK);
    uint64_t e1 = e;
    CHECK(follow(&R, 3, k, &e, &d2) == SM_RV_OK);
    CHECK(e == e1 && d1 == d2);                             /* too soon: no rotation */
    usleep(80000);
    CHECK(follow(&R, 3, k, &e, &d2) == SM_RV_OK);
    CHECK(e == e1 + 1 && d2 != d1);                         /* rotated by time */
    CHECK(sm_rv_resolve(&R, d1));                           /* inside grace */
    usleep(120000);
    CHECK(!sm_rv_resolve(&R, d1));                          /* grace expired */
    CHECK(sm_rv_resolve(&R, d2));
}

static void test_fenced_server(void)
{
    static sm_rendezvous R;
    uint64_t k[2], f2 = 0;
    CHECK(sm_rv_gen_key(k));
    CHECK(sm_rv_open(&R, 1, 50000000ull, 1, 0, 0));
    CHECK(sm_rv_enroll(&R, 3, k) == SM_RV_OK);
    uint64_t e = 0, d = 0;
    CHECK(follow(&R, 3, k, &e, &d) == SM_RV_OK);
    usleep(120000);                                         /* server stops heartbeating... */
    CHECK(sm_lease_acquire(&R.lease, 2, 50000000ull, &f2, NULL));   /* ...and is replaced */
    CHECK(follow(&R, 3, k, &e, &d) == SM_RV_FENCED);       /* deposed server refuses to rotate */
}

int main(void)
{
    test_siphash();                          puts("siphash: ok");
    test_followers_track_outsiders_do_not(); puts("followers/outsiders: ok");
    test_grace_and_time_rotation();          puts("grace/time rotation: ok");
    test_fenced_server();                    puts("fenced server: ok");
    return 0;
}
