/*
 * DS45PAD kernel shim - the DualShock 3 output payload to a DualSense
 * Bluetooth output report 0x31.
 *
 * The PS3 kernel's DS3 driver builds a 48 byte output payload (rumble and LED
 * state, the report id not included) for every controller it manages. For a
 * DualSense the shim translates it into the one report the pad understands, so
 * rumble, the player number and the light bar follow what the console asks.
 *
 * Pure logic: no SDK headers, no libc, no state. The same file is built by the
 * host tests and, freestanding, into the shim blob.
 *
 * DS3 payload, byte offsets (the shape this translator accepts):
 *   [0]      0
 *   [1]      right motor duration (0x96, ignored)
 *   [2]      small motor (right): 0 off, any other value on
 *   [3]      left motor duration (0x96, ignored)
 *   [4]      large motor (left) strength 0..255
 *   [5..6]   request bytes (ignored)
 *   [7..8]   0
 *   [9]      LED bitmap: 0x02 player 1, 0x04 player 2, 0x08 player 3, 0x10
 *            player 4 (the kernel masks it with 0x1E; other bits are ignored)
 *   [10..29] four 5 byte LED timing blocks (ignored: the LEDs are steady)
 *   [30..47] 0
 *
 * DualSense report 0x31, DS_OUT_LEN bytes:
 *   [0] 0x31   [1] sequence in the high nibble   [2] tag 0x10
 *   [3] valid_flag0 0x03 (compatible vibration | haptics select, as SDL sends
 *       it; Linux on firmware 2.21+ uses valid_flag2 bit 2 instead, not set)
 *   [4] valid_flag1 0x14 (light bar colour | player indicator)
 *   [5] motor_right 255 or 0   [6] motor_left
 *   [41] valid_flag2 0x02 (light bar setup control enable)
 *   [44] light bar setup 0x02 (fade light out): without it a DualSense on
 *        Bluetooth keeps its start-up animation on the light bar and ignores
 *        the colour, so every report carries it (as the Linux driver does)
 *   [46] player LEDs   [47..49] light bar R, G, B   [74..77] CRC-32, little-endian
 *   every other byte up to [73] is 0.
 * The CRC is the reflected CRC-32 (0xEDB88320, init and final XOR 0xFFFFFFFF)
 * over the byte 0xA2 followed by report bytes 0..73.
 *
 * The player number is the lowest set bit of the LED bitmap. Its player LED
 * pattern is 0x04, 0x0A, 0x15, 0x1B for players 1..4 and the light bar is blue,
 * red, green, pink (chosen values, not confirmed on hardware). No LED bit set
 * means LEDs and light bar off.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_DS3_OUTPUT_H
#define DS45_DS3_OUTPUT_H

#include <stdint.h>

#define DS_OUT_LEN              78    /* a whole Bluetooth report, CRC included */
#define DS3_OUT_PAYLOAD_LEN     48    /* the DS3 payload, report id not included */

/* Translate the DS3 output payload in[0..in_len) into the DualSense report out
 * with sequence number seq (only the low nibble is used).
 *
 * Returns 0 and writes all DS_OUT_LEN bytes of out, or -1 and leaves out
 * untouched when in or out is NULL, in_len is not DS3_OUT_PAYLOAD_LEN, or the
 * payload is not of the shape above (in[7], in[8] or any of in[30..47] is not
 * zero). Reads nothing past in[in_len - 1] and all of it before writing out;
 * in and out must not overlap. */
int ds3_output_to_dualsense(const uint8_t *in, int in_len, uint8_t seq,
                            uint8_t out[DS_OUT_LEN]);

/* Write the report that translating an idle payload (no rumble) with the
 * LED bitmap 0x02 gives: player 1's LED pattern and light bar colour. For
 * checking whether the pad accepts output at all. A NULL out is a no-op. */
void ds3_output_probe(uint8_t seq, uint8_t out[DS_OUT_LEN]);

/*
 * The DualSense power-off feature report (Bluetooth): report id
 * DS_POWEROFF_REPORT_ID, then DS_POWEROFF_PAYLOAD_LEN bytes:
 *   [0] 0x02   [1..41] 0   [42..45] CRC-32, little-endian
 * The CRC is the same reflected CRC-32 as above, over the byte 0x53 followed by
 * the report id 0x08 and payload bytes 0..41.
 */
#define DS_POWEROFF_REPORT_ID       0x08
#define DS_POWEROFF_PAYLOAD_LEN     46

/* Reflected CRC-32 (0xEDB88320, init and final XOR 0xFFFFFFFF) of the byte seed
 * followed by p[0..n). Reads nothing when n <= 0 (p may then be NULL). The
 * output report uses seed 0xA2. */
uint32_t ds3_crc32_seeded(uint8_t seed, const uint8_t *p, int n);

/* Write the whole power-off payload, CRC included. A NULL out is a no-op. */
void ds_poweroff_payload(uint8_t out[DS_POWEROFF_PAYLOAD_LEN]);

#endif /* DS45_DS3_OUTPUT_H */
