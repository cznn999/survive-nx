/* Survive - Wilderness survival 2.880 Unity/IL2CPP host for Nintendo Switch
 * (based on subwaysurfers_nx, MIT). */

#include "saltynx.h"
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "asset_pack.h"
#include "dlog.h"
#include "sv_game.h"

#define DATA_ROOT  GAME_HOME
#define LIB_MAIN   "lib/arm64-v8a/libmain.so"
#define LIB_UNITY  "lib/arm64-v8a/libunity.so"
#define LIB_IL2CPP "lib/arm64-v8a/libil2cpp.so"

/* Force FMOD output type 22 (OpenSL ES) instead of Java AudioTrack type 21. */
#define SS_FMOD_SETOUTPUT_SITE OFF_FMOD_SetOutput
#define SS_FMOD_SETOUTPUT_FROM 0x2A1503E1u    /* mov w1, w21 (requested type) */

/* Bypass the two waits driven by Android Choreographer callbacks. */
#define SS_CHOREO_WAIT_SITE    OFF_FrameTimeTracker_wait /* FrameTimeTracker ctor wait-loop exit test */
#define SS_CHOREO_WAIT_FROM    0xb50000a8u  /* cbnz x8, +0x14 */
#define SS_CHOREO_WAIT_TO      0x14000005u  /* b +0x14 (fall through; object fully constructed) */
#define SS_WAITVSYNC_SITE      OFF_WaitVSync_wait       /* WaitVSync counter-wait exit test */
#define SS_WAITVSYNC_FROM      0x540000aau  /* b.ge +0x14 */
#define SS_WAITVSYNC_TO        0x14000005u  /* b +0x14 (don't wait for the frozen vsync counter) */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
extern void oc_exclude_range(void *base, size_t len);
void  *g_oc_pool_base = NULL;
size_t g_oc_pool_size = 0;
int    g_oc_want = 0;
u64    g_stack_base = 0, g_stack_size = 0;

#define TLS_GUARD_SLOTS 96
#define TLS_GUARD_MAX   128

static Handle g_tls_guard_slots[TLS_GUARD_SLOTS];
static Handle g_tls_guard_sentinels[TLS_GUARD_MAX - TLS_GUARD_SLOTS];
static size_t g_tls_guard_slot_count;
static size_t g_tls_guard_sentinel_count;
static int g_tls_guard_ready;
static Mutex g_tls_guard_lock;
static uint8_t g_tls_guard_stack[0x1000] __attribute__((aligned(0x1000)));

extern Result __real_svcCreateThread(Handle *out, void *entry, void *arg,
                                     void *stack_top, int prio, int cpuid);

static void tls_guard_entry(void *arg) {
  (void)arg;
  svcExitThread();
}

static size_t tls_pages_in_stack(void) {
  if (!g_stack_base || !g_stack_size) return 0;
  const u64 end = g_stack_base + g_stack_size;
  size_t pages = 0;
  for (u64 addr = g_stack_base; addr < end; ) {
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    u64 next = mi.addr + mi.size;
    if (next <= addr) break;
    if (mi.type == MemType_ThreadLocal) pages += (size_t)(mi.size / 0x1000);
    addr = next < end ? next : end;
  }
  return pages;
}

static void tls_guard_prepare(void) {
  size_t initial_pages = tls_pages_in_stack();
  if (!initial_pages) return;

  while (g_tls_guard_slot_count < TLS_GUARD_SLOTS) {
    size_t before = tls_pages_in_stack();
    Handle handle = INVALID_HANDLE;
    Result rc = __real_svcCreateThread(&handle, (void *)tls_guard_entry, NULL,
                                       g_tls_guard_stack + sizeof g_tls_guard_stack,
                                       0x2C, -2);
    if (R_FAILED(rc)) break;

    size_t after = tls_pages_in_stack();
    if (after > before) {
      if (g_tls_guard_sentinel_count >=
          sizeof g_tls_guard_sentinels / sizeof g_tls_guard_sentinels[0]) {
        svcCloseHandle(handle);
        break;
      }
      g_tls_guard_sentinels[g_tls_guard_sentinel_count++] = handle;
    } else {
      g_tls_guard_slots[g_tls_guard_slot_count++] = handle;
    }
  }

  g_tls_guard_ready = g_tls_guard_slot_count != 0;
}

