/* SOURCES: lv2.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"

int run_tests(void)
{
	static const uint8_t img[16] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
		0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
	};
	uint64_t v = 0x1111111111111111ULL;
	uint8_t out[16];
	uint64_t a = DS45_LV2_BASE + 0x2000;

	fake_reset_all();

	/* out-of-window / unaligned: rejected without issuing a peek */
	CHECK(ds45_lv2_read(DS45_LV2_BASE - 8, &v) == -1);
	CHECK(ds45_lv2_read(DS45_LV2_BOUND - 4, &v) == -1);
	CHECK(ds45_lv2_read(DS45_LV2_BASE + 3, &v) == -1);
	CHECK(ds45_lv2_read(DS45_LV2_BOUND, &v) == -1);
	CHECK(ds45_lv2_read(0, &v) == -1);
	CHECK(ds45_lv2_read(0xFFFFFFFFFFFFFFF8ULL, &v) == -1);
	CHECK(ds45_lv2_read(a, NULL) == -1);
	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(v == 0x1111111111111111ULL);          /* untouched on failure */

	/* window edges */
	CHECK(ds45_lv2_read(DS45_LV2_BOUND - 8, &v) == 0);
	CHECK(ds45_lv2_read(DS45_LV2_BASE, &v) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* value round trip */
	fake_lv2_set(a, img, 16);
	CHECK(ds45_lv2_read(a, &v) == 0);
	CHECK(v == 0x0123456789ABCDEFULL);
	CHECK(ds45_peek(a + 8) == 0xFEDCBA9876543210ULL);

	/* byte reads come back in memory order */
	memset(out, 0xAA, sizeof(out));
	CHECK(ds45_lv2_read_bytes(a, out, 16) == 0);
	CHECK(memcmp(out, img, 16) == 0);
	CHECK(out[0] == 0x01 && out[7] == 0xEF);

	/* len must be a non-negative multiple of 8; zero is fine */
	CHECK(ds45_lv2_read_bytes(a, out, 0) == 0);
	CHECK(ds45_lv2_read_bytes(a, out, 4) == -1);
	CHECK(ds45_lv2_read_bytes(a, out, -8) == -1);
	CHECK(ds45_lv2_read_bytes(a, NULL, 8) == -1);

	/* no partial reads: the range is validated before any peek */
	memset(out, 0xAA, sizeof(out));
	CHECK(ds45_lv2_read_bytes(DS45_LV2_BOUND - 8, out, 16) == -1);
	CHECK(out[0] == 0xAA);
	CHECK(ds45_lv2_read_bytes(DS45_LV2_BASE - 8, out, 16) == -1);
	CHECK(ds45_lv2_read_bytes(a + 4, out, 8) == -1);
	CHECK(ds45_lv2_read_bytes(0xFFFFFFFFFFFFFFF8ULL, out, 16) == -1);  /* overflow */
	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(ds45_lv2_read_bytes(DS45_LV2_BOUND - 8, out, 8) == 0);

	/* FNV-1a 64 */
	CHECK(ds45_fnv1a64((const uint8_t *)"", 0) == 0xCBF29CE484222325ULL);
	CHECK(ds45_fnv1a64((const uint8_t *)"a", 1) == 0xAF63DC4C8601EC8CULL);
	CHECK(ds45_fnv1a64((const uint8_t *)"foobar", 6) == 0x85944171F73967E8ULL);

	return 0;
}
