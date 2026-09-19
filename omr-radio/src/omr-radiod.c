/*
 * omr-radiod -- pulls phone-side radio telemetry for OpenMPTCProuter surveys.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Each USB-tethered phone runs the omr-radio Android app, which listens on
 * TCP 8630 and writes one JSON line per second (and on every cell / signal
 * change). This daemon connects to that port through each WAN interface
 * (SO_BINDTODEVICE), using the interface's default gateway -- which IS the
 * phone -- as the address, so the carrier of every line is fixed by the
 * interface it arrived on. Lines are stored verbatim:
 *
 *   {"seq":N,"session":"drive-01","iface":"usb-au","peer":"172.25.74.2:8630",
 *    "recv_realtime_ns":...,"recv_monotonic_ns":...,"line":{...phone JSON...}}
 *
 * to <rawlog_dir>/radio-YYYYMMDD.jsonl and, while an omr-survey session is
 * running, to <survey_base>/<session>/radio.jsonl. The latest line per
 * interface is kept in a state file served as `ubus call radio get`.
 *
 * Gateways are re-resolved every 10 s while disconnected (netifd via
 * `ubus call network.interface dump`), so DHCP renewals and re-plugs are
 * followed without configuration. `-i iface=host` pins an address instead.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_IF     8
#define MAX_LINE   16384

typedef enum { ST_IDLE, ST_CONNECTING, ST_CONNECTED } st_t;

typedef struct {
    char name[IFNAMSIZ];
    char pinned[64];      /* -i iface=host, else "" */
    char host[64];        /* resolved gateway */
    char peer[80];
    int fd;
    st_t st;
    uint64_t next_try_mono, connect_started_mono, backoff_ns;
    char buf[MAX_LINE + 1];
    size_t len;
    uint64_t lines, bad, last_real, last_mono, connected_since_mono;
    char last[MAX_LINE + 1];
} peer_t;

static struct {
    peer_t p[MAX_IF];
    int n;
    int port;
    const char *state_path, *rawlog_dir, *survey_base;
    int no_bind;
} cfg;

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }
static uint64_t now_ns(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

static void json_puts(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) { unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20 || c == 0x7f) fprintf(f, "\\u%04x", c); else fputc(c, f); }
    fputc('"', f);
}

/* ── sinks: daily log + session tee ── */
static FILE *g_raw = NULL; static char g_raw_day[16] = ""; static uint64_t g_raw_seq = 0;
static FILE *g_sess = NULL; static char g_sess_name[136] = ""; static uint64_t g_sess_seq = 0, g_sess_checked = 0;

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
    if (name[0]) { char p[700]; snprintf(p, sizeof(p), "%s/%s/radio.jsonl", cfg.survey_base, name); g_sess = fopen(p, "a"); }
}

static void emit_one(FILE *f, uint64_t seq, const char *session, const peer_t *p, const char *line, uint64_t real, uint64_t mono)
{
    fprintf(f, "{\"seq\":%llu,\"session\":", (unsigned long long)seq);
    if (session && *session) json_puts(f, session); else fputs("null", f);
    fputs(",\"iface\":", f); json_puts(f, p->name);
    fputs(",\"peer\":", f); json_puts(f, p->peer);
    fprintf(f, ",\"recv_realtime_ns\":%llu,\"recv_monotonic_ns\":%llu,\"line\":%s}\n",
            (unsigned long long)real, (unsigned long long)mono, line);
    fflush(f);
}

static void emit(const peer_t *p, const char *line, uint64_t real, uint64_t mono)
{
    if (cfg.rawlog_dir) {
        time_t t = (time_t)(real / 1000000000ull); struct tm tm; gmtime_r(&t, &tm);
        char day[16]; strftime(day, sizeof(day), "%Y%m%d", &tm);
        if (!g_raw || strcmp(day, g_raw_day)) { if (g_raw) fclose(g_raw); char path[512]; snprintf(path, sizeof(path), "%s/radio-%s.jsonl", cfg.rawlog_dir, day); g_raw = fopen(path, "a"); snprintf(g_raw_day, sizeof(g_raw_day), "%s", day); g_raw_seq = 0; }
        if (g_raw) emit_one(g_raw, g_raw_seq++, g_sess_name, p, line, real, mono);
    }
    if (g_sess) emit_one(g_sess, g_sess_seq++, g_sess_name, p, line, real, mono);
}