static void tls_guard_release_slots(void) {
  mutexLock(&g_tls_guard_lock);
  while (g_tls_guard_slot_count)
    svcCloseHandle(g_tls_guard_slots[--g_tls_guard_slot_count]);
  g_tls_guard_ready = 0;
  mutexUnlock(&g_tls_guard_lock);
}

/* A kernel thread object freed by svcCloseHandle (a guard slot, or a thread
 * that just ended) is released a little later, so the next create can fail
 * on the thread limit for a moment: retry for up to ~0.5 s. Without this the
 * first job-system worker sometimes failed (pthread_create 12) and the
 * engine waited for it forever on a black screen. */
static Result create_thread_retry(Handle *out, void *entry, void *arg,
                                  void *stack_top, int prio, int cpuid) {
  Result rc = __real_svcCreateThread(out, entry, arg, stack_top, prio, cpuid);
  for (int i = 0; R_FAILED(rc) && i < 100; i++) {
    svcSleepThread(5000000ull);
    rc = __real_svcCreateThread(out, entry, arg, stack_top, prio, cpuid);
  }
  if (R_FAILED(rc)) dlog("[thr] svcCreateThread failed: 0x%x", rc);
  return rc;
}

Result __wrap_svcCreateThread(Handle *out, void *entry, void *arg,
                              void *stack_top, int prio, int cpuid) {
  if (!g_tls_guard_ready)
    return create_thread_retry(out, entry, arg, stack_top, prio, cpuid);

  mutexLock(&g_tls_guard_lock);
  if (g_tls_guard_slot_count)
    svcCloseHandle(g_tls_guard_slots[--g_tls_guard_slot_count]);
  Result rc = create_thread_retry(out, entry, arg, stack_top, prio, cpuid);
  mutexUnlock(&g_tls_guard_lock);
  return rc;
}

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Supply TimeManager::Update with a monotonic clock when Android frame timing
 * is unavailable. The hook preserves the original 2022.3 prologue fields. */
static void (*g_unity_update_body)(void *, double) = NULL; /* 0x4410f8 Update body */
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;
static Mutex   g_clock_lock;
static volatile uint64_t g_last_main_tick_ns = 0;
#define CLOCK_STALL_NS 100000000ULL
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall   = (double)(now - g_clk_base_ns) / 1e9;
  double sref   = *(volatile double *)((char *)tm + 0xe8);
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  g_last_main_tick_ns = nx_now_ns();
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return;
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
/* Keep time advancing while the render thread waits on synchronous scene work. */
static Thread g_clock_thr;
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);
    void *tm = g_tm;
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      nx_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_start_clock_thread(void) {
  Result create_rc = 0, start_rc = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    memset(&g_clock_thr, 0, sizeof g_clock_thr);
    create_rc = threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL,
                             0x8000, 0x2C, -2);
    if (R_SUCCEEDED(create_rc)) {
      start_rc = threadStart(&g_clock_thr);
      if (R_SUCCEEDED(start_rc)) return;
      threadClose(&g_clock_thr);
    } else {
      start_rc = create_rc;
    }
    svcSleepThread(25000000ULL);
  }
  fatal_error("Could not start the engine clock thread (%08x/%08x).",
              create_rc, start_rc);
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  if (so_patch_code((void *)(ub + OFF_TimeManager_Update_entry), stub, sizeof stub) != 0)
    fatal_error("Could not patch the Unity clock.");
}

/* Address space reserved for the three Android modules. */
#define SO_REGION_BYTES (192u * 1024 * 1024)

