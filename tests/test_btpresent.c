/* SOURCES: lv2.c kernel.c btcheck.c passthru.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "ds45pad.h"

/* passthru_bt_pad_present() (debug builds, for the LV2 dumper): any generic
 * pad counts, with or without a kernel record, bound or not. */

ds45_config_t g_cfg;
ds45_pad_t    g_pads[DS45_MAX_PADS];

static void clear_ports(void)
{
	fake_reset_all();
	memset(g_pads, 0, sizeof(g_pads));      /* no pads of our own */
	memset(&fake_pad_info, 0, sizeof(fake_pad_info));
	fake_pad_info_rc = CELL_PAD_OK;
}

static int test_no_pad(void)
{
	clear_ports();
	CHECK(passthru_bt_pad_present() == 0);
	return 0;
}

static int test_generic_pad_without_kernel_record(void)
{
	clear_ports();                        /* no kernel_resolve: nothing active */
	fake_pad_info.port_status[3] = DS45_STATUS_CONNECTED;
	CHECK(passthru_bt_pad_present() == 1);
	return 0;
}

static int test_ldd_and_ds3_do_not_count(void)
{
	clear_ports();
	fake_pad_info.port_status[0] = DS45_STATUS_CONNECTED;
	fake_pad_info.device_type[0] = DS45_DEV_TYPE_LDD;
	fake_pad_info.port_status[1] = DS45_STATUS_CONNECTED;
	fake_pad_info.device_capability[1] = DS45_CAP_PS3_CONFORMITY;
	CHECK(passthru_bt_pad_present() == 0);
	return 0;
}

static int test_pad_info_error(void)
{
	clear_ports();
	fake_pad_info.port_status[3] = DS45_STATUS_CONNECTED;
	fake_pad_info_rc = -1;
	CHECK(passthru_bt_pad_present() == 0);
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_no_pad()) != 0) return r;
	if ((r = test_generic_pad_without_kernel_record()) != 0) return r;
	if ((r = test_ldd_and_ds3_do_not_count()) != 0) return r;
	if ((r = test_pad_info_error()) != 0) return r;
	return 0;
}
