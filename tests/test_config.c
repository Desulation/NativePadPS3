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

/* PROBE_EXEC arms the debug-only executability pre-flight (design Amendment 2).
 * Off by default; any non-zero number turns it on, anything that is not a
 * number leaves it off - exactly like ENABLE_SHIM / SHIM_INLINE_PID. */
static int test_probe_exec_key(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.probe_exec == 0);
	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.probe_exec == 0);

	c = cfg_from("PROBE_EXEC = 1\n");
	CHECK(c.probe_exec == 1);
	CHECK(c.enable_shim == 0);                             /* a separate key */
	c = cfg_from("probe_exec = 7\n");
	CHECK(c.probe_exec == 1);

	c = cfg_from("PROBE_EXEC = abc\n");
	CHECK(c.probe_exec == 0);
	c = cfg_from("PROBE_EXEC = 1\nPROBE_EXEC = 0\n");
	CHECK(c.probe_exec == 0);
	return 0;
}

/* SHIM_INPUT_HOOK gates whether the shim installs its per-poll input redirect,
 * so the start-up/enable completion can be isolated with the input hook off. On
 * by default (a normal install is unchanged); 0 turns it off, any other number
 * is on, and anything that is not a number leaves the default (on) in place. */
static int test_shim_input_hook_key(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.shim_input_hook == 1);
	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.shim_input_hook == 1);                         /* never mentioned: default */

	c = cfg_from("SHIM_INPUT_HOOK = 0\n");
	CHECK(c.shim_input_hook == 0);
	c = cfg_from("shim_input_hook = 0\n");                 /* keys are case-insensitive */
	CHECK(c.shim_input_hook == 0);
	CHECK(c.enable_shim == 0);                             /* a separate key */

	c = cfg_from("SHIM_INPUT_HOOK = 1\n");
	CHECK(c.shim_input_hook == 1);
	c = cfg_from("SHIM_INPUT_HOOK = 7\n");                 /* any non-zero = on */
	CHECK(c.shim_input_hook == 1);

	c = cfg_from("SHIM_INPUT_HOOK = abc\n");               /* junk leaves the default */
	CHECK(c.shim_input_hook == 1);
	c = cfg_from("SHIM_INPUT_HOOK =\n");
	CHECK(c.shim_input_hook == 1);
	c = cfg_from("SHIM_INPUT_HOOK = 1\nSHIM_INPUT_HOOK = 0\n");  /* the last wins */
	CHECK(c.shim_input_hook == 0);
	c = cfg_from("SHIM_INPUT_HOOK = 0\nSHIM_INPUT_HOOK = 1\n");
	CHECK(c.shim_input_hook == 1);
	return 0;
}

static int test_shim_event_enable_hook_keys(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.shim_event_hook == 1 && c.shim_enable_hook == 1);   /* both default on */
	c = cfg_from("SHIM_EVENT_HOOK = 0\n");
	CHECK(c.shim_event_hook == 0 && c.shim_enable_hook == 1);   /* independent */
	c = cfg_from("SHIM_ENABLE_HOOK = 0\n");
	CHECK(c.shim_enable_hook == 0 && c.shim_event_hook == 1);
	c = cfg_from("shim_event_hook = 0\nshim_enable_hook = 0\n");/* case-insensitive */
	CHECK(c.shim_event_hook == 0 && c.shim_enable_hook == 0);
	c = cfg_from("SHIM_EVENT_HOOK = abc\n");                    /* junk leaves default */
	CHECK(c.shim_event_hook == 1);
	c = cfg_from("SHIM_EVENT_HOOK = 5\n");                      /* any non-zero = on */
	CHECK(c.shim_event_hook == 1);
	return 0;
}

/* SHIM_STABLE_ID switches the shim's stable per-controller identity (taken from
 * the Bluetooth connection) on or off; the counter identity is the fallback. On
 * by default; 0 turns it off, any other number is on, junk leaves the default. */