/* Partition the process heap into managed, commit-pool, module and mmap zones. */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;
  const size_t MB = 1024 * 1024;
  int overridden = envHasHeapOverride();

  if (overridden) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
  }

  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Back sparse mmap reservations from free stack-region holes. */
  {
    const size_t managed = OC_MANAGED_BYTES, pool = OC_POOL_BYTES;
    size_t heap_total = 0;
    if (!overridden) {
      size_t oc_heap = managed + pool + so_zone + OC_OVERFLOW_BYTES;
      if (oc_heap + 64 * MB < size && R_SUCCEEDED(svcSetHeapSize(&addr, oc_heap)))
        heap_total = oc_heap;
    } else if (size >= managed + pool + so_zone + 128 * MB) {
      heap_total = size;
    }
    if (heap_total) {
      uintptr_t hs = (uintptr_t)addr;
      uintptr_t heap_end   = hs + heap_total;
      uintptr_t arena_base = ALIGN_MEM(hs + managed + pool + so_zone, MMAP_ARENA_ALIGN);
      size_t overflow = (heap_end > arena_base)
                        ? ((heap_end - arena_base) & ~(size_t)(MMAP_ARENA_ALIGN - 1)) : 0;
      fake_heap_start   = (char *)addr;
      fake_heap_end     = (char *)addr + managed;
      g_oc_pool_base    = (void *)(hs + managed);
      g_oc_pool_size    = pool;
      heap_so_base      = (void *)ALIGN_MEM(hs + managed + pool, 0x1000);
      heap_so_limit     = so_zone;
      g_mmap_arena_base = (void *)arena_base;
      g_mmap_arena_size = overflow;
      g_oc_want         = 1;
      return;
    }
  }

  /* Heap-backed mode for constrained launch environments. */
  if (!overridden) {
    if (R_FAILED(svcSetHeapSize(&addr, size)))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    /* Preserve enough newlib heap for IL2CPP allocations. */
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * ARENA_CAP_PCT) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) { unlink(path); return; }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[768];
    struct stat st;
    snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
    else unlink(child);
  }
  closedir(dir);
  rmdir(path);
}

static void create_asset_skeleton(void) {
  static const char *dirs[] = {
    "assets", "assets/bin", "assets/bin/Data",
    "assets/bin/Data/Managed", "assets/bin/Data/Managed/Metadata",
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char path[768];
    snprintf(path, sizeof path, DATA_ROOT "/%s", dirs[i]);
    if (mkdir(path, 0777) < 0 && errno != EEXIST)
      fatal_error("Could not prepare the packed asset layout.");
  }
}

static int has_suffix(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcmp(s + a - b, suffix);
}

/* Accept old staged releases once, then keep the normal APK extraction tree. */
static void migrate_legacy_modules(void) {
  const char *names[] = { "libmain.so", "libunity.so", "libil2cpp.so" };
  mkdir(DATA_ROOT "/lib", 0777);
  mkdir(DATA_ROOT "/lib/arm64-v8a", 0777);
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
    char old_path[768], new_path[768];
    struct stat st;
    snprintf(old_path, sizeof old_path, "%s/%s", DATA_ROOT, names[i]);
    snprintf(new_path, sizeof new_path, "%s/lib/arm64-v8a/%s", DATA_ROOT, names[i]);
    if (stat(new_path, &st) == 0) { unlink(old_path); continue; }
    if (stat(old_path, &st) == 0 && rename(old_path, new_path) != 0)
      fatal_error("Could not migrate %s to the extracted APK layout.", names[i]);
  }
}

