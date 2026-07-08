/* channel.c — per-channel data movement: tap -> AIREC segments on NVMe.
 *
 * Heavy channels (eo_y10, eo_jpeg): drain thread frames records straight into
 * 4 MiB O_DIRECT-aligned chunks (eo_y10 is packed 16->10 bit in the same pass,
 * read shm / write chunk — the single mandatory memory sweep), a lock-free
 * SPSC ring hands full chunks to a writer thread doing O_DIRECT pwrite into
 * fallocate'd 256 MiB segments. Chunks flush at 4096 boundaries via PAD
 * pseudo-records. If the NVMe stalls until the ring fills, records are DROPPED
 * and counted — capture is never back-pressured.
 *
 * Small channels (radar_raw, radar_wire, events): mutexed buffered writes +
 * shared 1 s fdatasync from the housekeeping thread. radar taps are drained by
 * threads that feed the same small path; events come from chan_submit().
 */
#define _GNU_SOURCE
#include "recorder.h"
#include "eo_tonemap.h"    /* EO_TONEMAP_VERSION/hash: stamp it so a future ISP change is detectable */
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

const ChanCfg g_chan_cfg[CH_N] = {
    { "eo_y10",     "airpoc.eo_y10",     "y10p", "v4l2_sequence,exp_lines,gain,vmax,mean10_x100,drops_cum", 1, 3200 * 1024 },
    { "eo_jpeg",    "airpoc.eo_jpeg",    "jpeg", "eo_seq,dw,dh,zoom,res_idx,0",                             1, 1024 * 1024 },
    { "radar_raw",  "airpoc.radar_raw",  "tlv-bytes", "read_len,0,0,0,0,0",                                 0,   64 * 1024 },
    { "radar_wire", "airpoc.radar_wire", "json", "frame_number,n_points,n_targets,0,0,0",                   0,  256 * 1024 },
    { "det_wire",   "airpoc.det_wire",   "json", "n_detections,0,0,0,0,0",                                  0,  256 * 1024 },
    { "events",     NULL,                "json", "0,0,0,0,0,0",                                             0,   64 * 1024 },
};

Chan g_chan[CH_N];

/* phase: drain/writer coordination, __atomic accessed */
enum { PH_IDLE, PH_ACTIVE, PH_CLOSING, PH_FINALIZE };
static int g_phase[CH_N];

static inline int phase(ChanId id) { return __atomic_load_n(&g_phase[id], __ATOMIC_ACQUIRE); }
static inline void set_phase(ChanId id, int p) { __atomic_store_n(&g_phase[id], p, __ATOMIC_RELEASE); }

int chan_recording(void)
{
    return phase(CH_EVENTS) == PH_ACTIVE;
}

static void msleep_short(void) { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); }

/* ---- heavy path: chunk framing (drain thread only) ---- */

static Chunk *get_fill(Chan *c)
{
    if (c->fill) return c->fill;
    unsigned head = __atomic_load_n(&c->q_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&c->q_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= QUEUE_CHUNKS) return NULL;            /* NVMe stalled */
    c->fill = &c->chunks[head % QUEUE_CHUNKS];
    c->fill->used = 0;
    c->fill->seg_no = c->seg_no;
    c->fill->seg_off = c->seg_used;
    return c->fill;
}

static void push_fill(Chan *c)
{
    if (!c->fill || c->fill->used == 0) return;
    c->seg_used += c->fill->used;
    c->fill = NULL;
    __atomic_fetch_add(&c->q_head, 1, __ATOMIC_RELEASE);
}

/* pad current chunk to a 4096 boundary with a PAD pseudo-record and push it */
static void flush_chunk(Chan *c)
{
    Chunk *k = c->fill;
    if (!k || k->used == 0) return;
    uint32_t r = (4096u - (k->used & 4095u)) & 4095u;
    if (r) {
        if (r < sizeof(AirecRecHdr)) r += 4096;
        AirecRecHdr *h = (AirecRecHdr *)(k->buf + k->used);
        memset(h, 0, sizeof *h);
        h->magic = AIREC_REC_MAGIC;
        h->flags = RF_PAD;
        h->payload_len = r - sizeof(AirecRecHdr);
        memset(k->buf + k->used + sizeof(AirecRecHdr), 0, h->payload_len);
        k->used += r;
    }
    push_fill(c);
}

