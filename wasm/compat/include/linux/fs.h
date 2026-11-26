/* wasm-compat: block-device ioctls (never match in Wasm, since no block devices). */
#ifndef WASM_COMPAT_LINUX_FS_H
#define WASM_COMPAT_LINUX_FS_H
#include <sys/ioctl.h>
#define BLKGETSIZE64 0x80081272
#endif
