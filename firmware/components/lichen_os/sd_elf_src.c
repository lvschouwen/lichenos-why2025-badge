// A file as the ELF loader's byte source, see sd_elf_src.h.
#include "sd_elf_src.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int src_read(void *ctx, size_t off, void *buf, size_t len) {
    sd_elf_file_t *s = ctx;
    if (lseek(s->fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        return -EIO;
    }
    for (size_t got = 0; got < len;) {
        ssize_t n = read(s->fd, (uint8_t *)buf + got, len - got);
        if (n <= 0) {
            return -EIO;
        }
        got += (size_t)n;
    }
    return 0;
}

static size_t src_size(void *ctx) {
    return ((sd_elf_file_t *)ctx)->size;
}

static void src_close(void *ctx) {
    sd_elf_file_t *s = ctx;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

bool sd_elf_open_source(char const *path, sd_elf_file_t *s) {
    s->fd = open(path, O_RDONLY);
    if (s->fd < 0) {
        return false;
    }
    struct stat st;
    int         err = 0;
    if (fstat(s->fd, &st) != 0) {
        err = errno;
    } else if (!S_ISREG(st.st_mode)) {
        err = EISDIR;
    } else if (st.st_size <= 0) {
        err = ENOENT; // an empty file is no image
    }
    if (err) {
        src_close(s);
        errno = err;
        return false;
    }
    s->size = (size_t)st.st_size;
    return true;
}

elf_source_t sd_elf_source(sd_elf_file_t *f) {
    return (elf_source_t){.size = src_size, .read = src_read, .close = src_close, .ctx = f};
}