static void frame_seg_hdr(Chan *c, uint64_t t0)
{
    Chunk *k = get_fill(c);
    if (!k) return;
    AirecSegHdr *sh = (AirecSegHdr *)(k->buf + k->used);
    memset(sh, 0, sizeof *sh);
    sh->magic = AIREC_SEG_MAGIC;
    sh->version = AIREC_VERSION;
    sh->channel_id = (uint32_t)c->id;
    sh->session_t0_mono_ns = t0;
    sh->segment_no = c->seg_no;
    k->used += sizeof *sh;
}

/* Reserve space for one record in the fill chunk; returns hdr ptr or NULL(drop).
 * Rotation and chunk switching happen here. Payload area has >=8B slack for
 * the pack10 overlapping store. */
static AirecRecHdr *frame_begin(Chan *c, uint32_t payload_cap, uint64_t t0)
{
    uint32_t padded = (payload_cap + 7u) & ~7u;
    uint32_t need = sizeof(AirecRecHdr) + padded + 4224;     /* + pad reserve + slack */

    if (c->seg_used + (c->fill ? c->fill->used : 0) + need > SEG_BYTES) {
        flush_chunk(c);                                      /* rotate segment */
        c->seg_no++;
        c->seg_used = 0;
        frame_seg_hdr(c, t0);
    }
    Chunk *k = get_fill(c);
    if (!k) return NULL;
    if (k->used + need > CHUNK_BYTES) {
        flush_chunk(c);
        k = get_fill(c);
        if (!k) return NULL;
    }
    if (c->seg_used == 0 && k->used == 0) frame_seg_hdr(c, t0);  /* fresh seg via rotation path */
    return (AirecRecHdr *)(k->buf + k->used);
}

static void frame_commit(Chan *c, AirecRecHdr *h, uint32_t payload_len,
                         uint64_t seq, uint64_t t_src, uint64_t t_pub,
                         const uint32_t meta[6], uint32_t flags)
{
    Chunk *k = c->fill;
    uint8_t *payload = (uint8_t *)h + sizeof *h;
    h->magic = AIREC_REC_MAGIC;
    h->crc32c = crc32c(0, payload, payload_len);
    h->seq = seq;
    h->t_src_ns = t_src;
    h->t_pub_ns = t_pub;
    h->payload_len = payload_len;
    h->flags = flags;
    memcpy(h->meta, meta, sizeof h->meta);

    /* index timeline = t_pub (recorder's CLOCK_MONOTONIC, common to every
     * channel), NOT t_src: eo_y10's t_src is the camera's V4L2 clock, offset by
     * tens of seconds, so a source-clock timeline wouldn't align across channels.
     * The true source timestamp stays in the record header for algorithm use. */
    AirecIdxRow row = { seq, t_pub, k->seg_no,
                        (uint32_t)(k->seg_off + k->used), payload_len, flags };
    fwrite(&row, sizeof row, 1, c->idx_f);

    uint32_t padded = (payload_len + 7u) & ~7u;
    if (padded > payload_len) memset(payload + payload_len, 0, padded - payload_len);
    k->used += sizeof *h + padded;
    c->records++;
    c->bytes += sizeof *h + padded;
    c->last_rec_ns = now_ns();           /* loss watchdog */
}

/* ---- heavy drain thread ---- */