static int test_shim_stable_id_key(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.shim_stable_id == 1);
	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.shim_stable_id == 1);

	c = cfg_from("SHIM_STABLE_ID = 0\n");
	CHECK(c.shim_stable_id == 0);
	c = cfg_from("shim_stable_id = 0\n");
	CHECK(c.shim_stable_id == 0);
	CHECK(c.enable_shim == 0);                             /* a separate key */
	c = cfg_from("SHIM_STABLE_ID = 7\n");
	CHECK(c.shim_stable_id == 1);
	c = cfg_from("SHIM_STABLE_ID = abc\n");
	CHECK(c.shim_stable_id == 1);
	c = cfg_from("SHIM_STABLE_ID = 0\nSHIM_STABLE_ID = 1\n");
	CHECK(c.shim_stable_id == 1);
	return 0;
}

/* SHIM_OUTPUT_HOOK gates the output (rumble / LED / light bar) hook and
 * SHIM_OUTPUT_MODE picks its stage: 0 observe, 1 probe, 2 full. The hook is on
 * by default and the mode defaults to 0; the mode is clamped to 0..2 and junk
 * leaves either default. */
static int test_shim_output_keys(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	CHECK(c.shim_output_hook == 1);
	CHECK(c.shim_output_mode == 0);
	c = cfg_from("ENABLE_SHIM = 1\n");
	CHECK(c.shim_output_hook == 1);
	CHECK(c.shim_output_mode == 0);

	c = cfg_from("SHIM_OUTPUT_HOOK = 0\n");
	CHECK(c.shim_output_hook == 0);
	CHECK(c.shim_output_mode == 0);
	CHECK(c.enable_shim == 0);                             /* a separate key */
	c = cfg_from("shim_output_hook = 0\n");
	CHECK(c.shim_output_hook == 0);
	c = cfg_from("SHIM_OUTPUT_HOOK = 7\n");
	CHECK(c.shim_output_hook == 1);
	c = cfg_from("SHIM_OUTPUT_HOOK = abc\n");
	CHECK(c.shim_output_hook == 1);
	c = cfg_from("SHIM_OUTPUT_HOOK = 0\nSHIM_OUTPUT_HOOK = 1\n");
	CHECK(c.shim_output_hook == 1);

	c = cfg_from("SHIM_OUTPUT_MODE = 1\n");
	CHECK(c.shim_output_mode == 1);
	CHECK(c.shim_output_hook == 1);
	c = cfg_from("shim_output_mode = 2\n");
	CHECK(c.shim_output_mode == 2);
	c = cfg_from("SHIM_OUTPUT_MODE = 0\n");
	CHECK(c.shim_output_mode == 0);
	c = cfg_from("SHIM_OUTPUT_MODE = 9\n");                /* clamped to the top */
	CHECK(c.shim_output_mode == 2);
	c = cfg_from("SHIM_OUTPUT_MODE = -3\n");               /* clamped to the bottom */
	CHECK(c.shim_output_mode == 0);
	c = cfg_from("SHIM_OUTPUT_MODE = abc\n");              /* junk keeps the default */
	CHECK(c.shim_output_mode == 0);
	c = cfg_from("SHIM_OUTPUT_MODE = 2\nSHIM_OUTPUT_MODE = xyz\n");
	CHECK(c.shim_output_mode == 2);                        /* junk after a good line */
	c = cfg_from("SHIM_OUTPUT_MODE = 2\nSHIM_OUTPUT_MODE = 1\n");
	CHECK(c.shim_output_mode == 1);                        /* last line wins */
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_shim_output_keys()) != 0) return r;
	if ((r = test_shim_stable_id_key()) != 0) return r;
	if ((r = test_shim_is_off_by_default()) != 0) return r;
	if ((r = test_shim_is_enabled_by_one()) != 0) return r;
	if ((r = test_shim_can_be_switched_back_off()) != 0) return r;
	if ((r = test_shim_ignores_what_it_cannot_parse()) != 0) return r;
	if ((r = test_shim_key_is_independent_of_enable()) != 0) return r;
	if ((r = test_shim_inline_pid_key()) != 0) return r;
	if ((r = test_probe_exec_key()) != 0) return r;
	if ((r = test_shim_input_hook_key()) != 0) return r;
	if ((r = test_shim_event_enable_hook_keys()) != 0) return r;
	return 0;
}