/* Remove files supplied by Android packaging but unused by the native host. */
static void cleanup_apk_extract(void) {
  static const char *diagnostics[] = {
    "wallet_debug.log", "save_debug.log", "mmap_debug.log",
    "bootstrap_telemetry.csv", "pack_io_telemetry.csv", ".offline_consent_v1",
  };
  static const char *dirs[] = {
    "META-INF", "res", "kotlin", "explorestack", "google", "okhttp3", "org", "src",
    "lib/armeabi-v7a", "assets/ad-viewer", "assets/bm_networks", "assets/dexopt",
  };
  static const char *files[] = {
    "base.apk", "AndroidManifest.xml", "resources.arsc", "DebugProbesKt.bin",
    "androidsupportmultidexversion.txt", "info.txt", "stamp-cert-sha256",
    "age-signals.properties", "billing.properties", "client_analytics.proto",
    "core-common.properties", "firebase-analytics.properties",
    "firebase-annotations.properties", "firebase-encoders-proto.properties",
    "firebase-encoders.properties", "firebase-iid-interop.properties",
    "firebase-iid.properties", "firebase-measurement-connector.properties",
    "messaging_event_extension.proto", "messaging_event.proto",
    "play-services-ads-api.properties", "play-services-ads-identifier.properties",
    "play-services-ads.properties", "play-services-appset.properties",
    "play-services-base.properties", "play-services-basement.properties",
    "play-services-cloud-messaging.properties", "play-services-cronet.properties",
    "play-services-games-v2.properties", "play-services-location.properties",
    "play-services-measurement-api.properties", "play-services-measurement-base.properties",
    "play-services-measurement-impl.properties", "play-services-measurement-sdk-api.properties",
    "play-services-measurement-sdk.properties", "play-services-measurement.properties",
    "play-services-nearby.properties", "play-services-places-placereport.properties",
    "play-services-stats.properties", "play-services-tasks.properties",
    "user-messaging-platform.properties", "version.properties",
    "assets/audience_network.dex", "assets/com.moloco.sdk.xenoss.sdkdevkit.mraid.js",
    "assets/fyb_iframe_endcard_tmpl.html", "assets/fyb_static_endcard_tmpl.html",
    "assets/ia_js_load_monitor.txt", "assets/ia_mraid_bridge.txt",
    "assets/mbridge_download_dialog_view.xml", "assets/mraid-bridge.js",
    "assets/mraid.js", "assets/rv_binddatas.xml",
  };
  for (unsigned i = 0; i < sizeof(diagnostics) / sizeof(*diagnostics); i++) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, diagnostics[i]);
    unlink(path);
  }
  if (access(DATA_ROOT "/AndroidManifest.xml", F_OK) != 0 &&
      access(DATA_ROOT "/classes.dex", F_OK) != 0 &&
      access(DATA_ROOT "/base.apk", F_OK) != 0 &&
      access(DATA_ROOT "/res", F_OK) != 0)
    return;
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, dirs[i]);
    remove_tree(path);
  }
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    unlink(path);
  }

  DIR *root = opendir(DATA_ROOT);
  if (root) {
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
      const char *n = entry->d_name;
      if (!strncmp(n, "classes", 7) && has_suffix(n, ".dex")) {
        char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, n);
        unlink(path);
      }
    }
    closedir(root);
  }

  DIR *libs = opendir(DATA_ROOT "/lib/arm64-v8a");
  if (libs) {
    struct dirent *entry;
    while ((entry = readdir(libs)) != NULL) {
      const char *n = entry->d_name;
      if (!strcmp(n, ".") || !strcmp(n, "..") ||
          !strcmp(n, "libmain.so") || !strcmp(n, "libunity.so") ||
          !strcmp(n, "libil2cpp.so")) continue;
      char path[768];
      snprintf(path, sizeof path, "%s/lib/arm64-v8a/%s", DATA_ROOT, n);
      unlink(path);
    }
    closedir(libs);
  }
}

static int data_file_stat(const char *relative, uint64_t *size) {
  if (asset_pack_active() && !strncmp(relative, "assets/", 7))
    return asset_pack_stat_relative(relative + 7, size, NULL);
  char path[768];
  struct stat st;
  snprintf(path, sizeof path, DATA_ROOT "/%s", relative);
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (size) *size = (uint64_t)st.st_size;
  return 1;
}


/* the first `head` bytes of a data file contain `needle` (head 0: file exists) */
static int file_head_contains(const char *relative, size_t head, const char *needle) {
  if (!head) return data_file_stat(relative, NULL);
  char buf[256];
  size_t got = 0;
  if (head > sizeof buf) head = sizeof buf;
  if (asset_pack_active() && !strncmp(relative, "assets/", 7)) {
    int fd = asset_pack_open_path(relative);
    if (fd < 0) return 0;
    long r = asset_pack_read_fd(fd, buf, head);
    asset_pack_close_fd(fd);
    got = r > 0 ? (size_t)r : 0;
  } else {
    char path[768];
    snprintf(path, sizeof path, DATA_ROOT "/%s", relative);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    got = fread(buf, 1, head, f);
    fclose(f);
  }
  size_t n = strlen(needle);
  for (size_t i = 0; i + n <= got; i++)
    if (!memcmp(buf + i, needle, n)) return 1;
  return 0;
}

/* a (big) plain file contains `needle`, read in chunks */
static int file_contains_big(const char *relative, const char *needle) {
  char path[768];
  snprintf(path, sizeof path, DATA_ROOT "/%s", relative);
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  static char buf[1 << 16];
  size_t n = strlen(needle), keep = 0;
  int found = 0;
  for (;;) {
    size_t got = fread(buf + keep, 1, sizeof buf - keep, f);
    if (!got) break;
    size_t have = keep + got;
    for (size_t i = 0; !found && i + n <= have; i++)
      found = !memcmp(buf + i, needle, n);
    if (found) break;
    keep = n - 1 < have ? n - 1 : have;
    memmove(buf, buf + have - keep, keep);
  }
  fclose(f);
  return found;
}

