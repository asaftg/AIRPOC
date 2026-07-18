/* airpoc_tap.h — shared-memory tap: lossless-drainable, never-blocking frame ring.
 *
 * One publisher process, any number of reader processes. The publisher owns a
 * named POSIX shm ring of fixed-size slots and overwrites the oldest slot,
 * never waiting on readers. Readers drain sequentially and detect laps (gaps).
 * Protocol v1 — spec and producer checklist in recorder/docs/TAP.md.
 *
 * Publisher:  AirTap t;
 *             tap_create(&t, "airpoc.eo_y10", 16, slot_bytes, meta_json);
 *             uint8_t *p = tap_slot_begin(&t);      // write payload in place
 *             tap_slot_commit(&t, len, t_src_ns, meta, 0);
 *             ...or tap_write(&t, buf, len, t_src_ns, meta) to copy+commit.
 * Reader:     AirTapSub s;  tap_open(&s, "airpoc.eo_y10");
 *             tap_read(&s, dst, cap, &rec)  -> 1 record copied, 0 none new,
 *                                              -1 publisher gone.
 *
 * Every call is safe to no-op: tap_create() failure leaves the handle unusable
 * and all publish calls become no-ops (log once, degrade gracefully).
 */
#ifndef AIRPOC_TAP_H
#define AIRPOC_TAP_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define TAP_MAGIC       0x3130504154524941ULL   /* "AIRTAP01" LE */
#define TAP_VERSION     1
#define TAP_HDR_BYTES   4096
#define TAP_META_WORDS  6
#define TAP_NAME_MAX    64

#define TAP_FLAG_TRUNCATED 0x1u

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t hdr_bytes;
    uint32_t slot_bytes;        /* payload capacity per slot                */
    uint32_t n_slots;
    uint64_t wseq;              /* atomic: next sequence to be written      */
    char     meta_json[TAP_HDR_BYTES - 32];  /* channel self-description    */
} AirTapHdr;

typedef struct {
    uint64_t seq_begin;         /* atomic: seq being/last written to slot   */
    uint64_t seq_end;           /* atomic: seq whose payload is complete    */
    uint64_t t_src_ns;          /* source timestamp, CLOCK_MONOTONIC        */
    uint64_t t_pub_ns;          /* publish timestamp, CLOCK_MONOTONIC       */
    uint32_t payload_len;
    uint32_t flags;
    uint32_t meta[TAP_META_WORDS];
} AirTapSlotHdr;                /* 64 bytes */

typedef struct {
    AirTapHdr *h;
    uint8_t   *base;            /* first slot                               */
    size_t     stride;          /* slot header + payload, 64-aligned        */
    size_t     map_len;
    char       name[TAP_NAME_MAX];
    int        ok;
} AirTap;

typedef struct {
    AirTap    t;                /* same mapping, read-only view             */
    uint64_t  next_seq;         /* next record we want                      */
    uint64_t  gaps;             /* records lost to lapping (cumulative)     */
    uint64_t  shm_dev, shm_ino; /* identity of the mapped shm (see tap_stale) */
} AirTapSub;

typedef struct {
    uint64_t seq, t_src_ns, t_pub_ns;
    uint32_t payload_len, flags;
    uint32_t meta[TAP_META_WORDS];
    uint64_t gap_before;        /* records lost immediately before this one */
} AirTapRec;

static inline uint64_t tap_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline size_t tap__stride(uint32_t slot_bytes)
{
    size_t s = sizeof(AirTapSlotHdr) + slot_bytes;
    return (s + 63) & ~(size_t)63;
}

static inline AirTapSlotHdr *tap__slot(const AirTap *t, uint64_t seq)
{
    return (AirTapSlotHdr *)(t->base + (size_t)(seq % t->h->n_slots) * t->stride);
}

/* ---- publisher ---- */

