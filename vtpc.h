#ifndef VTPC_H
#define VTPC_H

#include <sys/types.h>
#include <stddef.h>


int vtpc_init(size_t cache_size_pages, size_t page_size);

void vtpc_destroy(void);

void vtpc_set_direct_mode(int enable);

int vtpc_open(const char *path);

int vtpc_close(int fd);

ssize_t vtpc_read(int fd, void *buf, size_t count);

ssize_t vtpc_write(int fd, const void *buf, size_t count);

off_t vtpc_lseek(int fd, off_t offset, int whence);

int vtpc_fsync(int fd);

typedef struct {
    size_t cache_hits;
    size_t cache_misses;
    size_t pages_evicted;
    size_t pages_written_back;
    size_t current_pages_used;
} vtpc_stats_t;

int vtpc_get_stats(vtpc_stats_t *stats);

void vtpc_reset_stats(void);

#endif