/*
 * gpu_sync.c - display synchronisation helpers (Windows: DXGI vblank; Android: Choreographer).
 *
 * Frame pacing is phase-locked to the real vertical blank of the monitor the
 * window is on (IDXGIOutput::WaitForVBlank) rather than to Present() blocking,
 * which is unreliable in a window (the compositor, occlusion, 120 Hz panels).
 * The presentation queue is also limited to one frame so a vsynced Present
 * cannot run several frames ahead of the display.
 */
#include "gpu_sync.h"
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <timeapi.h>
#include <SDL_system.h>
#include <SDL_syswm.h>

static IDXGIOutput *vblank_output;

int gpu_sync_init(SDL_Renderer *r) {
    ID3D11Device *dev = SDL_RenderGetD3D11Device(r);
    if (!dev) return 0;
    IDXGIDevice1 *dxgi = NULL;
    int ok = 0;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&dxgi)) && dxgi) {
        ok = SUCCEEDED(IDXGIDevice1_SetMaximumFrameLatency(dxgi, 1));
        IDXGIDevice1_Release(dxgi);
    }
    ID3D11Device_Release(dev);
    return ok;
}

int gpu_sync_vblank_init(SDL_Window *win) {
    if (vblank_output) { IDXGIOutput_Release(vblank_output); vblank_output = NULL; }
    HMONITOR mon = NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (win && SDL_GetWindowWMInfo(win, &info)) mon = MonitorFromWindow(info.info.win.window, MONITOR_DEFAULTTONEAREST);

    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || !factory) return 0;
    IDXGIOutput *first = NULL;
    for (UINT a = 0; !vblank_output; a++) {
        IDXGIAdapter1 *adapter = NULL;
        if (IDXGIFactory1_EnumAdapters1(factory, a, &adapter) != S_OK) break;
        for (UINT o = 0; ; o++) {
            IDXGIOutput *out = NULL;
            if (IDXGIAdapter1_EnumOutputs(adapter, o, &out) != S_OK) break;
            DXGI_OUTPUT_DESC desc;
            if (mon && SUCCEEDED(IDXGIOutput_GetDesc(out, &desc)) && desc.Monitor == mon) { vblank_output = out; break; }
            if (!first) first = out; else IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    if (!vblank_output) vblank_output = first;          /* no monitor match: use the first output */
    else if (first && first != vblank_output) IDXGIOutput_Release(first);
    return vblank_output != NULL;
}

int gpu_sync_wait_vblank(void) {
    if (!vblank_output) return 0;
    return SUCCEEDED(IDXGIOutput_WaitForVBlank(vblank_output));
}

void gpu_sync_timer_begin(void) { timeBeginPeriod(1); }
void gpu_sync_timer_end(void) { timeEndPeriod(1); }

#elif defined(__ANDROID__)
/* Android: the display's timing from the Choreographer, on a thread with its own looper. From
   Android 13 on, every vsync comes with frame timelines: for the preferred one, the deadline by
   which a frame must be queued and the time it is then on screen. Older versions report only the
   vsync time, which then serves as the deadline grid. The functions are looked up at run time
   (the app runs from Android 7). */
#include <android/choreographer.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*post_vsync_fn)(AChoreographer *, AChoreographer_vsyncCallback, void *);
typedef void (*post_frame64_fn)(AChoreographer *, AChoreographer_frameCallback64, void *);
typedef void (*post_frame_fn)(AChoreographer *, AChoreographer_frameCallback, void *);
typedef int64_t (*frame_time_fn)(const AChoreographerFrameCallbackData *);
typedef size_t (*index_fn)(const AChoreographerFrameCallbackData *);
typedef int64_t (*timeline_fn)(const AChoreographerFrameCallbackData *, size_t);

static post_vsync_fn post_vsync;
static post_frame64_fn post_frame64;
static post_frame_fn post_frame;
static frame_time_fn frame_time;
static index_fn preferred_timeline;
static timeline_fn timeline_deadline, timeline_present;

static AChoreographer *vs_choreographer;
static pthread_mutex_t vs_lock = PTHREAD_MUTEX_INITIALIZER;
static int vs_started;
#define VS_HIST 9
static int64_t vs_hist[VS_HIST];   /* the latest vsync intervals, ns */
static int vs_nhist, vs_hpos, vs_count;
static int64_t vs_last;            /* CLOCK_MONOTONIC ns of the latest vsync */
static int64_t vs_deadline;        /* the latest frame deadline */
static int64_t vs_lead;            /* deadline to on screen, ns (0 if unknown) */

static void vs_record(int64_t vsync, int64_t deadline, int64_t present) {
    pthread_mutex_lock(&vs_lock);
    if (vs_count > 0 && vsync > vs_last) {
        vs_hist[vs_hpos] = vsync - vs_last;
        vs_hpos = (vs_hpos + 1) % VS_HIST;
        if (vs_nhist < VS_HIST) vs_nhist++;
    }
    vs_last = vsync;
    vs_deadline = deadline;
    vs_lead = present > deadline ? present - deadline : 0;
    vs_count++;
    pthread_mutex_unlock(&vs_lock);
}

static void on_vsync(const AChoreographerFrameCallbackData *d, void *u) {
    size_t i = preferred_timeline(d);
    vs_record(frame_time(d), timeline_deadline(d, i), timeline_present(d, i));
    post_vsync(vs_choreographer, on_vsync, u);
}
static void on_frame64(int64_t t, void *u) { vs_record(t, t, t); post_frame64(vs_choreographer, on_frame64, u); }
static void on_frame(long t, void *u) { vs_record((int64_t)t, (int64_t)t, (int64_t)t); post_frame(vs_choreographer, on_frame, u); }

static void *vs_main(void *arg) {
    (void)arg;
    ALooper_prepare(0);
    vs_choreographer = AChoreographer_getInstance();
    if (!vs_choreographer) return NULL;
    if (post_vsync) post_vsync(vs_choreographer, on_vsync, NULL);
    else if (post_frame64) post_frame64(vs_choreographer, on_frame64, NULL);
    else if (post_frame) post_frame(vs_choreographer, on_frame, NULL);
    else return NULL;
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}

int gpu_sync_init(SDL_Renderer *r) { (void)r; return 0; }

int gpu_sync_vblank_init(SDL_Window *win) {
    (void)win;
    if (!vs_started) {
        vs_started = 1;
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib) {
            frame_time = (frame_time_fn)dlsym(lib, "AChoreographerFrameCallbackData_getFrameTimeNanos");
            preferred_timeline = (index_fn)dlsym(lib, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex");
            timeline_deadline = (timeline_fn)dlsym(lib, "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos");
            timeline_present = (timeline_fn)dlsym(lib, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
            if (frame_time && preferred_timeline && timeline_deadline && timeline_present)
                post_vsync = (post_vsync_fn)dlsym(lib, "AChoreographer_postVsyncCallback");
            post_frame64 = (post_frame64_fn)dlsym(lib, "AChoreographer_postFrameCallback64");
            post_frame = (post_frame_fn)dlsym(lib, "AChoreographer_postFrameCallback");
        }
        pthread_t t;
        if (pthread_create(&t, NULL, vs_main, NULL) == 0) pthread_detach(t);
        printf("vsync: Choreographer %s\n", post_vsync ? "frame timelines (Android 13+)" : post_frame64 ? "frame times" : "frame times (old API)");
    }
    for (int i = 0; i < 40; i++) {   /* the first few vsyncs give the period */
        pthread_mutex_lock(&vs_lock);
        int n = vs_nhist;
        pthread_mutex_unlock(&vs_lock);
        if (n >= 3) return 1;
        SDL_Delay(10);
    }
    return 0;
}

int gpu_sync_wait_vblank(void) { return 0; }

static int cmp64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static int64_t clock_ns(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int gpu_sync_deadline(double *deadline, double *period, double *lead) {
    int64_t h[VS_HIST], dl, ld;
    int n;
    pthread_mutex_lock(&vs_lock);
    n = vs_nhist;
    memcpy(h, vs_hist, sizeof h);
    dl = vs_deadline;
    ld = vs_lead;
    pthread_mutex_unlock(&vs_lock);
    if (n < 3) return 0;
    qsort(h, (size_t)n, sizeof h[0], cmp64);
    /* SDL's counter runs on CLOCK_MONOTONIC_RAW, the Choreographer's on CLOCK_MONOTONIC */
    int64_t off = clock_ns(CLOCK_MONOTONIC) - clock_ns(CLOCK_MONOTONIC_RAW);
    double f = (double)SDL_GetPerformanceFrequency() / 1e9;
    *deadline = (double)(dl - off) * f;
    *period = (double)h[n / 2] * f;   /* the median interval: a late callback does not count */
    *lead = (double)ld * f;
    return 1;
}

void gpu_sync_timer_begin(void) {}
void gpu_sync_timer_end(void) {}

#else
int gpu_sync_init(SDL_Renderer *r) { (void)r; return 0; }
int gpu_sync_vblank_init(SDL_Window *win) { (void)win; return 0; }
int gpu_sync_wait_vblank(void) { return 0; }
void gpu_sync_timer_begin(void) {}
void gpu_sync_timer_end(void) {}
#endif

#ifndef __ANDROID__
int gpu_sync_deadline(double *deadline, double *period, double *lead) { (void)deadline; (void)period; (void)lead; return 0; }
#endif