static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/data.unity3d",
  };
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    if (!data_file_stat(files[i], NULL)) {
      /* say what the SD card has on the way to it: a folder copied with the
       * archive bit set reads as a file on the Switch, and a nested extract
       * folder shows up here too */
      char report[1536], part[768];
      size_t used = (size_t)snprintf(report, sizeof report, "Missing data file:\n%s\n\n", files[i]);
      const char *rel = files[i];
      for (const char *p = rel; ; p++) {
        if (*p && *p != '/') continue;
        snprintf(part, sizeof part, DATA_ROOT "/%.*s", (int)(p - rel), rel);
        struct stat st;
        int r = stat(part, &st);
        const char *what = r != 0 ? "missing" : S_ISDIR(st.st_mode) ? "folder" : S_ISREG(st.st_mode)
                                                 ? "file" : "other";
        if (used < sizeof report)
          used += (size_t)snprintf(report + used, sizeof report - used, "  %s: %s\n",
                                   part + 5, what);
        if (!*p || r != 0) break;
      }
      DIR *dir = opendir(DATA_ROOT);
      if (dir && used < sizeof report) {
        used += (size_t)snprintf(report + used, sizeof report - used, "\nIn " GAME_HOME_POSIX ":");
        struct dirent *e;
        int n = 0;
        while ((e = readdir(dir)) && n < 12 && used < sizeof report) {
          used += (size_t)snprintf(report + used, sizeof report - used, " %s", e->d_name);
          n++;
        }
        closedir(dir);
      }
      if (used < sizeof report)
        snprintf(report + used, sizeof report - used,
                 "\n\nExtract the whole APK into " GAME_HOME_POSIX "/. If the folders are there,\n"
                 "fix the archive bit (Hekate: Tools > Arch bit > Fix Archive Bit).");
      fatal_error("%s", report);
    }
  }
  /* data.unity3d: "UnityFS\0", format 8, "5.x.x\0", then the engine version */
  if (!file_head_contains(LIB_UNITY, 0, "") ||
      !file_head_contains("assets/bin/Data/data.unity3d", 64, "2022.3.62f2"))
    fatal_error("The extracted game is not Survive " SV_VERSION_NAME " / Unity 2022.3.62f2.");
  if (!file_contains_big(LIB_UNITY, "2022.3.62f2 (7670c08855a9)"))
    fatal_error("libunity.so is not Unity 2022.3.62f2 (7670c08855a9).");
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  resolve_module_imports(mod);
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* Reduce Unity allocator regions from 256 MB to 64 MB. All original words are
 * verified before the 21-site patch is applied. (The sites of subwaysurfers_nx's
 * libunity, 0xFC5A8 lower in this build of 2022.3.62f2.) */
static void nx_patch_unity_regions(uintptr_t ub) {
  static const struct { uint32_t off, from, to; } P[] = {
    /* VirtualAllocator block-table index math. */
    {0x39edc8, 0xd35cdc33, 0xd35ad433}, {0x39edcc, 0xd35cfd15, 0xd35afd15},
    {0x39ee5c, 0x52a20008, 0x52a08008}, {0x39f198, 0xd35cfc28, 0xd35afc28},
    {0x39f1a8, 0x92646c28, 0x92667428}, {0x39f1b0, 0xd35c9c2a, 0xd35a942a},
    {0x39f1c4, 0xb25c6feb, 0xb25e77eb}, {0x39f1c8, 0xd35cdc29, 0xd35ad429}, /* COMPOSITE A */
    {0x39f1cc, 0xf2a2000b, 0xf2a0800b}, {0x39f20c, 0xcb0a7108, 0xcb0a6908}, /* COMPOSITE A */
    {0x39f234, 0xd35c9c29, 0xd35a9429}, {0x3a0f00, 0xd35c9e89, 0xd35a9689},
    /* Region rounding and DynamicHeap size. */
    {0x39cdc0, 0x12be000a, 0x12bf800a}, {0x39cdc8, 0x92648d36, 0x92669536},
    {0x398344, 0x12be0009, 0x12bf8009}, {0x39834c, 0x92648d36, 0x92669536},
    {0x39c914, 0xd35cfd29, 0xd35afd29}, {0x39c918, 0x52a2000a, 0x52a0800a},
    /* TLS and bucket size clamps. */
    {0x398be8, 0xd35cfc28, 0xd35afc28}, {0x398bec, 0x52a20009, 0x52a08009},
    {0x39a644, 0x52a20009, 0x52a08009},
  };
  const int N = (int)(sizeof P / sizeof P[0]);
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur != P[i].from) fatal_error("Unsupported libunity.so memory layout.");
  }
  for (int i = 0; i < N; i++) {
    if (so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to) != 0)
      fatal_error("Could not patch libunity.so memory layout.");
  }
}