static void *drain_heavy(void *arg)
{
    Chan *c = arg;
    uint64_t last_partial = 0;

    for (;;) {
        int ph = phase(c->id);
        if (ph == PH_IDLE || ph == PH_FINALIZE) {
            if (c->sub_ok) c->sub.next_seq = tap_wseq(&c->sub);   /* track, no copy */
            else if (tap_open(&c->sub, c->cfg->tap) == 0) c->sub_ok = 1;
            msleep_short();
            continue;
        }
        if (ph == PH_CLOSING) {
            flush_chunk(c);
            set_phase(c->id, PH_FINALIZE);
            continue;
        }
        /* ACTIVE */
        if (!c->sub_ok && tap_open(&c->sub, c->cfg->tap) == 0) c->sub_ok = 1;
        int got = 0;
        for (int burst = 0; c->sub_ok && burst < 8; burst++) {
            /* zero-copy peek: transform straight from the shm slot */
            uint64_t w = tap_wseq(&c->sub);
            if (c->sub.next_seq >= w) break;
            uint64_t gap = 0;
            const AirTapHdr *th = c->sub.t.h;
            if (w - c->sub.next_seq > (uint64_t)th->n_slots - 2) {
                uint64_t oldest = w - (th->n_slots - 2);
                gap = oldest - c->sub.next_seq;
                c->sub.next_seq = oldest;
            }
            uint64_t want = c->sub.next_seq + 1;
            const AirTapSlotHdr *sl = tap__slot(&c->sub.t, c->sub.next_seq);
            if (__atomic_load_n(&sl->seq_end, __ATOMIC_ACQUIRE) != want) {
                c->sub.next_seq++; c->drops_ring++; continue;
            }
            uint32_t plen = sl->payload_len, flags = 0;
            uint32_t meta[6]; memcpy(meta, sl->meta, sizeof meta);
            uint64_t t_src = sl->t_src_ns, t_pub = sl->t_pub_ns, seq = c->sub.next_seq;
            const uint8_t *src = (const uint8_t *)sl + sizeof(AirTapSlotHdr);

            /* decimation (eo_y10 only) */
            int keep = c->id == CH_EO_Y10 ? __atomic_load_n(&g_rec.keep, __ATOMIC_RELAXED) : 1;
            if (keep > 1 && (seq % (uint64_t)keep)) { c->sub.next_seq++; continue; }

            uint32_t out_cap = plen;
            VideoMode mode = MODE_RAW16;
            if (c->id == CH_EO_Y10) {
                mode = (VideoMode)__atomic_load_n((int *)&g_rec.mode, __ATOMIC_RELAXED);
                if (mode == MODE_Y10P) out_cap = plen / 16 * 10 + 8;
                else if (mode == MODE_Y8) out_cap = plen / 2;
            }
            AirecRecHdr *h = frame_begin(c, out_cap, g_rec.t0_mono_ns);
            if (!h) { c->sub.next_seq++; c->drops_queue++; continue; }
            uint8_t *dst = (uint8_t *)h + sizeof *h;
            uint32_t olen;
            if (c->id == CH_EO_Y10 && mode == MODE_Y10P) olen = pack10(src, plen / 2, dst);
            else if (c->id == CH_EO_Y10 && mode == MODE_Y8) olen = pack_y8(src, plen / 2, dst);
            else { memcpy(dst, src, plen); olen = plen; }

            /* validate the slot was not overwritten while we read it */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&sl->seq_begin, __ATOMIC_RELAXED) != want) {
                c->sub.next_seq++; c->drops_ring++; continue;   /* discard reservation */
            }
            c->sub.next_seq++;
            c->sub.gaps += gap;
            if (gap) { flags |= RF_GAP_BEFORE; c->drops_ring += gap; }
            if (sl->flags & TAP_FLAG_TRUNCATED) flags |= RF_TRUNCATED;
            frame_commit(c, h, olen, seq, t_src, t_pub, meta, flags);
            got = 1;
        }
        uint64_t t = now_ns();
        if (!got) {
            if (c->fill && c->fill->used && t - last_partial > FSYNC_NS) {
                flush_chunk(c);                              /* latency bound */
                last_partial = t;
            }
            msleep_short();
        } else last_partial = t;
    }
    return NULL;
}

/* ---- heavy writer thread ---- */

static int seg_open(Chan *c, const char *dir, uint32_t seg_no)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s/data.%05u.airec", dir, c->cfg->name, seg_no);
    int fd = open(path, O_WRONLY | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) { fprintf(stderr, "rec: open %s: %s\n", path, strerror(errno)); return -1; }
    if (posix_fallocate(fd, 0, SEG_BYTES) != 0) { /* non-fatal: extents help, absence works */ }
    return fd;
}

static void seg_close(Chan *c)
{
    if (c->wr_seg_fd < 0) return;
    fdatasync(c->wr_seg_fd);
    if (ftruncate(c->wr_seg_fd, (off_t)c->wr_seg_used) != 0) {}
    close(c->wr_seg_fd);
    c->wr_seg_fd = -1;
}

