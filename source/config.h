/* Survive - Wilderness survival 2.880 Switch wrapper configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)1920 * 1024 * 1024)
#define OC_MANAGED_BYTES    ((size_t)1280 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 768 * 1024 * 1024)
#define OC_OVERFLOW_BYTES   ((size_t) 512 * 1024 * 1024)
#define OC_STACK_LEAVE_MB   256u
#define OC_MIN_STACK_MB     1024u
#define ARENA_CAP_PCT       68u

#define SS_PACKAGE        "com.sandbaygames.survive"
#define SS_VERSION_CODE   200880
#define SS_VERSION_NAME   "2.880"
#define SV_VERSION_NAME   SS_VERSION_NAME

#define CONFIG_NAME "config.txt"
#define GAME_HOME   "sdmc:/switch/survive_nx"
#define GAME_HOME_POSIX "/switch/survive_nx"

extern int screen_width;
extern int screen_height;

// Language. 0 = follow the Switch system language.
#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

/* Controller mapping (config.txt). Positions are fractions of the screen. */
#define SV_NBUTTONS 10
typedef struct {
  const char *name;    /* config key suffix: button_<name> */
  unsigned long long mask; /* HidNpadButton_* */
  int set;
  float x, y;
} SvButton;

typedef struct {
  float joy_x, joy_y, joy_radius;  /* left stick: joystick finger */
  float look_x, look_y, look_speed; /* right stick: look finger */
  int invert_y;
  SvButton button[SV_NBUTTONS];
} SvControls;

typedef struct {
  int language;
  int debug;            /* debug 1: verbose debug.log and watchdog.log */
  SvControls controls;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
