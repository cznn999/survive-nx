/* sv_game.c -- hooks into Survive's own C# code (libil2cpp.so 2.880).
 *
 * The first scene ("Entering the wilderness...") is LoadNextScene: it waits
 * for six managers to report loaded, then opens the menu. The store manager
 * reports only after Unity Gaming Services and Unity IAP (Google Play
 * billing) have been initialized; without Google Play neither ever finishes,
 * so the loading screen stays forever. These hooks log the loading state and,
 * if the store alone is still missing after a while, report it as loaded so
 * the game goes on (paid editions then fall back to the game's offline
 * purchase records).
 *
 * Addresses are RVAs of libil2cpp.so (Il2CppDumper, metadata v31). Every hook
 * site is checked against the expected instructions first; on mismatch the
 * hooks stay off.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <switch.h>
#include "so_util.h"
#include "dlog.h"
#include "sv_game.h"

extern so_module il2cpp_mod;

/* LoadNextScene (TypeDefIndex 3640) */
#define RVA_LNS_FailedToLoadStore   0xF96DD4u
#define RVA_LNS_UpdateStatusText    0xF96DE0u
#define RVA_LNS_CheckLoadNextLevel  0xF97480u
/* StaticStoreManager (TypeDefIndex 3847) */
#define RVA_SSM_OnInitialized       0xFDB680u
#define RVA_SSM_OnInitFailed1       0xFDB9B4u
#define RVA_SSM_OnInitFailed2       0xFDB9C0u
#define RVA_SSM_InitAfterAsync      0xFDB3A0u
#define RVA_SSM_InitializePurchasing 0xFDB098u
/* Free RX space: end of the text segment (0x21f23b0) to its page end. */
#define RVA_TRAMP_START             0x21F2400u
#define RVA_TRAMP_END               0x21F3000u

/* LoadNextScene fields */
#define LNS_LOCALE      0x20
#define LNS_STORE       0x21
#define LNS_GAMEMANAGER 0x22
#define LNS_SETTINGS    0x23
#define LNS_ACHIEVE     0x24
#define LNS_PLAYER      0x25
#define LNS_STOREFAILED 0x26
#define SSM_STATUS      0x28

#define STORE_FORCE_AFTER_NS (12ull * 1000000000ull)

static uintptr_t g_base;
static uintptr_t g_tramp_next;
static int g_hooks_on;

static void (*orig_UpdateStatusText)(void *self, void *mi);
static void (*orig_OnInitialized)(void *self, void *ctl, void *ext, void *mi);
static void (*orig_InitAfterAsync)(void *self, void *mi);
static void (*fn_CheckLoadNextLevel)(void *self, void *mi);

static void *volatile g_lns;          /* LoadNextScene waiting for managers */
static uint64_t g_lns_since_ns;
static uint32_t g_lns_last_flags = 0xffffffffu;
static int g_store_forced;

static uint64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- tiny AArch64 patcher ---------------------------------------------- */

static uint32_t enc_ldr_lit(int rt, int byte_off) {
  return 0x58000000u | ((uint32_t)((byte_off / 4) & 0x7ffff) << 5) | (uint32_t)rt;
}

static int is_pc_relative(uint32_t w) {
  if ((w & 0x9f000000u) == 0x10000000u) return 1;  /* adr */
  if ((w & 0x7c000000u) == 0x14000000u) return 1;  /* b / bl */
  if ((w & 0x7e000000u) == 0x34000000u) return 1;  /* cbz / cbnz */
  if ((w & 0x7e000000u) == 0x36000000u) return 1;  /* tbz / tbnz */
  if ((w & 0xff000010u) == 0x54000000u) return 1;  /* b.cond */
  if ((w & 0x3b000000u) == 0x18000000u) return 1;  /* ldr literal */
  return 0;
}

/* Put `words` into the free RX area; returns its address or 0. */
static uintptr_t tramp_put(const uint32_t *words, size_t n) {
  uintptr_t at = (g_tramp_next + 15) & ~(uintptr_t)15;
  size_t len = n * 4;
  if (at + len > g_base + RVA_TRAMP_END) return 0;
  for (size_t i = 0; i < len; i++)
    if (((volatile uint8_t *)at)[i] != 0) return 0;
  if (so_patch_code((void *)at, words, len) != 0) return 0;
  g_tramp_next = at + len;
  return at;
}

/* Jump from `site` (12 bytes) to `fn`. Returns 0 on success. When `orig` is
 * given, a trampoline running the 3 replaced instructions and continuing at
 * site+12 is stored there. */
