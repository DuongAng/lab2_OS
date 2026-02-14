#ifndef VTPC_INTERNAL_H
#define VTPC_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

#define VTPC_MAX_OPEN_FILES 256
#define VTPC_DEFAULT_CACHE_SIZE 64
#define VTPC_DEFAULT_PAGE_SIZE 4096
#define HASH_TABLE_SIZE 256

typedef struct cache_page {
    int fd;
    off_t block_num;
    
    void *data;
    
    bool valid;
    bool dirty;
    bool reference_bit;
    
    struct cache_page *queue_next;
    struct cache_page *queue_prev;
    
    struct cache_page *hash_next;
    
} cache_page_t;

typedef struct {
    cache_page_t *head;
    cache_page_t *tail;
    size_t count;
} page_queue_t;

typedef struct {
    cache_page_t *buckets[HASH_TABLE_SIZE];
} page_hash_table_t;

typedef struct {
    int real_fd;
    off_t file_offset;
    off_t file_size;
    bool in_use;
    char *path;
} file_entry_t;

typedef struct {
    size_t cache_size;
    size_t page_size;

    cache_page_t *pages;

    page_queue_t fifo_queue;

    cache_page_t *free_list;

    page_hash_table_t hash_table;

    file_entry_t files[VTPC_MAX_OPEN_FILES];

    size_t cache_hits;
    size_t cache_misses;
    size_t pages_evicted;
    size_t pages_written_back;
    size_t pages_used;

    pthread_mutex_t lock;

    bool initialized;
    int use_direct;

} cache_state_t;

extern cache_state_t g_cache;

cache_page_t *cache_find_page(int fd, off_t block_num);
cache_page_t *cache_get_page(int fd, off_t block_num, bool load_from_disk);

cache_page_t *cache_evict_page(void);

int cache_flush_page(cache_page_t *page);
int cache_flush_file(int fd);
void cache_invalidate_file(int fd);

void queue_init(page_queue_t *q);
void queue_push_back(page_queue_t *q, cache_page_t *page);
cache_page_t *queue_pop_front(page_queue_t *q);
void queue_remove(page_queue_t *q, cache_page_t *page);
void queue_move_to_back(page_queue_t *q, cache_page_t *page);

uint32_t hash_function(int fd, off_t block_num);
void hash_insert(cache_page_t *page);
void hash_remove(cache_page_t *page);
cache_page_t *hash_lookup(int fd, off_t block_num);

void *aligned_alloc_page(size_t page_size);
void aligned_free_page(void *ptr);
ssize_t direct_read_block(int real_fd, off_t block_num, void *buf, size_t page_size);
ssize_t direct_write_block(int real_fd, off_t block_num, const void *buf, size_t page_size);
off_t get_file_size(int real_fd);

int find_free_fd_slot(void);
file_entry_t *get_file_entry(int fd);

#endif