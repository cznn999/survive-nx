/* Android NDK window, looper, sensor, and input shims used by libunity. */

#include <math.h>
#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>
#include "nx_pointer.h"   /* right-stick cursor / mouse support (menus) */

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

static u32 g_w = 1280, g_h = 720;

void android_native_update_mode(void){
  if (appletGetOperationMode() == AppletOperationMode_Console) { g_w = 1920; g_h = 1080; }
  else                                                         { g_w = 1280; g_h = 720;  }
}
static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  nwindowSetDimensions(w, bw, bh);
  nwindowSetTransform(w, 0u);
}

static ANativeWindow *native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return native_window();
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  if (width > 0 && height > 0) nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  return 0;
}

/* Unity uses ALooper as a per-thread wait/wake primitive. */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 0); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Sensors are unavailable. */
void *ASensorManager_getInstance(void){ static int x; return &x; }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* Handheld touch is injected directly. In docked mode, the left stick controls
 * a virtual cursor and A taps it. */
#include "unity_input.h"

static PadState g_pad;
static HidTouchScreenState g_touch;

#define VIBRATION_HANDLE_CAP 8
static HidVibrationDeviceHandle g_vibration_handles[VIBRATION_HANDLE_CAP];
static int g_vibration_count;
static int g_vibration_active;
static u64 g_vibration_deadline;
static Mutex g_vibration_lock;

static void vibration_add(HidNpadIdType id, HidNpadStyleTag style, int count) {
  if (g_vibration_count + count > VIBRATION_HANDLE_CAP) return;
  if (R_SUCCEEDED(hidInitializeVibrationDevices(
        &g_vibration_handles[g_vibration_count], count, id, style)))
    g_vibration_count += count;
}

static void vibration_send(const HidVibrationValue *value) {
  bool permitted = false;
  if (R_FAILED(hidIsVibrationPermitted(&permitted)) || !permitted) return;
  for (int i = 0; i < g_vibration_count; i++) {
    bool mounted = false;
    if (R_SUCCEEDED(hidIsVibrationDeviceMounted(g_vibration_handles[i], &mounted)) && mounted)
      hidSendVibrationValue(g_vibration_handles[i], value);
  }
}

static void vibration_start(int length_ms, float low, float high) {
  if (length_ms <= 0) return;
  if (length_ms > 1000) length_ms = 1000;
  HidVibrationValue value = { low, 160.0f, high, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&value);
  g_vibration_deadline = armGetSystemTick() + armNsToTicks((u64)length_ms * 1000000ULL);
  g_vibration_active = 1;
  mutexUnlock(&g_vibration_lock);
}

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  mutexInit(&g_vibration_lock);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft, 1);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight, 1);
  vibration_add(HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2);
}

void android_native_vibration_standard(int length_ms) {
  vibration_start(length_ms, 0.55f, 0.40f);
}

void android_native_vibration_haptic(int style) {
  static const struct { uint16_t ms; float low, high; } effects[] = {
    { 100, 0.90f, 0.65f }, { 75, 0.65f, 0.50f }, { 45, 0.35f, 0.30f },
    { 55, 0.45f, 0.80f },  { 85, 0.35f, 0.25f }, { 35, 0.30f, 0.65f },
    { 25, 0.20f, 0.45f },  { 120, 0.70f, 0.35f }, { 100, 0.45f, 0.70f },
    { 130, 0.70f, 0.45f }, { 180, 0.90f, 0.70f }, { 20, 0.18f, 0.35f },
    { 90, 0.25f, 0.65f },  { 140, 0.45f, 0.25f }, { 250, 0.40f, 0.30f },
    { 60, 0.50f, 0.40f },  { 75, 0.40f, 0.25f }, { 220, 0.35f, 0.18f },
    { 15, 0.12f, 0.22f },
  };
  unsigned index = (unsigned)style;
  if (index >= sizeof(effects) / sizeof(effects[0])) index = 15;
  vibration_start(effects[index].ms, effects[index].low, effects[index].high);
}

void android_native_vibration_update(void) {
  mutexLock(&g_vibration_lock);
  if (g_vibration_active && armGetSystemTick() >= g_vibration_deadline) {
    HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
    vibration_send(&stop);
    g_vibration_active = 0;
  }
  mutexUnlock(&g_vibration_lock);
}

void android_native_vibration_shutdown(void) {
  HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&stop);
  g_vibration_active = 0;
  mutexUnlock(&g_vibration_lock);
}

