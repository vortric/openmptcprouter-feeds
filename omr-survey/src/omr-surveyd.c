/*
 * omr-surveyd -- raw telemetry sampler for OpenMPTCProuter connectivity surveys.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Once per interval (default 1 s, scheduled on CLOCK_MONOTONIC so the cadence
 * does not drift), take a CLOCK_REALTIME + CLOCK_MONOTONIC timestamp, run each
 * configured `ubus -S call <object> <method>` and append ONE line to
 * <base>/<session>/survey.jsonl (envelope format "omr-survey/2"):
 *
 *   {"seq":N,"session":"...",
 *    "time":{"realtime_ns":...,"monotonic_ns":...,"end_monotonic_ns":...},
 *    "missed":0,
 *    "sources":{
 *      "omr":  {"ok":true,"collected_at_monotonic_ns":...,"collect_ms":7.1,
 *               "data":<raw ubus output>},
 *      "mqvpn":{"ok":false,"collected_at_monotonic_ns":...,"collect_ms":5001.2,
 *               "rc":1,"error":"ubus exit 1"}}}
 *
 * The envelope carries only what the collector itself knows: sequence,
 * session, when the sample started/ended, when each source was queried and
 * whether the query succeeded. The ubus payloads are embedded byte-for-byte
 * as printed by `ubus -S` (compact single-line JSON): no parsing, reshaping
 * or normalization happens on the router. A source that fails, or returns
 * something that is not a single JSON line, keeps its row with ok:false and
 * the raw bytes go to <session>/errors.log, so analysis can tell "value was
 * 0" from "not collected".
 *
 * <session>/meta.json is written at start and rewritten at stop (sample
 * count, stop reason, both clocks). SIGTERM/SIGINT finish the current sample,
 * fsync and close the file, then exit 0. SIGHUP rotates survey.jsonl.
 *
 * The router's syslog is captured alongside the samples: the ring buffer as
 * it stands at start goes to <session>/syslog-start.log, and a `logread -f`
 * child appends everything after that to <session>/syslog.log for the
 * session's lifetime (killed on stop). Without it, diagnosing what the other
 * daemons did during a drive depends on the ring buffer surviving, which it
 * does not across a reboot.
 *
 * Deliberately no libubus dependency: the ubus CLI already prints exactly
 * what this tool must archive, and shelling out keeps the sampler auditable.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOURCES     8
#define MAX_PAYLOAD     (4u * 1024u * 1024u)
#define UBUS_TIMEOUT_S  5

typedef struct {
    const char *key;    /* JSON key in the row, e.g. "omr" */
    const char *object; /* ubus object, e.g. "metrics" */
    const char *method; /* ubus method, e.g. "get_all" */
} source_t;

static struct {
    const char *session;
    const char *base_dir;
    long interval_ms;
    long max_samples;
    long duration_s;
    long rotate_bytes;
    source_t src[MAX_SOURCES];
    int n_src;
} cfg = { NULL, "/tmp/omr-survey", 1000, 0, 0, 0, {{0}}, 0 };

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_rotate = 0;

static char g_dir[512];
static char g_jsonl_path[600];
static FILE *g_out = NULL;
static FILE *g_err = NULL;
static long g_seq = 0;
static long g_rotations = 0;
static uint64_t g_start_real_ns, g_start_mono_ns;

static void on_stop(int sig) { (void)sig; g_stop = 1; }
static void on_hup(int sig) { (void)sig; g_rotate = 1; }

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Minimal JSON string escaper for the few strings we emit ourselves
 * (session id, hostname, source names). Payloads are never passed here. */
static void json_puts(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
    fputc('"', f);
}

/* Run `ubus -S -t N call obj method`, capture stdout. Returns the child's
 * exit status (or -errno on spawn failure); out and len receive a malloc'd
 * buffer without the trailing newline(s). */
