/*
 * macos.m - what the macOS build asks of Cocoa itself (macos.h): the open-file panel for the ROMs,
 * and the Trash for the app an update replaced. Built with ARC.
 */
#import <Cocoa/Cocoa.h>
#include "macos.h"
#include <string.h>

int macos_choose_file(const char *message, char *out, size_t cap) {
    @autoreleasepool {
        NSWindow *game = [NSApp keyWindow];
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.resolvesAliases = YES;
        if (message) panel.message = [NSString stringWithUTF8String:message];
        NSModalResponse r = [panel runModal];
        [game makeKeyAndOrderFront:nil];          /* the keyboard goes back to the game's window */
        if (r != NSModalResponseOK || panel.URLs.count == 0) return 0;
        const char *path = panel.URLs.firstObject.fileSystemRepresentation;
        size_t n = path ? strlen(path) : 0;
        if (n == 0 || n >= cap) return 0;
        memcpy(out, path, n + 1);
        return 1;
    }
}

int macos_trash(const char *path) {
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path isDirectory:YES relativeToURL:nil];
        return [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nil error:NULL] ? 1 : 0;
    }
}

int macos_dialog_selftest(void) {
    @autoreleasepool {
        __block int closed = 0;
        NSTimer *timer = [NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer *t) {
            (void)t;
            closed = 1;
            [NSApp abortModal];
        }];
        [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSModalPanelRunLoopMode];
        char path[16];
        int chose = macos_choose_file("dialog-test: this panel closes by itself", path, sizeof path);
        [timer invalidate];
        return closed && !chose;
    }
}