/* nativeInjectEvent(env, thiz, event, flags). */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

/* Lazy one-time init of the right-stick cursor module. Called at the top of
 * android_native_feed_hid so it runs once the window geometry (g_w/g_h) is known.
 * Survive renders landscape, so rotation is 0. The host keeps its own
 * touch handling, so handle_touch is 0. Settings/cursor.png live under the port's
 * SD folder; file I/O goes through the port's locked fopen/fclose wrappers. */
extern FILE *fopen_fake(const char *path, const char *mode);
extern int   fclose_fake(FILE *f);
static void nxp_ensure_init(void){
  static int done = 0;
  if (done) return;
  done = 1;
  NxpConfig c = {0};
  c.screen_w = (int)g_w; c.screen_h = (int)g_h;   /* render space              */
  c.panel_w  = 1280;     c.panel_h  = 720;         /* Switch touch panel        */
  c.data_dir = GAME_HOME;    /* cursor.png / pointer.cfg  */
  c.rotation = 0;                                  /* landscape, no rotation    */
  c.handle_touch = 0;                              /* host keeps its own touch  */
  c.cursor_id = 0; c.max_touch_slots = UI_MAX_POINTERS;
  c.fopen_fn = fopen_fake; c.fclose_fn = fclose_fake;
  nxp_init(&c);
}

/* --- Pointers ---------------------------------------------------------------
 * Every frame the host builds the set of fingers that are down -- the real
 * touchscreen's, the cursor's tap, the left stick's joystick finger, the right
 * stick's look finger and the mapped buttons' taps -- and sends Unity the
 * difference as Android would: ACTION_DOWN for the first finger,
 * ACTION_POINTER_DOWN/UP (with the finger's index) for the others, ACTION_MOVE
 * with every finger, ACTION_UP for the last. Ids: touchscreen 0-9 as the panel
 * numbers them, then fixed ids for the virtual fingers. */
enum { ID_CURSOR = 10, ID_JOY = 11, ID_LOOK = 12, ID_BTN0 = 13 };
#define MAXP 24
typedef struct { int id; float x, y; } Ptr;
static Ptr g_prev[MAXP];
static int g_nprev;

static int find_ptr(const Ptr *set, int n, int id) {
  for (int i = 0; i < n; i++) if (set[i].id == id) return i;
  return -1;
}

static void send_set(inject_fn inject, void *env, void *thiz, const Ptr *set, int n, int action) {
  int ids[MAXP]; float xs[MAXP], ys[MAXP];
  for (int i = 0; i < n; i++) { ids[i] = set[i].id; xs[i] = set[i].x; ys[i] = set[i].y; }
  inject(env, thiz, unity_motionevent(action, n, ids, xs, ys), 0);
}

static void emit_pointers(inject_fn inject, void *env, void *thiz, const Ptr *cur, int ncur) {
  Ptr live[MAXP]; int nlive = g_nprev;
  memcpy(live, g_prev, sizeof(Ptr) * (size_t)g_nprev);
  /* fingers lifted: POINTER_UP (or UP for the last) at their last position */
  for (int i = 0; i < nlive; ) {
    if (find_ptr(cur, ncur, live[i].id) >= 0) { i++; continue; }
    int action = nlive == 1 ? AMOTION_ACTION_UP
                            : AMOTION_ACTION_POINTER_UP | (i << AMOTION_ACTION_PTR_IDX_SHIFT);
    send_set(inject, env, thiz, live, nlive, action);
    memmove(&live[i], &live[i + 1], sizeof(Ptr) * (size_t)(nlive - i - 1));
    nlive--;
  }
  /* fingers still down: one MOVE with all of them, if any moved */
  int moved = 0;
  for (int i = 0; i < nlive; i++) {
    int j = find_ptr(cur, ncur, live[i].id);
    if (live[i].x != cur[j].x || live[i].y != cur[j].y) { live[i] = cur[j]; moved = 1; }
  }
  if (moved) send_set(inject, env, thiz, live, nlive, AMOTION_ACTION_MOVE);
  /* new fingers: DOWN for the first, POINTER_DOWN for the others */
  for (int j = 0; j < ncur && nlive < MAXP; j++) {
    if (find_ptr(live, nlive, cur[j].id) >= 0) continue;
    live[nlive++] = cur[j];
    int action = nlive == 1 ? AMOTION_ACTION_DOWN
                            : AMOTION_ACTION_POINTER_DOWN | ((nlive - 1) << AMOTION_ACTION_PTR_IDX_SHIFT);
    send_set(inject, env, thiz, live, nlive, action);
  }
  memcpy(g_prev, live, sizeof(Ptr) * (size_t)nlive);
  g_nprev = nlive;
}