static void *writer_heavy(void *arg)
{
    Chan *c = arg;
    uint64_t last_sync = 0;

    for (;;) {
        unsigned tail = __atomic_load_n(&c->q_tail, __ATOMIC_RELAXED);
        unsigned head = __atomic_load_n(&c->q_head, __ATOMIC_ACQUIRE);
        if (tail == head) {
            if (phase(c->id) == PH_FINALIZE) {
                seg_close(c);
                if (c->idx_f) { fflush(c->idx_f); fdatasync(fileno(c->idx_f)); fclose(c->idx_f); c->idx_f = NULL; }
                set_phase(c->id, PH_IDLE);
            }
            msleep_short();
            continue;
        }
        Chunk *k = &c->chunks[tail % QUEUE_CHUNKS];
        if (c->wr_seg_fd < 0 || k->seg_no != c->wr_seg_no) {
            seg_close(c);
            c->wr_seg_fd = seg_open(c, g_rec.dir, k->seg_no);
            c->wr_seg_no = k->seg_no;
            c->wr_seg_used = 0;
        }
        if (c->wr_seg_fd >= 0) {
            ssize_t n = pwrite(c->wr_seg_fd, k->buf, k->used, (off_t)k->seg_off);
            if (n != (ssize_t)k->used)
                fprintf(stderr, "rec: %s pwrite %u@%llu: %s\n", c->cfg->name, k->used,
                        (unsigned long long)k->seg_off, strerror(errno));
            c->wr_seg_used = k->seg_off + k->used;
            c->unsynced += k->used;
        }
        __atomic_store_n(&c->q_tail, tail + 1, __ATOMIC_RELEASE);

        uint64_t t = now_ns();
        if (c->unsynced >= FSYNC_BYTES || t - last_sync > FSYNC_NS) {
            if (c->wr_seg_fd >= 0) fdatasync(c->wr_seg_fd);
            if (c->idx_f) { fflush(c->idx_f); fdatasync(fileno(c->idx_f)); }
            c->unsynced = 0;
            last_sync = t;
        }
    }
    return NULL;
}

/* ---- small path ---- */

void chan_submit(ChanId id, const void *payload, uint32_t len,
                 uint64_t t_src_ns, const uint32_t meta[6])
{
    Chan *c = &g_chan[id];
    if (phase(id) != PH_ACTIVE) return;
    static const uint32_t zmeta[6];
    if (!meta) meta = zmeta;

    pthread_mutex_lock(&c->small_lk);
    /* small channels use a single segment; keep u32 index offsets valid */
    if (c->small_fd >= 0 && c->seg_used < 0xF0000000ull) {
        AirecRecHdr h = { AIREC_REC_MAGIC, crc32c(0, payload, len),
                          c->records, t_src_ns, now_ns(), len, 0,
                          { meta[0], meta[1], meta[2], meta[3], meta[4], meta[5] } };
        uint32_t padded = (len + 7u) & ~7u;
        static const uint8_t zpad[8];
        AirecIdxRow row = { h.seq, h.t_pub_ns, 0, (uint32_t)c->seg_used, len, 0 };  /* timeline = t_pub */
        if (write(c->small_fd, &h, sizeof h) == sizeof h &&
            write(c->small_fd, payload, len) == (ssize_t)len &&
            (padded == len || write(c->small_fd, zpad, padded - len) == (ssize_t)(padded - len))) {
            fwrite(&row, sizeof row, 1, c->idx_f);
            c->seg_used += sizeof h + padded;
            c->records++;
            c->bytes += sizeof h + padded;
            c->last_rec_ns = now_ns();       /* loss watchdog */
        }
    }
    pthread_mutex_unlock(&c->small_lk);
}

/* small tap channels: drain thread feeding chan_submit */
static void *drain_small(void *arg)
{
    Chan *c = arg;
    uint8_t *buf = malloc(c->cfg->max_rec);
    AirTapRec r;

    for (;;) {
        if (!c->sub_ok) {
            if (tap_open(&c->sub, c->cfg->tap) == 0) c->sub_ok = 1;
            else { msleep_short(); continue; }
        }
        if (phase(c->id) != PH_ACTIVE) {
            c->sub.next_seq = tap_wseq(&c->sub);
            msleep_short();
            continue;
        }
        int got = 0;
        while (tap_read(&c->sub, buf, c->cfg->max_rec, &r)) {
            if (r.gap_before) c->drops_ring += r.gap_before;
            chan_submit(c->id, buf, r.payload_len > c->cfg->max_rec ? c->cfg->max_rec : r.payload_len,
                        r.t_src_ns, r.meta);
            got = 1;
        }
        if (!got) msleep_short();
    }
    return NULL;
}

