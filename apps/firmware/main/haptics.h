#pragma once

/* DRV2605L haptic driver (I2C 0x5A). The device is operated one-handed
 * on the back of the phone, so the haptic answers exactly one question:
 * "did my press land?" — a single click for a photo, a double click for
 * the record toggle. Device state stays on the LED.
 *
 * Every call is a safe no-op when the chip is absent (not yet wired),
 * so the firmware runs unchanged on boards with and without haptics. */

/* Probes for the chip and configures it. Call once at boot. */
void haptics_init(void);

/* One strong click: a photo was captured. */
void haptics_click(void);

/* Double click: recording started or stopped. */
void haptics_double_click(void);
