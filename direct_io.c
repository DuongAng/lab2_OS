#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "vtpc_internal.h"

void *aligned_alloc_page(size_t page_size) {
    void *ptr = NULL;

    size_t alignment = page_size;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }

    if (posix_memalign(&ptr, alignment, page_size) != 0) {
        return NULL;
    }

    memset(ptr, 0, page_size);

    return ptr;
}

void aligned_free_page(void *ptr) {
    free(ptr);
}

ssize_t direct_read_block(int real_fd, off_t block_num, void *buf, size_t page_size) {
    off_t offset = block_num * (off_t)page_size;

    if (lseek(real_fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    ssize_t bytes_read = read(real_fd, buf, page_size);

    return bytes_read;
}

ssize_t direct_write_block(int real_fd, off_t block_num, const void *buf, size_t page_size) {
    off_t offset = block_num * (off_t)page_size;

    if (lseek(real_fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    ssize_t bytes_written = write(real_fd, buf, page_size);

    return bytes_written;
}

off_t get_file_size(int real_fd) {
    struct stat st;

    if (fstat(real_fd, &st) < 0) {
        return -1;
    }

    return st.st_size;
}
