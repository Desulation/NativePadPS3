/* Host-test replacement for the SDK's <sys/synchronization.h>: just the mutex
 * calls the plugin uses. The fakes in tests/fakes.c track the lock depth so a
 * test can assert that every lock is released and never taken twice. */
#ifndef DS45_TEST_SYNCHRONIZATION_H
#define DS45_TEST_SYNCHRONIZATION_H
#include <stdint.h>

#ifndef CELL_OK
#define CELL_OK 0
#endif

typedef uint32_t sys_mutex_t;
typedef uint64_t usecond_t;
typedef struct {
	uint32_t attr_protocol;
	uint32_t attr_recursive;
} sys_mutex_attribute_t;

#define sys_mutex_attribute_initialize(a) \
	do { (a).attr_protocol = 0; (a).attr_recursive = 0; } while (0)

int sys_mutex_create(sys_mutex_t *mutex_id, sys_mutex_attribute_t *attr);
int sys_mutex_destroy(sys_mutex_t mutex_id);
int sys_mutex_lock(sys_mutex_t mutex_id, usecond_t timeout);
int sys_mutex_trylock(sys_mutex_t mutex_id);
int sys_mutex_unlock(sys_mutex_t mutex_id);

#endif
