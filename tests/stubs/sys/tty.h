/* Host-test replacement for the SDK's <sys/tty.h>: TTY writes are captured in
 * fake_tty (tests/fakes.c). */
#ifndef DS45_TEST_SYS_TTY_H
#define DS45_TEST_SYS_TTY_H

#define SYS_TTYP_PPU_STDOUT 0

int sys_tty_write(unsigned int ch, const void *buf, unsigned int len,
                  unsigned int *pwritelen);

#endif
