// SPDX-License-Identifier: GPL-3.0-or-later
/* The slice of SDL3 that the mupen64plus core links against, on pthreads.
 *
 * The headless harness has no window, no audio device and no controller, so the core is
 * built against the SDL3 headers only and gets these functions instead of the library.
 * That keeps the arm64 and x86_64 builds on identical host code (no second SDL build to
 * keep in step) and is the starting point for the Switch, where there is no SDL3.
 *
 * Threads, mutexes, condition variables and clocks are real. Everything that touches a
 * display fails cleanly; nothing in a headless run calls it. */

#include <SDL3/SDL.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct SDL_Mutex     { pthread_mutex_t m; };
struct SDL_Condition { pthread_cond_t c; };
struct SDL_Thread    { pthread_t t; SDL_ThreadFunction fn; void* data; int result; };

static const char* l_error = "not available in the headless SDL shim";

/* ---- clocks ---- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

Uint64 SDL_GetTicks(void)                { return now_ns() / 1000000ull; }
Uint64 SDL_GetPerformanceCounter(void)   { return now_ns(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000ull; }
void   SDL_Delay(Uint32 ms)              { usleep((useconds_t)ms * 1000u); }

/* ---- threads ---- */

SDL_Mutex* SDL_CreateMutex(void)
{
    SDL_Mutex* mutex = malloc(sizeof(*mutex));
    pthread_mutexattr_t attr;
    if (mutex == NULL)
        return NULL;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); /* SDL mutexes are recursive */
    pthread_mutex_init(&mutex->m, &attr);
    pthread_mutexattr_destroy(&attr);
    return mutex;
}

void SDL_LockMutex(SDL_Mutex* mutex)   { if (mutex != NULL) pthread_mutex_lock(&mutex->m); }
void SDL_UnlockMutex(SDL_Mutex* mutex) { if (mutex != NULL) pthread_mutex_unlock(&mutex->m); }

void SDL_DestroyMutex(SDL_Mutex* mutex)
{
    if (mutex == NULL)
        return;
    pthread_mutex_destroy(&mutex->m);
    free(mutex);
}

SDL_Condition* SDL_CreateCondition(void)
{
    SDL_Condition* cond = malloc(sizeof(*cond));
    if (cond != NULL)
        pthread_cond_init(&cond->c, NULL);
    return cond;
}

void SDL_DestroyCondition(SDL_Condition* cond)
{
    if (cond == NULL)
        return;
    pthread_cond_destroy(&cond->c);
    free(cond);
}

void SDL_SignalCondition(SDL_Condition* cond) { if (cond != NULL) pthread_cond_signal(&cond->c); }

void SDL_WaitCondition(SDL_Condition* cond, SDL_Mutex* mutex)
{
    if (cond != NULL && mutex != NULL)
        pthread_cond_wait(&cond->c, &mutex->m);
}

static void* thread_entry(void* arg)
{
    SDL_Thread* thread = arg;
    thread->result = thread->fn(thread->data);
    return NULL;
}

SDL_Thread* SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char* name, void* data,
                                    SDL_FunctionPointer begin, SDL_FunctionPointer end)
{
    SDL_Thread* thread = calloc(1, sizeof(*thread));
    (void)name; (void)begin; (void)end;
    if (thread == NULL)
        return NULL;
    thread->fn = fn;
    thread->data = data;
    if (pthread_create(&thread->t, NULL, thread_entry, thread) != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}

void SDL_WaitThread(SDL_Thread* thread, int* status)
{
    if (thread == NULL)
        return;
    pthread_join(thread->t, NULL);
    if (status != NULL)
        *status = thread->result;
    free(thread);
}

/* ---- library state ---- */

static SDL_InitFlags l_init;

bool SDL_InitSubSystem(SDL_InitFlags flags)
{
    if (flags & SDL_INIT_VIDEO)
        return false; /* there is no display */
    l_init |= flags;
    return true;
}

void SDL_QuitSubSystem(SDL_InitFlags flags)       { l_init &= ~flags; }
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags)    { return flags ? (l_init & flags) : l_init; }
void SDL_Quit(void)                               { l_init = 0; }
const char* SDL_GetError(void)                    { return l_error; }
bool SDL_SetHint(const char* name, const char* v) { (void)name; (void)v; return true; }
void SDL_PumpEvents(void)                         {}
void SDL_SetEventFilter(SDL_EventFilter filter, void* userdata) { (void)filter; (void)userdata; }

int         SDL_atoi(const char* str)    { return atoi(str); }
void        SDL_free(void* mem)          { free(mem); }
const char* SDL_getenv(const char* name) { return getenv(name); }

/* ---- video: absent ---- */

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags)
{ (void)title; (void)w; (void)h; (void)flags; return NULL; }
void SDL_DestroyWindow(SDL_Window* window) { (void)window; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window) { (void)window; return 0; }
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h) { (void)window; (void)w; (void)h; return false; }
bool SDL_SetWindowSize(SDL_Window* window, int w, int h) { (void)window; (void)w; (void)h; return false; }
bool SDL_SetWindowTitle(SDL_Window* window, const char* title) { (void)window; (void)title; return false; }
bool SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen) { (void)window; (void)fullscreen; return false; }
bool SDL_ShowCursor(void) { return false; }
bool SDL_HideCursor(void) { return false; }
SDL_DisplayID* SDL_GetDisplays(int* count) { if (count != NULL) *count = 0; return NULL; }
SDL_DisplayMode** SDL_GetFullscreenDisplayModes(SDL_DisplayID id, int* count)
{ (void)id; if (count != NULL) *count = 0; return NULL; }

SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) { (void)window; return NULL; }
bool SDL_GL_DestroyContext(SDL_GLContext context) { (void)context; return false; }
bool SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) { (void)window; (void)context; return false; }
bool SDL_GL_GetAttribute(SDL_GLAttr attr, int* value) { (void)attr; (void)value; return false; }
bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value) { (void)attr; (void)value; return false; }
SDL_FunctionPointer SDL_GL_GetProcAddress(const char* proc) { (void)proc; return NULL; }
bool SDL_GL_GetSwapInterval(int* interval) { (void)interval; return false; }
bool SDL_GL_SetSwapInterval(int interval) { (void)interval; return false; }
bool SDL_GL_SwapWindow(SDL_Window* window) { (void)window; return false; }
