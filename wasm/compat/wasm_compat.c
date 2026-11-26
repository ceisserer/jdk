/*
 * wasm-compat: implementations of Linux APIs that Emscripten's libc lacks.
 * Linked into the static java launcher (see wasm/link.sh). Most are stubs that
 * fail with ENOSYS/ENOTSUP so the JDK takes its existing fallback paths.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>

/* eventfd / sendfile / inotify (declared in wasm/compat/include). */
int eventfd(unsigned int initval, int flags) { (void)initval; (void)flags; errno = ENOSYS; return -1; }
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
  (void)out_fd; (void)in_fd; (void)offset; (void)count; errno = ENOSYS; return -1;
}
int inotify_init(void) { errno = ENOSYS; return -1; }
int inotify_init1(int flags) { (void)flags; errno = ENOSYS; return -1; }
int inotify_add_watch(int fd, const char *p, uint32_t m) { (void)fd; (void)p; (void)m; errno = ENOSYS; return -1; }
int inotify_rm_watch(int fd, int wd) { (void)fd; (void)wd; errno = ENOSYS; return -1; }

/* sysinfo: report the maximum Wasm heap as "physical memory". */
int sysinfo(struct sysinfo *info) {
  memset(info, 0, sizeof(*info));
  info->totalram = (unsigned long)emscripten_get_heap_max();
  info->freeram  = info->totalram - (unsigned long)emscripten_get_heap_size();
  info->mem_unit = 1;
  info->procs = 1;
  return 0;
}

/* CPU affinity: report all logical cores (navigator.hardwareConcurrency / os.cpus()). */
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set) {
  (void)pid;
  memset(set, 0, size);
  int n = emscripten_num_logical_cores();
  unsigned char *p = (unsigned char *)set;
  for (int i = 0; i < n && (size_t)(i / 8) < size; i++) p[i / 8] |= (unsigned char)(1u << (i % 8));
  return 0;
}
int __sched_cpucount(size_t size, const cpu_set_t *set) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)set;
  for (size_t i = 0; i < size; i++) n += __builtin_popcount(p[i]);
  return n;
}
/* No CPU identity in Wasm. */
int sched_getcpu(void) { return 0; }

/* Extended attributes, dlinfo, sigsuspend: unsupported. */
ssize_t fgetxattr(int fd, const char *n, void *v, size_t s) { (void)fd; (void)n; (void)v; (void)s; errno = ENOTSUP; return -1; }
ssize_t flistxattr(int fd, char *l, size_t s) { (void)fd; (void)l; (void)s; errno = ENOTSUP; return -1; }
int fremovexattr(int fd, const char *n) { (void)fd; (void)n; errno = ENOTSUP; return -1; }
int fsetxattr(int fd, const char *n, const void *v, size_t s, int f) { (void)fd; (void)n; (void)v; (void)s; (void)f; errno = ENOTSUP; return -1; }
int dlinfo(void *h, int req, void *arg) { (void)h; (void)req; (void)arg; return -1; }
int sigsuspend(const sigset_t *mask) { (void)mask; errno = EINTR; return -1; }
