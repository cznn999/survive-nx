/* dlog.c -- sdmc:/switch/survive_nx/debug.log: start-up steps, the engine's
 * Android log, Java calls the port answers generically, a heartbeat, and a
 * dump of any CPU exception (registers, code addresses named by module). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "dlog.h"
#include "so_util.h"

static Mutex g_lock;
static FsFile g_file;
/* config.txt "debug 1": file/Java/thread traces, frame marks and the
 * watchdog (watchdog.log). Off: only start-up, game state and errors. */
int g_debug_log;
static int g_open; /* 0 not tried, 1 open, -1 failed */
static s64 g_off;
static unsigned g_lines;
#define MAX_LINES 20000

/* Written through the FS service directly, not newlib: the engine's threads
 * use newlib's file table all the time, and a second writer there (this log)
 * is not safe. A line is flushed as it is written. */
static void open_log(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  static const char path[] = GAME_HOME_POSIX "/debug.log";
  g_open = -1;
  if (!fs) return;
  fsFsDeleteFile(fs, path);
  if (R_FAILED(fsFsCreateFile(fs, path, 0, 0))) return;
  if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &g_file))) return;
  g_open = 1;
  g_off = 0;
}

static void vlog(const char *fmt, va_list va) {
  char line[1280];
  u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
  int n = snprintf(line, sizeof line, "%6llu.%03llu ", (unsigned long long)(ms / 1000),
                   (unsigned long long)(ms % 1000));
  int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, va);
  if (m < 0) m = 0;
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof line - 2) len = sizeof line - 2;
  if (!len || line[len - 1] != '\n') line[len++] = '\n';
  /* a fault inside this function must still reach the log */
  const int nested = mutexIsLockedByCurrentThread(&g_lock);
  if (!nested) mutexLock(&g_lock);
  if (!g_open) open_log();
  if (g_open > 0 && g_lines < MAX_LINES) {
    if (R_SUCCEEDED(fsFileWrite(&g_file, g_off, line, len, FsWriteOption_Flush)))
      g_off += (s64)len;
    g_lines++;
  }
  if (!nested) mutexUnlock(&g_lock);
}

void dlog(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vlog(fmt, va);
  va_end(va);
}

/* The watchdog writes its own file (watchdog.log) with its own handle and no
 * lock, so it reports even when debug.log's writers are stuck. */
static FsFile g_wfile;
static s64 g_woff;
static int g_wopen;
static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...) {
  if (!g_wopen) {
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    static const char path[] = GAME_HOME_POSIX "/watchdog.log";
    g_wopen = -1;
    if (fs) {
      fsFsDeleteFile(fs, path);
      if (R_SUCCEEDED(fsFsCreateFile(fs, path, 0, 0)) &&
          R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &g_wfile)))
        g_wopen = 1;
    }
  }
  if (g_wopen < 0) return;
  char line[512];
  u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
  int n = snprintf(line, sizeof line, "%6llu.%03llu ", (unsigned long long)(ms / 1000),
                   (unsigned long long)(ms % 1000));
  va_list va;
  va_start(va, fmt);
  int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, va);
  va_end(va);
  size_t len = (size_t)n + (size_t)(m > 0 ? m : 0);
  if (len > sizeof line - 2) len = sizeof line - 2;
  line[len++] = '\n';
  if (R_SUCCEEDED(fsFileWrite(&g_wfile, g_woff, line, len, FsWriteOption_Flush))) g_woff += (s64)len;
}

static volatile unsigned long long g_frame;
static volatile u64 g_frame_tick;

void dlog_frame(unsigned long long frame) {
  g_frame = frame;
  g_frame_tick = armGetSystemTick();
  if (g_debug_log && (frame == 1 || frame == 10 || frame == 60 || frame % 600 == 0))
    dlog("[frame] %llu", frame);
}

static const char *name_addr(u64 a, char *buf, size_t cap);
static int readable(u64 a);
static void dump_guest_threads(void);

/* Watchdog: when a frame has not ended for 10 s, pause the main thread for a
 * moment and write where it is (pc, lr and the frame-pointer chain); again
 * every 20 s while it stays stuck. */
