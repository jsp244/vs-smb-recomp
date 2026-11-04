/* gpu_sync.h - display vblank phase, GPU presentation queue depth, timer resolution */
#ifndef GPU_SYNC_H
#define GPU_SYNC_H
#include <SDL.h>

/* Limit the swap chain's maximum frame latency to 1 (Windows / Direct3D 11). Returns 1 on success. */
int gpu_sync_init(SDL_Renderer *r);

/* Find the DXGI output that drives the monitor the window is on (works with hybrid
   GPU laptops, where the display hangs off a different adapter than the renderer).
   Returns 1 when gpu_sync_wait_vblank() can be used. */
int gpu_sync_vblank_init(SDL_Window *win);

/* Block until the next vertical blank of that output. Returns 1 if it waited. */
int gpu_sync_wait_vblank(void);

/* Android: the compositor's frame deadlines, from the Choreographer. The latest deadline (by when
   a frame must be queued to reach the screen at the next refresh), the refresh period, and the
   lead from deadline to on-screen (0 if unknown), in performance-counter ticks. Returns 1 when
   known (after gpu_sync_vblank_init), 0 elsewhere. */
int gpu_sync_deadline(double *deadline, double *period, double *lead);

/* Raise the OS timer resolution for precise frame pacing (Windows); harmless elsewhere. */
void gpu_sync_timer_begin(void);
void gpu_sync_timer_end(void);

#endif
