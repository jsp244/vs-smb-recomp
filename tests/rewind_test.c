/*
 * rewind_test.c - exercises frontend/rewind.c with synthetic states: random small
 * changes, worst-case changes, eviction under a tiny budget, and push/pop/push
 * sequences. Build: gcc -O2 -std=gnu11 -Wall -Ifrontend tests/rewind_test.c frontend/rewind.c -o build/rewind_test
 */
#include "rewind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

#define N 24096
static uint8_t hist[400][N];

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void mutate(uint8_t *s, int bytes) {
    for (int i = 0; i < bytes; i++) s[rnd() % N] = (uint8_t)rnd();
}

int main(void) {
    uint8_t *tmp = malloc(N);

    /* 1. push 300 states with ~300 random byte changes each, pop them all back in order */
    CHECK(rewind_init(N, 4u << 20), "init with a 4 MB budget");
    for (int i = 0; i < N; i++) hist[0][i] = (uint8_t)rnd();
    rewind_push(hist[0]);
    for (int k = 1; k < 300; k++) {
        memcpy(hist[k], hist[k - 1], N);
        mutate(hist[k], 300);
        if (k == 150) memset(hist[k] + 1000, 0x5A, 5000);   /* a big contiguous change (CHR upload) */
        rewind_push(hist[k]);
    }
    CHECK(rewind_count() == 299, "299 records after 300 pushes");
    printf("      ring bytes used: %zu (%.0f bytes per record)\n", rewind_bytes_used(), rewind_bytes_used() / 299.0);
    int ok = 1;
    for (int k = 298; k >= 0; k--) {
        if (!rewind_pop(tmp) || memcmp(tmp, hist[k], N) != 0) { ok = 0; printf("      mismatch at record %d\n", k); break; }
    }
    CHECK(ok, "every pop returns the exact earlier state");
    CHECK(rewind_count() == 0 && rewind_pop(tmp) == 0, "history empty after popping everything");

    /* 2. push after a partial rewind continues from the rewound point */
    for (int k = 1; k < 20; k++) rewind_push(hist[k]);
    rewind_pop(tmp); rewind_pop(tmp);                     /* back to hist[17] */
    CHECK(memcmp(tmp, hist[17], N) == 0, "two pops reach state 17");
    memcpy(tmp, hist[17], N); mutate(tmp, 50);
    uint8_t *branch = malloc(N); memcpy(branch, tmp, N);
    rewind_push(branch);
    rewind_pop(tmp);
    CHECK(memcmp(tmp, hist[17], N) == 0, "pop after a branch push returns to state 17");
    for (int k = 16; k >= 0; k--) rewind_pop(tmp);
    CHECK(memcmp(tmp, hist[0], N) == 0 && rewind_count() == 0, "the rest of the history is intact");

    /* 3. eviction: a tiny budget keeps only the newest records, and they stay correct */
    rewind_shutdown();
    CHECK(rewind_init(N, 1), "init with the minimum budget");
    rewind_push(hist[0]);
    for (int k = 1; k < 300; k++) rewind_push(hist[k]);
    int kept = rewind_count();
    printf("      kept %d records in %zu bytes\n", kept, rewind_bytes_used());
    CHECK(kept >= 2 && kept < 299, "old records were evicted");
    ok = 1;
    for (int k = 298; k >= 299 - kept; k--) {
        if (!rewind_pop(tmp) || memcmp(tmp, hist[k], N) != 0) { ok = 0; printf("      mismatch at record %d\n", k); break; }
    }
    CHECK(ok && rewind_pop(tmp) == 0, "the kept records pop back exactly, then the history is empty");

    /* 4. worst case: every byte changes between pushes */
    rewind_shutdown();
    CHECK(rewind_init(N, 2u << 20), "init with 2 MB");
    rewind_push(hist[0]);
    for (int k = 1; k < 40; k++) { for (int i = 0; i < N; i++) hist[k][i] = (uint8_t)rnd(); rewind_push(hist[k]); }
    ok = 1;
    int n = rewind_count();
    for (int k = 38; k >= 39 - n; k--) if (!rewind_pop(tmp) || memcmp(tmp, hist[k], N) != 0) { ok = 0; break; }
    CHECK(ok && n > 10, "all-bytes-change records round-trip");

    /* 5. forward again: pop k, redo k gives the newer states back exactly; a push forgets the rest */
    rewind_shutdown();
    CHECK(rewind_init(N, 4u << 20), "init for the forward-again tests");
    for (int i = 0; i < N; i++) hist[0][i] = (uint8_t)rnd();
    rewind_push(hist[0]);
    for (int k = 1; k < 60; k++) { memcpy(hist[k], hist[k - 1], N); mutate(hist[k], 200); rewind_push(hist[k]); }
    for (int k = 58; k >= 30; k--) rewind_pop(tmp);                 /* back to state 30 */
    CHECK(memcmp(tmp, hist[30], N) == 0 && rewind_redo_count() == 29, "29 steps back to state 30, all 29 can be undone");
    ok = 1;
    for (int k = 31; k <= 50; k++) if (!rewind_redo(tmp) || memcmp(tmp, hist[k], N) != 0) { ok = 0; printf("      forward mismatch at %d\n", k); break; }
    CHECK(ok && rewind_redo_count() == 9 && rewind_count() == 50, "forward again through states 31-50, exact");
    rewind_pop(tmp); rewind_pop(tmp);                                /* back to 48, 11 can be undone */
    CHECK(memcmp(tmp, hist[48], N) == 0 && rewind_redo_count() == 11, "stepping back again in the middle keeps the rest");
    for (int k = 49; k <= 59; k++) rewind_redo(tmp);
    CHECK(memcmp(tmp, hist[59], N) == 0 && rewind_redo(tmp) == 0 && rewind_redo_count() == 0, "forward stops at the newest state");
    for (int k = 58; k >= 40; k--) rewind_pop(tmp);
    memcpy(tmp, hist[40], N); mutate(tmp, 77);
    rewind_push(tmp);                                                /* play on from state 40: a new branch */
    CHECK(rewind_redo_count() == 0 && rewind_redo(tmp) == 0, "a push forgets the stretch that was stepped back over");
    rewind_pop(tmp);
    CHECK(memcmp(tmp, hist[40], N) == 0, "and stepping back from the branch reaches state 40");
    for (int k = 39; k >= 0; k--) rewind_pop(tmp);
    CHECK(memcmp(tmp, hist[0], N) == 0 && rewind_count() == 0, "the history under the branch is intact");

    /* 6. clear forgets everything */
    rewind_push(hist[0]); rewind_push(hist[1]);
    rewind_clear();
    CHECK(rewind_count() == 0 && rewind_pop(tmp) == 0, "clear empties the history");
    rewind_shutdown();

    free(tmp); free(branch);
    printf(failures ? "%d FAILURE(S)\n" : "all rewind tests passed\n", failures);
    return failures ? 1 : 0;
}
