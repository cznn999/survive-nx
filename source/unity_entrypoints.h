/* Unity 2022.3.62f2 (7670c08855a9) entry points in Survive 2.880's libunity.so
 * (md5 8ab916a11e8023c9bea3e161da405ba9). Found with tools/find_unity_sites.py
 * (UnityPlayer RegisterNatives tables, the Application icall registrations)
 * and by the instruction patterns of subwaysurfers_nx's sites. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

#define OFF_JNI_OnLoad                     0x5de94c
#define OFF_initJni                        0x5ddb48
#define OFF_nativeRecreateGfxState         0x5ddd7c
#define OFF_nativeSendSurfaceChangedEvent  0x5ddde4
#define OFF_nativeRender                   0x5dde3c
#define OFF_nativeInjectEvent              0x5dde9c
#define OFF_nativeResume                   0x5ddc48
#define OFF_nativeFocusChanged             0x5ddd28
#define OFF_nativeDone                     0x5ddb54
#define OFF_nativeApplicationUnload        0x5ddcd8

#define OFF_TimeManager_Update_entry       0x449188
#define OFF_FMOD_SetOutput                 0x6babc0  /* mov w1, w21 before System::setOutput */
#define OFF_FrameTimeTracker_wait          0x5d533c  /* cbnz x8, +0x14 */
#define OFF_WaitVSync_wait                 0x5d2948  /* b.ge +0x14 */
#define OFF_Unity_UseSwappy                0x5b6dd8  /* bool: PlayerSettings frame pacing && !disabled */
#define OFF_Application_get_persistentDataPath  0x34b1ec
#define OFF_Application_get_streamingAssetsPath 0x34b154

#define OFF_TimeManager_Update_body        0x4491ac

typedef void    (*fn_initJni)(void *, void *, void *);
typedef void    (*fn_gfxstate)(void *, void *, int32_t, void *);
typedef void    (*fn_v)(void *, void *);
typedef uint8_t (*fn_z)(void *, void *);
typedef void    (*fn_vz)(void *, void *, int32_t);
typedef uint8_t (*fn_inject)(void *, void *, void *, int32_t);

#define UNITY_RESOLVE(mod, off) ((void *)((uintptr_t)(mod).load_virtbase + (off)))

#endif
