// tsfs.cpp — Step 1: Foundation
// A virtual folder that tracks metadata (size, mtime) for files "created" inside it.
// No real disk writes yet — that's Step 2 (ring buffer) and Step 3 (mmap).

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include "ring_buffer.h"

// --- In-memory metadata table -----------------------------------------
// Every "file" in our virtual folder is just an entry here.
// Step 2 will replace the `data` buffer with a ring-buffer-backed log.
struct FileMeta {
    size_t size = 0;
    time_t ctime = 0;
    time_t mtime = 0;
    std::string data; // placeholder storage for Step 1 only
};

static std::mutex g_table_mutex;                 // protects g_files
static std::map<std::string, FileMeta> g_files;  // path -> metadata

// --- Step 2: producer-consumer log engine -------------------------------
// write() (producer, runs on the kernel's FUSE call thread) pushes here and
// returns immediately. One background thread (consumer) drains it and
// applies the bytes to g_files. This decouples "accept a write" from
// "persist a write" — the actual point of this exercise.
static RingBuffer   g_ring(4096);
static std::thread  g_flusher_thread;

static void flusher_loop() {
    LogPacket pkt;
    while (g_ring.pop(pkt)) {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        auto it = g_files.find(pkt.path);
        if (it == g_files.end()) continue; // file was deleted before we got to it

        FileMeta &meta = it->second;
        if (meta.data.size() < pkt.offset + pkt.data.size())
            meta.data.resize(pkt.offset + pkt.data.size());
        std::memcpy(&meta.data[pkt.offset], pkt.data.data(), pkt.data.size());
        meta.size  = meta.data.size();
        meta.mtime = time(nullptr);
    }
}

// --- getattr: "what do you know about this path?" ----------------------
// Called by the kernel for ls -l, stat(), file managers, etc.
static int tsfs_getattr(const char *path, struct stat *st,
                         struct fuse_file_info *) {
    std::memset(st, 0, sizeof(struct stat));

    if (std::strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_files.find(path);
    if (it == g_files.end()) {
        return -ENOENT; // "no such file" — kernel understands this errno
    }

    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = it->second.size;
    st->st_ctime = it->second.ctime;
    st->st_mtime = it->second.mtime;
    return 0;
}

// --- readdir: "what's inside this directory?" ---------------------------
static int tsfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t, struct fuse_file_info *, enum fuse_readdir_flags) {
    if (std::strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", nullptr, 0, (fuse_fill_dir_flags)0);
    filler(buf, "..", nullptr, 0, (fuse_fill_dir_flags)0);

    std::lock_guard<std::mutex> lock(g_table_mutex);
    for (auto &entry : g_files) {
        // strip leading "/" for the filler call
        filler(buf, entry.first.c_str() + 1, nullptr, 0, (fuse_fill_dir_flags)0);
    }
    return 0;
}

// --- create: called when something does `open(O_CREAT)` / `touch file` --
static int tsfs_create(const char *path, mode_t, struct fuse_file_info *) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    FileMeta meta;
    meta.ctime = meta.mtime = time(nullptr);
    g_files[path] = meta;
    return 0;
}

// --- open: "can this path be opened?" -----------------------------------
static int tsfs_open(const char *path, struct fuse_file_info *) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (g_files.find(path) == g_files.end())
        return -ENOENT;
    return 0;
}

// --- write: Step 2 — enqueue and return immediately ----------------------
// The FUSE call thread does NOT touch g_files here. It just builds a packet
// and hands it to the ring buffer. The flusher thread applies it later.
// Tradeoff: a read() that lands microseconds after this write() may not see
// the bytes yet, if the flusher hasn't drained the queue. Acceptable for an
// append-only telemetry stream; not acceptable for a general-purpose FS.
static int tsfs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *) {
    {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        if (g_files.find(path) == g_files.end())
            return -ENOENT;
        // Optimistically report the size as if applied, so `ls -l` right
        // after a write looks sane even before the flusher catches up.
        FileMeta &meta = g_files[path];
        if (meta.size < offset + (off_t)size) meta.size = offset + size;
        meta.mtime = time(nullptr);
    }
    g_ring.push(LogPacket{path, std::string(buf, size), offset});
    return (int)size;
}

// --- read: so you can `cat` a file back and see it worked ---------------
static int tsfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_files.find(path);
    if (it == g_files.end())
        return -ENOENT;

    if ((size_t)offset >= it->second.data.size())
        return 0;
    size_t avail = it->second.data.size() - offset;
    size_t to_copy = std::min(size, avail);
    std::memcpy(buf, it->second.data.data() + offset, to_copy);
    return (int)to_copy;
}

// --- truncate: mmap requires the backing file to already be the target
// size before you can map it (you can't mmap a 0-byte file and expect
// space to write into). ftruncate() from the test program lands here.
static int tsfs_truncate(const char *path, off_t size, struct fuse_file_info *) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_files.find(path);
    if (it == g_files.end())
        return -ENOENT;
    it->second.data.resize(size, 0);
    it->second.size = size;
    it->second.mtime = time(nullptr);
    return 0;
}

static struct fuse_operations tsfs_ops = {};

int main(int argc, char *argv[]) {
    tsfs_ops.getattr  = tsfs_getattr;
    tsfs_ops.readdir  = tsfs_readdir;
    tsfs_ops.open     = tsfs_open;
    tsfs_ops.create   = tsfs_create;
    tsfs_ops.write    = tsfs_write;
    tsfs_ops.read     = tsfs_read;
    tsfs_ops.truncate = tsfs_truncate;

    g_flusher_thread = std::thread(flusher_loop);

    int ret = fuse_main(argc, argv, &tsfs_ops, nullptr);

    g_ring.shutdown();
    g_flusher_thread.join();
    return ret;
}
