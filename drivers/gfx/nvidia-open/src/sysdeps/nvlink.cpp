#include <assert.h>
#include <core/clock.hpp>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {

#include <nvlink_os.h>

}

#define STUBBED { assert(!"unimplemented"); }

// Memory management functions
void *nvlink_malloc(NvLength s) { return malloc(s); }

void nvlink_free(void *ptr) { free(ptr); }

void *nvlink_memset(void *dest, int c, NvLength len) { return memset(dest, c, len); }

void *nvlink_memcpy(void *dest, const void *src, NvLength l) { return memcpy(dest, src, l); }

int nvlink_memcmp(const void *a, const void *b, NvLength l) { return memcmp(a, b, l); }

NvU32 nvlink_memRd32(const volatile void *address) { return (*(const volatile NvU32 *)(address)); }

void nvlink_memWr32(volatile void *address, NvU32 data) { (*(volatile NvU32 *)(address)) = data; }

NvU64 nvlink_memRd64(const volatile void *address) { return (*(const volatile NvU64 *)(address)); }

void nvlink_memWr64(volatile void *address, NvU64 data) { (*(volatile NvU64 *)(address)) = data; }

// String management functions
char *nvlink_strcpy(char *dest, const char *src) { return strcpy(dest, src); }

NvLength nvlink_strlen(const char *s) { return strlen(s); }

int nvlink_strcmp(const char *a, const char *b) { return strcmp(a, b); }

int nvlink_snprintf(char *buf, NvLength len, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int ret = vsnprintf(buf, len, fmt, args);
	va_end(args);

	return ret;
}

// Locking support functions
void *nvlink_allocLock(void) {
	sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
	sem_init(sem, 0, 1);
	return sem;
}

void nvlink_acquireLock(void *s) {
	auto sem = reinterpret_cast<sem_t *>(s);
	sem_wait(sem);
}

NvBool nvlink_isLockOwner(void *) { return NV_TRUE; }

void nvlink_releaseLock(void *s) {
	auto sem = reinterpret_cast<sem_t *>(s);
	sem_post(sem);
}

void nvlink_freeLock(void *s) {
	auto sem = reinterpret_cast<sem_t *>(s);
	sem_destroy(sem);
	free(sem);
}

// Miscellaneous functions
void nvlink_assert(int expression) { assert(expression); }

void nvlink_sleep(unsigned int ms) { usleep(ms * 1'000); }

void nvlink_print(const char *file, int line, const char *func, int level, const char *fmt, ...) {
	printf("gfx/nvidia-open [%d %s:%d (%s)]: ", level, file, line, func);

	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

int nvlink_is_admin(void) { return NV_TRUE; }

NvU64 nvlink_get_platform_time(void) {
	auto ts = clk::getTimeSinceBoot();
	return (ts.tv_sec * 1'000'000'000) + ts.tv_nsec;
}

// Capability functions
NvlStatus nvlink_acquire_fabric_mgmt_cap(
    void *osPrivate [[maybe_unused]], NvU64 capDescriptor [[maybe_unused]]
) STUBBED;
int nvlink_is_fabric_manager(void *osPrivate [[maybe_unused]]) STUBBED;
