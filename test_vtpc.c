#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "vtpc.h"

#define TEST_FILE "test_data.tmp"
#define TEST_FILE2 "test_data2.tmp"

#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define RESET   "\033[0m"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_START(name) \
    do { \
        printf("Testing: %-45s ", name); \
        tests_run++; \
    } while(0)

#define TEST_PASS() \
    do { \
        printf("[" GREEN "PASS" RESET "]\n"); \
        tests_passed++; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        printf("[" RED "FAIL" RESET "] %s\n", msg); \
    } while(0)

static void create_test_file(const char *path, size_t size) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror("create_test_file");
        exit(1);
    }

    for (size_t i = 0; i < size; i++) {
        fputc((char)(i % 256), f);
    }

    fclose(f);
}

static void cleanup_test_files(void) {
    unlink(TEST_FILE);
    unlink(TEST_FILE2);
}

static void test_init_destroy(void) {
    TEST_START("vtpc_init and vtpc_destroy");

    vtpc_destroy();

    int result = vtpc_init(16, 4096);
    if (result != 0) {
        TEST_FAIL("vtpc_init failed");
        return;
    }

    vtpc_destroy();

    result = vtpc_init(32, 4096);
    if (result != 0) {
        TEST_FAIL("vtpc_init (reinit) failed");
        return;
    }

    vtpc_destroy();
    TEST_PASS();
}

