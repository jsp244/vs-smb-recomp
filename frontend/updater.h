/*
 * updater.h - the menu's Updates tab: compares this build with the newest release of the GitHub
 * repository it was built from and, when the player presses Update now, downloads that
 * release's package for this system, checks it and installs it in place (updater.c). Nothing
 * goes online until the player presses Check for updates.
 */
#ifndef UPDATER_H
#define UPDATER_H

enum { UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_CONFIRMING, UPD_INSTALLED, UPD_FAILED };

typedef struct {
    int state;                /* UPD_* (UPD_CONFIRMING: Android is asking the player to confirm the update) */
    int can_install;          /* UPD_AVAILABLE: Update now can install it (else only the release page helps) */
    char latest[64];          /* the newest release's version, once a check found it */
    char page[256];           /* its page on GitHub */
    char text[400];           /* what the menu shows (a menu INFO row holds as much) */
} update_info;

const char *updater_version(void);   /* this build: "1.2.0", "1.2.0-3-gabc1234" (commits after the tag) or "dev" */
const char *updater_edition(void);   /* "Windows", "Linux", "Steam Deck", "Android", "Android, phone" */
void updater_set_touch(int touch);   /* Android: the phone edition, which has its own package */

/* main thread, once at start: finds this program and removes what an interrupted update left */
void updater_startup(void);

/* main thread: start a check, or the installation of what the check found, in the background */
void updater_check(void);
void updater_install(void);
void updater_poll(update_info *out); /* the state, any time */

/* main thread, before the program exits: waits while the new version is being moved into place
   (a matter of milliseconds), then keeps any download still running from starting that step */
void updater_shutdown(void);

/* After the program has shut down for "Restart now": starts the installed version with argv. On
   Linux the new version replaces this process (a Steam session carries on) and this only returns
   when that failed; on Windows it returns 1 once the new process runs (the caller then exits). */
int updater_restart(char **argv);

/* --update-check: prints what a check finds. --update-test: the same as if this build were version
   `pretend`, then installs into this program's own folder and restarts into the new version with
   argv `then` (for tests in a scratch folder). Both return the exit status. */
int updater_selftest(const char *pretend, char **then);

#endif
