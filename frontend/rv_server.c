/* rv_server.c - local HTTP test bench for the rendezvous (sm_rv_*).
 *
 * Wraps the real library calls in a tiny JSON API and serves frontend/index.html.
 * The browser plays the followers and the outsider: it holds the follower keys,
 * computes proofs and unseals replies itself. This server never returns a
 * destination or the master key; /api/state exposes only the epoch and counters.
 *
 * Build:  make rv_server        (or: gcc -std=c11 -O2 -Wall -Wextra -pthread frontend/rv_server.c -o rv_server)
 * Run:    ./rv_server [port] [path/to/index.html]     default 8080, frontend/index.html
 * Binds 127.0.0.1 only. Single-threaded: one request at a time.
 */
#define SPINMON_NO_DEMO
#include "../spinmon.c"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static sm_rendezvous R;
static bool     g_open;
static uint64_t g_lease_ns, g_every_n, g_every_ns, g_grace_ns;
static const char *g_index = "frontend/index.html";

static const char *rv_status_name(sm_rv_status s)
{
    static const char *n[] = { "ok", "denied", "bad_proof", "fenced", "full" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "unknown";
}

/* Find key=value in a query string; copies value into buf. Returns false if absent. */
static bool qget(const char *q, const char *key, char *buf, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= n) l = n - 1;
            memcpy(buf, v, l); buf[l] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static bool qnum(const char *q, const char *key, int base, uint64_t *out)
{
    char b[64], *end;
    if (!qget(q, key, b, sizeof b) || !b[0]) return false;
    errno = 0;
    unsigned long long v = strtoull(b, &end, base);
    if (errno || *end) return false;
    *out = v;
    return true;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *ctype, const char *body, size_t len)
{
    char h[256];
    int hl = snprintf(h, sizeof h,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request",
                      ctype, len);
    send_all(fd, h, (size_t)hl);
    send_all(fd, body, len);
}

static void json(int fd, int code, const char *body)
{
    respond(fd, code, "application/json", body, strlen(body));
}

static void serve_index(int fd)
{
    FILE *f = fopen(g_index, "rb");
    if (!f) { json(fd, 404, "{\"error\":\"index.html not found; pass its path as the 2nd argument\"}"); return; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    respond(fd, 200, "text/html; charset=utf-8", buf, n);
}

static void api(int fd, const char *path, const char *q)
{
    char out[8192];
    uint64_t id = 0, v = 0;

    if (!strcmp(path, "/api/open")) {
        uint64_t n = 1, every_ms = 0, grace_ms = 2000, lease_ms = 5000;
        qnum(q, "every_n", 10, &n); qnum(q, "every_ms", 10, &every_ms);
        qnum(q, "grace_ms", 10, &grace_ms); qnum(q, "lease_ms", 10, &lease_ms);
        g_every_n = n; g_every_ns = every_ms * 1000000ull;
        g_grace_ns = grace_ms * 1000000ull; g_lease_ns = lease_ms * 1000000ull;
        g_open = sm_rv_open(&R, 1, g_lease_ns, g_every_n, g_every_ns, g_grace_ns);
        snprintf(out, sizeof out, "{\"ok\":%s}", g_open ? "true" : "false");
        json(fd, 200, out);
        return;
    }
    if (!g_open) { json(fd, 400, "{\"error\":\"not open: call /api/open first\"}"); return; }

    if (!strcmp(path, "/api/state")) {
        uint16_t owner = 0; uint64_t age = 0;
        bool held = sm_lease_peek(&R.lease, &owner, &age);
        sm_stats st; sm_snapshot(&R.lock, &st);
        uint64_t now = sm_now_ns();
        SM_LOCK(&R.lock);
        int o = snprintf(out, sizeof out,
            "{\"epoch\":%" PRIu64 ",\"pings_since\":%" PRIu64 ",\"since_rotation_ms\":%.1f,"
            "\"policy\":{\"every_n\":%" PRIu64 ",\"every_ms\":%" PRIu64 ",\"grace_ms\":%" PRIu64 ",\"lease_ms\":%" PRIu64 "},"
            "\"server\":%u,\"lease\":{\"held\":%s,\"owner\":%u,\"age_ms\":%.1f},"
            "\"lock\":{\"acquisitions\":%" PRIu64 ",\"contended\":%" PRIu64 ",\"spins\":%" PRIu64 "},"
            "\"followers\":[",
            R.epoch, R.pings_since, (now - R.rotated_ns) / 1e6,
            g_every_n, (uint64_t)(g_every_ns / 1000000u), (uint64_t)(g_grace_ns / 1000000u), (uint64_t)(g_lease_ns / 1000000u),
            R.server, held ? "true" : "false", owner, age / 1e6,
            st.acquisitions, st.contended, st.spins);
        bool first = true;
        for (int i = 0; i < SM_RV_MAX && o < (int)sizeof out - 128; i++) {
            if (!R.f[i].id) continue;
            o += snprintf(out + o, sizeof out - (size_t)o, "%s{\"id\":%u,\"last_ping_ms_ago\":%.1f}",
                          first ? "" : ",", R.f[i].id,
                          R.f[i].last_ping_ns ? (now - R.f[i].last_ping_ns) / 1e6 : -1.0);
            first = false;
        }
        sm_unlock(&R.lock);
        snprintf(out + o, sizeof out - (size_t)o, "]}");
        json(fd, 200, out);
        return;
    }
    if (!strcmp(path, "/api/enroll")) {
        /* Simulates the out-of-band key handoff: the key goes to the follower once, here. */
        uint64_t key[2];
        if (!qnum(q, "id", 10, &id) || id == 0 || id > 0xFFFF) { json(fd, 400, "{\"error\":\"id must be 1..65535\"}"); return; }
        if (!sm_rv_gen_key(key)) { json(fd, 500, "{\"error\":\"no entropy\"}"); return; }
        sm_rv_status s = sm_rv_enroll(&R, (uint16_t)id, key);
        if (s == SM_RV_OK)
            snprintf(out, sizeof out, "{\"status\":\"ok\",\"key\":[\"%016" PRIx64 "\",\"%016" PRIx64 "\"]}", key[0], key[1]);
        else
            snprintf(out, sizeof out, "{\"status\":\"%s\"}", rv_status_name(s));
        json(fd, 200, out);
        return;
    }
    if (!strcmp(path, "/api/revoke")) {
        bool ok = qnum(q, "id", 10, &id) && id <= 0xFFFF && sm_rv_revoke(&R, (uint16_t)id);
        snprintf(out, sizeof out, "{\"ok\":%s}", ok ? "true" : "false");
        json(fd, 200, out);
        return;
    }
    if (!strcmp(path, "/api/rotate")) {
        sm_rv_rotate(&R);
        json(fd, 200, "{\"ok\":true}");
        return;
    }
    if (!strcmp(path, "/api/ping")) {
        uint64_t epoch = 0, proof = 0;
        if (!qnum(q, "id", 10, &id) || id > 0xFFFF || !qnum(q, "epoch", 10, &epoch) || !qnum(q, "proof", 16, &proof)) {
            json(fd, 400, "{\"error\":\"need id, epoch, proof (hex)\"}");
            return;
        }
        sm_rv_reply r;
        sm_rv_status s = sm_rv_ping(&R, (uint16_t)id, epoch, proof, &r);
        if (s == SM_RV_OK)
            snprintf(out, sizeof out, "{\"status\":\"ok\",\"epoch\":%" PRIu64 ",\"sealed\":\"%016" PRIx64 "\"}", r.epoch, r.sealed);
        else
            snprintf(out, sizeof out, "{\"status\":\"%s\"}", rv_status_name(s));
        json(fd, 200, out);
        return;
    }
    if (!strcmp(path, "/api/resolve")) {
        if (!qnum(q, "dest", 16, &v)) { json(fd, 400, "{\"error\":\"need dest (hex)\"}"); return; }
        snprintf(out, sizeof out, "{\"live\":%s}", sm_rv_resolve(&R, v) ? "true" : "false");
        json(fd, 200, out);
        return;
    }
    if (!strcmp(path, "/api/takeover")) {
        /* A second server tries to take the lease. Succeeds only once the current
         * server's heartbeat (renewed on each valid ping) is older than lease_ms. */
        uint64_t fence = 0; uint16_t prev = 0, owner = 0; uint64_t age = 0;
        if (!qnum(q, "id", 10, &id) || id == 0 || id > 0xFFFF) { json(fd, 400, "{\"error\":\"id must be 1..65535\"}"); return; }
        sm_lease_peek(&R.lease, &owner, &age);
        bool ok = sm_lease_acquire(&R.lease, (uint16_t)id, g_lease_ns, &fence, &prev);
        snprintf(out, sizeof out, "{\"ok\":%s,\"from\":%u,\"fence\":%" PRIu64 ",\"holder_age_ms\":%.1f}",
                 ok ? "true" : "false", ok ? prev : owner, fence, age / 1e6);
        json(fd, 200, out);
        return;
    }
    json(fd, 404, "{\"error\":\"unknown endpoint\"}");
}

static void handle(int fd)
{
    char req[8192];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    char method[8], target[2048];
    if (sscanf(req, "%7s %2047s", method, target) != 2 || strcmp(method, "GET")) {
        json(fd, 400, "{\"error\":\"GET only\"}");
        return;
    }
    char *q = strchr(target, '?');
    if (q) *q++ = 0;
    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) serve_index(fd);
    else if (!strncmp(target, "/api/", 5)) api(fd, target, q ? q : "");
    else json(fd, 404, "{\"error\":\"not found\"}");
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    if (argc > 2) g_index = argv[2];
    signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("bind/listen"); return 1; }
    printf("rendezvous test bench: http://127.0.0.1:%d/\n", port);
    fflush(stdout);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        handle(c);
        close(c);
    }
}