static int run_ubus(const source_t *s, char **out, size_t *len)
{
    int pfd[2];
    *out = NULL;
    *len = 0;
    if (pipe(pfd) < 0) return -errno;

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(pfd[0]);
        close(pfd[1]);
        return -e;
    }
    if (pid == 0) {
        char tmo[16];
        snprintf(tmo, sizeof(tmo), "%d", UBUS_TIMEOUT_S);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("ubus", "ubus", "-S", "-t", tmo, "call", s->object, s->method, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) { close(pfd[0]); waitpid(pid, NULL, 0); return -ENOMEM; }
    for (;;) {
        if (n == cap) {
            char *nb = (cap < MAX_PAYLOAD) ? realloc(buf, cap * 2) : NULL;
            if (!nb) {
                /* Over the cap (or OOM): keep draining so the child never
                 * blocks on a full pipe, but discard the rest. n == cap
                 * makes payload_ok() reject the truncated result. */
                char sink[4096];
                ssize_t r;
                while ((r = read(pfd[0], sink, sizeof(sink))) > 0 || (r < 0 && errno == EINTR)) {}
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(pfd[0], buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf = realloc(buf, n + 1) ?: buf;
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return rc;
}

/* A payload is embeddable verbatim only if it is one line of JSON. */
static int payload_ok(const char *p, size_t len)
{
    /* cap grows 64 KiB -> ... -> MAX_PAYLOAD exactly (powers of two), so a
     * drained/truncated read ends with len == MAX_PAYLOAD. */
    if (len == 0 || len >= MAX_PAYLOAD) return 0;
    if (p[0] != '{' && p[0] != '[') return 0;
    if (memchr(p, '\n', len)) return 0;
    return 1;
}

/* ── syslog capture ─────────────────────────────────────────────────────── */

static pid_t g_logpid = 0;

/* Fork a child that runs `logread ...` with stdout on `path`.
 * The child must not outlive the recorder: PR_SET_PDEATHSIG plus a
 * getppid() recheck closes the window where the parent dies between fork
 * and prctl, and SIGTERM is reset to default so the inherited handler
 * cannot swallow the death signal. */
static pid_t spawn_logread(const char *path, const char *arg, int truncate)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) return pid;

    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    /* musl's prctl() is variadic and reads four unsigned longs. */
    prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0UL, 0UL, 0UL);
    if (getppid() != parent) _exit(0);

    int fd = open(path, O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
    if (arg) execlp("logread", "logread", arg, (char *)NULL);
    else execlp("logread", "logread", (char *)NULL);
    _exit(127);
}

static void start_log_capture(void)
{
    char path[700];
    /* Follower FIRST: anything logged between the snapshot's read and the
     * follower's subscribe would otherwise be in neither file. Starting it
     * first turns that gap into a harmless overlap. */
    snprintf(path, sizeof(path), "%s/syslog.log", g_dir);
    g_logpid = spawn_logread(path, "-f", 0);

    /* Then the ring buffer as it stands, for context before the session. */
    snprintf(path, sizeof(path), "%s/syslog-start.log", g_dir);
    pid_t pid = spawn_logread(path, NULL, 1);
    if (pid > 0) {
        int st;
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) { pid = 0; break; }
            if (r < 0 && errno != EINTR) { pid = 0; break; }
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
        if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    }
}

static void stop_log_capture(void)
{
    if (g_logpid <= 0) return;
    kill(g_logpid, SIGTERM);
    int st;
    for (int i = 0; i < 20; i++) {
        pid_t r = waitpid(g_logpid, &st, WNOHANG);
        if (r == g_logpid) { g_logpid = 0; return; }
        if (r < 0 && errno != EINTR) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    kill(g_logpid, SIGKILL);
    waitpid(g_logpid, &st, 0);
    g_logpid = 0;
}

static void open_jsonl(void)
{
    g_out = fopen(g_jsonl_path, "a");
    if (!g_out) die("omr-surveyd: cannot open %s: %s", g_jsonl_path, strerror(errno));
}

static void close_jsonl(void)
{
    if (!g_out) return;
    fflush(g_out);
    fsync(fileno(g_out));
    fclose(g_out);
    g_out = NULL;
}

static void rotate_jsonl(void)
{
    char rotated[700];
    close_jsonl();
    snprintf(rotated, sizeof(rotated), "%s/survey.%03ld.jsonl", g_dir, g_rotations++);
    if (rename(g_jsonl_path, rotated) < 0)
        fprintf(stderr, "omr-surveyd: rotate failed: %s\n", strerror(errno));
    open_jsonl();
}

static void write_meta(const char *stop_reason)
{
    char tmp[600], path[600], host[256] = "";
    snprintf(path, sizeof(path), "%s/meta.json", g_dir);
    snprintf(tmp, sizeof(tmp), "%s/meta.json.tmp", g_dir);
    if (gethostname(host, sizeof(host) - 1) < 0) host[0] = '\0';
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("{\"format\":\"omr-survey/2\",\"session\":", f); json_puts(f, cfg.session);
    fputs(",\"hostname\":", f); json_puts(f, host);
    fprintf(f, ",\"pid\":%ld,\"interval_ms\":%ld,\"max_samples\":%ld,\"duration_s\":%ld,"
               "\"rotate_bytes\":%ld,\"ubus_timeout_s\":%d",
            (long)getpid(), cfg.interval_ms, cfg.max_samples, cfg.duration_s,
            cfg.rotate_bytes, UBUS_TIMEOUT_S);
    fputs(",\"sources\":[", f);
    for (int i = 0; i < cfg.n_src; i++) {
        if (i) fputc(',', f);
        fputs("{\"key\":", f); json_puts(f, cfg.src[i].key);
        fputs(",\"object\":", f); json_puts(f, cfg.src[i].object);
        fputs(",\"method\":", f); json_puts(f, cfg.src[i].method);
        fputc('}', f);
    }
    fprintf(f, "],\"start_realtime_ns\":%llu,\"start_monotonic_ns\":%llu",
            (unsigned long long)g_start_real_ns, (unsigned long long)g_start_mono_ns);
    fprintf(f, ",\"samples\":%ld,\"rotations\":%ld", g_seq, g_rotations);
    if (stop_reason) {
        fprintf(f, ",\"stop_realtime_ns\":%llu,\"stop_monotonic_ns\":%llu,\"stop_reason\":",
                (unsigned long long)now_ns(CLOCK_REALTIME),
                (unsigned long long)now_ns(CLOCK_MONOTONIC));
        json_puts(f, stop_reason);
        fputs(",\"running\":false", f);
    } else {
        fputs(",\"running\":true", f);
    }
    fputs("}\n", f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

static void sample_once(void)
{
    uint64_t t_real = now_ns(CLOCK_REALTIME);
    uint64_t t_mono = now_ns(CLOCK_MONOTONIC);
    char *payload[MAX_SOURCES] = {0};
    size_t plen[MAX_SOURCES] = {0};
    int rc[MAX_SOURCES] = {0};
    uint64_t t_src[MAX_SOURCES] = {0}, t_src_end[MAX_SOURCES] = {0};

    for (int i = 0; i < cfg.n_src; i++) {
        t_src[i] = now_ns(CLOCK_MONOTONIC);
        rc[i] = run_ubus(&cfg.src[i], &payload[i], &plen[i]);
        t_src_end[i] = now_ns(CLOCK_MONOTONIC);
    }
    uint64_t t_mono_end = now_ns(CLOCK_MONOTONIC);

    /* Slots skipped because the previous sample overran the interval. */
    uint64_t slot = (t_mono - g_start_mono_ns) / ((uint64_t)cfg.interval_ms * 1000000ull);
    long missed = (long)slot - g_seq;
    if (missed < 0) missed = 0;

    fprintf(g_out, "{\"seq\":%ld,\"session\":", g_seq);
    json_puts(g_out, cfg.session);
    fprintf(g_out, ",\"time\":{\"realtime_ns\":%llu,\"monotonic_ns\":%llu,"
                   "\"end_monotonic_ns\":%llu},\"missed\":%ld,\"sources\":{",
            (unsigned long long)t_real, (unsigned long long)t_mono,
            (unsigned long long)t_mono_end, missed);
    for (int i = 0; i < cfg.n_src; i++) {
        if (i) fputc(',', g_out);
        json_puts(g_out, cfg.src[i].key);
        int ok = (rc[i] == 0 && payload_ok(payload[i], plen[i]));
        fprintf(g_out, ":{\"ok\":%s,\"collected_at_monotonic_ns\":%llu,\"collect_ms\":%.1f,",
                ok ? "true" : "false", (unsigned long long)t_src[i],
                (double)(t_src_end[i] - t_src[i]) / 1e6);
        if (ok) {
            fputs("\"data\":", g_out);
            fwrite(payload[i], 1, plen[i], g_out);
        } else {
            const char *why = rc[i] != 0 ? "ubus exit"
                              : plen[i] == 0 ? "empty response"
                              : plen[i] >= MAX_PAYLOAD ? "response truncated"
                              : "not a single JSON line";
            fprintf(g_out, "\"rc\":%d,\"error\":", rc[i]);
            if (rc[i] != 0) {
                char msg[32];
                snprintf(msg, sizeof(msg), "ubus exit %d", rc[i]);
                json_puts(g_out, msg);
            } else {
                json_puts(g_out, why);
            }
            if (g_err) {
                fprintf(g_err, "seq=%ld key=%s rc=%d len=%zu realtime_ns=%llu %s\n",
                        g_seq, cfg.src[i].key, rc[i], plen[i], (unsigned long long)t_real, why);
                if (payload[i] && plen[i]) {
                    fwrite(payload[i], 1, plen[i], g_err);
                    fputc('\n', g_err);
                }
                fflush(g_err);
            }
        }
        fputc('}', g_out);
    }
    fputs("}}\n", g_out);
    fflush(g_out);

    for (int i = 0; i < cfg.n_src; i++) free(payload[i]);
    g_seq++;

    if (cfg.rotate_bytes > 0) {
        long pos = ftell(g_out);
        if (pos >= cfg.rotate_bytes) rotate_jsonl();
    }
}

static void add_source(const char *spec)
{
    /* key=object.method */
    if (cfg.n_src >= MAX_SOURCES) die("omr-surveyd: too many -c sources (max %d)", MAX_SOURCES);
    char *s = strdup(spec);
    char *eq = strchr(s, '=');
    char *dot = eq ? strrchr(eq + 1, '.') : NULL;
    if (!eq || !dot || eq == s || dot == eq + 1 || dot[1] == '\0')
        die("omr-surveyd: bad -c '%s' (want key=object.method)", spec);
    *eq = '\0';
    *dot = '\0';
    cfg.src[cfg.n_src].key = s;
    cfg.src[cfg.n_src].object = eq + 1;
    cfg.src[cfg.n_src].method = dot + 1;
    cfg.n_src++;
}

static int valid_session(const char *s)
{
    if (!s || !*s || strlen(s) > 128) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

static void usage(void)
{
    fputs("usage: omr-surveyd -s <session-id> [-d <base-dir>] [-i <interval-ms>]\n"
          "                   [-n <max-samples>] [-t <duration-s>] [-r <rotate-bytes>]\n"
          "                   [-c key=object.method ...]\n"
          "default sources: -c omr=metrics.get_all -c mqvpn=mqvpn.metrics\n"
          "                 -c network=network.interface.dump\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "s:d:i:n:t:r:c:h")) != -1) {
        switch (opt) {
        case 's': cfg.session = optarg; break;
        case 'd': cfg.base_dir = optarg; break;
        case 'i': cfg.interval_ms = atol(optarg); break;
        case 'n': cfg.max_samples = atol(optarg); break;
        case 't': cfg.duration_s = atol(optarg); break;
        case 'r': cfg.rotate_bytes = atol(optarg); break;
        case 'c': add_source(optarg); break;
        default: usage();
        }
    }
    if (!valid_session(cfg.session)) die("omr-surveyd: -s <session-id> required ([A-Za-z0-9._-], <=128)");
    if (cfg.interval_ms < 100) die("omr-surveyd: interval must be >= 100 ms");
    if (cfg.n_src == 0) {
        add_source("omr=metrics.get_all");
        add_source("mqvpn=mqvpn.metrics");
        add_source("network=network.interface.dump");
    }

    snprintf(g_dir, sizeof(g_dir), "%s/%s", cfg.base_dir, cfg.session);
    if (mkdir(cfg.base_dir, 0755) < 0 && errno != EEXIST)
        die("omr-surveyd: mkdir %s: %s", cfg.base_dir, strerror(errno));
    if (mkdir(g_dir, 0755) < 0 && errno != EEXIST)
        die("omr-surveyd: mkdir %s: %s", g_dir, strerror(errno));
    snprintf(g_jsonl_path, sizeof(g_jsonl_path), "%s/survey.jsonl", g_dir);

    char errpath[600];
    snprintf(errpath, sizeof(errpath), "%s/errors.log", g_dir);
    g_err = fopen(errpath, "a");
    open_jsonl();

    /* Marker for `ubus call omr-survey status`: which session is live. */
    {
        char cur[600];
        snprintf(cur, sizeof(cur), "%s/current", cfg.base_dir);
        FILE *f = fopen(cur, "w");
        if (f) { fprintf(f, "%s\n", cfg.session); fclose(f); }
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_start_real_ns = now_ns(CLOCK_REALTIME);
    g_start_mono_ns = now_ns(CLOCK_MONOTONIC);
    /* meta.json first: the rpcd start handler waits for it to decide whether
     * the daemon came up, and the log snapshot below can take a moment. */
    write_meta(NULL);
    start_log_capture();

    const char *reason = "signal";
    uint64_t interval_ns = (uint64_t)cfg.interval_ms * 1000000ull;
    uint64_t deadline_ns = cfg.duration_s > 0 ? g_start_mono_ns + (uint64_t)cfg.duration_s * 1000000000ull : 0;

    for (;;) {
        if (g_stop) { reason = "signal"; break; }

        sample_once();

        if (cfg.max_samples > 0 && g_seq >= cfg.max_samples) { reason = "max_samples"; break; }
        if (deadline_ns && now_ns(CLOCK_MONOTONIC) >= deadline_ns) { reason = "duration"; break; }

        /* Next slot strictly after now: never burst to catch up after an overrun. */
        uint64_t now = now_ns(CLOCK_MONOTONIC);
        uint64_t next = g_start_mono_ns + ((now - g_start_mono_ns) / interval_ns + 1) * interval_ns;
        struct timespec ts = { (time_t)(next / 1000000000ull), (long)(next % 1000000000ull) };
        /* SIGHUP (rotate) must not shorten the sleep: rotate, then keep
         * waiting for the same slot. Only SIGTERM/SIGINT end the wait. */
        while (!g_stop) {
            if (g_rotate) { g_rotate = 0; rotate_jsonl(); }
            int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            if (r == 0) break;
            if (r != EINTR) break;
        }
    }

    stop_log_capture();
    write_meta(reason);
    close_jsonl();
    if (g_err) fclose(g_err);
    {
        char cur[600];
        snprintf(cur, sizeof(cur), "%s/current", cfg.base_dir);
        unlink(cur);
    }
    return 0;
}
