/* Host-test replacement for the SDK's <sys/sys_time.h>: the monotonic system
 * clock, served from fake_now_us (tests/fakes.c). */
#ifndef NPP_TEST_SYS_TIME_H
#define NPP_TEST_SYS_TIME_H
#include <stdint.h>

typedef int64_t system_time_t;

system_time_t sys_time_get_system_time(void);

#endif