static inline int tap_create(AirTap *t, const char *name, uint32_t n_slots,
                             uint32_t slot_bytes, const char *meta_json)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "/%s", name);
    size_t stride = tap__stride(slot_bytes);
    size_t len = TAP_HDR_BYTES + stride * n_slots;

    int fd = shm_open(t->name, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { fprintf(stderr, "tap: shm_open(%s) failed, publishing disabled\n", t->name); return -1; }
    if (ftruncate(fd, (off_t)len) != 0) { close(fd); return -1; }
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { fprintf(stderr, "tap: mmap(%s) failed, publishing disabled\n", t->name); return -1; }

    t->h = (AirTapHdr *)m;
    t->base = (uint8_t *)m + TAP_HDR_BYTES;
    t->stride = stride;
    t->map_len = len;

    /* (Re)initialize: readers key on magic last so they never see a half header. */
    __atomic_store_n(&t->h->magic, 0, __ATOMIC_RELAXED);
    memset((uint8_t *)m + 8, 0, TAP_HDR_BYTES - 8);
    memset(t->base, 0, stride * n_slots);
    t->h->version = TAP_VERSION;
    t->h->hdr_bytes = TAP_HDR_BYTES;
    t->h->slot_bytes = slot_bytes;
    t->h->n_slots = n_slots;
    if (meta_json) snprintf(t->h->meta_json, sizeof t->h->meta_json, "%s", meta_json);
    /* slots start at seq_begin=seq_end=0 but wseq=0 means "nothing published" */
    __atomic_store_n(&t->h->wseq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&t->h->magic, TAP_MAGIC, __ATOMIC_RELEASE);
    t->ok = 1;
    return 0;
}

/* Payload pointer for the next record; write into it, then tap_slot_commit(). */
static inline void *tap_slot_begin(AirTap *t)
{
    if (!t->ok) return NULL;
    uint64_t s = __atomic_load_n(&t->h->wseq, __ATOMIC_RELAXED);
    AirTapSlotHdr *sl = tap__slot(t, s);
    /* mark slot dirty BEFORE any payload byte lands (full fence: StoreStore) */
    __atomic_store_n(&sl->seq_begin, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return (uint8_t *)sl + sizeof(AirTapSlotHdr);
}

static inline void tap_slot_commit(AirTap *t, uint32_t payload_len,
                                   uint64_t t_src_ns, const uint32_t meta[TAP_META_WORDS],
                                   uint32_t flags)
{
    if (!t->ok) return;
    uint64_t s = __atomic_load_n(&t->h->wseq, __ATOMIC_RELAXED);
    AirTapSlotHdr *sl = tap__slot(t, s);
    if (payload_len > t->h->slot_bytes) { payload_len = t->h->slot_bytes; flags |= TAP_FLAG_TRUNCATED; }
    sl->t_src_ns = t_src_ns;
    sl->t_pub_ns = tap_now_ns();
    sl->payload_len = payload_len;
    sl->flags = flags;
    if (meta) memcpy(sl->meta, meta, sizeof sl->meta); else memset(sl->meta, 0, sizeof sl->meta);
    __atomic_store_n(&sl->seq_end, s + 1, __ATOMIC_RELEASE);   /* payload before this */
    __atomic_store_n(&t->h->wseq, s + 1, __ATOMIC_RELEASE);
}

static inline int tap_write(AirTap *t, const void *buf, uint32_t len,
                            uint64_t t_src_ns, const uint32_t meta[TAP_META_WORDS])
{
    void *p = tap_slot_begin(t);
    if (!p) return -1;
    uint32_t n = len > t->h->slot_bytes ? t->h->slot_bytes : len;
    memcpy(p, buf, n);
    tap_slot_commit(t, len, t_src_ns, meta, 0);
    return 0;
}

static inline void tap_destroy(AirTap *t)
{
    if (t->h) { munmap((void *)t->h, t->map_len); shm_unlink(t->name); }
    memset(t, 0, sizeof *t);
}

/* ---- reader ---- */

static inline int tap_open(AirTapSub *s, const char *name)
{
    memset(s, 0, sizeof *s);
    snprintf(s->t.name, sizeof s->t.name, "/%s", name);
    int fd = shm_open(s->t.name, O_RDONLY, 0);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < TAP_HDR_BYTES) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    AirTapHdr *h = (AirTapHdr *)m;
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != TAP_MAGIC ||
        h->version != TAP_VERSION ||
        (size_t)st.st_size < TAP_HDR_BYTES + tap__stride(h->slot_bytes) * h->n_slots) {
        munmap(m, (size_t)st.st_size); return -1;
    }
    s->t.h = h;
    s->t.base = (uint8_t *)m + TAP_HDR_BYTES;
    s->t.stride = tap__stride(h->slot_bytes);
    s->t.map_len = (size_t)st.st_size;
    s->t.ok = 1;
    s->shm_dev = (uint64_t)st.st_dev;      /* identity, for tap_stale() */
    s->shm_ino = (uint64_t)st.st_ino;
    /* start at the freshest data, not the historical backlog */
    s->next_seq = __atomic_load_n(&h->wseq, __ATOMIC_ACQUIRE);
    return 0;
}

