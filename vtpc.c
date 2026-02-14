/**
 * vtpc.c - Virtual Page Cache main API implementation
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

#include "vtpc.h"
#include "vtpc_internal.h"

int find_free_fd_slot(void) {
    for (int i = 0; i < VTPC_MAX_OPEN_FILES; i++) {
        if (!g_cache.files[i].in_use) {
            return i;
        }
    }
    return -1;
}

file_entry_t *get_file_entry(int fd) {
    if (fd < 0 || fd >= VTPC_MAX_OPEN_FILES) {
        return NULL;
    }
    return &g_cache.files[fd];
}

int vtpc_init(size_t cache_size_pages, size_t page_size) {
    if (g_cache.initialized) {
        errno = EALREADY;
        return -1;
    }

    if (cache_size_pages == 0) {
        cache_size_pages = VTPC_DEFAULT_CACHE_SIZE;
    }
    if (page_size == 0) {
        page_size = VTPC_DEFAULT_PAGE_SIZE;
    }

    if (page_size % 512 != 0) {
        errno = EINVAL;
        return -1;
    }

    if (pthread_mutex_init(&g_cache.lock, NULL) != 0) {
        return -1;
    }

    g_cache.cache_size = cache_size_pages;
    g_cache.page_size = page_size;

    g_cache.pages = calloc(cache_size_pages, sizeof(cache_page_t));
    if (g_cache.pages == NULL) {
        pthread_mutex_destroy(&g_cache.lock);
        errno = ENOMEM;
        return -1;
    }

    g_cache.free_list = NULL;
    for (size_t i = 0; i < cache_size_pages; i++) {
        cache_page_t *page = &g_cache.pages[i];

        page->data = aligned_alloc_page(page_size);
        if (page->data == NULL) {
            for (size_t j = 0; j < i; j++) {
                aligned_free_page(g_cache.pages[j].data);
            }
            free(g_cache.pages);
            pthread_mutex_destroy(&g_cache.lock);
            errno = ENOMEM;
            return -1;
        }

        page->fd = -1;
        page->valid = false;
        page->dirty = false;
        page->reference_bit = false;
        page->queue_next = NULL;
        page->queue_prev = NULL;

        page->hash_next = g_cache.free_list;
        g_cache.free_list = page;
    }

    queue_init(&g_cache.fifo_queue);
    memset(&g_cache.hash_table, 0, sizeof(g_cache.hash_table));

    for (int i = 0; i < VTPC_MAX_OPEN_FILES; i++) {
        g_cache.files[i].in_use = false;
        g_cache.files[i].real_fd = -1;
        g_cache.files[i].path = NULL;
    }

    g_cache.cache_hits = 0;
    g_cache.cache_misses = 0;
    g_cache.pages_evicted = 0;
    g_cache.pages_written_back = 0;
    g_cache.pages_used = 0;

    g_cache.initialized = true;
    g_cache.use_direct = 1;

    return 0;
}

void vtpc_destroy(void) {
    if (!g_cache.initialized) {
        return;
    }

    pthread_mutex_lock(&g_cache.lock);

    for (int i = 0; i < VTPC_MAX_OPEN_FILES; i++) {
        if (g_cache.files[i].in_use) {
            cache_flush_file(i);

            if (g_cache.files[i].real_fd >= 0) {
                close(g_cache.files[i].real_fd);
            }
            if (g_cache.files[i].path != NULL) {
                free(g_cache.files[i].path);
            }
            g_cache.files[i].in_use = false;
        }
    }

    if (g_cache.pages != NULL) {
        for (size_t i = 0; i < g_cache.cache_size; i++) {
            if (g_cache.pages[i].data != NULL) {
                aligned_free_page(g_cache.pages[i].data);
            }
        }
        free(g_cache.pages);
        g_cache.pages = NULL;
    }

    g_cache.free_list = NULL;
    g_cache.initialized = false;

    pthread_mutex_unlock(&g_cache.lock);
    pthread_mutex_destroy(&g_cache.lock);
}

void vtpc_set_direct_mode(int enable) {
    if (g_cache.initialized) {
        g_cache.use_direct = enable;
    }
}

int vtpc_open(const char *path) {
    if (!g_cache.initialized) {
        if (vtpc_init(VTPC_DEFAULT_CACHE_SIZE, VTPC_DEFAULT_PAGE_SIZE) < 0) {
            return -1;
        }
    }

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_cache.lock);

    int fd = find_free_fd_slot();
    if (fd < 0) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EMFILE;
        return -1;
    }

    int flags = O_RDWR | O_CREAT;
    if (g_cache.use_direct) {
        flags |= O_DIRECT;
    }

    int real_fd = open(path, flags, 0644);
    if (real_fd < 0 && g_cache.use_direct) {
        real_fd = open(path, O_RDWR | O_CREAT, 0644);
    }
    if (real_fd < 0) {
        pthread_mutex_unlock(&g_cache.lock);
        return -1;
    }

    off_t file_size = get_file_size(real_fd);
    if (file_size < 0) {
        close(real_fd);
        pthread_mutex_unlock(&g_cache.lock);
        return -1;
    }

    file_entry_t *file = &g_cache.files[fd];
    file->real_fd = real_fd;
    file->file_offset = 0;
    file->file_size = file_size;
    file->in_use = true;
    file->path = strdup(path);

    pthread_mutex_unlock(&g_cache.lock);

    return fd;
}

int vtpc_close(int fd) {
    if (!g_cache.initialized) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_cache.lock);

    file_entry_t *file = get_file_entry(fd);
    if (file == NULL || !file->in_use) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EBADF;
        return -1;
    }

    cache_flush_file(fd);
    cache_invalidate_file(fd);

    int result = close(file->real_fd);

    if (file->path != NULL) {
        free(file->path);
        file->path = NULL;
    }

    file->real_fd = -1;
    file->in_use = false;

    pthread_mutex_unlock(&g_cache.lock);

    return result;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
    if (!g_cache.initialized) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_cache.lock);

    file_entry_t *file = get_file_entry(fd);
    if (file == NULL || !file->in_use) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EBADF;
        return -1;
    }

    off_t new_offset;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = file->file_offset + offset;
            break;
        case SEEK_END:
            new_offset = file->file_size + offset;
            break;
        default:
            pthread_mutex_unlock(&g_cache.lock);
            errno = EINVAL;
            return -1;
    }

    if (new_offset < 0) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EINVAL;
        return -1;
    }

    file->file_offset = new_offset;

    pthread_mutex_unlock(&g_cache.lock);

    return new_offset;
}

int vtpc_fsync(int fd) {
    if (!g_cache.initialized) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_cache.lock);

    file_entry_t *file = get_file_entry(fd);
    if (file == NULL || !file->in_use) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EBADF;
        return -1;
    }

    int result = cache_flush_file(fd);

    if (result == 0) {
        result = fsync(file->real_fd);
    }

    pthread_mutex_unlock(&g_cache.lock);

    return result;
}

ssize_t vtpc_read(int fd, void *buf, size_t count) {
    if (!g_cache.initialized) {
        errno = EINVAL;
        return -1;
    }

    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    pthread_mutex_lock(&g_cache.lock);

    file_entry_t *file = get_file_entry(fd);
    if (file == NULL || !file->in_use) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EBADF;
        return -1;
    }

    size_t bytes_read = 0;
    size_t page_size = g_cache.page_size;

    while (bytes_read < count) {
        if (file->file_offset >= file->file_size) {
            break;
        }

        off_t block_num = file->file_offset / (off_t)page_size;
        size_t offset_in_block = file->file_offset % page_size;

        cache_page_t *page = cache_get_page(fd, block_num, true);
        if (page == NULL) {
            pthread_mutex_unlock(&g_cache.lock);
            if (bytes_read > 0) {
                return (ssize_t)bytes_read;
            }
            return -1;
        }

        size_t available_in_page = page_size - offset_in_block;
        size_t remaining = count - bytes_read;
        size_t to_read = (available_in_page < remaining) ? available_in_page : remaining;

        if (file->file_offset + (off_t)to_read > file->file_size) {
            to_read = (size_t)(file->file_size - file->file_offset);
        }

        if (to_read == 0) {
            break;
        }

        memcpy((char *)buf + bytes_read,
               (char *)page->data + offset_in_block,
               to_read);

        bytes_read += to_read;
        file->file_offset += (off_t)to_read;
    }

    pthread_mutex_unlock(&g_cache.lock);

    return (ssize_t)bytes_read;
}

ssize_t vtpc_write(int fd, const void *buf, size_t count) {
    if (!g_cache.initialized) {
        errno = EINVAL;
        return -1;
    }

    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    pthread_mutex_lock(&g_cache.lock);

    file_entry_t *file = get_file_entry(fd);
    if (file == NULL || !file->in_use) {
        pthread_mutex_unlock(&g_cache.lock);
        errno = EBADF;
        return -1;
    }

    size_t bytes_written = 0;
    size_t page_size = g_cache.page_size;

    while (bytes_written < count) {
        off_t block_num = file->file_offset / (off_t)page_size;
        size_t offset_in_block = file->file_offset % page_size;

        bool need_load = false;
        size_t remaining = count - bytes_written;

        if (offset_in_block != 0) {
            need_load = true;
        } else if (remaining < page_size && file->file_offset < file->file_size) {
            need_load = true;
        }

        cache_page_t *page = cache_get_page(fd, block_num, need_load);
        if (page == NULL) {
            pthread_mutex_unlock(&g_cache.lock);
            if (bytes_written > 0) {
                return (ssize_t)bytes_written;
            }
            return -1;
        }

        size_t available_in_page = page_size - offset_in_block;
        size_t to_write = (available_in_page < remaining) ? available_in_page : remaining;

        memcpy((char *)page->data + offset_in_block,
               (char *)buf + bytes_written,
               to_write);

        page->dirty = true;
        page->reference_bit = true;

        bytes_written += to_write;
        file->file_offset += (off_t)to_write;

        if (file->file_offset > file->file_size) {
            file->file_size = file->file_offset;
        }
    }

    pthread_mutex_unlock(&g_cache.lock);

    return (ssize_t)bytes_written;
}

int vtpc_get_stats(vtpc_stats_t *stats) {
    if (!g_cache.initialized || stats == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_cache.lock);

    stats->cache_hits = g_cache.cache_hits;
    stats->cache_misses = g_cache.cache_misses;
    stats->pages_evicted = g_cache.pages_evicted;
    stats->pages_written_back = g_cache.pages_written_back;
    stats->current_pages_used = g_cache.pages_used;

    pthread_mutex_unlock(&g_cache.lock);

    return 0;
}

void vtpc_reset_stats(void) {
    if (!g_cache.initialized) {
        return;
    }

    pthread_mutex_lock(&g_cache.lock);

    g_cache.cache_hits = 0;
    g_cache.cache_misses = 0;
    g_cache.pages_evicted = 0;
    g_cache.pages_written_back = 0;

    pthread_mutex_unlock(&g_cache.lock);
}