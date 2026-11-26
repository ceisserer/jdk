/* wasm-compat: Emscripten has no inotify; stubs return ENOSYS. */
#ifndef WASM_COMPAT_SYS_INOTIFY_H
#define WASM_COMPAT_SYS_INOTIFY_H
#include <stdint.h>
#include <fcntl.h>
struct inotify_event { int wd; uint32_t mask, cookie, len; char name[]; };
#define IN_ACCESS 0x001
#define IN_MODIFY 0x002
#define IN_ATTRIB 0x004
#define IN_CLOSE_WRITE 0x008
#define IN_CLOSE_NOWRITE 0x010
#define IN_OPEN 0x020
#define IN_MOVED_FROM 0x040
#define IN_MOVED_TO 0x080
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_DELETE_SELF 0x400
#define IN_MOVE_SELF 0x800
#define IN_UNMOUNT 0x2000
#define IN_Q_OVERFLOW 0x4000
#define IN_IGNORED 0x8000
#define IN_NONBLOCK O_NONBLOCK
#define IN_CLOEXEC O_CLOEXEC
#ifdef __cplusplus
extern "C" {
#endif
int inotify_init(void);
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
#ifdef __cplusplus
}
#endif
#endif
