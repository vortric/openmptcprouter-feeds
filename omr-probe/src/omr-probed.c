/*
 * omr-probed -- per-carrier active measurement for OpenMPTCProuter surveys.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Runs directly on each WAN interface (SO_BINDTODEVICE / curl --interface),
 * independent of the mqvpn scheduler, so every carrier is measured under the
 * same conditions:
 *
 *   udp_echo   1 Hz per interface: a datagram to the probe server's UDP echo
 *              port; RTT from the reply, "lost" after the timeout.
 *   http_down  every capacity_period_s per interface (round-robin, one at a
 *   http_up    time): a fixed-size GET /down?bytes=N and POST /up with N
 *              bytes; curl's own timing/size/speed numbers are stored raw.
 *
 * Every result is one JSON line (event) to <rawlog_dir>/probe-YYYYMMDD.jsonl
 * and, while omr-surveyd has a session running (<survey_base>/current), to
 * <survey_base>/<session>/probe.jsonl. The latest values per interface are
 * kept in a state file that the rpcd plugin serves as `ubus call probe get`,
 * so omr-survey can also sample them as a source. Nothing is aggregated on
 * the router beyond simple counters.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_IF        8
#define OUTSTANDING   64
#define RTT_WINDOW    60

typedef struct {
    char name[IFNAMSIZ];
    int fd;
    uint32_t next_seq;
    struct { uint32_t seq; uint64_t sent_mono, sent_real; } out[OUTSTANDING];
    uint64_t next_send_mono;
    /* Bind-failure log throttle: the socket is reopened every latency_ms, so
     * an absent device (tun0 before the tunnel is up, a WAN mid-flap) logged
     * once per second per interface -- 85% of the router's syslog volume,
     * which now also lands in the survey's syslog.log. */
    uint64_t last_bind_warn_mono;
    int bind_failing;
    /* counters + window for the state file */
    uint64_t sent, recvd, lost;
    double last_rtt_ms;
    double rtt_win[RTT_WINDOW];
    int rtt_n, rtt_pos;
    /* latest capacity results (raw curl json) */
    char last_down[2048], last_up[2048];
    uint64_t last_down_mono, last_up_mono;
} iface_t;

static struct {
    iface_t ifs[MAX_IF];
    int n_if;
    const char *host;
    int udp_port;
    const char *http_base;
    int latency_ms, udp_timeout_ms;
    int cap_period_s, cap_max_s;
    long down_bytes, up_bytes;
    const char *state_path, *rawlog_dir, *survey_base;
    int no_bind; /* test only */
} cfg;

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

/* ── event sinks (daily log + session tee), same scheme as omr-gnssd ── */
static FILE *g_raw = NULL; static char g_raw_day[16] = ""; static uint64_t g_raw_seq = 0;
static FILE *g_sess = NULL; static char g_sess_name[136] = ""; static uint64_t g_sess_seq = 0, g_sess_checked = 0;

static void json_puts(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) { unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20 || c == 0x7f) fprintf(f, "\\u%04x", c); else fputc(c, f); }
    fputc('"', f);
}

static void session_refresh(uint64_t mono)
{
    if (!cfg.survey_base || mono - g_sess_checked < 250000000ull) return;
    g_sess_checked = mono;
    char cur[512], name[136] = "";
    snprintf(cur, sizeof(cur), "%s/current", cfg.survey_base);
    FILE *f = fopen(cur, "r");
    if (f) { if (fgets(name, sizeof(name), f)) { size_t n = strlen(name); while (n && (name[n-1] == '\n' || name[n-1] == '\r' || name[n-1] == ' ')) name[--n] = '\0'; } fclose(f); }
    if (strcmp(name, g_sess_name) == 0) return;
    if (g_sess) { fclose(g_sess); g_sess = NULL; }
    snprintf(g_sess_name, sizeof(g_sess_name), "%s", name);
    g_sess_seq = 0;
    if (name[0]) { char p[700]; snprintf(p, sizeof(p), "%s/%s/probe.jsonl", cfg.survey_base, name); g_sess = fopen(p, "a"); }
}