static void watchdog(void *arg) {
  (void)arg;
  Handle main = envGetMainThreadHandle();
  u64 last_dump = 0;
  wlog("[watchdog] running");
  for (int tick = 0;; tick++) {
    svcSleepThread(1000000000ull);
    u64 t = g_frame_tick, now = armGetSystemTick();
    if (tick % 2 == 0)
      wlog("[alive] frame %llu, debug.log lock 0x%x, %d lines", g_frame, (unsigned)g_lock, g_lines);
    /* every 30 s, where every engine thread is (a load that never ends shows
     * here while frames still run) */
    if (tick % 30 == 29) {
      wlog("[watchdog] periodic thread dump at frame %llu", g_frame);
      dump_guest_threads();
    }
    if (!g_frame_tick || armTicksToNs(now - t) < 6000000000ull) continue;
    if (last_dump && armTicksToNs(now - last_dump) < 15000000000ull) continue;
    last_dump = now;
    if (R_FAILED(svcSetThreadActivity(main, ThreadActivity_Paused))) {
      wlog("[watchdog] frame %llu stuck; main thread could not be paused", g_frame);
      continue;
    }
    ThreadContext c;
    Result rc = svcGetThreadContext3(&c, main);
    svcSetThreadActivity(main, ThreadActivity_Runnable);
    if (R_FAILED(rc)) { wlog("[watchdog] context: 0x%x", rc); continue; }
    char b[64];
    wlog("[watchdog] frame %llu not finished for %llu s; main thread:", g_frame,
         (unsigned long long)(armTicksToNs(now - t) / 1000000000ull));
    wlog("[watchdog]   pc %016llx %s", (unsigned long long)c.pc.x, name_addr(c.pc.x, b, sizeof b));
    wlog("[watchdog]   lr %016llx %s", (unsigned long long)c.lr, name_addr(c.lr, b, sizeof b));
    for (int i = 0; i < 8; i++)
      wlog("[watchdog]   x%d %016llx %s", i, (unsigned long long)c.cpu_gprs[i].x,
           name_addr(c.cpu_gprs[i].x, b, sizeof b));
    u64 fp = c.fp;
    for (int i = 0; i < 24 && readable(fp); i++) {
      u64 ret = ((u64 *)(uintptr_t)fp)[1];
      wlog("[watchdog]   #%-2d %016llx %s", i, (unsigned long long)ret, name_addr(ret, b, sizeof b));
      u64 next = ((u64 *)(uintptr_t)fp)[0];
      if (next <= fp) break;
      fp = next;
    }
    /* the frame-pointer chain breaks in libc++/libnx code: also list every
     * stack word that points into the game libraries (return addresses) */
    {
      u64 sp = c.sp, hits = 0;
      for (int i = 0; i < 4096 && hits < 32 && readable(sp + i * 8); i++) {
        u64 v = ((u64 *)(uintptr_t)sp)[i];
        name_addr(v, b, sizeof b);
        if (!strncmp(b, "lib", 3)) {
          wlog("[watchdog]   sp+0x%-5x %s", i * 8, b);
          hits++;
        }
      }
    }
    dump_guest_threads();
  }
}

void dlog_thread_join(void *entry) {
  char b[64];
  wlog("[thread] joining the thread started at %s", name_addr((u64)(uintptr_t)entry, b, sizeof b));
}

static void dump_thread(Handle h, const char *what) {
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
    wlog("[watchdog] %s: could not be paused", what);
    return;
  }
  ThreadContext c;
  Result rc = svcGetThreadContext3(&c, h);
  svcSetThreadActivity(h, ThreadActivity_Runnable);
  if (R_FAILED(rc)) { wlog("[watchdog] %s: context 0x%x", what, rc); return; }
  char b[64], l[64];
  wlog("[watchdog] %s: pc %s lr %s", what, name_addr(c.pc.x, b, sizeof b), name_addr(c.lr, l, sizeof l));
  u64 fp = c.fp;
  for (int i = 0; i < 10 && readable(fp); i++) {
    u64 ret = ((u64 *)(uintptr_t)fp)[1];
    wlog("[watchdog]     #%d %s", i, name_addr(ret, b, sizeof b));
    u64 next = ((u64 *)(uintptr_t)fp)[0];
    if (next <= fp) break;
    fp = next;
  }
}

static void dump_guest_threads(void) {
  for (int i = 0; i < MAX_GUEST_THREADS; i++) {
    GuestThread *t = &g_guest_threads[i];
    if (!t->alive) continue;
    char b[64], what[96];
    snprintf(what, sizeof what, "thread %d (entry %s)", i, name_addr((u64)(uintptr_t)t->entry, b, sizeof b));
    dump_thread(t->handle, what);
  }
}