/* Has the publisher REPLACED the shm behind our mapping (unlink + recreate on a
 * producer restart, or an external cleanup)? Our mapping stays valid but is
 * orphaned — frozen forever — so a reader that doesn't check this reads a dead
 * ring indefinitely while still looking "connected". Returns 1 = stale/gone.
 * Cheap: one shm_open + fstat, meant to be polled ~1 Hz while idle. */
static inline int tap_stale(const AirTapSub *s)
{
    if (!s->t.ok) return 1;
    int fd = shm_open(s->t.name, O_RDONLY, 0);
    if (fd < 0) return 1;                                  /* unlinked: publisher gone */
    struct stat st;
    int bad = (fstat(fd, &st) != 0) ||
              (uint64_t)st.st_ino != s->shm_ino ||
              (uint64_t)st.st_dev != s->shm_dev;           /* recreated: different inode */
    close(fd);
    return bad;
}

/* Position of the publisher's write cursor (for idle tracking without copying). */
static inline uint64_t tap_wseq(const AirTapSub *s)
{
    return s->t.ok ? __atomic_load_n(&s->t.h->wseq, __ATOMIC_ACQUIRE) : 0;
}

/* Copy the next record into buf. Returns 1 = copied, 0 = nothing new,
 * -1 = not attached (caller should re-open; see tap_stale).
 * Lap losses are counted in s->gaps and reported in rec->gap_before.
 * NOTE: callers must test > 0, not truthiness. */
static inline int tap_read(AirTapSub *s, void *buf, uint32_t buf_cap, AirTapRec *rec)
{
    if (!s->t.ok) return -1;
    const AirTapHdr *h = s->t.h;
    uint64_t gap = 0;

    for (;;) {
        uint64_t w = __atomic_load_n(&h->wseq, __ATOMIC_ACQUIRE);
        if (s->next_seq >= w) return 0;                     /* fully drained */

        /* if the publisher lapped us, jump to the oldest slot that is safe:
         * keep one slot of margin so the slot being written now is excluded */
        if (w - s->next_seq > (uint64_t)h->n_slots - 2) {
            uint64_t oldest = w - (h->n_slots - 2);
            gap += oldest - s->next_seq;
            s->next_seq = oldest;
        }

        uint64_t want = s->next_seq + 1;                    /* slot seq marks = seq+1 */
        const AirTapSlotHdr *sl = tap__slot(&s->t, s->next_seq);

        if (__atomic_load_n(&sl->seq_end, __ATOMIC_ACQUIRE) != want) {
            /* writer mid-flight on this very slot (only possible right after a
             * lap-jump); step past it */
            gap += 1; s->next_seq += 1; continue;
        }

        AirTapRec r;
        r.seq = s->next_seq;
        r.t_src_ns = sl->t_src_ns;
        r.t_pub_ns = sl->t_pub_ns;
        r.payload_len = sl->payload_len;
        r.flags = sl->flags;
        memcpy(r.meta, sl->meta, sizeof r.meta);
        uint32_t n = r.payload_len > buf_cap ? buf_cap : r.payload_len;
        memcpy(buf, (const uint8_t *)sl + sizeof(AirTapSlotHdr), n);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);            /* copies before check */
        if (__atomic_load_n(&sl->seq_begin, __ATOMIC_RELAXED) != want) {
            gap += 1; s->next_seq += 1; continue;           /* torn: overwritten under us */
        }

        s->next_seq += 1;
        s->gaps += gap;
        r.gap_before = gap;
        if (rec) *rec = r;
        return 1;
    }
}

static inline void tap_close(AirTapSub *s)
{
    if (s->t.h) munmap((void *)s->t.h, s->t.map_len);
    memset(s, 0, sizeof *s);
}

#endif /* AIRPOC_TAP_H */