/* housekeeping: 1 s fdatasync of small channels + throughput EMA */
static void *housekeeper(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        for (int i = 0; i < CH_N; i++) {
            Chan *c = &g_chan[i];
            if (!c->cfg->heavy && phase((ChanId)i) != PH_IDLE) {
                pthread_mutex_lock(&c->small_lk);
                if (c->small_fd >= 0) fdatasync(c->small_fd);
                if (c->idx_f) { fflush(c->idx_f); fdatasync(fileno(c->idx_f)); }
                pthread_mutex_unlock(&c->small_lk);
            }
            uint64_t t = now_ns(), b = c->bytes;
            if (c->ema_t_ns) {
                double dt = (t - c->ema_t_ns) / 1e9;
                double inst = (b - c->ema_bytes) / 1e6 / (dt > 0 ? dt : 1);
                c->mb_s = c->mb_s ? 0.7 * c->mb_s + 0.3 * inst : inst;
            }
            c->ema_bytes = b; c->ema_t_ns = t;

            /* loss watchdog: a tap-fed channel that WAS producing but has gone
             * silent >2 s mid-recording means the live feed died under us (e.g.
             * the producer's shm tap vanished). Never let that be silent. */
            if (c->cfg->tap && chan_recording() && c->records > 0 && c->last_rec_ns) {
                if (!c->lost && t - c->last_rec_ns > 2000000000ull) {
                    c->lost = 1;
                    char ev[192];
                    int n = snprintf(ev, sizeof ev,
                        "{\"type\":\"channel_lost\",\"channel\":\"%s\",\"t_mono_ns\":%llu,"
                        "\"last_frame_mono_ns\":%llu}", c->cfg->name,
                        (unsigned long long)t, (unsigned long long)c->last_rec_ns);
                    chan_submit(CH_EVENTS, ev, (uint32_t)n, t, NULL);
                    fprintf(stderr, "rec: FEED LOST mid-recording: %s (no frames for %.1fs)\n",
                            c->cfg->name, (t - c->last_rec_ns) / 1e9);
                } else if (c->lost && t - c->last_rec_ns < 1000000000ull) {
                    c->lost = 0;
                    char ev[160];
                    int n = snprintf(ev, sizeof ev,
                        "{\"type\":\"channel_resumed\",\"channel\":\"%s\",\"t_mono_ns\":%llu}",
                        c->cfg->name, (unsigned long long)t);
                    chan_submit(CH_EVENTS, ev, (uint32_t)n, t, NULL);
                    fprintf(stderr, "rec: feed resumed: %s\n", c->cfg->name);
                }
            }
        }
    }
    return NULL;
}

/* ---- lifecycle ---- */

static void write_channel_json(Chan *c, const char *dir)
{
    char path[640], body[640];
    const char *enc = c->cfg->encoding;
    const char *meta_desc = c->cfg->meta_desc;
    char geom[96] = "";
    if (c->id == CH_EO_Y10) {
        VideoMode m = g_rec.mode;
        enc = m == MODE_Y10P ? "y10p" : m == MODE_Y8 ? "y8" : "y16le";
        /* native geometry + illuminator flag from the EO tap's meta_json */
        int w = 1440, h = 1088, illum = 0;
        if (c->sub_ok && c->sub.t.h) {
            char tmp[24];
            if (store_manifest_field(c->sub.t.h->meta_json, "w", tmp, sizeof tmp) == 0 && atoi(tmp) > 0) w = atoi(tmp);
            if (store_manifest_field(c->sub.t.h->meta_json, "h", tmp, sizeof tmp) == 0 && atoi(tmp) > 0) h = atoi(tmp);
            if (store_manifest_field(c->sub.t.h->meta_json, "illum", tmp, sizeof tmp) == 0 && atoi(tmp) == 1) illum = 1;
        }
        /* when the EO tap provides per-frame illuminator, meta[4] carries it
         * (packed) instead of mean10_x100. tonemap_hash flags tone-map drift. */
        if (illum) meta_desc = "v4l2_sequence,exp_lines,gain,vmax,illum_packed,drops_cum";
        snprintf(geom, sizeof geom,
                 ",\"w\":%d,\"h\":%d,\"tonemap_version\":%d,\"tonemap_hash\":%u,\"illum\":%d",
                 w, h, EO_TONEMAP_VERSION, eo_tonemap_hash(), illum);
    }
    snprintf(path, sizeof path, "%s/%s/channel.json", dir, c->cfg->name);
    int n = snprintf(body, sizeof body,
        "{\"name\":\"%s\",\"encoding\":\"%s\"%s,\"meta\":[%s],\"source\":\"%s%s\",\"airec\":%d}\n",
        c->cfg->name, enc, geom, meta_desc,
        c->cfg->tap ? "shm:" : "internal", c->cfg->tap ? c->cfg->tap : "", AIREC_VERSION);
    store_write_file_atomic(path, body, (size_t)n);
}

