/* SOURCES: config.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "ds45pad.h"

/* The configuration file's ENABLE_SHIM key, in a release-shaped build (neither
 * DS45_DEBUG nor DS45_GAME). Synthetic config text only.
 *
 * ENABLE_SHIM is the safety gate in front of the DS3-route kernel shim: the shim
 * patches live kernel code, so a deployed plugin must never install it on its own.
 * It is off unless ds45pad.txt says ENABLE_SHIM = 1, which is also the way out of a
 * hang - remove the line and reboot. */

static ds45_config_t cfg_from(const char *text)
{
	ds45_config_t c;

	fake_reset_all();
	ds45_config_defaults(&c);
	fake_file_set("/c.txt", text);
	ds45_config_load(&c, "/c.txt");
	return c;
}

static int test_shim_is_off_by_default(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.enable_shim == 0);

	/* no config file at all: the defaults stand */
	fake_reset_all();
	CHECK(ds45_config_load(&c, "/absent.txt") != 0);
	CHECK(c.enable_shim == 0);

	/* a file that never mentions it, however much else it sets, or that only
	 * mentions it in a comment */
	c = cfg_from("ENABLE = 1\nMODE = AUTO\nMAX_PADS = 2\nNOTIFY = 1\n");
	CHECK(c.enable_shim == 0);
	c = cfg_from("# ENABLE_SHIM = 1\n");
	CHECK(c.enable_shim == 0);
	return 0;
}

static int test_shim_is_enabled_by_one(void)
{
	ds45_config_t c;

	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.enable_shim == 1);

	c = cfg_from("enable_shim=1\n");                       /* keys are case-insensitive */
	CHECK(c.enable_shim == 1);

	c = cfg_from("  ENABLE_SHIM   =   1   # trying it\n");  /* blanks and a trailing comment */
	CHECK(c.enable_shim == 1);

	/* its neighbours still parse */
	c = cfg_from("MODE = BT\nENABLE_SHIM = 1\nNOTIFY = 0\n");
	CHECK(c.enable_shim == 1);
	CHECK(c.mode == DS45_MODE_BT);
	CHECK(c.notify == 0);
	return 0;
}

static int test_shim_can_be_switched_back_off(void)
{
	ds45_config_t c;

	c = cfg_from("ENABLE_SHIM = 0\n");
	CHECK(c.enable_shim == 0);

	c = cfg_from("ENABLE_SHIM = 1\nENABLE_SHIM = 0\n");      /* the last line wins */
	CHECK(c.enable_shim == 0);
	c = cfg_from("ENABLE_SHIM = 0\nENABLE_SHIM = 1\n");
	CHECK(c.enable_shim == 1);
	return 0;
}

/* Anything that is not a number leaves the gate where it was: shut. */
static int test_shim_ignores_what_it_cannot_parse(void)
{
	ds45_config_t c;

	c = cfg_from("ENABLE_SHIM = yes\n");
	CHECK(c.enable_shim == 0);
	c = cfg_from("ENABLE_SHIM = on\n");
	CHECK(c.enable_shim == 0);
	c = cfg_from("ENABLE_SHIM =\n");
	CHECK(c.enable_shim == 0);
	c = cfg_from("ENABLE_SHIM\n");                         /* no '=' at all */
	CHECK(c.enable_shim == 0);
	return 0;
}

/* ENABLE (the plugin's master switch) and ENABLE_SHIM are different keys. */
static int test_shim_key_is_independent_of_enable(void)
{
	ds45_config_t c;

	c = cfg_from("ENABLE = 0\n");
	CHECK(c.enable == 0);
	CHECK(c.enable_shim == 0);

	c = cfg_from("ENABLE = 0\nENABLE_SHIM = 1\n");
	CHECK(c.enable == 0);
	CHECK(c.enable_shim == 1);

	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.enable == 1);                                  /* ENABLE keeps its default */
	CHECK(c.enable_shim == 1);
	return 0;
}

/* SHIM_INLINE_PID selects whether the shim installer also applies the two inline
 * product-ID words. Off by default; any non-zero number turns it on, anything
 * that is not a number leaves it off. */
static int test_shim_inline_pid_key(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.shim_inline_pid == 0);
	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.shim_inline_pid == 0);

	c = cfg_from("SHIM_INLINE_PID = 1\n");
	CHECK(c.shim_inline_pid == 1);
	CHECK(c.enable_shim == 0);                             /* a separate key */
	c = cfg_from("shim_inline_pid = 7\n");
	CHECK(c.shim_inline_pid == 1);

	c = cfg_from("SHIM_INLINE_PID = abc\n");
	CHECK(c.shim_inline_pid == 0);
	c = cfg_from("SHIM_INLINE_PID = 1\nSHIM_INLINE_PID = 0\n");
	CHECK(c.shim_inline_pid == 0);
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_shim_is_off_by_default()) != 0) return r;
	if ((r = test_shim_is_enabled_by_one()) != 0) return r;
	if ((r = test_shim_can_be_switched_back_off()) != 0) return r;
	if ((r = test_shim_ignores_what_it_cannot_parse()) != 0) return r;
	if ((r = test_shim_key_is_independent_of_enable()) != 0) return r;
	if ((r = test_shim_inline_pid_key()) != 0) return r;
	return 0;
}
