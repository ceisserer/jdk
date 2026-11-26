/* wasm-compat: Emscripten has no sendfile; stub returns ENOSYS (callers fall back to read/write). */
#ifndef WASM_COMPAT_SYS_SENDFILE_H
#define WASM_COMPAT_SYS_SENDFILE_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
#define sendfile64 sendfile
#ifdef __cplusplus
}
#endif
#endif
