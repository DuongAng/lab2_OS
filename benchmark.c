#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "vtpc.h"

#define BENCH_FILE      "benchmark_data.tmp"
#define FILE_SIZE       (64 * 1024 * 1024)  /* 64 MB */
#define PAGE_SIZE       4096
#define CACHE_PAGES     256                  /* 1 MB cache */
#define NUM_RANDOM_OPS  10000

static long long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/**
 * Tạo benchmark file
 */
static void create_bench_file(void) {
    printf("Creating benchmark file (%d MB)...\n", FILE_SIZE / (1024 * 1024));

    int fd = open(BENCH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to create benchmark file");
        exit(1);
    }

    char *buf = malloc(PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; i++) {
        buf[i] = (char)(i % 256);
    }

    size_t written = 0;
    while (written < FILE_SIZE) {
        ssize_t w = write(fd, buf, PAGE_SIZE);
        if (w <= 0) break;
        written += w;
    }

    fsync(fd);
    close(fd);
    free(buf);

    printf("Done.\n\n");
}

static double bench_seq_read_direct(void) {
    void *buf = NULL;
    posix_memalign(&buf, PAGE_SIZE, PAGE_SIZE);

    int fd = open(BENCH_FILE, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fd = open(BENCH_FILE, O_RDONLY);
    }

    long long start = get_time_us();

    size_t total = 0;
    while (total < FILE_SIZE) {
        ssize_t r = read(fd, buf, PAGE_SIZE);
        if (r <= 0) break;
        total += r;
    }

    long long end = get_time_us();

    close(fd);
    free(buf);

    return (double)(end - start) / 1000.0;
}

static double bench_rand_read_direct(int num_ops, int max_pages) {
    void *buf = NULL;
    posix_memalign(&buf, PAGE_SIZE, PAGE_SIZE);

    int fd = open(BENCH_FILE, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fd = open(BENCH_FILE, O_RDONLY);
    }

    off_t *offsets = malloc(num_ops * sizeof(off_t));
    srand(12345);
    for (int i = 0; i < num_ops; i++) {
        offsets[i] = (off_t)(rand() % max_pages) * PAGE_SIZE;
    }

    long long start = get_time_us();

    for (int i = 0; i < num_ops; i++) {
        lseek(fd, offsets[i], SEEK_SET);
        read(fd, buf, PAGE_SIZE);
    }

    long long end = get_time_us();

    close(fd);
    free(buf);
    free(offsets);

    return (double)(end - start) / 1000.0;
}

static double bench_seq_read_vtpc(void) {
    char *buf = malloc(PAGE_SIZE);

    int fd = vtpc_open(BENCH_FILE);
    if (fd < 0) {
        perror("vtpc_open");
        free(buf);
        return -1;
    }

    long long start = get_time_us();

    size_t total = 0;
    while (total < FILE_SIZE) {
        ssize_t r = vtpc_read(fd, buf, PAGE_SIZE);
        if (r <= 0) break;
        total += r;
    }

    long long end = get_time_us();

    vtpc_close(fd);
    free(buf);

    return (double)(end - start) / 1000.0;
}

static double bench_rand_read_vtpc(int num_ops, int max_pages) {
    char *buf = malloc(PAGE_SIZE);

    int fd = vtpc_open(BENCH_FILE);
    if (fd < 0) {
        perror("vtpc_open");
        free(buf);
        return -1;
    }

    off_t *offsets = malloc(num_ops * sizeof(off_t));
    srand(12345);
    for (int i = 0; i < num_ops; i++) {
        offsets[i] = (off_t)(rand() % max_pages) * PAGE_SIZE;
    }

    long long start = get_time_us();

    for (int i = 0; i < num_ops; i++) {
        vtpc_lseek(fd, offsets[i], SEEK_SET);
        vtpc_read(fd, buf, PAGE_SIZE);
    }

    long long end = get_time_us();

    vtpc_close(fd);
    free(buf);
    free(offsets);

    return (double)(end - start) / 1000.0;
}

static void print_result(const char *name, double direct_ms, double vtpc_ms) {
    double speedup = direct_ms / vtpc_ms;

    printf("%-40s | %10.2f ms | %10.2f ms | ", name, direct_ms, vtpc_ms);

    if (speedup >= 1.0) {
        printf("\033[32m%6.2fx faster\033[0m\n", speedup);
    } else {
        printf("\033[31m%6.2fx slower\033[0m\n", 1.0 / speedup);
    }
}

int main(void) {
    printf("\n");

    printf("File size:   %d MB\n", FILE_SIZE / (1024 * 1024));
    printf("Page size:   %d bytes\n", PAGE_SIZE);
    printf("Cache size:  %d pages (%d KB)\n", CACHE_PAGES, CACHE_PAGES * PAGE_SIZE / 1024);
    printf("========================================================\n\n");

    create_bench_file();

    vtpc_destroy();
    if (vtpc_init(CACHE_PAGES, PAGE_SIZE) < 0) {
        perror("vtpc_init");
        return 1;
    }

    printf("%-40s | %12s | %12s | %s\n",
           "Benchmark", "Direct I/O", "VTPC", "Result");
    printf("------------------------------------------------------------------------\n");

    {
        double t_direct = bench_seq_read_direct();
        double t_vtpc = bench_seq_read_vtpc();
        print_result("Sequential read (64 MB)", t_direct, t_vtpc);
    }

    {
        int total_pages = FILE_SIZE / PAGE_SIZE;
        double t_direct = bench_rand_read_direct(NUM_RANDOM_OPS, total_pages);
        double t_vtpc = bench_rand_read_vtpc(NUM_RANDOM_OPS, total_pages);
        print_result("Random read (10K ops, no locality)", t_direct, t_vtpc);
    }

    {
        double t_direct = bench_rand_read_direct(NUM_RANDOM_OPS, 64);
        double t_vtpc = bench_rand_read_vtpc(NUM_RANDOM_OPS, 64);
        print_result("Random read (10K ops, 64 hot pages)", t_direct, t_vtpc);
    }

    {
        double t_direct = bench_rand_read_direct(NUM_RANDOM_OPS, 512);
        double t_vtpc = bench_rand_read_vtpc(NUM_RANDOM_OPS, 512);
        print_result("Random read (10K ops, 512 pages)", t_direct, t_vtpc);
    }

    printf("\n");

    vtpc_stats_t stats;
    vtpc_get_stats(&stats);

    printf("Cache Statistics:\n");
    printf("  Cache hits:       %zu\n", stats.cache_hits);
    printf("  Cache misses:     %zu\n", stats.cache_misses);
    printf("  Hit rate:         %.2f%%\n",
           100.0 * stats.cache_hits / (stats.cache_hits + stats.cache_misses));
    printf("  Pages evicted:    %zu\n", stats.pages_evicted);
    printf("  Pages written:    %zu\n", stats.pages_written_back);
    printf("\n");

    vtpc_destroy();
    unlink(BENCH_FILE);

    printf("Benchmark complete.\n\n");

    return 0;
}
