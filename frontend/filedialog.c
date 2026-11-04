/*
 * filedialog.c - Windows: GetOpenFileNameW (comdlg32); macOS: osascript;
 * Linux/BSD: zenity or kdialog if installed; Android: the system document picker
 * (asynchronous: the chosen file arrives as an SDL_DROPFILE event).
 */
#include "filedialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>

int file_dialog_available(void) { return 1; }

int file_dialog_open(SDL_Window *parent, char *out, size_t out_len) { return file_dialog_open_kind(parent, FILE_KIND_DUMP, out, out_len); }

int file_dialog_open_kind(SDL_Window *parent, int kind, char *out, size_t out_len) {
    HWND owner = NULL;
    if (parent) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(parent, &info)) owner = info.info.win.window;
    }
    wchar_t buf[2048] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    if (kind == FILE_KIND_ALLSTARS) {
        ofn.lpstrFilter = L"Super Mario All-Stars ROM (*.sfc, *.smc)\0*.sfc;*.smc\0All files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select your own Super Mario All-Stars ROM (SNES, USA)";
    } else {
        ofn.lpstrFilter = L"VS. Super Mario Bros. dump (*.nes, MAME *.zip)\0*.nes;*.zip\0All files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select your own VS. Super Mario Bros. dump (.nes or MAME suprmrio.zip)";
    }
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof buf / sizeof buf[0]);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)out_len, NULL, NULL) == 0) return 0;
    return 1;
}

#elif defined(__ANDROID__)
#include <jni.h>
#include <SDL_system.h>

int file_dialog_available(void) { return 1; }

int file_dialog_open(SDL_Window *parent, char *out, size_t out_len) { return file_dialog_open_kind(parent, FILE_KIND_DUMP, out, out_len); }

/* The activity shows the document picker, copies the chosen file into the app's storage and
   hands its path over as a drop event, which the main loop already treats as "load this ROM"
   (the board's dump or, by its size, the All-Stars ROM). Nothing is chosen yet when this returns. */
int file_dialog_open_kind(SDL_Window *parent, int kind, char *out, size_t out_len) {
    (void)parent; (void)kind; (void)out; (void)out_len;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, "pickRom", "()V") : NULL;
    if (m) (*env)->CallVoidMethod(env, activity, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return 0;
}

#else
#include <unistd.h>

static int run_capture(const char *cmd, char *out, size_t out_len) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    out[0] = 0;
    if (!fgets(out, (int)out_len, p)) { pclose(p); return 0; }
    int rc = pclose(p);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return rc == 0 && n > 0;
}

static int have_cmd(const char *name) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

int file_dialog_available(void) {
#ifdef __APPLE__
    return 1;
#else
    return have_cmd("zenity") || have_cmd("kdialog");
#endif
}

int file_dialog_open(SDL_Window *parent, char *out, size_t out_len) { return file_dialog_open_kind(parent, FILE_KIND_DUMP, out, out_len); }

int file_dialog_open_kind(SDL_Window *parent, int kind, char *out, size_t out_len) {
    (void)parent;
#ifdef __APPLE__
    if (kind == FILE_KIND_ALLSTARS)
        return run_capture("osascript -e 'POSIX path of (choose file with prompt \"Select your Super Mario All-Stars ROM\")' 2>/dev/null",
                           out, out_len);
    return run_capture("osascript -e 'POSIX path of (choose file with prompt \"Select your VS. Super Mario Bros. dump\")' 2>/dev/null",
                       out, out_len);
#else
    if (kind == FILE_KIND_ALLSTARS) {
        if (have_cmd("zenity"))
            return run_capture("zenity --file-selection --title='Select your Super Mario All-Stars ROM' "
                               "--file-filter='Super Mario All-Stars ROM (*.sfc, *.smc) | *.sfc *.smc' --file-filter='All files | *' 2>/dev/null", out, out_len);
        if (have_cmd("kdialog"))
            return run_capture("kdialog --getopenfilename . 'Super Mario All-Stars ROM (*.sfc *.smc)' 2>/dev/null", out, out_len);
        return 0;
    }
    if (have_cmd("zenity"))
        return run_capture("zenity --file-selection --title='Select your VS. Super Mario Bros. dump' "
                           "--file-filter='VS. Super Mario Bros. dump (*.nes, *.zip) | *.nes *.zip' --file-filter='All files | *' 2>/dev/null", out, out_len);
    if (have_cmd("kdialog"))
        return run_capture("kdialog --getopenfilename . 'VS. Super Mario Bros. dump (*.nes *.zip)' 2>/dev/null", out, out_len);
    return 0;
#endif
}
#endif
