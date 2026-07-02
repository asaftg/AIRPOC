/*
 * sg_ir850.h — Savgood SG-IR850-8M 850 nm IR laser illuminator controller.
 *
 * Production on-device controller (C, per docs/ENGINEERING_GUIDELINES.md).
 * Drives the illuminator over a TTL UART serial link: laser on/off, optical
 * power (drive current), and beam FOV via the motorized zoom lens, plus status
 * queries.
 *
 * Protocol: 9600 8N1, 7-byte frames, see docs/ILLUMINATOR.md.
 * Wiring   : device pin4 TX -> adapter RX, pin5 RX -> adapter TX, common GND.
 */
#ifndef SG_IR850_H
#define SG_IR850_H

#include <stdbool.h>
#include <stdint.h>

#define SG_IR850_DEFAULT_ADDR   0x01
#define SG_IR850_DEFAULT_BAUD   9600

/* Motor position limits (raw 16-bit motor target, from the protocol spec). */
#define SG_POS_MIN              0x0001u   /* narrowest beam (spot)            */
#define SG_POS_MAX              0x06FEu   /* widest beam (flood) / mech origin */

/* Beam half-coverage angle limits in degrees, from the spec's angle table.   */
#define SG_FOV_MIN_DEG          1.96      /* tightest spot                     */
#define SG_FOV_MAX_DEG          70.0      /* widest flood                      */

/* Optical drive level (the protocol's "DA" / current code). 0xFF = max.       */
#define SG_POWER_MAX            0xFF

typedef struct {
    int     fd;      /* open serial fd, or -1                                  */
    uint8_t addr;    /* logical device address (default 0x01)                 */
    int     tx_gap_us; /* inter-command spacing; spec advises 1-2 ms          */
} sg_ir850_t;

/*
 * Open the serial port and configure it 9600 8N1, raw. Returns 0 on success,
 * negative errno on failure. `addr` 0 selects SG_IR850_DEFAULT_ADDR.
 */
int  sg_open(sg_ir850_t *dev, const char *port, uint8_t addr);
void sg_close(sg_ir850_t *dev);

/* --- control --- */

/* Laser on/off. NOTE: per spec the drive level resets to max (0xFF) on every
 * power-on, so call sg_set_power() right after sg_power(dev, true) if a lower
 * level is wanted. */
int sg_power(sg_ir850_t *dev, bool on);

/* Set optical drive level directly, 0..255 (the protocol "DA"/current). */
int sg_set_power(sg_ir850_t *dev, uint8_t level);

/* Nudge drive level one step up/down (device-defined increment). */
int sg_power_step(sg_ir850_t *dev, bool increase);

/* Move the zoom motor to an absolute position (SG_POS_MIN..SG_POS_MAX).
 * Lower = narrower beam, higher = wider. Value is clamped to range. */
int sg_zoom_to_position(sg_ir850_t *dev, uint16_t pos);

/* Relative zoom by motor steps (0..255). tele=true narrows, false widens. */
int sg_zoom_step(sg_ir850_t *dev, bool tele, uint8_t steps);

/* Set beam coverage angle in degrees via the spec angle table (clamped to
 * [SG_FOV_MIN_DEG, SG_FOV_MAX_DEG]); maps to the nearest motor position. */
int sg_set_fov_deg(sg_ir850_t *dev, double degrees);

/* Re-home the zoom motor (self-inspection, parks at max angle). */
int sg_zoom_reset(sg_ir850_t *dev);

/* --- queries (return 0 on success and fill *out) --- */
int sg_query_power(sg_ir850_t *dev, int *on);        /* 0=off 1=on            */
int sg_query_current(sg_ir850_t *dev, int *level);   /* 0..255 DA             */
int sg_query_position(sg_ir850_t *dev, uint16_t *pos);
int sg_query_fan(sg_ir850_t *dev, int *on);          /* 0=off 1=on            */

/* --- angle table helpers (pure, no I/O) --- */
uint16_t sg_angle_to_position(double degrees);
double   sg_position_to_angle(uint16_t pos);

#endif /* SG_IR850_H */