/* Provide the Android application paths that Unity normally gets from Context.
 * The two Application path icalls left in this build (dataPath and
 * temporaryCachePath are stripped) start with sub sp, sp, #0x50. */
#define SV_PATHICALL_FROM  0xd10143ffu
static void *(*g_il2cpp_string_new)(const char *) = NULL;
/* A fresh managed string per call: a cached one is not a GC root here. */
static void *sv_persistentdatapath_hook(void) {
  return g_il2cpp_string_new ? g_il2cpp_string_new(GAME_HOME_POSIX "/save") : NULL;
}
/* Android returns a URL here (jar:file://...!/assets) and Unity Gaming
 * Services fetch their config from it with UnityWebRequest; a bare path
 * became https://localhost/... and the services never initialized. */
static void *sv_streamingassetspath_hook(void) {
  return g_il2cpp_string_new ? g_il2cpp_string_new("file://" GAME_HOME_POSIX "/assets") : NULL;
}
static void nx_patch_path_icall(uintptr_t ub, uint32_t off, void *hook) {
  volatile uint32_t *site = (volatile uint32_t *)(ub + off);
  if (*site != SV_PATHICALL_FROM) fatal_error("Unsupported libunity.so.");
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)hook & 0xffffffffu),
    (uint32_t)((uintptr_t)hook >> 32),
  };
  if (so_patch_code((void *)site, stub, sizeof stub) != 0)
    fatal_error("Could not patch libunity.so.");
}

static void nx_install_persistentdatapath_hook(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_il2cpp_string_new = (void *(*)(const char *))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_string_new");
  if (!g_il2cpp_string_new) fatal_error("il2cpp_string_new is unavailable.");
  nx_patch_path_icall(ub, OFF_Application_get_persistentDataPath, (void *)&sv_persistentdatapath_hook);
  nx_patch_path_icall(ub, OFF_Application_get_streamingAssetsPath, (void *)&sv_streamingassetspath_hook);
  mkdir(GAME_HOME "/save", 0777);
}

