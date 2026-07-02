/*
 * sgctl — command-line control for the Savgood SG-IR850-8M IR illuminator.
 *
 * Usage:
 *   sgctl [--port DEV] [--addr N] <command> [args]
 *
 * Port resolution: --port, else $SG_IR850_PORT, else /dev/sg-ir850.
 *
 * Commands:
 *   on                    turn the laser on (NOTE: power resets to MAX on every
 *                         power-on; follow with `power N` to lower it)
 *   off                   turn the laser off
 *   power <0-255|N%>      set optical drive level
 *   power-up | power-down nudge level one step
 *   fov <deg>             set beam angle in degrees (1.96 .. 70)
 *   zoom-pos <1..1790>    set absolute motor position (0x0001..0x06FE)
 *   tele <steps>          narrow beam by N motor steps (0..255)
 *   wide <steps>          widen beam by N motor steps (0..255)
 *   reset                 re-home the zoom motor
 *   status                query power / level / position / fan
 */
#include "sg_ir850.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT "/dev/sg-ir850"

static void usage(FILE *f)
{
    fputs(
"sgctl [--port DEV] [--addr N] <command> [args]\n"
"  on | off\n"
"  power <0-255|N%>\n"
"  power-up | power-down\n"
"  fov <deg>            (1.96 .. 70)\n"
"  zoom-pos <1..1790>\n"
"  tele <steps> | wide <steps>   (0..255)\n"
"  reset\n"
"  status\n"
"port: --port, else $SG_IR850_PORT, else " DEFAULT_PORT "\n",
        f);
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "sgctl: %s: %s\n", what, strerror(rc < 0 ? -rc : rc));
    return 1;
}

/* parse "128" or "50%" into a 0..255 level. Returns -1 on error. */
static int parse_level(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s)
        return -1;
    if (*end == '%') {
        if (v < 0 || v > 100)
            return -1;
        return (int)((v * 255 + 50) / 100);
    }
    if (*end != '\0' || v < 0 || v > 255)
        return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    const char *port = getenv("SG_IR850_PORT");
    int addr = 0;

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if (!strcmp(argv[i], "--addr") && i + 1 < argc) {
            addr = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            break;          /* first non-option = command                     */
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    if (!port)
        port = DEFAULT_PORT;

    const char *cmd = argv[i++];
    const char *arg = (i < argc) ? argv[i] : NULL;

    sg_ir850_t dev;
    int rc = sg_open(&dev, port, (uint8_t)addr);
    if (rc)
        return fail(port, rc);

    int ret = 0;

    if (!strcmp(cmd, "on")) {
        rc = sg_power(&dev, true);
        if (rc) ret = fail("on", rc);
    } else if (!strcmp(cmd, "off")) {
        rc = sg_power(&dev, false);
        if (rc) ret = fail("off", rc);
    } else if (!strcmp(cmd, "power")) {
        int lvl = arg ? parse_level(arg) : -1;
        if (lvl < 0) { fprintf(stderr, "sgctl: power needs 0-255 or N%%\n"); ret = 2; }
        else { rc = sg_set_power(&dev, (uint8_t)lvl); if (rc) ret = fail("power", rc); }
    } else if (!strcmp(cmd, "power-up")) {
        rc = sg_power_step(&dev, true);  if (rc) ret = fail("power-up", rc);
    } else if (!strcmp(cmd, "power-down")) {
        rc = sg_power_step(&dev, false); if (rc) ret = fail("power-down", rc);
    } else if (!strcmp(cmd, "fov")) {
        if (!arg) { fprintf(stderr, "sgctl: fov needs degrees\n"); ret = 2; }
        else {
            double deg = strtod(arg, NULL);
            rc = sg_set_fov_deg(&dev, deg);
            if (rc) ret = fail("fov", rc);
            else printf("fov %.2f deg -> position %u\n", deg, sg_angle_to_position(deg));
        }
    } else if (!strcmp(cmd, "zoom-pos")) {
        if (!arg) { fprintf(stderr, "sgctl: zoom-pos needs a value\n"); ret = 2; }
        else {
            long p = strtol(arg, NULL, 0);
            rc = sg_zoom_to_position(&dev, (uint16_t)p);
            if (rc) ret = fail("zoom-pos", rc);
            else printf("position %ld -> %.2f deg\n", p, sg_position_to_angle((uint16_t)p));
        }
    } else if (!strcmp(cmd, "tele") || !strcmp(cmd, "wide")) {
        long steps = arg ? strtol(arg, NULL, 0) : -1;
        if (steps < 0 || steps > 255) { fprintf(stderr, "sgctl: %s needs 0..255 steps\n", cmd); ret = 2; }
        else { rc = sg_zoom_step(&dev, !strcmp(cmd, "tele"), (uint8_t)steps); if (rc) ret = fail(cmd, rc); }
    } else if (!strcmp(cmd, "reset")) {
        rc = sg_zoom_reset(&dev); if (rc) ret = fail("reset", rc);
    } else if (!strcmp(cmd, "status")) {
        int on = -1, lvl = -1, fan = -1; uint16_t pos = 0;
        if ((rc = sg_query_power(&dev, &on)))      { ret = fail("query power", rc); goto done; }
        if ((rc = sg_query_current(&dev, &lvl)))   { ret = fail("query current", rc); goto done; }
        if ((rc = sg_query_position(&dev, &pos)))  { ret = fail("query position", rc); goto done; }
        if ((rc = sg_query_fan(&dev, &fan)))       { ret = fail("query fan", rc); goto done; }
        printf("laser   : %s\n", on ? "ON" : "off");
        printf("level   : %d / 255\n", lvl);
        printf("position: %u  (%.2f deg)\n", pos, sg_position_to_angle(pos));
        printf("fan     : %s\n", fan ? "ON" : "off");
    } else {
        fprintf(stderr, "sgctl: unknown command '%s'\n", cmd);
        usage(stderr);
        ret = 2;
    }

done:
    sg_close(&dev);
    return ret;
}
