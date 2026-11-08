/*
 * DS45PAD - data-folder resolution.
 *
 * The plugin keeps four data files: its configuration, the USB device list,
 * the stick calibration and the kernel match cache. They live in one folder,
 * chosen once at plugin start:
 *
 *   1. the nativepadps3 folder, if its configuration file exists;
 *   2. else the legacy folder (old name, old file names), if its configuration file exists, so a
 *      console that was set up under the old name keeps working unchanged;
 *   3. else the nativepadps3 folder (nothing exists yet, the defaults apply).
 *
 * Only the configuration file decides. The choice is passive: it leaves a
 * trace marker and nothing else, and nothing in either folder is ever deleted
 * or moved. All eight paths are compile-time literals, so there is nothing to
 * allocate.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

#include "ds45pad.h"

/* legacy on-console name, read-only compatibility, do not rename */
#define LEGACY_DIR "/dev_hdd0/plugins/ds45pad"

static const char *const s_new_paths[DS45_PATH_COUNT] = {
	DS45_DATADIR_NEW "/nativepadps3.txt",
	DS45_DATADIR_NEW "/nativepadps3_devices.txt",
	DS45_DATADIR_NEW "/nativepadps3_cal.txt",
	DS45_DATADIR_NEW "/kernel_cache.txt"
};

/* legacy on-console name, read-only compatibility, do not rename */
static const char *const s_legacy_paths[DS45_PATH_COUNT] = {
	/* legacy on-console name, read-only compatibility, do not rename */
	LEGACY_DIR "/ds45pad.txt",
	/* legacy on-console name, read-only compatibility, do not rename */
	LEGACY_DIR "/ds45pad_devices.txt",
	/* legacy on-console name, read-only compatibility, do not rename */
	LEGACY_DIR "/ds45pad_cal.txt",
	/* legacy on-console name, read-only compatibility, do not rename */
	LEGACY_DIR "/kernel_cache.txt"
};

static const char *const *s_paths = s_new_paths;

/* 1 if path can be opened. The in-game build must not gain a cellFsStat
 * import (ds45_file_exists is VSH-only), so there the file is opened for
 * reading through ds45_read_file(), which the game build already uses for its
 * configuration; it fails (-1) only when the open fails. */
static int probe(const char *path)
{
#ifdef DS45_GAME
	char c;

	return ds45_read_file(path, &c, 1) >= 0;
#else
	return ds45_file_exists(path);
#endif
}

void ds45_paths_init(void)
{
	if (probe(s_new_paths[DS45_PATH_CONFIG])) {
		s_paths = s_new_paths;
		ds45_boot_mark("datadir_new");
	} else if (probe(s_legacy_paths[DS45_PATH_CONFIG])) {
		s_paths = s_legacy_paths;
		ds45_boot_mark("datadir_legacy");
	} else {
		s_paths = s_new_paths;
		ds45_boot_mark("datadir_new");
	}
}

const char *ds45_path(ds45_path_kind_t kind)
{
	if ((int)kind < 0 || (int)kind >= (int)DS45_PATH_COUNT)
		return "";
	return s_paths[kind];
}
