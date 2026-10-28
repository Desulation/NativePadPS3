/* Host-test replacement for the SDK's <sys/process.h>.
 *
 * The real system_call_N macros declare locals (p1) holding the syscall
 * result inside the calling function, and return_to_user_prog(t) returns it.
 * These do the same, routed through fake_syscall() in tests/fakes.c. */
#ifndef DS45_TEST_PROCESS_H
#define DS45_TEST_PROCESS_H
#include <stdint.h>

uint64_t fake_syscall(int n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

#define system_call_1(n, a1) \
	uint64_t p1 = fake_syscall((n), (uint64_t)(a1), 0, 0, 0); (void)p1
#define system_call_2(n, a1, a2) \
	uint64_t p1 = fake_syscall((n), (uint64_t)(a1), (uint64_t)(a2), 0, 0); (void)p1
#define system_call_3(n, a1, a2, a3) \
	uint64_t p1 = fake_syscall((n), (uint64_t)(a1), (uint64_t)(a2), (uint64_t)(a3), 0); (void)p1
#define system_call_4(n, a1, a2, a3, a4) \
	uint64_t p1 = fake_syscall((n), (uint64_t)(a1), (uint64_t)(a2), (uint64_t)(a3), (uint64_t)(a4)); (void)p1
#define return_to_user_prog(t) return (t)p1

#endif