void dlog_start_watchdog(void) {
  static Thread th;
  Result rc = threadCreate(&th, watchdog, NULL, NULL, 0x10000, 0x2B, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&th);
  dlog("[boot] watchdog thread: 0x%x", rc);
}

/* ---- CPU exceptions ------------------------------------------------------ */
extern so_module main_mod, unity_mod, il2cpp_mod;

static const char *name_addr(u64 a, char *buf, size_t cap) {
  static const struct { so_module *m; const char *n; } mods[] = {
      {&main_mod, "libmain.so"}, {&unity_mod, "libunity.so"}, {&il2cpp_mod, "libil2cpp.so"}};
  for (unsigned i = 0; i < 3; i++) {
    u64 b = (u64)(uintptr_t)mods[i].m->load_virtbase;
    if (b && a >= b && a < b + mods[i].m->load_size) {
      snprintf(buf, cap, "%s+0x%llx", mods[i].n, (unsigned long long)(a - b));
      return buf;
    }
  }
  /* this NRO: its text mapping starts at the image base (the ELF is linked
   * at 0, text first) */
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&name_addr))) {
    u64 base = mi.addr;
    if (a >= base && a < base + 0x4000000ull) {
      snprintf(buf, cap, "survive_nx+0x%llx", (unsigned long long)(a - base));
      return buf;
    }
  }
  snprintf(buf, cap, "?");
  return buf;
}

static int readable(u64 a) {
  MemoryInfo mi;
  u32 pi;
  return a && !(a & 7) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, a)) && (mi.perm & Perm_R) &&
         a + 16 <= mi.addr + mi.size;
}

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  char b1[64];
  dlog("[crash] ===== exception 0x%x =====", ctx->error_desc);
  dlog("[crash] pc  %016llx %s", (unsigned long long)ctx->pc.x, name_addr(ctx->pc.x, b1, sizeof b1));
  dlog("[crash] lr  %016llx %s", (unsigned long long)ctx->lr.x, name_addr(ctx->lr.x, b1, sizeof b1));
  dlog("[crash] far %016llx esr %08x sp %016llx", (unsigned long long)ctx->far.x, ctx->esr,
       (unsigned long long)ctx->sp.x);
  for (int i = 0; i < 28; i += 4)
    dlog("[crash] x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)ctx->cpu_gprs[i].x,
         (unsigned long long)ctx->cpu_gprs[i + 1].x, (unsigned long long)ctx->cpu_gprs[i + 2].x,
         (unsigned long long)ctx->cpu_gprs[i + 3].x);
  dlog("[crash] x28 %016llx fp %016llx", (unsigned long long)ctx->cpu_gprs[28].x,
       (unsigned long long)ctx->fp.x);
  /* frame-pointer chain: [fp] = previous fp, [fp+8] = return address */
  u64 fp = ctx->fp.x;
  for (int i = 0; i < 24 && readable(fp); i++) {
    u64 ret = ((u64 *)(uintptr_t)fp)[1];
    dlog("[crash]   #%-2d %016llx %s", i, (unsigned long long)ret, name_addr(ret, b1, sizeof b1));
    u64 next = ((u64 *)(uintptr_t)fp)[0];
    if (next <= fp) break;
    fp = next;
  }
  svcSleepThread(200000000ull);
}

/* The engine's abort(): newlib's quits back to the menu without a word. Log
 * who called it and the game-library return addresses on the stack, then
 * show the error. */
extern void fatal_error(const char *fmt, ...);
__attribute__((noreturn)) void abort_fake(void) {
  char b[64], c[64];
  u64 lr = (u64)(uintptr_t)__builtin_return_address(0);
  dlog("[abort] abort() called from %s", name_addr(lr, b, sizeof b));
  u64 sp = (u64)(uintptr_t)__builtin_frame_address(0);
  int hits = 0;
  for (int i = 0; i < 4096 && hits < 32 && readable(sp + i * 8); i++) {
    u64 v = ((u64 *)(uintptr_t)sp)[i];
    name_addr(v, c, sizeof c);
    if (!strncmp(c, "lib", 3)) { dlog("[abort]   sp+0x%-5x %s", i * 8, c); hits++; }
  }
  fatal_error("The game stopped (abort from %s). See debug.log.", b);
  for (;;) svcSleepThread(1000000000ull);
}