/* Left stick: a finger on the movement joystick, put down at its centre and
 * pushed out by the stick. */
static float stick_f(int v) {
  float f = (float)v / 32767.0f;
  return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

/* Right stick: a finger dragged over the look area. When it nears the edge of
 * the area it is lifted, and put down again in the middle next frame, so
 * turning never stops. */
static float g_lx, g_ly;
static int g_look_down, g_look_skip;

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  nxp_ensure_init();
  padUpdate(&g_pad);
  Ptr cur[MAXP]; int n = 0;
  const float W = (float)g_w, H = (float)g_h;

  /* real touchscreen (handheld) */
  if (hidGetTouchScreenStates(&g_touch, 1) > 0) {
    int c = g_touch.count > 10 ? 10 : g_touch.count;
    for (int i = 0; i < c; i++) {
      cur[n].id = (int)g_touch.touches[i].finger_id % 10;
      cur[n].x = (float)g_touch.touches[i].x * (W / 1280.0f);
      cur[n].y = (float)g_touch.touches[i].y * (H / 720.0f);
      n++;
    }
  }

  u64 btn = padGetButtons(&g_pad);
  const SvControls *k = &config.controls;

  /* cursor (ZL toggles it; right stick moves it, A/ZR tap at it) */
  nxp_update();
  static int cursor_down;
  static float cursor_x, cursor_y;
  {
    NxpEvent pev[8];
    int pn = nxp_poll(pev, 8);
    for (int i = 0; i < pn; i++) {
      cursor_x = pev[i].x; cursor_y = pev[i].y;
      cursor_down = pev[i].phase != NXP_UP;
    }
    if (!nxp_cursor_visible()) cursor_down = 0;
  }
  if (cursor_down) { cur[n].id = ID_CURSOR; cur[n].x = cursor_x; cur[n].y = cursor_y; n++; }

  /* left stick -> joystick finger */
  {
    HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
    float sx = stick_f(ls.x), sy = -stick_f(ls.y);
    if (sx * sx + sy * sy > 0.15f * 0.15f) {
      float r = k->joy_radius * H;
      cur[n].id = ID_JOY;
      cur[n].x = k->joy_x * W + sx * r;
      cur[n].y = k->joy_y * H + sy * r;
      n++;
    }
  }

  /* right stick -> look finger (unless it moves the cursor) */
  if (!nxp_cursor_visible()) {
    HidAnalogStickState rs = padGetStickPos(&g_pad, 1);
    float sx = stick_f(rs.x), sy = -stick_f(rs.y);
    float m = sqrtf(sx * sx + sy * sy);
    if (m > 0.12f && !g_look_skip) {
      if (!g_look_down) { g_lx = k->look_x * W; g_ly = k->look_y * H; g_look_down = 1; }
      float speed = H * 0.025f * k->look_speed * (0.3f + 0.7f * m);
      float nx = g_lx + sx * speed, ny = g_ly + sy * (k->invert_y ? -speed : speed);
      if (nx < W * 0.45f || nx > W * 0.97f || ny < H * 0.05f || ny > H * 0.95f) {
        g_look_down = 0;            /* lift now ... */
        g_look_skip = 1;            /* ... and put down again next frame */
      } else {
        g_lx = nx; g_ly = ny;
      }
    } else {
      g_look_down = 0;
      g_look_skip = 0;
    }
    if (g_look_down) { cur[n].id = ID_LOOK; cur[n].x = g_lx; cur[n].y = g_ly; n++; }
  } else {
    g_look_down = 0;
  }

  /* mapped buttons -> taps at their screen positions (config.txt) */
  for (int b = 0; b < SV_NBUTTONS && n < MAXP; b++) {
    const SvButton *m = &k->button[b];
    if (!m->set || !(btn & m->mask)) continue;
    if ((m->mask & (HidNpadButton_A | HidNpadButton_ZR)) && nxp_cursor_visible()) continue;
    cur[n].id = ID_BTN0 + b; cur[n].x = m->x * W; cur[n].y = m->y * H; n++;
  }

  emit_pointers(inject, env, thiz, cur, n);

  /* B -> Android BACK */
  static int prev_b = 0;
  int b = (btn & HidNpadButton_B) ? 1 : 0;
  if (b && !prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (!b && prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  prev_b = b;
}
