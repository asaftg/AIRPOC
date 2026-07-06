/* recorder.h — module-wide types and constants.
 *
 * Architecture: tap drain threads (lock-free reads from producer shm rings)
 * frame AIREC records into aligned chunks; writer threads push chunks to the
 * NVMe with O_DIRECT. Small channels (radar/events) go through a mutexed
 * buffered path. Nothing here ever back-pressures a producer.
 */
#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include "../tap/airpoc_tap.h"

#define REC_DEF_PORT   8093
#define REC_DEF_ROOT   "/data/recordings"
#define REC_EO_HOST    "127.0.0.1"
#define REC_EO_PORT    8091
#define REC_RD_PORT    8092
#define REC_APP_PORT   8080

#define SID_LEN        16                 /* "20260704T142233Z" */
#define NAME_MAX_LEN   96
#define TAGS_MAX_LEN   256
#define NOTE_MAX_LEN   1024

/* ---- AIREC v1 on-disk format (docs/FORMAT.md) ---- */

#define AIREC_SEG_MAGIC 0x3147534345524941ULL  /* "AIRECSG1" LE */
#define AIREC_REC_MAGIC 0x30434552u            /* "REC0" LE     */
#define AIREC_VERSION   1

#define RF_TRUNCATED  0x1u
#define RF_GAP_BEFORE 0x2u
#define RF_PAD        0x4u                /* padding pseudo-record: skip */

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t channel_id;
    uint64_t session_t0_mono_ns;
    uint32_t segment_no;
    uint8_t  pad[36];
} AirecSegHdr;                            /* 64 bytes */

typedef struct {
    uint32_t magic;
    uint32_t crc32c;                      /* of payload */
    uint64_t seq;
    uint64_t t_src_ns;
    uint64_t t_pub_ns;
    uint32_t payload_len;
    uint32_t flags;
    uint32_t meta[6];
} AirecRecHdr;                            /* 64 bytes */

typedef struct {
    uint64_t seq;
    uint64_t t_ns;                        /* timeline clock = record t_pub (recorder
                                            CLOCK_MONOTONIC, common to all channels);
                                            NOT the camera's V4L2 source clock */
    uint32_t segment_no;
    uint32_t offset;                      /* of AirecRecHdr within segment */
    uint32_t payload_len;
    uint32_t flags;
} AirecIdxRow;                            /* 32 bytes */

/* ---- channels ---- */

typedef enum { CH_EO_Y10, CH_EO_JPEG, CH_RADAR_RAW, CH_RADAR_WIRE, CH_EVENTS, CH_N } ChanId;
typedef enum { MODE_Y10P, MODE_RAW16, MODE_Y8 } VideoMode;

/* per-frame illuminator, packed by the EO tap into eo_y10 meta[4] (present only
 * when the tap's meta_json carries "illum":1). See recorder/docs/FORMAT.md. */
#define ILLUM_ON(m)      ((int)((m) & 1))
#define ILLUM_PRESENT(m) ((int)(((m) >> 1) & 1))
#define ILLUM_POWER(m)   ((int)(((m) >> 8) & 0xFF))
#define ILLUM_FOV_X10(m) ((int)(((m) >> 16) & 0x3FF))
#define EO_META_ILLUM    4                       /* meta slot carrying the above */

#define CHUNK_BYTES   (4u << 20)
#define SEG_BYTES     (256u << 20)
#define QUEUE_CHUNKS  16
#define FSYNC_BYTES   (64u << 20)
#define FSYNC_NS      1000000000ull

typedef struct {
    const char *name;                     /* dir name: "eo_y10"          */
    const char *tap;                      /* shm name, NULL = internal   */
    const char *encoding;                 /* channel.json                */
    const char *meta_desc;                /* channel.json meta[] doc     */
    int heavy;                            /* O_DIRECT chunk path         */
    uint32_t max_rec;                     /* scratch/copy sizing         */
} ChanCfg;

typedef struct { uint8_t *buf; uint32_t used; uint32_t seg_no; uint64_t seg_off; int rotate_after; } Chunk;

typedef struct Chan {
    const ChanCfg *cfg;
    ChanId id;
    /* tap side */
    AirTapSub sub;
    int sub_ok;
    /* live session state (heavy path) */
    int seg_fd;
    FILE *idx_f;
    uint32_t seg_no;
    uint64_t seg_used;                    /* bytes framed into current segment */
    uint64_t wr_off;                      /* bytes written (writer thread)     */
    uint64_t wr_seg_used;                 /* per-open-segment written bytes    */
    int wr_seg_fd;
    uint32_t wr_seg_no;
    /* chunk SPSC ring: drain produces, writer consumes */
    Chunk chunks[QUEUE_CHUNKS];
    unsigned q_head, q_tail;              /* __atomic accessed */
    Chunk *fill;                          /* chunk being filled by drain */
    uint64_t last_flush_ns;
    uint64_t unsynced;                    /* bytes since fdatasync (writer) */
    /* small path */
    pthread_mutex_t small_lk;
    int small_fd;
    /* counters (this session) */
    uint64_t records, bytes, drops_ring, drops_queue, next_seq_out;
    double   mb_s;                        /* EMA, /stats */
    uint64_t ema_bytes, ema_t_ns;
    /* threads */
    pthread_t drain_tid, write_tid;
} Chan;

