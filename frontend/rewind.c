/*
 * rewind.c - see rewind.h.
 *
 * Records live in one byte ring, each record contiguous (a record that would
 * cross the end of the ring is placed at the start instead). An index ring
 * remembers where each record is. Records are made and dropped in order, so
 * when a new record needs room only the oldest record can be in the way.
 *
 * Stepping back leaves the record in place (only the index forgets it), so
 * until the next push the step can be undone by applying the same record again.
 *
 * Record format: pairs of <zero run><literal run><literal bytes>, both runs as
 * LEB128-style varints, until the state length is covered. The literal bytes are
 * the XOR of the new state with the previous one, so applying a record to the
 * newer state gives back the older one.
 */
#include "rewind.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { size_t off, len; } rec_t;

static size_t   ssize;            /* bytes per state */
static uint8_t *cur;              /* the most recently pushed state */
static int      have_cur;
static uint8_t *enc;              /* scratch for encoding one record */
static uint8_t *ring;
static size_t   cap, head;        /* ring capacity, next write offset */
static rec_t   *idx;
static int      idx_cap, idx_first, idx_count;   /* circular: oldest at idx_first */
static size_t   used;
static int      redo_count;       /* records stepped back over and still intact, newest last */

static void drop_oldest(void) {
    if (idx_count == 0) return;
    used -= idx[idx_first].len;
    idx_first = (idx_first + 1) % idx_cap;
    idx_count--;
}

int rewind_init(size_t state_size, size_t budget_bytes) {
    rewind_shutdown();
    if (state_size == 0) return 0;
    ssize = state_size;
    size_t worst = state_size + state_size / 2 + 32;   /* a record that changes every other byte */
    if (budget_bytes < worst * 4) budget_bytes = worst * 4;
    cur = malloc(ssize);
    enc = malloc(worst);
    ring = malloc(budget_bytes);
    idx_cap = (int)(budget_bytes / 48) + 16;          /* records are rarely smaller than ~48 bytes */
    idx = malloc((size_t)idx_cap * sizeof *idx);
    if (!cur || !enc || !ring || !idx) { rewind_shutdown(); return 0; }
    cap = budget_bytes;
    rewind_clear();
    return 1;
}

void rewind_shutdown(void) {
    free(cur); free(enc); free(ring); free(idx);
    cur = enc = ring = NULL; idx = NULL;
    ssize = cap = head = used = 0;
    idx_cap = idx_first = idx_count = 0;
    have_cur = 0;
}

void rewind_clear(void) {
    head = used = 0;
    idx_first = idx_count = 0;
    have_cur = 0;
    redo_count = 0;
}

static size_t put_varint(uint8_t *p, size_t v) {
    size_t n = 0;
    while (v >= 0x80) { p[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}

static size_t get_varint(const uint8_t *p, size_t *v) {
    size_t n = 0, shift = 0, r = 0;
    for (;;) {
        uint8_t b = p[n++];
        r |= (size_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    *v = r;
    return n;
}

/* XOR difference of a and b as zero-run / literal-run pairs; returns the record length */
static size_t encode(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < ssize) {
        size_t z = 0;
        while (i + z < ssize && a[i + z] == b[i + z]) z++;
        i += z;
        /* a literal run ends at four unchanged bytes in a row (or the end) */
        size_t l = 0;
        while (i + l < ssize) {
            if (a[i + l] == b[i + l] && i + l + 3 < ssize &&
                a[i + l + 1] == b[i + l + 1] && a[i + l + 2] == b[i + l + 2] && a[i + l + 3] == b[i + l + 3]) break;
            if (a[i + l] == b[i + l] && i + l + 3 >= ssize) {
                /* unchanged tail: stop the literal here */
                size_t k = l; int same = 1;
                for (size_t j = i + k; j < ssize; j++) if (a[j] != b[j]) { same = 0; break; }
                if (same) break;
            }
            l++;
        }
        o += put_varint(out + o, z);
        o += put_varint(out + o, l);
        for (size_t j = 0; j < l; j++) out[o + j] = (uint8_t)(a[i + j] ^ b[i + j]);
        o += l; i += l;
        if (i >= ssize) break;
    }
    return o;
}

/* state ^= record */
static void apply(uint8_t *state, const uint8_t *rec, size_t len) {
    size_t i = 0, o = 0;
    while (o < len && i < ssize) {
        size_t z, l;
        o += get_varint(rec + o, &z);
        o += get_varint(rec + o, &l);
        i += z;
        for (size_t j = 0; j < l && i + j < ssize; j++) state[i + j] ^= rec[o + j];
        o += l; i += l;
    }
}

void rewind_push(const void *state) {
    if (!ring) return;
    redo_count = 0;                                      /* the timeline goes on from here: no going forward into the old one */
    if (!have_cur) { memcpy(cur, state, ssize); have_cur = 1; return; }
    size_t len = encode((const uint8_t *)state, cur, enc);
    if (head + len > cap) head = 0;                      /* would cross the end: start over at the front */
    /* make room: the oldest records are the only ones that can overlap [head, head + len) */
    while (idx_count > 0) {
        rec_t *o = &idx[idx_first];
        int overlap = o->off < head + len && head < o->off + o->len;
        if (!overlap) break;
        drop_oldest();
    }
    if (idx_count == idx_cap) drop_oldest();
    memcpy(ring + head, enc, len);
    int slot = (idx_first + idx_count) % idx_cap;
    idx[slot].off = head; idx[slot].len = len;
    idx_count++;
    head += len;
    used += len;
    memcpy(cur, state, ssize);
}

int rewind_pop(void *state_out) {
    if (!ring || !have_cur || idx_count == 0) return 0;
    int slot = (idx_first + idx_count - 1) % idx_cap;
    apply(cur, ring + idx[slot].off, idx[slot].len);
    used -= idx[slot].len;
    idx_count--;
    redo_count++;                                        /* its bytes stay where they are until the next push */
    head = idx[slot].off;                                /* the freed space is reused next */
    memcpy(state_out, cur, ssize);
    return 1;
}

int rewind_redo(void *state_out) {
    if (!ring || !have_cur || redo_count == 0) return 0;
    int slot = (idx_first + idx_count) % idx_cap;        /* the record just after the newest one kept */
    apply(cur, ring + idx[slot].off, idx[slot].len);     /* XOR again: back to the newer state */
    used += idx[slot].len;
    idx_count++;
    redo_count--;
    head = idx[slot].off + idx[slot].len;
    memcpy(state_out, cur, ssize);
    return 1;
}

int rewind_redo_count(void) { return redo_count; }
void rewind_forget_redo(void) { redo_count = 0; }

int rewind_count(void) { return idx_count; }
size_t rewind_bytes_used(void) { return used; }
