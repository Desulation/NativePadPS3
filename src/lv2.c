/*
 * DS45PAD - bounded LV2 (kernel) memory reads and writes. See lv2.h.
 */

#include <stddef.h>
#include <sys/process.h>

#include "lv2.h"

uint64_t ds45_peek(uint64_t addr)
{
	system_call_1(6, addr);
	return_to_user_prog(uint64_t);
}

/* Syscall 7 (sys_cfw_poke). Raw and unchecked, hence static. */
static void ds45_poke(uint64_t addr, uint64_t value)
{
	system_call_2(7, addr, value);
}

int ds45_lv2_read(uint64_t addr, uint64_t *out)
{
	if (!out || (addr & 7) || addr < DS45_LV2_BASE ||
	    addr > DS45_LV2_BOUND - 8)
		return -1;

	*out = ds45_peek(addr);
	return 0;
}

int ds45_lv2_read_bytes(uint64_t addr, uint8_t *out, int len)
{
	int i, b;

	if (!out || len < 0 || (len & 7) || (addr & 7))
		return -1;
	if (len == 0)
		return 0;

	/* Validate the whole range first so a bad tail cannot leave a partial
	 * read behind. Written as a subtraction so it cannot overflow. */
	if (addr < DS45_LV2_BASE || addr > DS45_LV2_BOUND - (uint64_t)len)
		return -1;

	for (i = 0; i < len; i += 8) {
		uint64_t q = ds45_peek(addr + (uint64_t)i);

		for (b = 0; b < 8; b++)
			out[i + b] = (uint8_t)(q >> (56 - 8 * b));
	}

	return 0;
}

int ds45_lv2_write(uint64_t addr, uint64_t value)
{
	if ((addr & 7) || addr < DS45_LV2_BASE || addr > DS45_LV2_BOUND - 8)
		return -1;

	ds45_poke(addr, value);

	/* The poke returns nothing useful; trust only what the kernel now holds. */
	return ds45_peek(addr) == value ? 0 : -1;
}

uint64_t ds45_fnv1a64(const uint8_t *p, int n)
{
	uint64_t h = 0xCBF29CE484222325ULL;
	int i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001B3ULL;
	}

	return h;
}
