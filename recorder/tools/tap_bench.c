/* tap_bench — synthetic tap publisher/verifier for soak-testing the recorder
 * without a camera or radar.
 *
 *   tap_bench pub <name> <slot_kb> <hz> [seconds]   publish patterned frames
 *   tap_bench sub <name> [seconds]                  drain + verify pattern/gaps
 *
 * Pattern: every 8 bytes of payload = record seq (LE), so a verifier (this tool
 * or airec_dump.py) can prove end-to-end integrity of any byte of any record.
 * meta[0] carries the low 32 bits of seq, mimicking v4l2_sequence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "../tap/airpoc_tap.h"

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static void sleep_until_ns(uint64_t t)
{
    struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static int run_pub(const char *name, uint32_t slot_bytes, double hz, double secs)
{
    AirTap t;
    if (tap_create(&t, name, 16, slot_bytes,
                   "{\"name\":\"tap_bench\",\"encoding\":\"seq64-pattern\"}") != 0)
        return 1;
    uint64_t period = (uint64_t)(1e9 / hz);
    uint64_t next = tap_now_ns() + period;
    uint64_t end = secs > 0 ? tap_now_ns() + (uint64_t)(secs * 1e9) : 0;
    uint64_t seq = 0, t0 = tap_now_ns();

    while (g_run && (!end || tap_now_ns() < end)) {
        uint64_t *p = (uint64_t *)tap_slot_begin(&t);
        if (!p) return 1;
        for (uint32_t i = 0; i < slot_bytes / 8; i++) p[i] = seq;
        uint32_t meta[TAP_META_WORDS] = { (uint32_t)seq, 0, 0, 0, 0, 0 };
        tap_slot_commit(&t, slot_bytes, tap_now_ns(), meta, 0);
        seq++;
        sleep_until_ns(next);
        next += period;
    }
    double dt = (tap_now_ns() - t0) / 1e9;
    printf("pub: %llu records in %.1fs (%.2f Hz, %.1f MB/s)\n",
           (unsigned long long)seq, dt, seq / dt, seq / dt * slot_bytes / 1e6);
    tap_destroy(&t);
    return 0;
}

static int run_sub(const char *name, double secs)
{
    AirTapSub s;
    if (tap_open(&s, name) != 0) { fprintf(stderr, "sub: tap %s not found\n", name); return 1; }
    uint8_t *buf = malloc(s.t.h->slot_bytes);
    uint64_t got = 0, bad = 0, t0 = tap_now_ns();
    uint64_t end = secs > 0 ? t0 + (uint64_t)(secs * 1e9) : 0;
    AirTapRec r;

    while (g_run && (!end || tap_now_ns() < end)) {
        int n = tap_read(&s, buf, s.t.h->slot_bytes, &r);
        if (n <= 0) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); continue; }
        const uint64_t *p = (const uint64_t *)buf;
        for (uint32_t i = 0; i < r.payload_len / 8; i++)
            if (p[i] != r.seq) { bad++; break; }
        got++;
    }
    double dt = (tap_now_ns() - t0) / 1e9;
    printf("sub: %llu records (%.1f MB/s), %llu gaps, %llu CORRUPT\n",
           (unsigned long long)got, got / dt * s.t.h->slot_bytes / 1e6,
           (unsigned long long)s.gaps, (unsigned long long)bad);
    free(buf);
    tap_close(&s);
    return bad ? 2 : 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    if (argc >= 5 && !strcmp(argv[1], "pub"))
        return run_pub(argv[2], (uint32_t)atoi(argv[3]) * 1024u, atof(argv[4]),
                       argc > 5 ? atof(argv[5]) : 0);
    if (argc >= 3 && !strcmp(argv[1], "sub"))
        return run_sub(argv[2], argc > 3 ? atof(argv[3]) : 0);
    fprintf(stderr, "usage: tap_bench pub <name> <slot_kb> <hz> [seconds]\n"
                    "       tap_bench sub <name> [seconds]\n");
    return 1;
}