static int hook12(uint32_t rva, void *fn, void **orig) {
  uintptr_t site = g_base + rva;
  const uint32_t *code = (const uint32_t *)site;

  if (orig) {
    uint32_t t[16]; int n = 0;
    uint64_t lits[4]; int nl = 0; int lit_at[4];
    for (int i = 0; i < 3; i++) {
      uint32_t w = code[i];
      if ((w & 0x9f000000u) == 0x90000000u) {           /* adrp xd, page */
        int64_t imm = (int64_t)((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3));
        if (imm & (1 << 20)) imm -= (1 << 21);
        uint64_t page = ((site + i * 4) & ~0xfffull) + (uint64_t)(imm << 12);
        lit_at[nl] = n; lits[nl++] = page;
        t[n++] = (w & 0x1f);                               /* rd, fixed below */
      } else if (is_pc_relative(w)) {
        dlog("[game] hook %#x: pc-relative instruction %08x", rva, w);
        return -1;
      } else {
        t[n++] = w;
      }
    }
    lit_at[nl] = n; lits[nl++] = site + 12;
    t[n++] = 16;                                           /* x16, fixed below */
    t[n++] = 0xd61f0200u;                                  /* br x16 */
    if (n & 1) t[n++] = 0xd503201fu;                       /* nop: align literals */
    int lit0 = n;
    for (int i = 0; i < nl; i++) {
      t[n++] = (uint32_t)lits[i];
      t[n++] = (uint32_t)(lits[i] >> 32);
      int rt = (int)t[lit_at[i]];
      t[lit_at[i]] = enc_ldr_lit(rt, (lit0 + i * 2 - lit_at[i]) * 4);
    }
    uintptr_t tr = tramp_put(t, (size_t)n);
    if (!tr) { dlog("[game] hook %#x: no trampoline space", rva); return -1; }
    *orig = (void *)tr;
  }

  uint32_t v[4] = {
    0x58000050u, 0xd61f0200u,                              /* ldr x16,#8; br x16 */
    (uint32_t)((uintptr_t)fn), (uint32_t)((uintptr_t)fn >> 32),
  };
  uintptr_t ven = tramp_put(v, 4);
  if (!ven) { dlog("[game] hook %#x: no veneer space", rva); return -1; }

  int64_t pages = (int64_t)((ven & ~0xfffull) - (site & ~0xfffull)) >> 12;
  uint32_t stub[3] = {
    0x90000010u | ((uint32_t)(pages & 3) << 29) | ((uint32_t)((pages >> 2) & 0x7ffff) << 5),
    0x91000210u | ((uint32_t)(ven & 0xfff) << 10),         /* add x16,x16,#lo */
    0xd61f0200u,                                           /* br x16 */
  };
  if (so_patch_code((void *)site, stub, sizeof stub) != 0) {
    dlog("[game] hook %#x: patch failed", rva);
    return -1;
  }
  return 0;
}

/* ---- helpers ------------------------------------------------------------- */

static void il2cpp_str(const void *s, char *out, size_t cap) {
  out[0] = 0;
  if (!s || cap < 2) return;
  int32_t len = *(const int32_t *)((const char *)s + 0x10);
  const uint16_t *ch = (const uint16_t *)((const char *)s + 0x14);
  size_t o = 0;
  for (int32_t i = 0; i < len && o + 1 < cap; i++)
    out[o++] = ch[i] < 0x80 ? (char)ch[i] : '?';
  out[o] = 0;
}

static uint32_t lns_flags(const void *self) {
  const uint8_t *p = (const uint8_t *)self;
  uint32_t f = 0;
  for (int i = 0; i < 7; i++) if (p[LNS_LOCALE + i]) f |= 1u << i;
  return f;
}

static void lns_log(const void *self, const char *why) {
  uint32_t f = lns_flags(self);
  dlog("[game] loading (%s): locale %d store %d gamemanager %d settings %d "
       "achievements %d player %d storefailed %d",
       why, !!(f & 1), !!(f & 2), !!(f & 4), !!(f & 8), !!(f & 16), !!(f & 32), !!(f & 64));
}

/* ---- hooks --------------------------------------------------------------- */

static void h_UpdateStatusText(void *self, void *mi) {
  if (self) {
    uint32_t f = lns_flags(self);
    if (f != g_lns_last_flags) { g_lns_last_flags = f; lns_log(self, "status"); }
    if ((f & 0x3f) == 0x3f) {
      g_lns = NULL;                       /* all loaded: next scene starts */
    } else if (g_lns != self) {
      g_lns = self;
      g_lns_since_ns = now_ns();
    }
  }
  orig_UpdateStatusText(self, mi);
}

/* Original: storeLoadingFailed = true; UpdateStatusText(). */
static void h_FailedToLoadStore(void *self, void *mi) {
  dlog("[game] FailedToLoadStore (Unity Gaming Services init failed)");
  ((uint8_t *)self)[LNS_STOREFAILED] = 1;
  h_UpdateStatusText(self, mi);
}

static void h_InitAfterAsync(void *self, void *mi) {
  dlog("[game] store: InitStoreAfterAsyncStart");
  orig_InitAfterAsync(self, mi);
  dlog("[game] store: InitStoreAfterAsyncStart done (status %d)",
       *(int32_t *)((char *)self + SSM_STATUS));
}

static void h_OnInitialized(void *self, void *ctl, void *ext, void *mi) {
  dlog("[game] store: OnInitialized");
  orig_OnInitialized(self, ctl, ext, mi);
}

