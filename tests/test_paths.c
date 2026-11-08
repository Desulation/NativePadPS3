/* SOURCES: paths.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "npp.h"

/* The data-folder resolver: the nativepadps3 folder if its config file exists,
 * else the legacy folder if its config file exists, else the
 * nativepadps3 folder. Synthetic files in the fake file system only. */

#define NEW_CFG  "/dev_hdd0/plugins/nativepadps3/nativepadps3.txt"
#define NEW_DEV  "/dev_hdd0/plugins/nativepadps3/nativepadps3_devices.txt"
#define NEW_CAL  "/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt"
#define NEW_KC   "/dev_hdd0/plugins/nativepadps3/kernel_cache.txt"
/* legacy on-console name, read-only compatibility, do not rename */
#define OLD_CFG  "/dev_hdd0/plugins/ds45pad/ds45pad.txt"
/* legacy on-console name, read-only compatibility, do not rename */
#define OLD_DEV  "/dev_hdd0/plugins/ds45pad/ds45pad_devices.txt"
/* legacy on-console name, read-only compatibility, do not rename */
#define OLD_CAL  "/dev_hdd0/plugins/ds45pad/ds45pad_cal.txt"
/* legacy on-console name, read-only compatibility, do not rename */
#define OLD_KC   "/dev_hdd0/plugins/ds45pad/kernel_cache.txt"

/* The boot-trace markers, recorded here (the fakes' versions are weak). */
#define MAX_MARKS 8
static const char *marks[MAX_MARKS];
static int mark_count;

void npp_boot_mark(const char *stage)
{
	if (mark_count < MAX_MARKS)
		marks[mark_count] = stage;
	mark_count++;
}

void npp_boot_mark_hex(const char *stage, uint32_t value)
{
	(void)stage;
	(void)value;
}

static void reset(void)
{
	fake_reset_all();
	mark_count = 0;
	memset(marks, 0, sizeof(marks));
}

static int is_new_set(void)
{
	return strcmp(npp_path(NPP_PATH_CONFIG), NEW_CFG) == 0
	    && strcmp(npp_path(NPP_PATH_DEVICES), NEW_DEV) == 0
	    && strcmp(npp_path(NPP_PATH_CAL), NEW_CAL) == 0
	    && strcmp(npp_path(NPP_PATH_KERNEL_CACHE), NEW_KC) == 0;
}

static int is_old_set(void)
{
	return strcmp(npp_path(NPP_PATH_CONFIG), OLD_CFG) == 0
	    && strcmp(npp_path(NPP_PATH_DEVICES), OLD_DEV) == 0
	    && strcmp(npp_path(NPP_PATH_CAL), OLD_CAL) == 0
	    && strcmp(npp_path(NPP_PATH_KERNEL_CACHE), OLD_KC) == 0;
}

/* Must run first: nothing has called npp_paths_init() yet. */
static int test_uninitialised_is_new_set(void)
{
	CHECK(is_new_set());
	CHECK(strcmp(npp_path(NPP_PATH_COUNT), "") == 0);
	CHECK(strcmp(npp_path((npp_path_kind_t)-1), "") == 0);
	return 0;
}

static int test_new_config_selects_new_folder(void)
{
	reset();
	fake_file_set(NEW_CFG, "ENABLE = 1\n");
	npp_paths_init();
	CHECK(is_new_set());

	reset();
	fake_file_set(NEW_CFG, "ENABLE = 1\n");
	fake_file_set(OLD_CFG, "ENABLE = 0\n");
	npp_paths_init();
	CHECK(is_new_set());
	return 0;
}

static int test_legacy_config_selects_legacy_folder(void)
{
	reset();
	fake_file_set(OLD_CFG, "ENABLE_SHIM = 1\n");
	npp_paths_init();
	CHECK(is_old_set());

	/* a stray file in the new folder is not its config */
	reset();
	fake_file_set(OLD_CFG, "ENABLE_SHIM = 1\n");
	fake_file_set(NEW_DEV, "0x054C 0x0CE6\n");
	npp_paths_init();
	CHECK(is_old_set());
	return 0;
}

static int test_no_config_defaults_to_new_folder(void)
{
	reset();
	npp_paths_init();
	CHECK(is_new_set());
	return 0;
}

/* The choice is made at init; files that appear later do not move it. */
static int test_resolution_happens_once(void)
{
	reset();
	npp_paths_init();
	fake_file_set(OLD_CFG, "ENABLE = 1\n");
	CHECK(is_new_set());

	reset();
	fake_file_set(OLD_CFG, "ENABLE = 1\n");
	npp_paths_init();
	fake_file_set(NEW_CFG, "ENABLE = 1\n");
	CHECK(is_old_set());
	return 0;
}

/* One trace marker per init, naming the folder that was chosen. */
static int test_trace_marker(void)
{
	reset();
	fake_file_set(NEW_CFG, "ENABLE = 1\n");
	npp_paths_init();
	CHECK(mark_count == 1);
	CHECK(strcmp(marks[0], "datadir_new") == 0);

	reset();
	fake_file_set(OLD_CFG, "ENABLE = 1\n");
	npp_paths_init();
	CHECK(mark_count == 1);
	CHECK(strcmp(marks[0], "datadir_legacy") == 0);

	reset();
	npp_paths_init();
	CHECK(mark_count == 1);
	CHECK(strcmp(marks[0], "datadir_new") == 0);
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_uninitialised_is_new_set()) != 0) return r;
	if ((r = test_new_config_selects_new_folder()) != 0) return r;
	if ((r = test_legacy_config_selects_legacy_folder()) != 0) return r;
	if ((r = test_no_config_defaults_to_new_folder()) != 0) return r;
	if ((r = test_resolution_happens_once()) != 0) return r;
	if ((r = test_trace_marker()) != 0) return r;
	return 0;
}