int chan_session_open(Chan *c, const char *dir, uint64_t t0_mono_ns)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s", dir, c->cfg->name);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    write_channel_json(c, dir);

    snprintf(path, sizeof path, "%s/%s/index.bin", dir, c->cfg->name);
    c->idx_f = fopen(path, "wb");
    if (!c->idx_f) return -1;

    c->records = c->bytes = c->drops_ring = c->drops_queue = 0;
    c->mb_s = 0; c->ema_bytes = 0; c->ema_t_ns = 0;
    c->last_rec_ns = 0; c->lost = 0;
    c->seg_no = 0; c->seg_used = 0;
    c->fill = NULL;
    c->q_head = c->q_tail = 0;
    c->wr_seg_fd = -1; c->wr_seg_no = 0; c->wr_seg_used = 0; c->unsynced = 0;

    if (c->cfg->heavy) {
        if (c->sub_ok) c->sub.next_seq = tap_wseq(&c->sub);
        frame_seg_hdr(c, t0_mono_ns);
    } else {
        snprintf(path, sizeof path, "%s/%s/data.00000.airec", dir, c->cfg->name);
        c->small_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (c->small_fd < 0) { fclose(c->idx_f); c->idx_f = NULL; return -1; }
        AirecSegHdr sh; memset(&sh, 0, sizeof sh);
        sh.magic = AIREC_SEG_MAGIC; sh.version = AIREC_VERSION;
        sh.channel_id = (uint32_t)c->id; sh.session_t0_mono_ns = t0_mono_ns;
        if (write(c->small_fd, &sh, sizeof sh) != sizeof sh) {}
        c->seg_used = sizeof sh;
        if (c->sub_ok) c->sub.next_seq = tap_wseq(&c->sub);
    }
    set_phase(c->id, PH_ACTIVE);
    return 0;
}

void chan_session_close(Chan *c)
{
    if (phase(c->id) != PH_ACTIVE) return;
    if (c->cfg->heavy) {
        set_phase(c->id, PH_CLOSING);
        for (int i = 0; i < 3000 && phase(c->id) != PH_IDLE; i++) msleep_short();
        if (phase(c->id) != PH_IDLE) {
            fprintf(stderr, "rec: %s close timeout\n", c->cfg->name);
            set_phase(c->id, PH_IDLE);
        }
    } else {
        set_phase(c->id, PH_IDLE);
        pthread_mutex_lock(&c->small_lk);
        if (c->small_fd >= 0) { fdatasync(c->small_fd); close(c->small_fd); c->small_fd = -1; }
        if (c->idx_f) { fflush(c->idx_f); fdatasync(fileno(c->idx_f)); fclose(c->idx_f); c->idx_f = NULL; }
        pthread_mutex_unlock(&c->small_lk);
    }
}

int chan_init_all(void)
{
    for (int i = 0; i < CH_N; i++) {
        Chan *c = &g_chan[i];
        memset(c, 0, sizeof *c);
        c->cfg = &g_chan_cfg[i];
        c->id = (ChanId)i;
        c->small_fd = -1;
        c->wr_seg_fd = -1;
        pthread_mutex_init(&c->small_lk, NULL);
        g_phase[i] = PH_IDLE;

        if (c->cfg->heavy) {
            for (int k = 0; k < QUEUE_CHUNKS; k++) {
                if (posix_memalign((void **)&c->chunks[k].buf, 4096, CHUNK_BYTES) != 0)
                    return -1;
            }
            pthread_create(&c->drain_tid, NULL, drain_heavy, c);
            pthread_create(&c->write_tid, NULL, writer_heavy, c);
        } else if (c->cfg->tap) {
            pthread_create(&c->drain_tid, NULL, drain_small, c);
        }
    }
    static pthread_t hk;
    pthread_create(&hk, NULL, housekeeper, NULL);
    return 0;
}