static void test_open_close(void) {
    TEST_START("vtpc_open and vtpc_close");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 4096);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    int result = vtpc_close(fd);
    if (result != 0) {
        TEST_FAIL("vtpc_close failed");
        vtpc_destroy();
        return;
    }

    result = vtpc_close(fd);
    if (result == 0) {
        TEST_FAIL("vtpc_close should fail on closed fd");
        vtpc_destroy();
        return;
    }

    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_basic_read(void) {
    TEST_START("vtpc_read basic");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 8192);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char buf[1024];
    ssize_t bytes = vtpc_read(fd, buf, sizeof(buf));

    if (bytes != sizeof(buf)) {
        TEST_FAIL("vtpc_read wrong byte count");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    for (int i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != (char)(i % 256)) {
            TEST_FAIL("vtpc_read wrong data");
            vtpc_close(fd);
            vtpc_destroy();
            return;
        }
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_read_across_pages(void) {
    TEST_START("vtpc_read across page boundaries");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 16384);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    vtpc_lseek(fd, 4000, SEEK_SET);

    char buf[200];
    ssize_t bytes = vtpc_read(fd, buf, sizeof(buf));

    if (bytes != sizeof(buf)) {
        TEST_FAIL("vtpc_read wrong byte count");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    for (int i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != (char)((4000 + i) % 256)) {
            TEST_FAIL("vtpc_read wrong data across boundary");
            vtpc_close(fd);
            vtpc_destroy();
            return;
        }
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_basic_write(void) {
    TEST_START("vtpc_write basic");

    vtpc_destroy();
    vtpc_init(16, 4096);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char write_buf[1024];
    for (int i = 0; i < (int)sizeof(write_buf); i++) {
        write_buf[i] = (char)(i % 256);
    }

    ssize_t written = vtpc_write(fd, write_buf, sizeof(write_buf));
    if (written != sizeof(write_buf)) {
        TEST_FAIL("vtpc_write wrong byte count");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_fsync(fd);
    vtpc_close(fd);

    fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open (reopen) failed");
        vtpc_destroy();
        return;
    }

    char read_buf[1024];
    ssize_t bytes = vtpc_read(fd, read_buf, sizeof(read_buf));

    if (bytes != sizeof(read_buf)) {
        TEST_FAIL("vtpc_read wrong byte count after write");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    if (memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        TEST_FAIL("Data mismatch after write");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_seek(void) {
    TEST_START("vtpc_lseek operations");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 8192);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    off_t pos = vtpc_lseek(fd, 1000, SEEK_SET);
    if (pos != 1000) {
        TEST_FAIL("SEEK_SET failed");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    pos = vtpc_lseek(fd, 500, SEEK_CUR);
    if (pos != 1500) {
        TEST_FAIL("SEEK_CUR failed");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    pos = vtpc_lseek(fd, -100, SEEK_END);
    if (pos != 8092) {
        TEST_FAIL("SEEK_END failed");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_cache_hits(void) {
    TEST_START("Cache hits on repeated access");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 8192);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char buf[4096];
    vtpc_read(fd, buf, sizeof(buf));

    vtpc_stats_t stats1;
    vtpc_get_stats(&stats1);

    vtpc_lseek(fd, 0, SEEK_SET);
    vtpc_read(fd, buf, sizeof(buf));

    vtpc_stats_t stats2;
    vtpc_get_stats(&stats2);

    if (stats2.cache_hits <= stats1.cache_hits) {
        TEST_FAIL("No cache hit on repeated access");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_second_chance(void) {
    TEST_START("Second Chance eviction policy");

    vtpc_destroy();
    vtpc_init(4, 4096);

    create_test_file(TEST_FILE, 32768);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char buf[4096];

    for (int i = 0; i < 8; i++) {
        vtpc_lseek(fd, i * 4096, SEEK_SET);
        vtpc_read(fd, buf, sizeof(buf));
    }

    vtpc_stats_t stats;
    vtpc_get_stats(&stats);

    if (stats.pages_evicted == 0) {
        TEST_FAIL("No evictions with small cache");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_lseek(fd, 0, SEEK_SET);
    vtpc_read(fd, buf, sizeof(buf));
    vtpc_lseek(fd, 4096, SEEK_SET);
    vtpc_read(fd, buf, sizeof(buf));

    for (int i = 4; i < 8; i++) {
        vtpc_lseek(fd, i * 4096, SEEK_SET);
        vtpc_read(fd, buf, sizeof(buf));
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_fsync(void) {
    TEST_START("vtpc_fsync");

    vtpc_destroy();
    vtpc_init(16, 4096);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    vtpc_write(fd, buf, sizeof(buf));

    int result = vtpc_fsync(fd);
    if (result != 0) {
        TEST_FAIL("vtpc_fsync failed");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_stats_t stats;
    vtpc_get_stats(&stats);

    if (stats.pages_written_back == 0) {
        TEST_FAIL("fsync didn't write back dirty pages");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_multiple_files(void) {
    TEST_START("Multiple files");

    vtpc_destroy();
    vtpc_init(16, 4096);

    create_test_file(TEST_FILE, 4096);
    create_test_file(TEST_FILE2, 4096);

    int fd1 = vtpc_open(TEST_FILE);
    int fd2 = vtpc_open(TEST_FILE2);

    if (fd1 < 0 || fd2 < 0) {
        TEST_FAIL("Failed to open multiple files");
        vtpc_destroy();
        return;
    }

    if (fd1 == fd2) {
        TEST_FAIL("Same fd for different files");
        vtpc_close(fd1);
        vtpc_close(fd2);
        vtpc_destroy();
        return;
    }

    char buf1[1024], buf2[1024];
    ssize_t r1 = vtpc_read(fd1, buf1, sizeof(buf1));
    ssize_t r2 = vtpc_read(fd2, buf2, sizeof(buf2));

    if (r1 != sizeof(buf1) || r2 != sizeof(buf2)) {
        TEST_FAIL("Read from multiple files failed");
        vtpc_close(fd1);
        vtpc_close(fd2);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd1);
    vtpc_close(fd2);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void test_large_file(void) {
    TEST_START("Large file operations (1MB)");

    vtpc_destroy();
    vtpc_init(32, 4096);

    size_t file_size = 1024 * 1024;
    create_test_file(TEST_FILE, file_size);

    int fd = vtpc_open(TEST_FILE);
    if (fd < 0) {
        TEST_FAIL("vtpc_open failed");
        vtpc_destroy();
        return;
    }

    char buf[8192];
    size_t total = 0;

    while (total < file_size) {
        ssize_t bytes = vtpc_read(fd, buf, sizeof(buf));
        if (bytes <= 0) break;

        for (ssize_t i = 0; i < bytes; i++) {
            if (buf[i] != (char)((total + i) % 256)) {
                TEST_FAIL("Data corruption in large file");
                vtpc_close(fd);
                vtpc_destroy();
                return;
            }
        }

        total += bytes;
    }

    if (total != file_size) {
        TEST_FAIL("Didn't read entire file");
        vtpc_close(fd);
        vtpc_destroy();
        return;
    }

    vtpc_close(fd);
    vtpc_destroy();
    cleanup_test_files();
    TEST_PASS();
}

static void print_summary(void) {
    printf("\n");
    printf("  Results: %d/%d tests passed", tests_passed, tests_run);

    if (tests_passed == tests_run) {
        printf(" [" GREEN "ALL PASS" RESET "]\n");
    } else {
        printf(" [" RED "%d FAILED" RESET "]\n", tests_run - tests_passed);
    }
}

int main(void) {
    printf("\n");
    printf("  VTPC Test Suite (Second Chance)\n");
    printf("==========================================\n\n");

    test_init_destroy();
    test_open_close();
    test_basic_read();
    test_read_across_pages();
    test_basic_write();
    test_seek();
    test_cache_hits();
    test_second_chance();
    test_fsync();
    test_multiple_files();
    test_large_file();

    print_summary();

    return (tests_passed == tests_run) ? 0 : 1;
}