int main(int argc, char *argv[]) {
  saltynx_start();
  atexit(saltynx_stop);
  (void)argc; (void)argv;
  int optimized_assets = 0;
  {
    extern int g_net_on;
    g_net_on = R_SUCCEEDED(socketInitializeDefault()) ? 1 : 0;
  }
  dlog("[boot] survive_nx " PORT_VERSION " (Survive " SV_VERSION_NAME ")");
  {
    /* leftovers from earlier versions: Unity's file-system probe and the
     * old on-card copies of /dev/urandom and /proc data (.synth*) */
    static char names[32][128];
    int n = 0, removed = 0;
    DIR *d = opendir(GAME_HOME);
    struct dirent *e;
    while (d && n < 32 && (e = readdir(d)) != NULL)
      if ((!strncasecmp(e->d_name, "CASESENSITIVETEST", 17) || !strncmp(e->d_name, ".synth", 6)) &&
          strlen(e->d_name) < 128)
        strcpy(names[n++], e->d_name);
    if (d) closedir(d);
    for (int i = 0; i < n; i++) {
      char p[512];
      snprintf(p, sizeof p, "%.380s/%.127s", GAME_HOME, names[i]);
      if (!unlink(p)) removed++;
    }
    if (removed) dlog("[boot] removed %d leftover files", removed);
  }
  startup_status_begin("Preparing game data");

  /* Unity and IL2CPP use paths relative to the game directory. */
  if (chdir(DATA_ROOT) != 0) fatal_error("Could not enter %s.", DATA_ROOT);
  migrate_legacy_modules();
  {
    struct stat loose;
    if (stat(DATA_ROOT "/assets/bin/Data/globalgamemanagers", &loose) != 0 ||
        !S_ISREG(loose.st_mode))
      asset_pack_open_existing(DATA_ROOT);
  }

  /* Load language selection and rewrite stale configurations. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    int rc = read_config(cfg);
    if (rc != 0) write_config(cfg);
  }
  g_debug_log = config.debug;
  if (g_debug_log) {
    dlog("[boot] debug log on");
    dlog_start_watchdog();
  } else {
    unlink(GAME_HOME "/watchdog.log");
  }

  startup_status_update("Validating game data");
  check_data();
  startup_status_update("Removing unused Android files");
  cleanup_apk_extract();
  if (!asset_pack_active()) {
    startup_status_update("Optimizing game assets (first boot)");
    if (!asset_pack_build(DATA_ROOT "/assets", DATA_ROOT))
      fatal_error("Could not optimize the extracted assets.\n%s\nThe original files were kept.",
                  asset_pack_error());
    optimized_assets = 1;
  }
  {
    struct stat st;
    if (stat(DATA_ROOT "/assets/bin/Data/globalgamemanagers", &st) == 0 &&
        S_ISREG(st.st_mode)) {
      startup_status_update("Removing unpacked asset files");
      remove_tree(DATA_ROOT "/assets");
      create_asset_skeleton();
    }
  }
  if (optimized_assets)
    startup_status_complete("Game data is ready.\n\n  Launch Survive again to play.");
  dlog("[boot] data ok (asset pack %s)", asset_pack_active() ? "active" : "off");
  startup_status_update("Starting the game");

  /* Re-extract IL2CPP resources because writable file mappings are not persisted. */
  unlink(DATA_ROOT "/il2cpp/unity.ver");
  unlink(DATA_ROOT "/il2cpp/Metadata/global-metadata.dat");
  unlink(DATA_ROOT "/il2cpp/Resources/mscorlib.dll-resources.dat");

  check_syscalls();

  /* Back large mmap reservations with reserved stack-region holes. */
  if (g_oc_want == 1 && g_oc_pool_base) {
    svcGetInfo(&g_stack_base, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&g_stack_size, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
    tls_guard_prepare();
  }
  nx_start_clock_thread();
  {
    uintptr_t ps = (uintptr_t)g_oc_pool_base, pe = ps + g_oc_pool_size;
    int pool_in_stack = g_stack_base && g_oc_pool_base &&
                        !(pe <= (uintptr_t)g_stack_base || ps >= (uintptr_t)(g_stack_base + g_stack_size));
    if (g_oc_want == 1 && g_oc_pool_base && g_stack_base &&
        g_stack_size >= (u64)OC_MIN_STACK_MB * 1024 * 1024 && !pool_in_stack) {
      uintptr_t rbase = ((uintptr_t)g_stack_base + (MMAP_ARENA_ALIGN - 1)) & ~(uintptr_t)(MMAP_ARENA_ALIGN - 1);
      size_t rsize = ((size_t)g_stack_size - (rbase - (uintptr_t)g_stack_base)) & ~(size_t)(MMAP_ARENA_ALIGN - 1);
      if (oc_arena_init((void *)rbase, rsize, g_oc_pool_base, g_oc_pool_size)) {
        struct { u64 base; u64 size; } holes[24];
        int nh = 0;
        for (u64 a = rbase, end = rbase + rsize; a < end && nh < 24; ) {
          MemoryInfo mi; u32 pi;
          if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
          u64 me = mi.addr + mi.size;
          if (me <= a) break;
          if (mi.type == MemType_Unmapped) {
            u64 hb = mi.addr < rbase ? rbase : mi.addr;
            u64 he = me > end ? end : me;
            if (he > hb) { holes[nh].base = hb; holes[nh].size = he - hb; nh++; }
          }
          a = me;
        }
        int big = 0;
        for (int i = 1; i < nh; i++) if (holes[i].size > holes[big].size) big = i;
        const size_t leave = (size_t)OC_STACK_LEAVE_MB * 1024 * 1024;
        for (int i = 0; i < nh; i++) {
          u64 hb = holes[i].base;
          size_t hsz = (size_t)holes[i].size;
          size_t reserve_len = hsz;
          if (i == big && hsz > leave + MMAP_ARENA_ALIGN) {
            reserve_len = (hsz - leave) & ~(MMAP_ARENA_ALIGN - 1);
            oc_exclude_range((void *)(hb + reserve_len), hsz - reserve_len);
          }
          virtmemLock();
          VirtmemReservation *rv = virtmemAddReservation((void *)hb, reserve_len);
          virtmemUnlock();
          if (!rv) oc_exclude_range((void *)hb, reserve_len);
        }
        g_oc_want = 2;
      }
    }
  }
  if (g_oc_want == 2) {
    tls_guard_release_slots();
  }

  /* Match the surface to the physical display. */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1920; screen_height = 1080; }
  else                                                         { screen_width = 1280; screen_height = 720;  }

  startup_status_end();
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());

  dlog("[boot] screen %dx%d, oc arena %d", screen_width, screen_height, g_oc_want);
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  dlog("[boot] libmain %p, libunity %p, libil2cpp %p", main_mod.load_virtbase, unity_mod.load_virtbase, il2cpp_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);

  /* Route FMOD through the OpenSL ES shim instead of Java AudioTrack. */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *site = (volatile uint32_t *)(ub + SS_FMOD_SETOUTPUT_SITE);
    if (*site == SS_FMOD_SETOUTPUT_FROM) {
      uint32_t req_opensl = 0x528002C1u;
      if (so_patch_code((void *)site, &req_opensl, sizeof req_opensl) != 0)
        fatal_error("Could not patch the Unity audio output.");
    } else {
      fatal_error("Unsupported libunity.so at FMOD patch site.");
    }
  }

  /* Apply allocator patches before any module constructors run. */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  /* No Android Frame Pacing (Swappy): it needs the Android Choreographer and
   * the EGL_ANDROID presentation-time extensions. Its init half-fails here
   * (no EGL wrapper), and SwappyGL::swap then calls through a null EGL table
   * (libunity+0xaecaec, 2026-11-13). Unity's own test for it answers false
   * now, so the engine presents with eglSwapBuffers itself. */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *site = (volatile uint32_t *)(ub + OFF_Unity_UseSwappy);
    if (site[0] != 0xa9bf4ffeu || site[1] != 0xf0005813u)
      fatal_error("Unsupported libunity.so (frame pacing).");
    static const uint32_t no[2] = {0x2a1f03e0u /* mov w0, wzr */, 0xd65f03c0u /* ret */};
    if (so_patch_code((void *)site, no, sizeof no) != 0)
      fatal_error("Could not patch libunity.so (frame pacing).");
  }

  nx_install_persistentdatapath_hook();

  /* Main thread runs init_array + the engine lifecycle; give it its own stable
   * bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  dlog("[boot] patches applied; module constructors");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  sv_game_install();
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);

  dlog("[boot] constructors done");
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();

  /* Resolve the UnityPlayer JNI entry points. */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Initialize libunity's internal JNI manager. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    Unity_JNI_OnLoad(fake_vm, NULL);
  }

  /* Register the JavaVM through IL2CPP's exported Android entry point. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Il2Cpp_JNI_OnLoad = (fn_jnionload)
      so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (!Il2Cpp_JNI_OnLoad || Il2Cpp_JNI_OnLoad(fake_vm, NULL) != 0x00010006)
      fatal_error("Could not initialize IL2CPP JNI support.");
  }

  dlog("[boot] JNI_OnLoad done; initJni");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  dlog("[boot] nativeRecreateGfxState");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* Enter the resumed and focused lifecycle states before rendering. */
  dlog("[boot] nativeResume");
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);

  /* Configure IL2CPP and frame pacing before the first render. */
  {
    /* The Android collector depends on POSIX signals unavailable on Switch. */
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (!il2cpp_gc_set_mode || !il2cpp_gc_disable)
      fatal_error("Required IL2CPP GC exports are missing.");
    il2cpp_gc_set_mode(1);
    il2cpp_gc_disable();

    nx_install_time_fix();

    /* Bypass Android Choreographer waits. */
    {
      uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
      static const struct { uint32_t off, from, to; } CH[] = {
        { SS_CHOREO_WAIT_SITE, SS_CHOREO_WAIT_FROM, SS_CHOREO_WAIT_TO },
        { SS_WAITVSYNC_SITE,   SS_WAITVSYNC_FROM,   SS_WAITVSYNC_TO   },
      };
      for (unsigned i = 0; i < sizeof(CH)/sizeof(CH[0]); i++) {
        volatile uint32_t *site = (volatile uint32_t *)(ub + CH[i].off);
        if (*site != CH[i].from) fatal_error("Unsupported libunity.so frame pacing.");
        uint32_t to = CH[i].to;
        if (so_patch_code((void *)site, &to, sizeof to) != 0)
          fatal_error("Could not patch Unity frame pacing.");
      }
    }
  }
  dlog("[boot] frame loop");
  dlog_frame(0);
  unsigned long long frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    dlog_frame(++frame);
    android_native_update_mode();
    android_native_vibration_update();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) {
      dlog("[boot] nativeRender returned false at frame %llu", frame);
      break;
    }
    sv_game_frame();
  }

  dlog("[boot] leaving (quit requested %d)", (int)jni_quit_requested);
  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  android_native_vibration_shutdown();
  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
