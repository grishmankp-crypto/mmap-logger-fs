// mmap_writer.cpp — Step 3: prove zero-copy writes actually happen.
//
// This program never calls write(). It opens a file on your FUSE mount,
// ftruncate()s it to a target size, mmap()s it, and writes sensor floats
// directly into that mapped memory. The kernel's page cache owns the
// pages; your FUSE driver's write() only gets called later, during
// writeback (msync or page eviction) — proving the copy was eliminated
// from the hot path.
//
// Usage: ./mmap_writer <path-inside-mount> <num-samples>

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <path> <num_samples>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    long n = atol(argv[2]);
    size_t region_size = n * sizeof(double);

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }

    if (ftruncate(fd, region_size) != 0) { perror("ftruncate"); return 1; }

    void *map = mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    double *samples = (double *)map;

    // --- The actual "zero-copy" write path ---
    // No write() syscall anywhere in this loop. Every store is a plain
    // memory write into page-cache-backed pages.
    for (long i = 0; i < n; ++i) {
        samples[i] = 10.00 + 0.01 * (i % 100); // fake sensor readings
    }

    // Ask the kernel to flush dirty pages back to our FUSE driver now
    // (otherwise it happens whenever the kernel feels like it).
    if (msync(map, region_size, MS_SYNC) != 0) { perror("msync"); return 1; }

    printf("Wrote %ld samples (%zu bytes) via mmap, zero write() syscalls.\n", n, region_size);

    munmap(map, region_size);
    close(fd);
    return 0;
}
