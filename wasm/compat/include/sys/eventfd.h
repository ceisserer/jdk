/* wasm-compat: Emscripten has no eventfd; stub returns ENOSYS. */
#ifndef WASM_COMPAT_SYS_EVENTFD_H
#define WASM_COMPAT_SYS_EVENTFD_H
#include <stdint.h>
#include <fcntl.h>
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK
typedef uint64_t eventfd_t;
#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned int initval, int flags);
#ifdef __cplusplus
}
#endif
#endif
