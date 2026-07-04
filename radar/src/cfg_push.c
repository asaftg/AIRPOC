#define _GNU_SOURCE
#include "cfg_push.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INTER_LINE_US   50000    /* 50 ms between commands (TI convention) */
#define ACK_TIMEOUT_MS  2000

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* Drain any bytes currently sitting in the OS buffer. */
static void drain(int fd) {
    char tmp[256];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
    }
}

/* Read until an ack token appears or the timeout elapses. */
static void read_until_ack(int fd, char *out, size_t outsz) {
    double deadline = now_ms() + ACK_TIMEOUT_MS;
    size_t len = 0;
    out[0] = 0;
    while (now_ms() < deadline) {
        char tmp[256];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            size_t cp = (len + (size_t)n < outsz - 1) ? (size_t)n : (outsz - 1 - len);
            memcpy(out + len, tmp, cp);
            len += cp; out[len] = 0;
            if (strstr(out, "Done") || strstr(out, "Error") ||
                strstr(out, "mmwDemo:/>") || strstr(out, "mmw_pro:/>"))
                return;
        } else {
            struct timespec s = {0, 2 * 1000 * 1000};   /* 2 ms */
            nanosleep(&s, NULL);
        }
    }
}

/* Wait until the mmw_demo CLI is up (it echoes a "mmwDemo:/>" prompt). After a
 * power-cycle the chip needs a few seconds to boot; pushing the cfg before the
 * console is ready loses the early lines (silently — no "Error", just no ack) so
 * sensorStart runs on a half-applied config and the sensor produces no frames.
 * Returns 1 if the prompt appeared within timeout_s. */
static int wait_cli_ready(int fd, double timeout_s) {
    double deadline = now_ms() + timeout_s * 1000.0;
    char buf[512];
    while (now_ms() < deadline) {
        drain(fd);
        ssize_t w = write(fd, "\n", 1); (void)w;   /* nudge → demo prints its prompt */
        double d2 = now_ms() + 700;
        size_t len = 0; buf[0] = 0;
        while (now_ms() < d2) {
            ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
            if (n > 0) {
                len += (size_t)n; buf[len] = 0;
                if (strstr(buf, "mmwDemo") || strstr(buf, "mmw_pro") || strstr(buf, ":/>"))
                    return 1;
                if (len > sizeof(buf) - 8) { buf[0] = 0; len = 0; }
            } else {
                struct timespec s = {0, 5 * 1000 * 1000}; nanosleep(&s, NULL);
            }
        }
    }
    return 0;
}

int cfg_push(int cli_fd, const char *cfg_path) {
    FILE *f = fopen(cfg_path, "r");
    if (!f) { perror("radar: open cfg"); return -1; }

    /* Don't push until the CLI console answers — avoids losing early lines to a
     * still-booting chip. */
    if (wait_cli_ready(cli_fd, 15.0))
        fprintf(stderr, "radar: CLI ready — pushing cfg\n");
    else
        fprintf(stderr, "radar: CLI not confirmed after 15 s — pushing anyway\n");

    drain(cli_fd);
    char line[512];
    int rc = 0, n_sent = 0;
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing whitespace/newline */
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r' ||
                     line[L - 1] == ' '  || line[L - 1] == '\t')) line[--L] = 0;
        /* skip leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '%' || *s == '#') continue;

        drain(cli_fd);            /* clear a slow prior ack before writing */
        dprintf(cli_fd, "%s\n", s);

        char resp[1024];
        read_until_ack(cli_fd, resp, sizeof(resp));
        if (strstr(resp, "Error") || strstr(resp, "error")) {
            fprintf(stderr, "radar cfg REJECTED: %s -> %.120s\n", s, resp);
            rc = -1;
        }
        n_sent++;
        usleep(INTER_LINE_US);
    }
    fclose(f);
    fprintf(stderr, "radar: pushed %d cfg lines from %s (%s)\n",
            n_sent, cfg_path, rc == 0 ? "ok" : "with errors");
    return rc;
}