/* body: the event's own fields, already JSON (without braces) */
static void emit(const char *body_json, uint64_t real)
{
    if (cfg.rawlog_dir) {
        time_t t = (time_t)(real / 1000000000ull); struct tm tm; gmtime_r(&t, &tm);
        char day[16]; strftime(day, sizeof(day), "%Y%m%d", &tm);
        if (!g_raw || strcmp(day, g_raw_day)) { if (g_raw) fclose(g_raw); char p[512]; snprintf(p, sizeof(p), "%s/probe-%s.jsonl", cfg.rawlog_dir, day); g_raw = fopen(p, "a"); snprintf(g_raw_day, sizeof(g_raw_day), "%s", day); g_raw_seq = 0; }
        if (g_raw) { fprintf(g_raw, "{\"seq\":%llu,\"session\":", (unsigned long long)g_raw_seq++); if (g_sess_name[0]) json_puts(g_raw, g_sess_name); else fputs("null", g_raw); fprintf(g_raw, ",%s}\n", body_json); fflush(g_raw); }
    }
    if (g_sess) { fprintf(g_sess, "{\"seq\":%llu,\"session\":", (unsigned long long)g_sess_seq++); json_puts(g_sess, g_sess_name); fprintf(g_sess, ",%s}\n", body_json); fflush(g_sess); }
}

/* ── UDP echo probes ── */
/* One throttled warning per interface: first failure, then at most once a
 * minute while it stays down, then one line when it comes back. */
static void bind_warn(iface_t *it, const char *what, int err)
{
    uint64_t now = now_ns(CLOCK_MONOTONIC);
    if (!it->bind_failing || now - it->last_bind_warn_mono >= 60000000000ull) {
        fprintf(stderr, "omr-probed: %s %s: %s%s\n", what, it->name, strerror(err),
                it->bind_failing ? " (still down)" : "");
        it->last_bind_warn_mono = now;
    }
    it->bind_failing = 1;
}

