/* dlog.h -- debug.log next to the NRO (dlog.c). */
#pragma once
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dlog_frame(unsigned long long frame);
void dlog_start_watchdog(void);
#include <pthread.h>
#include <switch.h>
typedef struct { volatile int used, alive; void *entry; pthread_t self; Handle handle; } GuestThread;
#define MAX_GUEST_THREADS 96
extern GuestThread g_guest_threads[MAX_GUEST_THREADS];
void dlog_thread_join(void *entry);
__attribute__((noreturn)) void abort_fake(void);
extern int g_debug_log;
