#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "vtpc.h"

#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_BLOCK_COUNT 1000
#define CACHE_PAGES 256

typedef enum {
    MODE_READ,
    MODE_WRITE,
    MODE_SEARCH_REPLACE
} AccessMode;

typedef enum {
    TYPE_SEQUENTIAL,
    TYPE_RANDOM
} AccessType;

typedef struct {
    AccessMode mode;
    size_t block_size;
    size_t block_count;
    char filename[256];
    long range_start;
    long range_end;
    int use_direct;
    AccessType access_type;
    int search_value;
    int replace_value;
    int iterations;
    int use_vtpc;
} Config;

void generate_file(Config *cfg) {
    int fd = open(cfg->filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Cannot create file");
        exit(1);
    }

    printf("Generating file '%s' with %zu blocks of %zu bytes...\n",
           cfg->filename, cfg->block_count, cfg->block_size);

    void *buffer;
    if (posix_memalign(&buffer, 512, cfg->block_size) != 0) {
        perror("posix_memalign failed");
        exit(1);
    }

    srand(time(NULL));

    for (size_t i = 0; i < cfg->block_count; i++) {
        int *int_buffer = (int *)buffer;
        size_t int_count = cfg->block_size / sizeof(int);

        for (size_t j = 0; j < int_count; j++) {
            int_buffer[j] = rand() % 100000;
        }

        if (write(fd, buffer, cfg->block_size) != (ssize_t)cfg->block_size) {
            perror("Write failed");
            free(buffer);
            close(fd);
            exit(1);
        }
    }

    free(buffer);
    fsync(fd);
    close(fd);
    printf("File generated successfully!\n\n");
}

