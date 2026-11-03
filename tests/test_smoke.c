/* SOURCES: */
#include <stdint.h>
#include <string.h>
#include <cell/pad.h>
#include "ds45pad.h"
#include "fakes.h"
#include "lv2.h"
#include <sys/process.h>

static uint64_t peek(uint64_t addr)
{
	system_call_1(6, addr);
	return_to_user_prog(uint64_t);
}

static int32_t sc8(void)
{
	uint32_t pids[4];
	system_call_3(8, 0x1234, 0x5678, (uint64_t)(uintptr_t)pids);
	return (int32_t)p1;
}

int run_tests(void)
{
	static const uint8_t q[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
	uint64_t a = DS45_LV2_BASE + 0x1000;
	char buf[32];

	fake_reset_all();
	CHECK(fake_toasts == 0);
	CHECK(fake_lv2_out_of_bounds == 0);

	fake_lv2_set(a, q, 8);
	CHECK(peek(a) == 0x0123456789ABCDEFULL);
	CHECK(peek(a + 8) == 0);                       /* unset reads 0 */
	CHECK(peek(DS45_LV2_BASE) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* misaligned and out-of-window reads are counted and return 0 */
	CHECK(peek(a + 1) == 0);
	CHECK(fake_lv2_out_of_bounds == 1);
	CHECK(peek(DS45_LV2_BOUND) == 0);
	CHECK(peek(DS45_LV2_BASE - 8) == 0);
	CHECK(fake_lv2_out_of_bounds == 3);
	CHECK(peek(DS45_LV2_BOUND - 8) == 0);          /* last valid qword */
	CHECK(fake_lv2_out_of_bounds == 3);

	/* a write spanning a page boundary */
	fake_lv2_set(DS45_LV2_BASE + 0x1FFC, q, 8);
	CHECK(peek(DS45_LV2_BASE + 0x1FF8) == 0x0000000001234567ULL);
	CHECK(peek(DS45_LV2_BASE + 0x2000) == 0x89ABCDEF00000000ULL);

	fake_lv2_reset();
	CHECK(peek(a) == 0);

	CHECK(sc8() == -1);

	ds45_notify("hello");
	CHECK(fake_toasts == 1);
	CHECK(strcmp(fake_last_toast, "hello") == 0);

	CHECK(ds45_file_exists("/x") == 0);
	fake_file_set("/x", "abc");
	CHECK(ds45_file_exists("/x") == 1);
	CHECK(ds45_read_file("/x", buf, sizeof(buf)) == 3);
	CHECK(strcmp(buf, "abc") == 0);
	CHECK(ds45_write_file("/y", "zz", 2) == 0);
	CHECK(strcmp(fake_file_get("/y"), "zz") == 0);
	CHECK(ds45_delete_file("/y") == 0);
	CHECK(fake_file_deleted("/y") == 1);
	CHECK(fake_file_get("/y") == NULL);

	/* binary-safe files: embedded zeros, and larger than 64 KiB */
	{
		static const char z[6] = { 'a', 0, 'b', 0, 0, 'c' };
		static char big[(8u << 20)];
		static char back[(8u << 20)];
		size_t k;

		CHECK(ds45_write_file("/z", z, sizeof(z)) == 0);
		CHECK(fake_file_size("/z") == 6);
		CHECK(memcmp(fake_file_get("/z"), z, sizeof(z)) == 0);
		CHECK(fake_file_size("/absent") == -1);

		for (k = 0; k < sizeof(big); k++)
			big[k] = (char)((k * 31u + (k >> 16)) & 0xFF);   /* includes zeros */
		CHECK(ds45_write_file("/big", big, 70000) == 0);
		CHECK(fake_file_size("/big") == 70000);
		CHECK(memcmp(fake_file_get("/big"), big, 70000) == 0);

		CHECK(ds45_write_file("/big", big, (int)sizeof(big)) == 0);   /* 8 MiB */
		CHECK(fake_file_size("/big") == (int)sizeof(big));
		CHECK(memcmp(fake_file_get("/big"), big, sizeof(big)) == 0);
		CHECK(ds45_read_file("/big", back, sizeof(back)) == (int)sizeof(back) - 1);
		CHECK(memcmp(back, big, sizeof(back) - 1) == 0);

		CHECK(ds45_write_file("/huge", big, 64 * 1024 * 1024) == -1);  /* loud */
		CHECK(ds45_file_exists("/huge") == 0);
		fake_reset_all();
		CHECK(fake_file_size("/big") == -1);
	}

	fake_now_us = 12345;
	fake_reset_all();
	CHECK(fake_now_us == 0);
	CHECK(fake_toasts == 0);
	CHECK(fake_last_toast[0] == 0);
	CHECK(ds45_file_exists("/x") == 0);
	return 0;
}
