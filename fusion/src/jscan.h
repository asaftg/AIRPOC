/* jscan.h - tolerant JSON scanning helpers, shared by the two wire parsers.
 * Same approach as the eotrack det_feed scanner: our own producers, unique
 * keys per object slice, so simple substring search is safe. */
#ifndef FUS_JSCAN_H
#define FUS_JSCAN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline const char *js_val(const char *s, const char *end, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = s; p + kl + 2 < end; p++) {
        if (p[0] == '"' && !strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
            const char *q = p + 1 + kl + 1;
            while (q < end && *q == ' ') q++;
            if (q >= end || *q != ':') continue;   /* a VALUE equal to a key
                                                    * name (e.g. "state":"conf")
                                                    * must not match */
            q++;
            while (q < end && *q == ' ') q++;
            return q;
        }
    }
    return NULL;
}

static inline double js_num(const char *s, const char *end, const char *key, double dflt)
{
    const char *v = js_val(s, end, key);
    if (!v || v >= end) return dflt;
    return atof(v);
}

static inline uint64_t js_u64(const char *s, const char *end, const char *key)
{
    const char *v = js_val(s, end, key);
    if (!v || v >= end) return 0;
    return strtoull(v, NULL, 10);
}

static inline int js_bool(const char *s, const char *end, const char *key, int dflt)
{
    const char *v = js_val(s, end, key);
    if (!v || v >= end) return dflt;
    return *v == 't';
}

static inline int js_arr(const char *s, const char *end, const char *key,
                         double *o, int n)
{
    const char *v = js_val(s, end, key);
    if (!v || v >= end || *v != '[') return 0;
    v++;
    for (int i = 0; i < n; i++) {
        while (v < end && (*v == ' ' || *v == ',')) v++;
        if (v >= end) return 0;
        o[i] = atof(v);
        while (v < end && *v != ',' && *v != ']') v++;
    }
    return 1;
}

/* Find the [start,end) of the array value of `key` ("\"targets\"" form),
 * tracking nested brackets. Returns 0 if absent. */
static inline int js_array_bounds(const char *json, const char *key,
                                  const char **a_out, const char **end_out)
{
    const char *a = strstr(json, key);
    if (!a) return 0;
    a = strchr(a, '[');
    if (!a) return 0;
    const char *p = a + 1; int depth = 1; const char *end = NULL;
    for (; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) { end = p; break; } }
    }
    if (!end) end = a + strlen(a);
    *a_out = a; *end_out = end;
    return 1;
}

/* Iterate objects inside [a,end): on each call returns the next object slice
 * [o,oe) or 0 when exhausted. *cursor starts at a+1. */
static inline int js_next_obj(const char **cursor, const char *end,
                              const char **o_out, const char **oe_out)
{
    const char *o = *cursor;
    while (o < end && *o != '{') o++;
    if (o >= end) return 0;
    const char *oe = o + 1; int od = 1;
    for (; oe < end; oe++) {
        if (*oe == '{') od++;
        else if (*oe == '}') { if (--od == 0) break; }
    }
    if (oe >= end) return 0;
    *o_out = o; *oe_out = oe;
    *cursor = oe + 1;
    return 1;
}

#endif
