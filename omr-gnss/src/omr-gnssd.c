/*
 * omr-gnssd -- NMEA-over-TCP receiver for OpenMPTCProuter surveys.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Listens on a TCP port (default 8620) for an NMEA 0183 stream pushed by a
 * GNSS receiver / phone app. For every valid sentence it keeps the latest
 * copy per key (talker+type, plus the message index for GSV and the NMEA
 * 4.1 system id for GSA) together with CLOCK_REALTIME / CLOCK_MONOTONIC
 * receive stamps, and publishes the table as JSON in a state file that the
 * rpcd plugin serves as `ubus call gnss get`. Optionally every sentence is
 * also appended verbatim, with both receive stamps, to a daily raw log so
 * the full receiver rate (5 Hz RMC etc.) survives 1 Hz survey sampling.
 *
 * No NMEA field parsing beyond the key: position decoding is off-router.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_KEYS      96
#define MAX_SENTENCE  256
#define STATE_MIN_INTERVAL_MS 200

typedef struct {
    char key[16];
    char nmea[MAX_SENTENCE];
    uint64_t rx_real_ns, rx_mono_ns;
    uint64_t count;
} entry_t;

static struct {
    const char *bind_addr;
    int port;
    const char *state_path;
    const char *rawlog_dir;
} cfg = { "0.0.0.0", 8620, "/tmp/gnss/state.json", NULL };

static volatile sig_atomic_t g_stop = 0;
static entry_t g_tab[MAX_KEYS];
static int g_ntab = 0;
static uint64_t g_total = 0, g_bad = 0, g_last_real = 0, g_last_mono = 0, g_state_written_mono = 0;
static int g_dirty = 0;
static char g_peer[64] = "";
static FILE *g_raw = NULL;
static char g_raw_day[16] = "";

static void on_stop(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void json_puts(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20 || c == 0x7f) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* $TTSSS,...*HH -> verify checksum, derive key. Returns 0 if invalid. */
static int sentence_key(const char *s, size_t len, char *key, size_t klen)
{
    if (len < 9 || s[0] != '$') return 0;
    const char *star = memchr(s, '*', len);
    if (!star || (size_t)(star - s) + 3 > len) return 0;
    unsigned sum = 0;
    for (const char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
    unsigned want;
    if (sscanf(star + 1, "%2x", &want) != 1 || want != sum) return 0;

    /* talker (2) + type (3), e.g. GNRMC; proprietary $P... use up to 5 chars */
    char tt[6] = {0};
    size_t i;
    for (i = 0; i < 5 && s[1 + i] && s[1 + i] != ','; i++) tt[i] = s[1 + i];
    if (i < 3) return 0;
    const char *type = tt + (i >= 5 ? 2 : 0);

    /* Multi-message groups: keep every member. GSV: field 2 = message
     * number. GSA (NMEA 4.1): last field before '*' = system id. */
    if (strcmp(type, "GSV") == 0) {
        const char *f1 = memchr(s, ',', star - s);
        const char *f2 = f1 ? memchr(f1 + 1, ',', star - f1 - 1) : NULL;
        int msg = f2 ? atoi(f2 + 1) : 0;
        snprintf(key, klen, "%s-%d", tt, msg);
    } else if (strcmp(type, "GSA") == 0) {
        const char *last = star;
        while (last > s && last[-1] != ',') last--;
        int sys = (last < star) ? atoi(last) : 0;
        snprintf(key, klen, "%s-%d", tt, sys);
    } else {
        snprintf(key, klen, "%s", tt);
    }
    return 1;
}

static void store(const char *key, const char *s, size_t len, uint64_t real, uint64_t mono)
{
    entry_t *e = NULL;
    for (int i = 0; i < g_ntab; i++)
        if (strcmp(g_tab[i].key, key) == 0) { e = &g_tab[i]; break; }
    if (!e) {
        if (g_ntab >= MAX_KEYS) return;
        e = &g_tab[g_ntab++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    if (len >= sizeof(e->nmea)) len = sizeof(e->nmea) - 1;
    memcpy(e->nmea, s, len);
    e->nmea[len] = '\0';
    e->rx_real_ns = real;
    e->rx_mono_ns = mono;
    e->count++;
    g_dirty = 1;
}

static void raw_log(const char *s, size_t len, uint64_t real, uint64_t mono)
{
    if (!cfg.rawlog_dir) return;
    time_t t = (time_t)(real / 1000000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    char day[16];
    strftime(day, sizeof(day), "%Y%m%d", &tm);
    if (!g_raw || strcmp(day, g_raw_day) != 0) {
        if (g_raw) fclose(g_raw);
        char path[512];
        snprintf(path, sizeof(path), "%s/nmea-%s.log", cfg.rawlog_dir, day);
        g_raw = fopen(path, "a");
        snprintf(g_raw_day, sizeof(g_raw_day), "%s", day);
    }
    if (!g_raw) return;
    fprintf(g_raw, "%llu %llu ", (unsigned long long)real, (unsigned long long)mono);
    fwrite(s, 1, len, g_raw);
    fputc('\n', g_raw);
}

static void write_state(int connected)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    uint64_t real = now_ns(CLOCK_REALTIME), mono = now_ns(CLOCK_MONOTONIC);
    fprintf(f, "{\"connected\":%s,\"peer\":", connected ? "true" : "false");
    json_puts(f, g_peer);
    fprintf(f, ",\"clock_realtime_ns\":%llu,\"clock_monotonic_ns\":%llu,"
               "\"sentences_total\":%llu,\"checksum_errors\":%llu,"
               "\"last_rx_realtime_ns\":%llu,\"last_rx_monotonic_ns\":%llu,\"sentences\":{",
            (unsigned long long)real, (unsigned long long)mono,
            (unsigned long long)g_total, (unsigned long long)g_bad,
            (unsigned long long)g_last_real, (unsigned long long)g_last_mono);
    for (int i = 0; i < g_ntab; i++) {
        entry_t *e = &g_tab[i];
        if (i) fputc(',', f);
        json_puts(f, e->key);
        fprintf(f, ":{\"rx_realtime_ns\":%llu,\"rx_monotonic_ns\":%llu,\"count\":%llu,\"nmea\":",
                (unsigned long long)e->rx_real_ns, (unsigned long long)e->rx_mono_ns,
                (unsigned long long)e->count);
        json_puts(f, e->nmea);
        fputc('}', f);
    }
    fputs("}}\n", f);
    fclose(f);
    rename(tmp, cfg.state_path);
    g_state_written_mono = mono;
    g_dirty = 0;
}

static void handle_line(char *s, size_t len)
{
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ')) len--;
    if (!len) return;
    s[len] = '\0';
    uint64_t real = now_ns(CLOCK_REALTIME), mono = now_ns(CLOCK_MONOTONIC);
    char key[16];
    if (!sentence_key(s, len, key, sizeof(key))) { g_bad++; g_dirty = 1; return; }
    g_total++;
    g_last_real = real;
    g_last_mono = mono;
    store(key, s, len, real, mono);
    raw_log(s, len, real, mono);
}

static void usage(void)
{
    fputs("usage: omr-gnssd [-b bind] [-p port] [-s state.json] [-r rawlog-dir]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "b:p:s:r:h")) != -1) {
        switch (opt) {
        case 'b': cfg.bind_addr = optarg; break;
        case 'p': cfg.port = atoi(optarg); break;
        case 's': cfg.state_path = optarg; break;
        case 'r': cfg.rawlog_dir = (*optarg ? optarg : NULL); break;
        default: usage();
        }
    }
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", cfg.state_path);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = '\0'; mkdir(dir, 0755); }
        if (cfg.rawlog_dir) mkdir(cfg.rawlog_dir, 0755);
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa4 = {0};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons((uint16_t)cfg.port);
    if (inet_pton(AF_INET, cfg.bind_addr, &sa4.sin_addr) != 1) { fputs("bad bind addr\n", stderr); return 1; }
    if (bind(lfd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0 || listen(lfd, 2) < 0) { perror("bind/listen"); return 1; }

    int cfd = -1;
    char buf[4096];
    size_t blen = 0;
    write_state(0);

    while (!g_stop) {
        struct pollfd pf[2] = { { lfd, POLLIN, 0 }, { cfd, POLLIN, 0 } };
        int n = poll(pf, cfd >= 0 ? 2 : 1, 250);
        if (n < 0 && errno != EINTR) break;
        if (n > 0 && (pf[0].revents & POLLIN)) {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            int nfd = accept(lfd, (struct sockaddr *)&peer, &pl);
            if (nfd >= 0) {
                /* One stream at a time: a new sender replaces the old one. */
                if (cfd >= 0) close(cfd);
                cfd = nfd;
                blen = 0;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                snprintf(g_peer, sizeof(g_peer), "%s:%u", ip, ntohs(peer.sin_port));
                g_dirty = 1;
            }
        }
        if (n > 0 && cfd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(cfd, buf + blen, sizeof(buf) - blen - 1);
            if (r <= 0) {
                close(cfd);
                cfd = -1;
                g_dirty = 1;
            } else {
                blen += (size_t)r;
                size_t start = 0;
                for (size_t i = 0; i < blen; i++) {
                    if (buf[i] == '\n') {
                        handle_line(buf + start, i - start);
                        start = i + 1;
                    }
                }
                if (start < blen) memmove(buf, buf + start, blen - start);
                blen -= start;
                if (blen >= sizeof(buf) - 1) blen = 0; /* garbage without newline */
            }
        }
        uint64_t mono = now_ns(CLOCK_MONOTONIC);
        if (g_dirty && mono - g_state_written_mono >= STATE_MIN_INTERVAL_MS * 1000000ull)
            write_state(cfd >= 0);
        if (g_raw) fflush(g_raw);
    }
    write_state(0);
    if (cfd >= 0) close(cfd);
    close(lfd);
    if (g_raw) fclose(g_raw);
    return 0;
}
