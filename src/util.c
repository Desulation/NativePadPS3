/*
 * NPP - odds and ends: XMB notifications, file reading, CRC32 and the
 * remap helpers.
 *
 * There is deliberately no allocator here. Every controller slot lives in a
 * static array, so the plugin never touches the heap and cannot leak or
 * fragment the VSH's memory.
 */

#include <string.h>
#include <cell/cell_fs.h>

#include "npp.h"
#include "hook.h"

/* ------------------------------------------------------------------------ */
/* XMB notifications                                                         */
/*                                                                           */
/* vshtask's notification entry point has no SDK stub, so it is resolved from */
/* the import table at runtime. Only the VSH process imports vshtask, so in   */
/* the in-game build the lookup simply comes back empty and notifications go  */
/* nowhere - which is the right behaviour there anyway.                      */
/* ------------------------------------------------------------------------ */

static int (*vshtask_notify)(int, const char *);
static int  notify_tried;

void npp_notify(const char *msg)
{
	if (!notify_tried) {
		notify_tried = 1;
		vshtask_notify = (int (*)(int, const char *))
		                 npp_resolve("vshtask", NID_VSHTASK_NOTIFY);
	}

	if (vshtask_notify)
		vshtask_notify(0, msg);
}

/* ------------------------------------------------------------------------ */
/* Boot trace                                                                */
/*                                                                           */
/* A marker is written before each startup step that touches the system, and  */
/* flushed immediately by closing the file. If the console hangs while the    */
/* plugin loads there is no other way to find out how far it got - so this is */
/* deliberately dumb and synchronous rather than buffered.                    */
/* ------------------------------------------------------------------------ */

static void boot_write(const char *line, int len)
{
	int fd;
	uint64_t written;

	if (cellFsOpen(NPP_BOOT_FILE,
	               CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
	               &fd, NULL, 0) == CELL_FS_SUCCEEDED) {
		cellFsWrite(fd, line, (uint64_t)len, &written);
		cellFsClose(fd);
	}
}

void npp_boot_mark(const char *stage)
{
	char line[64];
	int i = 0;

	while (stage[i] && i < (int)sizeof(line) - 2) {
		line[i] = stage[i];
		i++;
	}
	line[i++] = '\n';

	boot_write(line, i);
}

/* Same, with a value appended in hex - used to record what the system actually
 * reports back for the virtual pad, which is the only way to tell a working
 * registration from one that silently fell back. */
void npp_boot_mark_hex(const char *stage, uint32_t value)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[80];
	int i = 0, d;

	while (stage[i] && i < 60) {
		line[i] = stage[i];
		i++;
	}

	line[i++] = '=';
	line[i++] = '0';
	line[i++] = 'x';
	for (d = 28; d >= 0; d -= 4)
		line[i++] = hex[(value >> d) & 0xF];
	line[i++] = '\n';

	boot_write(line, i);
}

/* ------------------------------------------------------------------------ */
/* CRC32 (IEEE, reflected) - Bluetooth output reports are checksummed         */
/* ------------------------------------------------------------------------ */

uint32_t npp_crc32(uint32_t crc, const uint8_t *data, int len)
{
	int i, b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
	}

	return crc;
}

/* ------------------------------------------------------------------------ */
/* File reading                                                              */
/* ------------------------------------------------------------------------ */

int npp_read_file(const char *path, char *out, int cap)
{
	int fd;
	uint64_t nread = 0;

	if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
		return -1;

	cellFsRead(fd, out, (uint64_t)cap, &nread);
	cellFsClose(fd);

	return (int)nread;
}

/* Open path with flags and write len bytes. 0 on success, -1 on any failure
 * (including a short write). */
