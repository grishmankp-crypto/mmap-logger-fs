// write_baseline.cpp — same workload as mmap_writer, but using write() calls,
// so you get a real (not fabricated) before/after comparison.
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <path> <num_samples>\n", argv[0]); return 1; }
    const char *path = argv[1];
    long n = atol(argv[2]);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }

    for (long i = 0; i < n; ++i) {
        double v = 10.00 + 0.01 * (i % 100);
        write(fd, &v, sizeof(v)); // one syscall + one copy per sample
    }
    fsync(fd);
    close(fd);
    printf("Wrote %ld samples via write().\n", n);
    return 0;
}
