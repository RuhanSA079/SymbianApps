/* ns_symbian.h: force-included (-include) into every NetSurf C file built
 * for Symbian. Declares what P.I.P.S. lacks; ns_compat.c/.cpp define it. */
#ifndef NS_SYMBIAN_H
#define NS_SYMBIAN_H
#include <sys/types.h>
#include <stddef.h>
#include <limits.h>

/* P.I.P.S. has no PATH_MAX; Symbian file names are at most 256 characters
 * (KMaxFileName). */
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#ifdef __cplusplus
extern "C" {
#endif
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
/* Cryptographically secure random bytes (TRandom::SecureRandomL). */
void arc4random_buf(void *buf, size_t count);
#ifdef __cplusplus
}
#endif
#endif