static int write_with_flags(const char *path, int flags, const void *data, int len)
{
	int fd;
	const uint8_t *p = (const uint8_t *)data;
	int rc = 0;

	if (!path || len < 0 || (len > 0 && !data))
		return -1;

	if (cellFsOpen(path, flags, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
		return -1;

	while (len > 0) {
		uint64_t written = 0;

		if (cellFsWrite(fd, p, (uint64_t)len, &written) != CELL_FS_SUCCEEDED ||
		    written == 0 || written > (uint64_t)len) {
			rc = -1;
			break;
		}
		p += written;
		len -= (int)written;
	}

	cellFsClose(fd);
	return rc;
}

/* Create or truncate path and write len bytes. 0 on success, -1 on any
 * failure (including a short write). */
int npp_write_file(const char *path, const void *data, int len)
{
	return write_with_flags(path,
	                        CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
	                        data, len);
}

/* VSH-only: the game build must not gain cellFsUnlink/cellFsStat imports. */
#ifndef NPP_GAME
int npp_delete_file(const char *path)
{
	return cellFsUnlink(path) == CELL_FS_SUCCEEDED ? 0 : -1;
}

int npp_file_exists(const char *path)
{
	CellFsStat st;

	return cellFsStat(path, &st) == CELL_FS_SUCCEEDED ? 1 : 0;
}

/* Append len bytes to path, creating it if absent. 0 ok, -1 fail. Used to
 * write files too big to hold in memory (the LV2 dump) a chunk at a time. */
int npp_append_file(const char *path, const void *data, int len)
{
	return write_with_flags(path,
	                        CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
	                        data, len);
}
#endif /* !NPP_GAME */

/* ------------------------------------------------------------------------ */
/* Remapping                                                                 */
/* ------------------------------------------------------------------------ */

static const struct {
	const char *name;
	npp_map_t  map;
} map_names[] = {
	{ "NONE",     NPP_MAP_NONE     },
	{ "CROSS",    NPP_MAP_CROSS    },
	{ "CIRCLE",   NPP_MAP_CIRCLE   },
	{ "SQUARE",   NPP_MAP_SQUARE   },
	{ "TRIANGLE", NPP_MAP_TRIANGLE },
	{ "L1",       NPP_MAP_L1       },
	{ "R1",       NPP_MAP_R1       },
	{ "L2",       NPP_MAP_L2       },
	{ "R2",       NPP_MAP_R2       },
	{ "L3",       NPP_MAP_L3       },
	{ "R3",       NPP_MAP_R3       },
	{ "START",    NPP_MAP_START    },
	{ "SELECT",   NPP_MAP_SELECT   },
	{ "PS",       NPP_MAP_PS       },
	{ "UP",       NPP_MAP_UP       },
	{ "RIGHT",    NPP_MAP_RIGHT    },
	{ "DOWN",     NPP_MAP_DOWN     },
	{ "LEFT",     NPP_MAP_LEFT     },
};

npp_map_t npp_parse_map(const char *name)
{
	unsigned i;

	for (i = 0; i < sizeof(map_names) / sizeof(map_names[0]); i++)
		if (strcmp(name, map_names[i].name) == 0)
			return map_names[i].map;

	return NPP_MAP_NONE;
}

void npp_apply_map(npp_map_t m, uint16_t *button)
{
	switch (m) {
	case NPP_MAP_CROSS:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_CROSS;
		button[NPP_BTN_PRESS_CROSS] = 255;
		break;
	case NPP_MAP_CIRCLE:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_CIRCLE;
		button[NPP_BTN_PRESS_CIRCLE] = 255;
		break;
	case NPP_MAP_SQUARE:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_SQUARE;
		button[NPP_BTN_PRESS_SQUARE] = 255;
		break;
	case NPP_MAP_TRIANGLE:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_TRIANGLE;
		button[NPP_BTN_PRESS_TRIANGLE] = 255;
		break;
	case NPP_MAP_L1:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_L1;
		button[NPP_BTN_PRESS_L1] = 255;
		break;
	case NPP_MAP_R1:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_R1;
		button[NPP_BTN_PRESS_R1] = 255;
		break;
	case NPP_MAP_L2:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_L2;
		button[NPP_BTN_PRESS_L2] = 255;
		break;
	case NPP_MAP_R2:
		button[NPP_BTN_DIGITAL2] |= NPP_CTRL_R2;
		button[NPP_BTN_PRESS_R2] = 255;
		break;
	case NPP_MAP_L3:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_L3;
		break;
	case NPP_MAP_R3:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_R3;
		break;
	case NPP_MAP_START:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_START;
		break;
	case NPP_MAP_SELECT:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_SELECT;
		break;
	case NPP_MAP_PS:
		button[NPP_BTN_LDD]      |= NPP_CTRL_LDD_PS;
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_PS;
		break;
	case NPP_MAP_UP:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_UP;
		button[NPP_BTN_PRESS_UP] = 255;
		break;
	case NPP_MAP_RIGHT:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_RIGHT;
		button[NPP_BTN_PRESS_RIGHT] = 255;
		break;
	case NPP_MAP_DOWN:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_DOWN;
		button[NPP_BTN_PRESS_DOWN] = 255;
		break;
	case NPP_MAP_LEFT:
		button[NPP_BTN_DIGITAL1] |= NPP_CTRL_LEFT;
		button[NPP_BTN_PRESS_LEFT] = 255;
		break;
	default:
		break;
	}
}

int npp_active_pads(void)
{
	int i, n = 0;

	for (i = 0; i < NPP_MAX_PADS; i++)
		if (g_pads[i].in_use)
			n++;

	return n;
}

int npp_usb_pads_active(void)
{
	int i, n = 0;

	for (i = 0; i < NPP_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == NPP_LINK_USB)
			n++;

	return n;
}

int npp_bt_pads_active(void)
{
	int i, n = 0;

	for (i = 0; i < NPP_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == NPP_LINK_BT)
			n++;

	return n;
}
