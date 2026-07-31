/*
 * ab_wasi_stubs.c -- the symbols that keep an interpreter's wasm importing
 * ONLY ab_host.
 *
 * Every language runtime here is built with emscripten in STANDALONE_WASM
 * mode, and every one of them drags the same three unwanted imports unless
 * these definitions exist. The host provides ab_host and nothing else, so an
 * `env` or `wasi_snapshot_preview1` import is not a warning -- the module
 * fails to instantiate.
 *
 * Each stub below is here because a real build failed without it.
 */
#include <stdint.h>
#include <stddef.h>

/* ALLOW_MEMORY_GROWTH emits this in standalone mode. Nobody outside the
 * module needs the notification. */
void emscripten_notify_memory_growth(int index) { (void)index; }

/* libc's clock forwarding stub. A bezel has no wall clock -- time comes from
 * the host via time_elapsed_ms -- so this returns a monotonic counter that
 * is honest about not being real time, and keeps replays deterministic. */
unsigned short __wasi_clock_time_get(unsigned int id, unsigned long long precision,
                                     unsigned long long *out) {
  static unsigned long long fake_ns = 0;
  (void)id; (void)precision;
  fake_ns += 1000000ull;               /* one millisecond per call */
  if (out) *out = fake_ns;
  return 0;
}

/* Interpreters that keep a `print`/`puts` path alive reach for these. There
 * is no stdio in a bezel; the runtime's own log binding goes to ab_log. */
unsigned short __wasi_fd_write(int fd, const void *iovs, size_t iovs_len, size_t *nwritten) {
  (void)fd; (void)iovs; (void)iovs_len;
  if (nwritten) *nwritten = 0;
  return 0;
}

unsigned short __wasi_fd_close(int fd) { (void)fd; return 0; }

unsigned short __wasi_fd_seek(int fd, int64_t offset, int whence, uint64_t *newoffset) {
  (void)fd; (void)offset; (void)whence;
  if (newoffset) *newoffset = 0;
  return 0;
}

unsigned short __wasi_fd_read(int fd, const void *iovs, size_t iovs_len, size_t *nread) {
  (void)fd; (void)iovs; (void)iovs_len;
  if (nread) *nread = 0;
  return 0;                            /* EOF: there is nothing to read */
}

unsigned short __wasi_fd_fdstat_get(int fd, void *stat) {
  (void)fd; (void)stat;
  return 8;                            /* WASI EBADF */
}

/* proc_exit would terminate the whole host session. A guest that calls
 * exit() should stop being a guest, not take the emulator with it -- the
 * runtime's error panel is the honest failure mode. */
void __wasi_proc_exit(int code) { (void)code; for (;;) { } }
