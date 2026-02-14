#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "vtpc_internal.h"

cache_state_t g_cache = { .initialized = false };

void queue_init(page_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

void queue_push_back(page_queue_t *q, cache_page_t *page) {
    page->queue_next = NULL;
    page->queue_prev = q->tail;

    if (q->tail != NULL) {
        q->tail->queue_next = page;
    } else {
        /* Queue rỗng */
        q->head = page;
    }

    q->tail = page;
    q->count++;
}

cache_page_t *queue_pop_front(page_queue_t *q) {
    if (q->head == NULL) {
        return NULL;
    }

    cache_page_t *page = q->head;

    q->head = page->queue_next;
    if (q->head != NULL) {
        q->head->queue_prev = NULL;
    } else {
        q->tail = NULL;
    }

    page->queue_next = NULL;
    page->queue_prev = NULL;
    q->count--;

    return page;
}

void queue_remove(page_queue_t *q, cache_page_t *page) {
    if (page->queue_prev != NULL) {
        page->queue_prev->queue_next = page->queue_next;
    } else {
        q->head = page->queue_next;
    }

    if (page->queue_next != NULL) {
        page->queue_next->queue_prev = page->queue_prev;
    } else {
        q->tail = page->queue_prev;
    }

    page->queue_next = NULL;
    page->queue_prev = NULL;
    q->count--;
}

void queue_move_to_back(page_queue_t *q, cache_page_t *page) {
    if (page == q->tail) {
        return;
    }

    queue_remove(q, page);

    queue_push_back(q, page);
}

uint32_t hash_function(int fd, off_t block_num) {
    uint64_t key = ((uint64_t)fd << 32) | (uint64_t)(block_num & 0xFFFFFFFF);

    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    key = key ^ (key >> 31);

    return (uint32_t)(key % HASH_TABLE_SIZE);
}

void hash_insert(cache_page_t *page) {
    uint32_t idx = hash_function(page->fd, page->block_num);

    page->hash_next = g_cache.hash_table.buckets[idx];
    g_cache.hash_table.buckets[idx] = page;
}

void hash_remove(cache_page_t *page) {
    uint32_t idx = hash_function(page->fd, page->block_num);
    cache_page_t **pp = &g_cache.hash_table.buckets[idx];

    while (*pp != NULL) {
        if (*pp == page) {
            *pp = page->hash_next;
            page->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

cache_page_t *hash_lookup(int fd, off_t block_num) {
    uint32_t idx = hash_function(fd, block_num);
    cache_page_t *page = g_cache.hash_table.buckets[idx];

    while (page != NULL) {
        if (page->fd == fd && page->block_num == block_num && page->valid) {
            return page;
        }
        page = page->hash_next;
    }

    return NULL;
}

int cache_flush_page(cache_page_t *page) {
    /* Không cần flush nếu không dirty */
    if (!page->valid || !page->dirty) {
        return 0;
    }

    file_entry_t *file = get_file_entry(page->fd);
    if (file == NULL || !file->in_use) {
        errno = EBADF;
        return -1;
    }

    ssize_t written = direct_write_block(
        file->real_fd,
        page->block_num,
        page->data,
        g_cache.page_size
    );

    if (written < 0) {
        return -1;
    }

    page->dirty = false;
    g_cache.pages_written_back++;

    return 0;
}

int cache_flush_file(int fd) {
    int result = 0;

    for (size_t i = 0; i < g_cache.cache_size; i++) {
        cache_page_t *page = &g_cache.pages[i];

        if (page->valid && page->fd == fd && page->dirty) {
            if (cache_flush_page(page) < 0) {
                result = -1;
            }
        }
    }

    return result;
}

void cache_invalidate_file(int fd) {
    for (size_t i = 0; i < g_cache.cache_size; i++) {
        cache_page_t *page = &g_cache.pages[i];

        if (page->valid && page->fd == fd) {
            hash_remove(page);

            queue_remove(&g_cache.fifo_queue, page);

            page->valid = false;
            page->fd = -1;
            page->dirty = false;
            page->reference_bit = false;

            page->hash_next = g_cache.free_list;
            g_cache.free_list = page;

            g_cache.pages_used--;
        }
    }
}

cache_page_t *cache_evict_page(void) {
    if (g_cache.free_list != NULL) {
        cache_page_t *page = g_cache.free_list;
        g_cache.free_list = page->hash_next;
        page->hash_next = NULL;
        return page;
    }

    while (g_cache.fifo_queue.count > 0) {
        cache_page_t *page = queue_pop_front(&g_cache.fifo_queue);

        if (page == NULL) {
            break;
        }

        if (page->reference_bit) {

            page->reference_bit = false;
            queue_push_back(&g_cache.fifo_queue, page);

            continue;
        }

        if (page->dirty) {
            if (cache_flush_page(page) < 0) {
                queue_push_back(&g_cache.fifo_queue, page);
                continue;
            }
        }

        hash_remove(page);

        page->valid = false;
        page->fd = -1;
        page->dirty = false;
        page->reference_bit = false;

        g_cache.pages_evicted++;
        g_cache.pages_used--;

        return page;
    }

    errno = ENOMEM;
    return NULL;
}

cache_page_t *cache_find_page(int fd, off_t block_num) {
    cache_page_t *page = hash_lookup(fd, block_num);

    if (page != NULL) {
        page->reference_bit = true;
        g_cache.cache_hits++;
    }

    return page;
}

cache_page_t *cache_get_page(int fd, off_t block_num, bool load_from_disk) {
    cache_page_t *page = cache_find_page(fd, block_num);
    if (page != NULL) {
        return page;
    }

    g_cache.cache_misses++;

    page = cache_evict_page();
    if (page == NULL) {
        return NULL;
    }

    page->fd = fd;
    page->block_num = block_num;
    page->valid = true;
    page->dirty = false;
    page->reference_bit = true;

    hash_insert(page);

    queue_push_back(&g_cache.fifo_queue, page);

    g_cache.pages_used++;

    if (load_from_disk) {
        file_entry_t *file = get_file_entry(fd);
        if (file == NULL || !file->in_use) {
            hash_remove(page);
            queue_remove(&g_cache.fifo_queue, page);
            page->valid = false;
            page->fd = -1;
            page->hash_next = g_cache.free_list;
            g_cache.free_list = page;
            g_cache.pages_used--;

            errno = EBADF;
            return NULL;
        }

        memset(page->data, 0, g_cache.page_size);

        ssize_t bytes_read = direct_read_block(
            file->real_fd,
            block_num,
            page->data,
            g_cache.page_size
        );

        (void)bytes_read;
    } else {
        memset(page->data, 0, g_cache.page_size);
    }

    return page;
}
