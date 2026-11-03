/* Host-test replacement for the SDK's <cell/cell_fs.h>, just enough for util.c
 * to compile. Tests that link util.c do it for ds45_crc32 and the remap
 * helpers; none of them touches the file system, so every call here fails. */
#ifndef DS45_TEST_CELL_FS_H
#define DS45_TEST_CELL_FS_H
#include <stdint.h>

#define CELL_FS_SUCCEEDED 0

#define CELL_FS_O_RDONLY  0x0000
#define CELL_FS_O_WRONLY  0x0001
#define CELL_FS_O_CREAT   0x0200
#define CELL_FS_O_TRUNC   0x0400
#define CELL_FS_O_APPEND  0x0008

typedef struct {
	uint64_t st_size;
} CellFsStat;

static inline int cellFsOpen(const char *path, int flags, int *fd,
                             const void *arg, uint64_t size)
{
	(void)path; (void)flags; (void)fd; (void)arg; (void)size;
	return -1;
}

static inline int cellFsRead(int fd, void *buf, uint64_t nbytes, uint64_t *nread)
{
	(void)fd; (void)buf; (void)nbytes; (void)nread;
	return -1;
}

static inline int cellFsWrite(int fd, const void *buf, uint64_t nbytes,
                              uint64_t *nwrite)
{
	(void)fd; (void)buf; (void)nbytes; (void)nwrite;
	return -1;
}

static inline int cellFsClose(int fd)
{
	(void)fd;
	return -1;
}

static inline int cellFsStat(const char *path, CellFsStat *sb)
{
	(void)path; (void)sb;
	return -1;
}

static inline int cellFsUnlink(const char *path)
{
	(void)path;
	return -1;
}

#endif
