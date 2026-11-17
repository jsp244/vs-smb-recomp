/* macos.h - what the macOS build asks of Cocoa itself (macos.m); SDL covers the rest */
#ifndef MACOS_H
#define MACOS_H
#include <stddef.h>

/* the system's open-file panel with the given message; 1 and the chosen file's path (UTF-8) in out,
   or 0 when the player cancelled */
int macos_choose_file(const char *message, char *out, size_t cap);

/* moves a file or folder to the Trash (the app an update replaced); 1 when it went there */
int macos_trash(const char *path);

/* --dialog-test: opens the open-file panel and closes it again a second later, as a cancel would;
   1 when the panel came and went */
int macos_dialog_selftest(void);

#endif