/* ---- globals (recorder.c/main.c owns) ---- */

typedef enum { ST_IDLE, ST_RECORDING } RecState;

typedef struct {
    RecState state;
    char sid[SID_LEN + 1];
    char dir[512];
    uint64_t t0_mono_ns, t0_real_ns, t_start_ns;
    VideoMode mode;
    int keep;                             /* record every Nth eo_y10 frame */
    pthread_mutex_t lk;
    char root[256];
    int port;
    char pending_sid[SID_LEN + 1];        /* last stopped, awaiting save/discard */
} Rec;

extern Rec g_rec;
extern Chan g_chan[CH_N];
extern const ChanCfg g_chan_cfg[CH_N];

/* channel.c */
int  chan_init_all(void);
int  chan_session_open(Chan *c, const char *dir, uint64_t t0_mono_ns);
void chan_session_close(Chan *c);        /* flush + fsync + truncate */
void chan_submit(ChanId id, const void *payload, uint32_t len,
                 uint64_t t_src_ns, const uint32_t meta[6]);  /* small channels */
int  chan_recording(void);               /* atomic read of session state */

/* session.c */
int  session_start(char *err, size_t errlen);
int  session_stop(const char *reason);   /* -> pending */
int  session_save(const char *sid, const char *name, const char *tags, const char *note);
int  session_discard(const char *sid);
int  session_delete(const char *sids_csv);
int  session_purge_native(const char *sid);
void session_recover_all(void);
void session_stats_json(char *buf, size_t len);
void session_guard_start(void);
void session_marker(const char *text);
void session_clock_anchor(void);

/* store.c */
int  store_manifest_read(const char *dir, char *buf, size_t len);
int  store_manifest_field(const char *json, const char *key, char *out, size_t outlen);
int  store_manifest_set_state(const char *dir, const char *state);
void store_write_file_atomic(const char *path, const char *data, size_t len);
int  store_list_sids(const char *root, char sids[][SID_LEN + 1], int max);
void store_rm_rf_async(const char *dir);

/* disk.c */
double disk_free_gb(const char *path);
double disk_total_gb(const char *path);
int    disk_present(const char *path);

/* events.c */
void events_start(void);                 /* 5 Hz stats poller + clock anchors */
int  http_get_local(int port, const char *path, char *out, size_t outlen);

/* pack10.c */
uint32_t pack10(const uint8_t *y10le, uint32_t n_px, uint8_t *out);  /* returns bytes out */
uint32_t pack_y8(const uint8_t *y10le, uint32_t n_px, uint8_t *out);
int      pack10_selftest(void);

/* crc32c: hw on aarch64, table fallback elsewhere */
uint32_t crc32c(uint32_t seed, const void *buf, size_t len);

/* http.c */
int  httpd_start(int port);
void urldecode(char *s);
int  query_get(const char *qs, const char *key, char *out, size_t outlen);

/* library.c */
void library_json(const char *qs, char *buf, size_t len);
int  thumbs_serve_path(const char *sid, int n, char *path, size_t plen);

/* thumbs.c */
int  thumbs_generate(const char *dir);   /* 8 stills from eo_jpeg */

/* render.c — reconstruct a native-res JPEG from a recorded eo_y10 frame, using
 * the EO module's shared tone map (eo_tonemap.c) so it matches the live view.
 * tone_state is an EoToneState* (opaque here); reseed=1 on a seek/jump. */
int  render_native_jpeg(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                        int median_on, void *tone_state, int reseed, int quality,
                        uint8_t *out, uint32_t cap, uint32_t *outlen);
int  render_native_gray8(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                         int median_on, void *tone_state, int reseed, uint8_t *out8);
int  render_decode_jpeg_gray(const uint8_t *jpg, uint32_t len, uint8_t *out, uint32_t cap,
                             int *ow, int *oh);
int  render_selftest(void);

/* transcode.c — cache a smooth H.264 MP4 of a session's native replay */
void transcode_request(const char *sid);         /* kick an async build (replay) */
int  transcode_ensure(const char *sid);          /* build eo native.mp4 sync if missing (export) */
int  radar_movie_ensure(const char *sid);        /* build radar.mp4 (scope) sync if missing */
int  transcode_status(const char *sid, int *pct);/* 2=ready 1=building 0=none -1=failed */
int  transcode_mp4_path(const char *sid, char *path, size_t plen);  /* 0 if file exists */

/* replay.c */
void replay_ctl(const char *qs, char *resp, size_t rlen);
void replay_state_json(char *buf, size_t len);
void replay_stats_json(char *buf, size_t len);
int  replay_radar_json(char *buf, size_t len);   /* frame at <= clock */
int  replay_rstats_json(char *buf, size_t len);
void replay_stream(int fd);                      /* blocks: MJPEG pusher */
int  replay_frame_copy(int64_t t_ms, uint8_t *buf, uint32_t cap, uint32_t *len);
void replay_close(void);

static inline uint64_t now_ns(void) { return tap_now_ns(); }
static inline uint64_t real_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