/* Unity IAP talks to Google Play billing through Java, which is not here:
 * do not start it, report the store as failed (the game then uses its
 * offline purchase records). InitStoreAfterAsyncStart still sends the
 * "store loaded" event afterwards. */
static void h_InitializePurchasing(void *self, void *mi) {
  (void)mi;
  dlog("[game] store: InitializePurchasing skipped (no Google Play)");
  *(int32_t *)((char *)self + SSM_STATUS) = 2;
}

/* Original: storeInitializedStatus = STORE_INIT_FAILED. */
static void h_OnInitFailed1(void *self, int32_t reason, void *mi) {
  (void)mi;
  dlog("[game] store: OnInitializeFailed reason %d", reason);
  *(int32_t *)((char *)self + SSM_STATUS) = 2;
}
static void h_OnInitFailed2(void *self, int32_t reason, void *info, void *mi) {
  (void)mi;
  char s[160]; il2cpp_str(info, s, sizeof s);
  dlog("[game] store: OnInitializeFailed reason %d: %s", reason, s);
  *(int32_t *)((char *)self + SSM_STATUS) = 2;
}

/* ---- public -------------------------------------------------------------- */

struct site { uint32_t rva; uint32_t w0, w1, w2; };
static const struct site k_sites[] = {
  { RVA_LNS_FailedToLoadStore,  0x52800028u, 0x39009808u, 0x14000001u },
  { RVA_LNS_UpdateStatusText,   0xa9bc67feu, 0xa9015ff8u, 0xa90257f6u },
  { RVA_LNS_CheckLoadNextLevel, 0xf81e0ffeu, 0xa9014ff4u, 0xf000a814u },
  { RVA_SSM_OnInitialized,      0xf81c0ffeu, 0xa9015ff8u, 0xa90257f6u },
  { RVA_SSM_OnInitFailed1,      0x52800048u, 0xb9002808u, 0xd65f03c0u },
  { RVA_SSM_OnInitFailed2,      0x52800048u, 0xb9002808u, 0xd65f03c0u },
  { RVA_SSM_InitAfterAsync,     0xa9be57feu, 0xa9014ff4u, 0xf000a5f4u },
  { RVA_SSM_InitializePurchasing, 0xa9bd5ffeu, 0xa90157f6u, 0xa9024ff4u },
};

void sv_game_install(void) {
  g_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_tramp_next = g_base + RVA_TRAMP_START;
  for (unsigned i = 0; i < sizeof k_sites / sizeof k_sites[0]; i++) {
    const uint32_t *c = (const uint32_t *)(g_base + k_sites[i].rva);
    if (c[0] != k_sites[i].w0 || c[1] != k_sites[i].w1 || c[2] != k_sites[i].w2) {
      dlog("[game] libil2cpp.so differs at %#x (%08x %08x %08x): game hooks off",
           k_sites[i].rva, c[0], c[1], c[2]);
      return;
    }
  }
  fn_CheckLoadNextLevel = (void (*)(void *, void *))(g_base + RVA_LNS_CheckLoadNextLevel);
  int bad = 0;
  bad |= hook12(RVA_LNS_UpdateStatusText, (void *)h_UpdateStatusText, (void **)&orig_UpdateStatusText);
  bad |= hook12(RVA_LNS_FailedToLoadStore, (void *)h_FailedToLoadStore, NULL);
  bad |= hook12(RVA_SSM_OnInitialized, (void *)h_OnInitialized, (void **)&orig_OnInitialized);
  bad |= hook12(RVA_SSM_OnInitFailed1, (void *)h_OnInitFailed1, NULL);
  bad |= hook12(RVA_SSM_OnInitFailed2, (void *)h_OnInitFailed2, NULL);
  bad |= hook12(RVA_SSM_InitAfterAsync, (void *)h_InitAfterAsync, (void **)&orig_InitAfterAsync);
  bad |= hook12(RVA_SSM_InitializePurchasing, (void *)h_InitializePurchasing, NULL);
  g_hooks_on = !bad;
  dlog("[game] hooks %s (trampolines %#lx bytes)", bad ? "FAILED" : "on",
       (unsigned long)(g_tramp_next - (g_base + RVA_TRAMP_START)));
}

/* Called on the Unity main thread between frames. */
void sv_game_frame(void) {
  void *lns = g_lns;
  if (!g_hooks_on || !lns) return;
  if (now_ns() - g_lns_since_ns < STORE_FORCE_AFTER_NS) return;
  uint8_t *p = (uint8_t *)lns;
  if (!p[LNS_STORE]) {
    lns_log(lns, "stuck, store reported loaded by the port");
    p[LNS_STORE] = 1;
    g_store_forced = 1;
    g_lns_since_ns = now_ns();
    fn_CheckLoadNextLevel(lns, NULL);
  } else {
    lns_log(lns, "stuck");
    g_lns_since_ns = now_ns();      /* log again in 12 s */
  }
}