/* ── gateway resolution via netifd ── */
static int resolve_gateway(peer_t *p)
{
    if (p->pinned[0]) { snprintf(p->host, sizeof(p->host), "%s", p->pinned); return 1; }
    /* OMR keeps each WAN's default route in a per-interface table, so
     * netifd reports it under inactive.route; plain route[] is the
     * fallback, and the DHCP server address the last resort. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "D=$(ubus -S call network.interface dump 2>/dev/null); "
             "for e in '@.interface[@.l3_device=\"%s\"].inactive.route[@.target=\"0.0.0.0\"].nexthop' "
             "'@.interface[@.l3_device=\"%s\"].route[@.target=\"0.0.0.0\"].nexthop' "
             "'@.interface[@.l3_device=\"%s\"].data.dhcpserver'; do "
             "g=$(echo \"$D\" | jsonfilter -e \"$e\" 2>/dev/null | head -n1); [ -n \"$g\" ] && { echo \"$g\"; break; }; done",
             p->name, p->name, p->name);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char out[64] = "";
    if (fgets(out, sizeof(out), f)) { size_t n = strlen(out); while (n && (out[n-1] == '\n' || out[n-1] == ' ')) out[--n] = '\0'; }
    pclose(f);
    struct in_addr a;
    if (!out[0] || inet_pton(AF_INET, out, &a) != 1) return 0;
    snprintf(p->host, sizeof(p->host), "%s", out);
    return 1;
}

static void peer_close(peer_t *p, uint64_t mono, const char *why)
{
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->st == ST_CONNECTED) {
        char l[256]; snprintf(l, sizeof(l), "{\"event\":\"disconnected\",\"reason\":\"%s\"}", why);
        emit(p, l, now_ns(CLOCK_REALTIME), mono);
    }
    p->st = ST_IDLE; p->len = 0;
    p->next_try_mono = mono + p->backoff_ns;
    p->backoff_ns = p->backoff_ns < 10000000000ull ? p->backoff_ns * 2 : 10000000000ull;
}

static void peer_try_connect(peer_t *p, uint64_t mono)
{
    if (!resolve_gateway(p)) { p->next_try_mono = mono + 10000000000ull; return; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { p->next_try_mono = mono + 5000000000ull; return; }
    if (!cfg.no_bind && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, p->name, strlen(p->name) + 1) < 0) { close(fd); p->next_try_mono = mono + 10000000000ull; return; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)cfg.port); inet_pton(AF_INET, p->host, &sa.sin_addr);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(fd); p->next_try_mono = mono + p->backoff_ns; p->backoff_ns = p->backoff_ns < 10000000000ull ? p->backoff_ns * 2 : 10000000000ull; return; }
    p->fd = fd; p->st = ST_CONNECTING; p->connect_started_mono = mono;
    snprintf(p->peer, sizeof(p->peer), "%s:%d", p->host, cfg.port);
}

static void peer_on_connected(peer_t *p, uint64_t mono)
{
    int err = 0; socklen_t el = sizeof(err);
    getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { peer_close(p, mono, "connect failed"); return; }
    p->st = ST_CONNECTED; p->connected_since_mono = mono; p->backoff_ns = 2000000000ull; p->len = 0;
    char l[128]; snprintf(l, sizeof(l), "{\"event\":\"connected\"}");
    emit(p, l, now_ns(CLOCK_REALTIME), mono);
}

static void peer_read(peer_t *p, uint64_t mono)
{
    ssize_t r = read(p->fd, p->buf + p->len, MAX_LINE - p->len);
    if (r == 0) { peer_close(p, mono, "eof"); return; }
    if (r < 0) { if (errno == EAGAIN || errno == EINTR) return; peer_close(p, mono, "read error"); return; }
    p->len += (size_t)r;
    uint64_t real = now_ns(CLOCK_REALTIME);
    size_t start = 0;
    for (size_t i = 0; i < p->len; i++) {
        if (p->buf[i] != '\n') continue;
        size_t n = i - start;
        while (n && (p->buf[start + n - 1] == '\r' || p->buf[start + n - 1] == ' ')) n--;
        if (n) {
            p->buf[start + n] = '\0';
            const char *line = p->buf + start;
            if (line[0] == '{' && line[n - 1] == '}') {
                emit(p, line, real, mono);
                memcpy(p->last, line, n + 1);
                p->lines++; p->last_real = real; p->last_mono = mono;
            } else {
                p->bad++;
            }
        }
        start = i + 1;
    }
    if (start < p->len) memmove(p->buf, p->buf + start, p->len - start);
    p->len -= start;
    if (p->len >= MAX_LINE) { p->len = 0; p->bad++; } /* oversized line: drop */
}