static int open_udp(iface_t *it)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { bind_warn(it, "socket", errno); return -1; }
    if (!cfg.no_bind && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, it->name, strlen(it->name) + 1) < 0) {
        int e = errno;
        close(fd);
        bind_warn(it, "SO_BINDTODEVICE", e);
        return -1;
    }
    if (it->bind_failing) {
        fprintf(stderr, "omr-probed: %s available again\n", it->name);
        it->bind_failing = 0;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

static void udp_send(iface_t *it, const struct sockaddr_in *dst, uint64_t mono, uint64_t real)
{
    if (it->fd < 0) { it->fd = open_udp(it); if (it->fd < 0) return; }
    char pkt[64];
    uint32_t seq = it->next_seq++;
    int n = snprintf(pkt, sizeof(pkt), "omrprobe %s %u %llu", it->name, seq, (unsigned long long)mono);
    int slot = seq % OUTSTANDING;
    if (it->out[slot].sent_mono) { /* slot recycled before reply: count as lost */
        it->lost++;
        char b[256]; snprintf(b, sizeof(b), "\"type\":\"udp_echo\",\"iface\":\"%s\",\"probe_seq\":%u,\"sent_realtime_ns\":%llu,\"sent_monotonic_ns\":%llu,\"lost\":true,\"timeout_ms\":%d",
                 it->name, it->out[slot].seq, (unsigned long long)it->out[slot].sent_real, (unsigned long long)it->out[slot].sent_mono, cfg.udp_timeout_ms);
        emit(b, it->out[slot].sent_real);
    }
    it->out[slot].seq = seq; it->out[slot].sent_mono = mono; it->out[slot].sent_real = real;
    if (sendto(it->fd, pkt, (size_t)n, 0, (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
        it->out[slot].sent_mono = 0; it->lost++;
        char b[256]; snprintf(b, sizeof(b), "\"type\":\"udp_echo\",\"iface\":\"%s\",\"probe_seq\":%u,\"sent_realtime_ns\":%llu,\"sent_monotonic_ns\":%llu,\"lost\":true,\"error\":\"send: %s\"",
                 it->name, seq, (unsigned long long)real, (unsigned long long)mono, strerror(errno));
        emit(b, real);
        if (errno == ENODEV || errno == ENETUNREACH || errno == ENETDOWN) { close(it->fd); it->fd = -1; }
        return;
    }
    it->sent++;
}

static void udp_recv(iface_t *it, uint64_t mono)
{
    char pkt[128];
    for (;;) {
        ssize_t r = recv(it->fd, pkt, sizeof(pkt) - 1, 0);
        if (r <= 0) return;
        pkt[r] = '\0';
        char name[IFNAMSIZ]; unsigned seq; unsigned long long smono;
        if (sscanf(pkt, "omrprobe %15s %u %llu", name, &seq, &smono) != 3) continue;
        int slot = seq % OUTSTANDING;
        if (it->out[slot].sent_mono == 0 || it->out[slot].seq != seq) continue; /* late or dup */
        double rtt = (double)(mono - it->out[slot].sent_mono) / 1e6;
        char b[256]; snprintf(b, sizeof(b), "\"type\":\"udp_echo\",\"iface\":\"%s\",\"probe_seq\":%u,\"sent_realtime_ns\":%llu,\"sent_monotonic_ns\":%llu,\"rtt_ms\":%.3f",
                 it->name, seq, (unsigned long long)it->out[slot].sent_real, (unsigned long long)it->out[slot].sent_mono, rtt);
        emit(b, it->out[slot].sent_real);
        it->out[slot].sent_mono = 0;
        it->recvd++; it->last_rtt_ms = rtt;
        it->rtt_win[it->rtt_pos] = rtt; it->rtt_pos = (it->rtt_pos + 1) % RTT_WINDOW; if (it->rtt_n < RTT_WINDOW) it->rtt_n++;
    }
}

static void udp_expire(iface_t *it, uint64_t mono)
{
    for (int i = 0; i < OUTSTANDING; i++) {
        if (it->out[i].sent_mono && mono - it->out[i].sent_mono > (uint64_t)cfg.udp_timeout_ms * 1000000ull) {
            char b[256]; snprintf(b, sizeof(b), "\"type\":\"udp_echo\",\"iface\":\"%s\",\"probe_seq\":%u,\"sent_realtime_ns\":%llu,\"sent_monotonic_ns\":%llu,\"lost\":true,\"timeout_ms\":%d",
                     it->name, it->out[i].seq, (unsigned long long)it->out[i].sent_real, (unsigned long long)it->out[i].sent_mono, cfg.udp_timeout_ms);
            emit(b, it->out[i].sent_real);
            it->out[i].sent_mono = 0; it->lost++;
        }
    }
}

/* ── capacity probes via curl (one child at a time) ── */
static const char *CURL_W =
    /* http_code is quoted: curl prints it zero-padded ("000" on failure),
     * which is not valid JSON as a bare number (2 rows lost on the
     * 2026-09-20 drive). Kept verbatim as a string rather than normalized. */
    "{\"http_code\":\"%{http_code}\",\"exitcode\":%{exitcode},\"remote_ip\":\"%{remote_ip}\","
    "\"time_namelookup\":%{time_namelookup},\"time_connect\":%{time_connect},"
    "\"time_starttransfer\":%{time_starttransfer},\"time_total\":%{time_total},"
    "\"size_download\":%{size_download},\"speed_download\":%{speed_download},"
    "\"size_upload\":%{size_upload},\"speed_upload\":%{speed_upload},\"num_connects\":%{num_connects}}";

static struct { pid_t pid; int fd; int iface; int up; uint64_t start_mono, start_real; char buf[2048]; size_t len; } cap = { 0, -1, 0, 0, 0, 0, {0}, 0 };

/* Non-blocking drain of curl's stdout into cap.buf (bounded). */
static void cap_read(void)
{
    for (;;) {
        if (cap.len >= sizeof(cap.buf) - 1) return;
        size_t room = sizeof(cap.buf) - 1 - cap.len;
        ssize_t r = read(cap.fd, cap.buf + cap.len, room);
        if (r <= 0) return;
        cap.len += (size_t)r;
    }
}
static char g_upfile[256];

static void cap_start(int iface, int up)
{
    int pfd[2]; if (pipe(pfd) < 0) return;
    char url[512], bytes[300], maxs[16];
    if (up) snprintf(url, sizeof(url), "%s/up", cfg.http_base);
    else snprintf(url, sizeof(url), "%s/down?bytes=%ld", cfg.http_base, cfg.down_bytes);
    snprintf(bytes, sizeof(bytes), "@%s", g_upfile);
    snprintf(maxs, sizeof(maxs), "%d", cfg.cap_max_s);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO); close(pfd[0]); close(pfd[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        const char *ifname = cfg.ifs[iface].name;
        if (up) {
            if (cfg.no_bind) execlp("curl", "curl", "-s", "-m", maxs, "-o", "/dev/null", "-w", CURL_W, "-X", "POST", "-H", "Content-Type: application/octet-stream", "--data-binary", bytes, url, (char *)NULL);
            else execlp("curl", "curl", "-s", "-m", maxs, "--interface", ifname, "-o", "/dev/null", "-w", CURL_W, "-X", "POST", "-H", "Content-Type: application/octet-stream", "--data-binary", bytes, url, (char *)NULL);
        } else {
            if (cfg.no_bind) execlp("curl", "curl", "-s", "-m", maxs, "-o", "/dev/null", "-w", CURL_W, url, (char *)NULL);
            else execlp("curl", "curl", "-s", "-m", maxs, "--interface", ifname, "-o", "/dev/null", "-w", CURL_W, url, (char *)NULL);
        }
        _exit(127);
    }
    close(pfd[1]);
    cap.pid = pid; cap.fd = pfd[0]; cap.iface = iface; cap.up = up; cap.len = 0;
    cap.start_mono = now_ns(CLOCK_MONOTONIC); cap.start_real = now_ns(CLOCK_REALTIME);
    fcntl(cap.fd, F_SETFL, O_NONBLOCK);
}

static void cap_finish(uint64_t mono)
{
    int st = 0;
    if (waitpid(cap.pid, &st, WNOHANG) != cap.pid) return;
    cap_read();
    close(cap.fd); cap.fd = -1; cap.buf[cap.len] = '\0';
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    iface_t *it = &cfg.ifs[cap.iface];
    int ok = (cap.len > 2 && cap.buf[0] == '{' && !memchr(cap.buf, '\n', cap.len));
    char b[2600];
    snprintf(b, sizeof(b), "\"type\":\"%s\",\"iface\":\"%s\",\"bytes\":%ld,\"start_realtime_ns\":%llu,\"start_monotonic_ns\":%llu,\"end_monotonic_ns\":%llu,\"curl_exit\":%d,\"curl\":%s",
             cap.up ? "http_up" : "http_down", it->name, cap.up ? cfg.up_bytes : cfg.down_bytes,
             (unsigned long long)cap.start_real, (unsigned long long)cap.start_mono, (unsigned long long)mono, rc, ok ? cap.buf : "null");
    emit(b, cap.start_real);
    if (cap.up) { snprintf(it->last_up, sizeof(it->last_up), "%s", ok ? cap.buf : "null"); it->last_up_mono = mono; }
    else { snprintf(it->last_down, sizeof(it->last_down), "%s", ok ? cap.buf : "null"); it->last_down_mono = mono; }
    cap.pid = 0;
}

/* ── state file ── */
static void write_state(void)
{
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "{\"clock_realtime_ns\":%llu,\"clock_monotonic_ns\":%llu,\"server\":", (unsigned long long)now_ns(CLOCK_REALTIME), (unsigned long long)now_ns(CLOCK_MONOTONIC));
    json_puts(f, cfg.host);
    fprintf(f, ",\"udp_port\":%d,\"http_base\":", cfg.udp_port); json_puts(f, cfg.http_base ? cfg.http_base : "");
    fprintf(f, ",\"latency_interval_ms\":%d,\"udp_timeout_ms\":%d,\"capacity_period_s\":%d,\"down_bytes\":%ld,\"up_bytes\":%ld,\"interfaces\":{",
            cfg.latency_ms, cfg.udp_timeout_ms, cfg.cap_period_s, cfg.down_bytes, cfg.up_bytes);
    for (int i = 0; i < cfg.n_if; i++) {
        iface_t *it = &cfg.ifs[i];
        double mn = 0, mx = 0, sum = 0, jit = 0; int n = it->rtt_n;
        for (int k = 0; k < n; k++) { double v = it->rtt_win[k]; if (k == 0 || v < mn) mn = v; if (k == 0 || v > mx) mx = v; sum += v; }
        for (int k = 1; k < n; k++) { int a = (it->rtt_pos - k - 1 + RTT_WINDOW) % RTT_WINDOW, b2 = (it->rtt_pos - k + RTT_WINDOW) % RTT_WINDOW; double d = it->rtt_win[b2] - it->rtt_win[a]; jit += d < 0 ? -d : d; }
        if (i) fputc(',', f);
        json_puts(f, it->name);
        fprintf(f, ":{\"udp\":{\"sent\":%llu,\"received\":%llu,\"lost\":%llu,\"last_rtt_ms\":%.3f,\"window\":{\"n\":%d,\"min_ms\":%.3f,\"avg_ms\":%.3f,\"max_ms\":%.3f,\"jitter_ms\":%.3f}},"
                   "\"http_down\":{\"age_monotonic_ns\":%llu,\"curl\":%s},\"http_up\":{\"age_monotonic_ns\":%llu,\"curl\":%s}}",
                (unsigned long long)it->sent, (unsigned long long)it->recvd, (unsigned long long)it->lost, it->last_rtt_ms,
                n, mn, n ? sum / n : 0.0, mx, n > 1 ? jit / (n - 1) : 0.0,
                (unsigned long long)it->last_down_mono, it->last_down[0] ? it->last_down : "null",
                (unsigned long long)it->last_up_mono, it->last_up[0] ? it->last_up : "null");
    }
    fputs("}}\n", f); fclose(f); rename(tmp, cfg.state_path);
}

static void usage(void)
{
    fputs("usage: omr-probed -H host [-p udp_port] [-u http_base] -i iface [-i iface ...]\n"
          "  [-l latency_ms] [-t udp_timeout_ms] [-P capacity_period_s] [-D down_bytes] [-U up_bytes]\n"
          "  [-m capacity_max_s] [-s state.json] [-r rawlog_dir] [-S survey_base] [-N (no bind, test)]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.udp_port = 4433; cfg.latency_ms = 1000; cfg.udp_timeout_ms = 2000; cfg.cap_period_s = 120; cfg.cap_max_s = 25;
    cfg.down_bytes = 5000000; cfg.up_bytes = 2000000; cfg.state_path = "/tmp/probe/state.json";
    int opt;
    while ((opt = getopt(argc, argv, "H:p:u:i:l:t:P:D:U:m:s:r:S:Nh")) != -1) {
        switch (opt) {
        case 'H': cfg.host = optarg; break;
        case 'p': cfg.udp_port = atoi(optarg); break;
        case 'u': cfg.http_base = (*optarg ? optarg : NULL); break;
        case 'i': if (cfg.n_if < MAX_IF && *optarg) { snprintf(cfg.ifs[cfg.n_if].name, IFNAMSIZ, "%s", optarg); cfg.ifs[cfg.n_if].fd = -1; cfg.n_if++; } break;
        case 'l': cfg.latency_ms = atoi(optarg); break;
        case 't': cfg.udp_timeout_ms = atoi(optarg); break;
        case 'P': cfg.cap_period_s = atoi(optarg); break;
        case 'D': cfg.down_bytes = atol(optarg); break;
        case 'U': cfg.up_bytes = atol(optarg); break;
        case 'm': cfg.cap_max_s = atoi(optarg); break;
        case 's': cfg.state_path = optarg; break;
        case 'r': cfg.rawlog_dir = (*optarg ? optarg : NULL); break;
        case 'S': cfg.survey_base = (*optarg ? optarg : NULL); break;
        case 'N': cfg.no_bind = 1; break;
        default: usage();
        }
    }
    if (!cfg.host || cfg.n_if == 0 || cfg.latency_ms < 100) usage();

    struct sockaddr_in dst = {0}; dst.sin_family = AF_INET; dst.sin_port = htons((uint16_t)cfg.udp_port);
    if (inet_pton(AF_INET, cfg.host, &dst.sin_addr) != 1) { fputs("omr-probed: -H must be an IPv4 address\n", stderr); return 1; }
    { char d[512]; snprintf(d, sizeof(d), "%s", cfg.state_path); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); }
      if (cfg.rawlog_dir) mkdir(cfg.rawlog_dir, 0755);
      snprintf(g_upfile, sizeof(g_upfile), "%s.upload.bin", cfg.state_path);
      if (cfg.http_base && cfg.up_bytes > 0) { FILE *uf = fopen(g_upfile, "w"); if (uf) { char z[4096]; FILE *ur = fopen("/dev/urandom", "r"); long left = cfg.up_bytes; while (left > 0) { size_t k = left > (long)sizeof(z) ? sizeof(z) : (size_t)left; if (!ur || fread(z, 1, k, ur) != k) memset(z, 'x', k); fwrite(z, 1, k, uf); left -= (long)k; } if (ur) fclose(ur); fclose(uf); } } }

    struct sigaction sa = {0}; sa.sa_handler = on_stop; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); signal(SIGPIPE, SIG_IGN);

    uint64_t start = now_ns(CLOCK_MONOTONIC);
    for (int i = 0; i < cfg.n_if; i++) cfg.ifs[i].next_send_mono = start + (uint64_t)i * 100000000ull; /* stagger 100 ms */
    uint64_t cap_interval = cfg.n_if ? (uint64_t)cfg.cap_period_s * 1000000000ull / (uint64_t)cfg.n_if : 0;
    uint64_t next_cap = start + 5000000000ull; int cap_iface = 0, cap_phase = 0; /* 0 = down, 1 = up */
    uint64_t last_state = 0;
    write_state();

    while (!g_stop) {
        struct pollfd pf[MAX_IF + 1]; int np = 0;
        for (int i = 0; i < cfg.n_if; i++) if (cfg.ifs[i].fd >= 0) { pf[np].fd = cfg.ifs[i].fd; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        if (cap.pid) { pf[np].fd = cap.fd; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        poll(pf, np, 100);
        uint64_t mono = now_ns(CLOCK_MONOTONIC), real = now_ns(CLOCK_REALTIME);
        session_refresh(mono);
        for (int i = 0; i < cfg.n_if; i++) {
            iface_t *it = &cfg.ifs[i];
            if (it->fd >= 0) udp_recv(it, mono);
            udp_expire(it, mono);
            if (mono >= it->next_send_mono) { udp_send(it, &dst, mono, real); it->next_send_mono += (uint64_t)cfg.latency_ms * 1000000ull; if (it->next_send_mono < mono) it->next_send_mono = mono + (uint64_t)cfg.latency_ms * 1000000ull; }
        }
        if (cap.pid) {
            cap_read();
            cap_finish(mono);
            if (!cap.pid && cap_phase == 1) { cap_phase = 0; cap_iface = (cap_iface + 1) % cfg.n_if; }
            else if (!cap.pid && cap_phase == 0) { cap_phase = 1; if (cfg.up_bytes > 0) cap_start(cap_iface, 1); else { cap_phase = 0; cap_iface = (cap_iface + 1) % cfg.n_if; } }
        } else if (cfg.http_base && cap_interval && mono >= next_cap) {
            next_cap += cap_interval; if (next_cap < mono) next_cap = mono + cap_interval;
            cap_phase = 0; cap_start(cap_iface, 0);
        }
        if (mono - last_state >= 1000000000ull) { write_state(); last_state = mono; }
    }
    write_state();
    if (cap.pid) { kill(cap.pid, SIGTERM); waitpid(cap.pid, NULL, 0); }
    for (int i = 0; i < cfg.n_if; i++) if (cfg.ifs[i].fd >= 0) close(cfg.ifs[i].fd);
    if (g_raw) fclose(g_raw);
    if (g_sess) fclose(g_sess);
    return 0;
}
