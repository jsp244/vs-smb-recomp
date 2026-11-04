/* audio_out.h - SDL audio output with dynamic rate control (low, stable latency) */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/* preset: 0 = low (about 25 ms), 1 = normal (about 45 ms), 2 = safe (about 90 ms) */
int  audio_out_init(int sample_rate, int preset);  /* 1 on success */
void audio_out_set_preset(int preset);             /* reopens the device with the new buffering */
void audio_out_pump(int muted, int volume);        /* once per emulated frame; volume 0..100 */
void audio_out_clear(void);                        /* drop queued sound and re-prime (scene changes, pause) */
void audio_out_shutdown(void);
int  audio_out_queued_ms(void);
int  audio_out_rate(void);                         /* current APU sample rate after rate control */

#endif