static void write_state(void)
{
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "{\"clock_realtime_ns\":%llu,\"clock_monotonic_ns\":%llu,\"port\":%d,\"interfaces\":{",
            (unsigned long long)now_ns(CLOCK_REALTIME), (unsigned long long)now_ns(CLOCK_MONOTONIC), cfg.port);
    for (int i = 0; i < cfg.n; i++) {
        peer_t *p = &cfg.p[i];
        if (i) fputc(',', f);
        json_puts(f, p->name);
        fprintf(f, ":{\"connected\":%s,\"peer\":", p->st == ST_CONNECTED ? "true" : "false"); json_puts(f, p->peer);
        fprintf(f, ",\"lines\":%llu,\"bad_lines\":%llu,\"last_recv_realtime_ns\":%llu,\"last_recv_monotonic_ns\":%llu,\"last\":%s}",
                (unsigned long long)p->lines, (unsigned long long)p->bad, (unsigned long long)p->last_real, (unsigned long long)p->last_mono,
                p->last[0] ? p->last : "null");
    }
    fputs("}}\n", f); fclose(f); rename(tmp, cfg.state_path);
}

static void usage(void)
{
    fputs("usage: omr-radiod -i iface[=host] [-i ...] [-p port] [-s state.json] [-r rawlog_dir] [-S survey_base] [-N]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 8630; cfg.state_path = "/tmp/radio/state.json";
    int opt;
    while ((opt = getopt(argc, argv, "i:p:s:r:S:Nh")) != -1) {
        switch (opt) {
        case 'i': if (cfg.n < MAX_IF && *optarg) { peer_t *p = &cfg.p[cfg.n++]; char *eq = strchr(optarg, '='); if (eq) { *eq = '\0'; snprintf(p->pinned, sizeof(p->pinned), "%s", eq + 1); } snprintf(p->name, IFNAMSIZ, "%s", optarg); p->fd = -1; p->backoff_ns = 2000000000ull; } break;
        case 'p': cfg.port = atoi(optarg); break;
        case 's': cfg.state_path = optarg; break;
        case 'r': cfg.rawlog_dir = (*optarg ? optarg : NULL); break;
        case 'S': cfg.survey_base = (*optarg ? optarg : NULL); break;
        case 'N': cfg.no_bind = 1; break;
        default: usage();
        }
    }
    if (cfg.n == 0) usage();
    { char d[512]; snprintf(d, sizeof(d), "%s", cfg.state_path); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); } if (cfg.rawlog_dir) mkdir(cfg.rawlog_dir, 0755); }
    struct sigaction sa = {0}; sa.sa_handler = on_stop; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); signal(SIGPIPE, SIG_IGN);

    uint64_t last_state = 0;
    write_state();
    while (!g_stop) {
        struct pollfd pf[MAX_IF]; int np = 0; int idx[MAX_IF];
        for (int i = 0; i < cfg.n; i++) { peer_t *p = &cfg.p[i]; if (p->fd >= 0) { pf[np].fd = p->fd; pf[np].events = p->st == ST_CONNECTING ? POLLOUT : POLLIN; pf[np].revents = 0; idx[np] = i; np++; } }
        poll(pf, np, 200);
        uint64_t mono = now_ns(CLOCK_MONOTONIC);
        session_refresh(mono);
        for (int k = 0; k < np; k++) {
            peer_t *p = &cfg.p[idx[k]];
            if (p->st == ST_CONNECTING && (pf[k].revents & (POLLOUT | POLLERR | POLLHUP))) peer_on_connected(p, mono);
            else if (p->st == ST_CONNECTED && (pf[k].revents & (POLLIN | POLLHUP | POLLERR))) peer_read(p, mono);
        }
        for (int i = 0; i < cfg.n; i++) {
            peer_t *p = &cfg.p[i];
            if (p->st == ST_IDLE && mono >= p->next_try_mono) peer_try_connect(p, mono);
            else if (p->st == ST_CONNECTING && mono - p->connect_started_mono > 5000000000ull) peer_close(p, mono, "connect timeout");
            else if (p->st == ST_CONNECTED && p->last_mono && mono - p->last_mono > 15000000000ull) peer_close(p, mono, "no data for 15s");
        }
        if (mono - last_state >= 1000000000ull) { write_state(); last_state = mono; }
    }
    for (int i = 0; i < cfg.n; i++) if (cfg.p[i].fd >= 0) close(cfg.p[i].fd);
    write_state();
    if (g_raw) fclose(g_raw);
    if (g_sess) fclose(g_sess);
    return 0;
}
