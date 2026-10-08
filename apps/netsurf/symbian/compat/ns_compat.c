/* ns_compat.c: POSIX functions NetSurf uses that P.I.P.S. lacks. */
#include <unistd.h>
#include "ns_symbian.h"

/* Not atomic like the real ones; NetSurf's callers (the disc cache via
 * libnsutils) never share a file descriptor between threads. */
ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t n;
    if (old == (off_t)-1 || lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    n = read(fd, buf, count);
    lseek(fd, old, SEEK_SET);
    return n;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    ssize_t n;
    if (old == (off_t)-1 || lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    n = write(fd, buf, count);
    lseek(fd, old, SEEK_SET);
    return n;
}
