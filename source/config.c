/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "config.h"

Config config;
static int config_needs_rewrite = 0;

int screen_width = 1280;
int screen_height = 720;

static int seen_controls;

static void controls_defaults(SvControls *k) {
  static const struct { const char *n; u64 m; } b[SV_NBUTTONS] = {
    {"A", HidNpadButton_A}, {"B", HidNpadButton_B}, {"X", HidNpadButton_X}, {"Y", HidNpadButton_Y},
    {"L", HidNpadButton_L}, {"R", HidNpadButton_R}, {"ZR", HidNpadButton_ZR},
    {"PLUS", HidNpadButton_Plus}, {"MINUS", HidNpadButton_Minus}, {"RSTICK", HidNpadButton_StickR},
  };
  k->joy_x = 0.14f; k->joy_y = 0.72f; k->joy_radius = 0.10f;
  k->look_x = 0.70f; k->look_y = 0.50f; k->look_speed = 1.0f;
  k->invert_y = 0;
  for (int i = 0; i < SV_NBUTTONS; i++) {
    k->button[i].name = b[i].n;
    k->button[i].mask = b[i].m;
    k->button[i].set = 0;
  }
}

static void parse_var(const char *name, const char *value) {
  SvControls *k = &config.controls;
  if (!strcmp(name, "joy_x"))      { k->joy_x = atof(value); seen_controls = 1; return; }
  if (!strcmp(name, "joy_y"))      { k->joy_y = atof(value); return; }
  if (!strcmp(name, "joy_radius")) { k->joy_radius = atof(value); return; }
  if (!strcmp(name, "look_x"))     { k->look_x = atof(value); return; }
  if (!strcmp(name, "look_y"))     { k->look_y = atof(value); return; }
  if (!strcmp(name, "look_speed")) { k->look_speed = atof(value); return; }
  if (!strcmp(name, "invert_y"))   { k->invert_y = atoi(value); return; }
  if (!strncmp(name, "button_", 7)) {
    for (int i = 0; i < SV_NBUTTONS; i++) {
      SvButton *b = &k->button[i];
      if (strcmp(name + 7, b->name)) continue;
      float x, y;
      b->set = sscanf(value, "%f,%f", &x, &y) == 2;
      if (b->set) { b->x = x; b->y = y; }
    }
    return;
  }
  if (!strcmp(name, "touchscreen") || !strcmp(name, "controller_cursor") || !strcmp(name, "widescreen") ||
      !strcmp(name, "screen_width") || !strcmp(name, "screen_height") ||
      !strcmp(name, "portrait")) {
    config_needs_rewrite = 1;
    return;
  }
  if (!strcmp(name, "language")) config.language = atoi(value);
  if (!strcmp(name, "debug")) config.debug = atoi(value);
}

int read_config(const char *file) {
  char line[1024];

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  config.language = LANG_EN;
  controls_defaults(&config.controls);
  seen_controls = 0;

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  while (fgets(line, sizeof(line), f)) {
    char name[64], value[64];
    if (sscanf(line, " %63s %63s", name, value) == 2 && name[0] != '#')
      parse_var(name, value);
  }

  fclose(f);

  return (config_needs_rewrite || !seen_controls) ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  const SvControls *k = &config.controls;
  fprintf(f, "language %d\n", config.language);
  fprintf(f, "# 1: detailed debug.log and watchdog.log (for bug reports; slower)\n");
  fprintf(f, "debug %d\n", config.debug);
  fprintf(f, "\n# Controller. Positions are fractions of the screen: 0,0 top left, 1,1 bottom right.\n");
  fprintf(f, "# Left stick: a finger on the movement joystick, centred at joy_x joy_y.\n");
  fprintf(f, "joy_x %.3f\njoy_y %.3f\njoy_radius %.3f\n", k->joy_x, k->joy_y, k->joy_radius);
  fprintf(f, "# Right stick: a finger dragged over the screen to look around.\n");
  fprintf(f, "look_x %.3f\nlook_y %.3f\nlook_speed %.2f\ninvert_y %d\n", k->look_x, k->look_y,
          k->look_speed, k->invert_y);
  fprintf(f, "# Buttons that tap a point of the screen: button_<A|B|X|Y|L|R|ZR|PLUS|MINUS|RSTICK> x,y\n");
  fprintf(f, "# (none: not mapped). ZL shows/hides the cursor; with the cursor shown the\n");
  fprintf(f, "# right stick moves it and A or ZR taps at it. B is Android BACK.\n");
  for (int i = 0; i < SV_NBUTTONS; i++) {
    const SvButton *b = &k->button[i];
    if (b->set) fprintf(f, "button_%s %.3f,%.3f\n", b->name, b->x, b->y);
    else        fprintf(f, "button_%s none\n", b->name);
  }

  fclose(f);

  return 0;
}