int search_and_replace_direct(Config *cfg) {
    int found_count = 0;
    int replaced_count = 0;

    int flags = O_RDWR;
    if (cfg->use_direct) {
        flags |= O_DIRECT;
    }

    int fd = open(cfg->filename, flags);
    if (fd < 0) {
        perror("Cannot open file");
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    size_t blocks_to_process = file_size / cfg->block_size;

    printf("[Direct I/O%s] Processing %zu blocks...\n",
           cfg->use_direct ? " + O_DIRECT" : "", blocks_to_process);

    void *buffer;
    if (posix_memalign(&buffer, 512, cfg->block_size) != 0) {
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < blocks_to_process; i++) {
        off_t offset;
        if (cfg->access_type == TYPE_RANDOM) {
            offset = (rand() % blocks_to_process) * cfg->block_size;
        } else {
            offset = i * cfg->block_size;
        }

        lseek(fd, offset, SEEK_SET);
        ssize_t bytes_read = read(fd, buffer, cfg->block_size);
        if (bytes_read != (ssize_t)cfg->block_size) continue;

        int *int_buffer = (int *)buffer;
        size_t int_count = cfg->block_size / sizeof(int);
        int modified = 0;

        for (size_t j = 0; j < int_count; j++) {
            if (int_buffer[j] == cfg->search_value) {
                int_buffer[j] = cfg->replace_value;
                found_count++;
                modified = 1;
            }
        }

        if (modified) {
            lseek(fd, offset, SEEK_SET);
            if (write(fd, buffer, cfg->block_size) == (ssize_t)cfg->block_size) {
                replaced_count++;
            }
        }
    }

    free(buffer);
    close(fd);

    printf("  Values found: %d, Blocks modified: %d\n", found_count, replaced_count);
    return found_count;
}

int search_and_replace_vtpc(Config *cfg) {
    int found_count = 0;
    int replaced_count = 0;

    int fd = vtpc_open(cfg->filename);
    if (fd < 0) {
        perror("vtpc_open failed");
        return -1;
    }

    off_t file_size = vtpc_lseek(fd, 0, SEEK_END);
    vtpc_lseek(fd, 0, SEEK_SET);
    size_t blocks_to_process = file_size / cfg->block_size;

    printf("[VTPC Cache] Processing %zu blocks...\n", blocks_to_process);

    char *buffer = malloc(cfg->block_size);
    if (buffer == NULL) {
        vtpc_close(fd);
        return -1;
    }

    for (size_t i = 0; i < blocks_to_process; i++) {
        off_t offset;
        if (cfg->access_type == TYPE_RANDOM) {
            offset = (rand() % blocks_to_process) * cfg->block_size;
        } else {
            offset = i * cfg->block_size;
        }

        vtpc_lseek(fd, offset, SEEK_SET);
        ssize_t bytes_read = vtpc_read(fd, buffer, cfg->block_size);
        if (bytes_read != (ssize_t)cfg->block_size) continue;

        int *int_buffer = (int *)buffer;
        size_t int_count = cfg->block_size / sizeof(int);
        int modified = 0;

        for (size_t j = 0; j < int_count; j++) {
            if (int_buffer[j] == cfg->search_value) {
                int_buffer[j] = cfg->replace_value;
                found_count++;
                modified = 1;
            }
        }

        if (modified) {
            vtpc_lseek(fd, offset, SEEK_SET);
            if (vtpc_write(fd, buffer, cfg->block_size) == (ssize_t)cfg->block_size) {
                replaced_count++;
            }
        }
    }

    free(buffer);
    vtpc_fsync(fd);
    vtpc_close(fd);

    printf("  Values found: %d, Blocks modified: %d\n", found_count, replaced_count);
    return found_count;
}

void print_usage(char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  --rw <write|search>           Mode (default: search)\n");
    printf("  --block_size <size>           Block size in bytes (default: 4096)\n");
    printf("  --block_count <count>         Number of blocks (default: 1000)\n");
    printf("  --file <path>                 File path (default: test_data.bin)\n");
    printf("  --direct <on|off>             Use O_DIRECT for direct mode (default: off)\n");
    printf("  --type <sequence|random>      Access type (default: sequence)\n");
    printf("  --search <value>              Value to search (default: 12345)\n");
    printf("  --replace <value>             Value to replace (default: 99999)\n");
    printf("  --iterations <count>          Iterations (default: 1)\n");
    printf("  --vtpc <on|off>               Use VTPC cache (default: off)\n");
    printf("  --vtpc_direct <on|off>        VTPC uses O_DIRECT (default: off)\n");
}

int main(int argc, char *argv[]) {
    Config cfg = {
        .mode = MODE_SEARCH_REPLACE,
        .block_size = DEFAULT_BLOCK_SIZE,
        .block_count = DEFAULT_BLOCK_COUNT,
        .filename = "test_data.bin",
        .range_start = 0,
        .range_end = 0,
        .use_direct = 0,
        .access_type = TYPE_SEQUENTIAL,
        .search_value = 12345,
        .replace_value = 99999,
        .iterations = 1,
        .use_vtpc = 0
    };

    int vtpc_direct = 0;

    if (argc > 1 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) break;

        if (strcmp(argv[i], "--rw") == 0) {
            if (strcmp(argv[i+1], "write") == 0) cfg.mode = MODE_WRITE;
            else cfg.mode = MODE_SEARCH_REPLACE;
        } else if (strcmp(argv[i], "--block_size") == 0) {
            cfg.block_size = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--block_count") == 0) {
            cfg.block_count = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--file") == 0) {
            strncpy(cfg.filename, argv[i+1], sizeof(cfg.filename) - 1);
        } else if (strcmp(argv[i], "--direct") == 0) {
            cfg.use_direct = (strcmp(argv[i+1], "on") == 0);
        } else if (strcmp(argv[i], "--type") == 0) {
            cfg.access_type = (strcmp(argv[i+1], "random") == 0) ? TYPE_RANDOM : TYPE_SEQUENTIAL;
        } else if (strcmp(argv[i], "--search") == 0) {
            cfg.search_value = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--replace") == 0) {
            cfg.replace_value = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--iterations") == 0) {
            cfg.iterations = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--vtpc") == 0) {
            cfg.use_vtpc = (strcmp(argv[i+1], "on") == 0);
        } else if (strcmp(argv[i], "--vtpc_direct") == 0) {
            vtpc_direct = (strcmp(argv[i+1], "on") == 0);
        }
    }

    printf("  EMA Replace Integer - IO Loader\n");
    printf("  Mode: %s\n", cfg.use_vtpc ? "VTPC Cache (Second Chance)" : "Direct I/O");
    if (!cfg.use_vtpc) {
        printf("  O_DIRECT: %s\n", cfg.use_direct ? "ON" : "OFF");
    } else {
        printf("  VTPC O_DIRECT: %s\n", vtpc_direct ? "ON" : "OFF");
    }
    printf("  Access: %s\n", cfg.access_type == TYPE_RANDOM ? "Random" : "Sequential");
    printf("================================================\n\n");

    if (cfg.use_vtpc) {
        if (vtpc_init(CACHE_PAGES, cfg.block_size) < 0) {
            perror("vtpc_init failed");
            return 1;
        }
        vtpc_set_direct_mode(vtpc_direct);
        printf("VTPC: %d pages, %zu bytes/page, O_DIRECT=%s\n\n",
               CACHE_PAGES, cfg.block_size, vtpc_direct ? "ON" : "OFF");
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (cfg.mode == MODE_WRITE) {
        generate_file(&cfg);
    } else {
        for (int i = 0; i < cfg.iterations; i++) {
            printf("--- Iteration %d/%d ---\n", i + 1, cfg.iterations);
            srand(12345);

            if (cfg.use_vtpc) {
                search_and_replace_vtpc(&cfg);
            } else {
                search_and_replace_direct(&cfg);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    if (cfg.use_vtpc) {
        vtpc_stats_t stats;
        vtpc_get_stats(&stats);

        printf("\n--- VTPC Statistics ---\n");
        printf("  Cache hits:    %zu\n", stats.cache_hits);
        printf("  Cache misses:  %zu\n", stats.cache_misses);
        if (stats.cache_hits + stats.cache_misses > 0) {
            printf("  Hit rate:      %.2f%%\n",
                   100.0 * stats.cache_hits / (stats.cache_hits + stats.cache_misses));
        }
        printf("  Pages evicted: %zu\n", stats.pages_evicted);
        printf("  Pages written: %zu\n", stats.pages_written_back);

        vtpc_destroy();
    }

    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;

    printf("\n================================================\n");
    printf("  Total time: %.2f ms\n", elapsed);

    return 0;
}