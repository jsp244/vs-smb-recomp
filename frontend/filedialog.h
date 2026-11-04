/* filedialog.h - native "open file" dialog for picking the cartridge dump */
#ifndef FILEDIALOG_H
#define FILEDIALOG_H
#include <SDL.h>
#include <stddef.h>

/* Returns 1 and fills out (UTF-8) when the user chose a file, 0 otherwise.
   On platforms without a native dialog it returns 0 and the caller should
   offer drag-and-drop / command line instead. */
int file_dialog_open(SDL_Window *parent, char *out, size_t out_len);

/* 1 when file_dialog_open can show a dialog on this platform */
int file_dialog_available(void);

#endif
