/*
 * Copyright 2026 @macario_moreno
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Disclaimer: This is a personal project. The views, code, and opinions
 * expressed here are my own and do not represent those of my current or past
 * employers. Neither those of M5Stack, with whom I am not affiliated.
 *
 * cz_sdl2_sym_shim.c - LD_PRELOAD evdev shim for M5Stack CardputerZero + SDL2 (KMSDRM)
 *
 * Why this exists:
 *   On CardputerZero, the tca8418_keypad_m5stack kernel driver handles the Sym
 *   modifier (hold, one-shot tap, and double-tap lock) in kernel space by
 *   switching to `linux,keymap1`, which emits 32 repurposed keycodes (26..94 on
 *   v0.5 hardware, 183..233 on v0.3) intended for a custom keymap
 *   (/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map).
 *
 *   When SDL2 runs under the `kmsdrm` video driver, `SDL_EVDEV_Poll()` reads
 *   raw `struct input_event` packets from `/dev/input/event*` and maps Linux
 *   keycodes using a hardcoded US 104-key scancode table—causing Sym combinations
 *   like `_`, `-`, `[`, `]`, `{`, `}` to either fail or emit wrong keys.
 *
 * What this shim does:
 *   1. Identifies file descriptors opened on the built-in `tca8418` keypad
 *      (leaving external USB/Bluetooth keyboards untouched).
 *   2. Intercepts `read()` on that evdev fd and translates CardputerZero's
 *      `linux,keymap1` Sym keycodes (plus raw Sym + key combos) into standard
 *      US 104-key `EV_KEY` codes, injecting synthetic `KEY_LEFTSHIFT` press/release
 *      events whenever the target symbol is a shifted character (`_`, `{`, `}`, etc.).
 *   3. Swallows the raw `KEY_CD` (Sym) and `KEY_DVD` (Fn) indicator keycodes so
 *      SDL2 apps do not receive spurious media key events.
 *   4. Normalizes `KDGKBENT` console keymap lookups for standard US punctuation
 *      keys in case `loadkeys` overwrote keycodes 26..53 on the active VT.
 *
 * Usage:
 *   LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so tic80-c0
 *   SDL_VIDEODRIVER=kmsdrm LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so pico8_64
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/keyboard.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef KEY_CD
#define KEY_CD 0x17f /* 383: emitted by tca8418_keypad_m5stack for Sym */
#endif
#ifndef KEY_DVD
#define KEY_DVD 0x185 /* 389: emitted by tca8418_keypad_m5stack for Fn */
#endif

#define MAX_TRACKED_FDS 256
#define PENDING_CAP     256

typedef ssize_t (*read_fn_t)(int, void*, size_t);
typedef ssize_t (*read_chk_fn_t)(int, void*, size_t, size_t);
typedef int (*close_fn_t)(int);
typedef int (*ioctl_fn_t)(int, unsigned long, ...);
typedef int (*poll_fn_t)(struct pollfd*, nfds_t, int);
typedef int (*ppoll_fn_t)(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*);
typedef int (*dup2_fn_t)(int, int);
typedef int (*dup3_fn_t)(int, int, int);

static read_fn_t real_read_fn = NULL;
static read_chk_fn_t real_read_chk_fn = NULL;
static close_fn_t real_close_fn = NULL;
static ioctl_fn_t real_ioctl_fn = NULL;
static poll_fn_t real_poll_fn = NULL;
static ppoll_fn_t real_ppoll_fn = NULL;
static dup2_fn_t real_dup2_fn = NULL;
static dup3_fn_t real_dup3_fn = NULL;

typedef struct {
    uint16_t code; /* Standard US 104-key EV_KEY code (0 means unmapped) */
    bool shift;    /* Needs synthetic KEY_LEFTSHIFT */
} sym_target_t;

typedef enum {
    FD_STATE_UNKNOWN = 0,
    FD_STATE_PASSTHROUGH,
    FD_STATE_KEYPAD,
} fd_kind_t;

typedef struct {
    bool active;
    uint16_t out_code;
    bool used_synth_shift;
    bool released_early;
} active_key_t;

typedef struct {
    fd_kind_t kind;

    /* Ring buffer of translated input_events ready for caller */
    struct input_event pending[PENDING_CAP];
    size_t head;
    size_t count;

    /* Modifier & rollover tracking */
    bool phys_shift_down;
    bool synth_shift_emitted;
    int synth_shift_refcount;
    bool raw_sym_down;
    bool frame_has_events;

    /* Active pressed keys indexed by incoming ev.code */
    active_key_t active_in[KEY_MAX + 1];
    /* Reference count of currently held output keycodes */
    uint8_t out_refcount[KEY_MAX + 1];
} fd_slot_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static fd_slot_t g_fds[MAX_TRACKED_FDS];

static bool g_config_loaded = false;
static bool g_debug = false;
static bool g_use_proto_layout = false;
static int g_force_fd = -1;
static char g_device_match[64] = "tca8418";

static sym_target_t g_keymap1_table[KEY_MAX + 1];
static sym_target_t g_raw_sym_table[KEY_MAX + 1];

static void resolve_libc_symbols(void) {
    if (!real_read_fn)
        real_read_fn = (read_fn_t)dlsym(RTLD_NEXT, "read");
    if (!real_read_chk_fn)
        real_read_chk_fn = (read_chk_fn_t)dlsym(RTLD_NEXT, "__read_chk");
    if (!real_close_fn)
        real_close_fn = (close_fn_t)dlsym(RTLD_NEXT, "close");
    if (!real_ioctl_fn)
        real_ioctl_fn = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_poll_fn)
        real_poll_fn = (poll_fn_t)dlsym(RTLD_NEXT, "poll");
    if (!real_ppoll_fn)
        real_ppoll_fn = (ppoll_fn_t)dlsym(RTLD_NEXT, "ppoll");
    if (!real_dup2_fn)
        real_dup2_fn = (dup2_fn_t)dlsym(RTLD_NEXT, "dup2");
    if (!real_dup3_fn)
        real_dup3_fn = (dup3_fn_t)dlsym(RTLD_NEXT, "dup3");
}

static void set_sym_pair(uint16_t v5_code, uint16_t v3_code, uint16_t base_key,
                         uint16_t target_code, bool shift) {
    sym_target_t t = { .code = target_code, .shift = shift };
    if (v5_code <= KEY_MAX)
        g_keymap1_table[v5_code] = t;
    if (v3_code <= KEY_MAX)
        g_keymap1_table[v3_code] = t;
    if (base_key <= KEY_MAX)
        g_raw_sym_table[base_key] = t;
}

static void init_tables_locked(void) {
    if (g_config_loaded)
        return;
    g_config_loaded = true;

    const char* dbg = getenv("CZ_SYM_DEBUG");
    if (dbg && dbg[0] != '\0' && strcmp(dbg, "0") != 0)
        g_debug = true;

    const char* dev = getenv("CZ_SYM_DEVICE");
    if (dev && dev[0] != '\0') {
        snprintf(g_device_match, sizeof(g_device_match), "%s", dev);
    }

    const char* ffd = getenv("CZ_SYM_FORCE_FD");
    if (ffd && ffd[0] != '\0') {
        g_force_fd = atoi(ffd);
    }

    const char* layout = getenv("CZ_SYM_LAYOUT");
    if (layout && (strcasecmp(layout, "proto") == 0 || strcasecmp(layout, "render") == 0)) {
        g_use_proto_layout = true;
    }

    memset(g_keymap1_table, 0, sizeof(g_keymap1_table));
    memset(g_raw_sym_table, 0, sizeof(g_raw_sym_table));

    /*
     * Number row (identical on shipped v0.5, v0.3, and prototype render):
     *   1!  2@  3#  4$  5%  6^  7&  8*  9(  0)
     */
    set_sym_pair(26, 0xb7, KEY_1, KEY_1, true);  /* ! */
    set_sym_pair(27, 0xb8, KEY_2, KEY_2, true);  /* @ */
    set_sym_pair(39, 0xb9, KEY_3, KEY_3, true);  /* # */
    set_sym_pair(40, 0xba, KEY_4, KEY_4, true);  /* $ */
    set_sym_pair(41, 0xbb, KEY_5, KEY_5, true);  /* % */
    set_sym_pair(43, 0xbc, KEY_6, KEY_6, true);  /* ^ */
    set_sym_pair(51, 0xbd, KEY_7, KEY_7, true);  /* & */
    set_sym_pair(52, 0xbe, KEY_8, KEY_8, true);  /* * */
    set_sym_pair(53, 0xbf, KEY_9, KEY_9, true);  /* ( */
    set_sym_pair(94, 0xc0, KEY_0, KEY_0, true);  /* ) */

    if (!g_use_proto_layout) {
        /*
         * Shipped CardputerZero hardware layout (v0.5 / v0.3)
         * Matches m5stack-linux-dtoverlays/modules/tca8418-1.0/tca8418_keypad_m5stack_keymap.map:
         *   Row 1: Q~  W`  E_  R-  T+  Y=  U[  I]  O{  P}
         *   Row 2: A;  S:  D'  (F) G"  H<  J>  K\  L|
         *   Row 3: (Z) (X) (C) V,  B.  N/  M?
         */
        set_sym_pair(55, 0xc1, KEY_Q, KEY_GRAVE,      true);   /* ~ */
        set_sym_pair(69, 0xc2, KEY_W, KEY_GRAVE,      false);  /* ` */
        set_sym_pair(70, 0xd4, KEY_E, KEY_MINUS,      true);   /* _ */
        set_sym_pair(71, 0xc4, KEY_R, KEY_MINUS,      false);  /* - */
        set_sym_pair(72, 0xc3, KEY_T, KEY_EQUAL,      true);   /* + */
        set_sym_pair(73, 0xd1, KEY_Y, KEY_EQUAL,      false);  /* = */
        set_sym_pair(74, 0xc9, KEY_U, KEY_LEFTBRACE,  false);  /* [ */
        set_sym_pair(75, 0xca, KEY_I, KEY_RIGHTBRACE, false);  /* ] */
        set_sym_pair(76, 0xc7, KEY_O, KEY_LEFTBRACE,  true);   /* { */
        set_sym_pair(77, 0xc8, KEY_P, KEY_RIGHTBRACE, true);   /* } */

        set_sym_pair(79, 0xd3, KEY_A, KEY_SEMICOLON,  false);  /* ; */
        set_sym_pair(80, 0xd2, KEY_S, KEY_SEMICOLON,  true);   /* : */
        set_sym_pair(81, 0xd8, KEY_D, KEY_APOSTROPHE, false);  /* ' */
        set_sym_pair(82, 0xd9, KEY_G, KEY_APOSTROPHE, true);   /* " */
        set_sym_pair(83, 0xd6, KEY_H, KEY_COMMA,      true);   /* < */
        set_sym_pair(85, 0xd7, KEY_J, KEY_DOT,        true);   /* > */
        set_sym_pair(86, 0xc6, KEY_K, KEY_BACKSLASH,  false);  /* \ */
        set_sym_pair(89, 0xe9, KEY_L, KEY_BACKSLASH,  true);   /* | */

        set_sym_pair(90, 0xe7, KEY_V, KEY_COMMA,      false);  /* , */
        set_sym_pair(91, 0xe8, KEY_B, KEY_DOT,        false);  /* . */
        set_sym_pair(92, 0xc5, KEY_N, KEY_SLASH,      false);  /* / */
        set_sym_pair(93, 0xd5, KEY_M, KEY_SLASH,      true);   /* ? */
    } else {
        /*
         * Early Kickstarter render layout (selectable via CZ_SYM_LAYOUT set to proto):
         *   Row 1: Q~  W`  E+  R-  T/  Y\  U{  I}  O[  P]
         *   Row 2: (A) (S) (D) F|  G=  H:  J;  K_  L?
         *   Row 3: (Z) (X) (C) V<  B>  N'  M"
         */
        set_sym_pair(55, 0xc1, KEY_Q, KEY_GRAVE,      true);   /* ~ */
        set_sym_pair(69, 0xc2, KEY_W, KEY_GRAVE,      false);  /* ` */
        set_sym_pair(70, 0xd4, KEY_E, KEY_EQUAL,      true);   /* + */
        set_sym_pair(71, 0xc4, KEY_R, KEY_MINUS,      false);  /* - */
        set_sym_pair(72, 0xc3, KEY_T, KEY_SLASH,      false);  /* / */
        set_sym_pair(73, 0xd1, KEY_Y, KEY_BACKSLASH,  false);  /* \ */
        set_sym_pair(74, 0xc9, KEY_U, KEY_LEFTBRACE,  true);   /* { */
        set_sym_pair(75, 0xca, KEY_I, KEY_RIGHTBRACE, true);   /* } */
        set_sym_pair(76, 0xc7, KEY_O, KEY_LEFTBRACE,  false);  /* [ */
        set_sym_pair(77, 0xc8, KEY_P, KEY_RIGHTBRACE, false);  /* ] */

        set_sym_pair(0,  0,    KEY_F, KEY_BACKSLASH,  true);   /* | */
        set_sym_pair(82, 0xd9, KEY_G, KEY_EQUAL,      false);  /* = */
        set_sym_pair(83, 0xd6, KEY_H, KEY_SEMICOLON,  true);   /* : */
        set_sym_pair(85, 0xd7, KEY_J, KEY_SEMICOLON,  false);  /* ; */
        set_sym_pair(86, 0xc6, KEY_K, KEY_MINUS,      true);   /* _ */
        set_sym_pair(89, 0xe9, KEY_L, KEY_SLASH,      true);   /* ? */

        set_sym_pair(90, 0xe7, KEY_V, KEY_COMMA,      true);   /* < */
        set_sym_pair(91, 0xe8, KEY_B, KEY_DOT,        true);   /* > */
        set_sym_pair(92, 0xc5, KEY_N, KEY_APOSTROPHE, false);  /* ' */
        set_sym_pair(93, 0xd5, KEY_M, KEY_APOSTROPHE, true);   /* " */
    }
}

static bool contains_icase(const char* haystack, const char* needle) {
    if (!haystack || !needle)
        return false;
    if (strcmp(needle, "*") == 0)
        return true;
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return false;
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

static void reset_fd_slot_locked(int fd) {
    if (fd >= 0 && fd < MAX_TRACKED_FDS) {
        memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
    }
}

static fd_kind_t classify_fd_locked(int fd) {
    if (fd < 0 || fd >= MAX_TRACKED_FDS)
        return FD_STATE_PASSTHROUGH;

    init_tables_locked();

    if (g_fds[fd].kind != FD_STATE_UNKNOWN)
        return g_fds[fd].kind;

    if (g_force_fd >= 0 && fd == g_force_fd) {
        g_fds[fd].kind = FD_STATE_KEYPAD;
        if (g_debug)
            fprintf(stderr, "[cz_sdl2_sym] attached to forced fd=%d\n", fd);
        return FD_STATE_KEYPAD;
    }

    char name[128] = {0};
    if (real_ioctl_fn && real_ioctl_fn(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
        if (contains_icase(name, g_device_match)) {
            g_fds[fd].kind = FD_STATE_KEYPAD;
            if (g_debug) {
                fprintf(stderr, "[cz_sdl2_sym] attached to evdev fd=%d name=\"%s\" layout=%s\n",
                        fd, name, g_use_proto_layout ? "proto" : "v0.5");
            }
            return FD_STATE_KEYPAD;
        }
    }

    g_fds[fd].kind = FD_STATE_PASSTHROUGH;
    return FD_STATE_PASSTHROUGH;
}

static void push_event(fd_slot_t* slot, const struct input_event* proto_ev,
                       uint16_t type, uint16_t code, int32_t value) {
    if (slot->count >= PENDING_CAP)
        return;

    size_t idx = (slot->head + slot->count) % PENDING_CAP;
    struct input_event ev = *proto_ev;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    slot->pending[idx] = ev;
    slot->count++;

    if (type != EV_SYN)
        slot->frame_has_events = true;
}

static void push_sync(fd_slot_t* slot, const struct input_event* proto_ev) {
    if (!slot->frame_has_events)
        return;
    push_event(slot, proto_ev, EV_SYN, SYN_REPORT, 0);
    slot->frame_has_events = false;
}

static void ensure_synth_shift(fd_slot_t* slot, const struct input_event* proto_ev, bool want_shift) {
    if (want_shift) {
        if (!slot->synth_shift_emitted && !slot->phys_shift_down) {
            push_event(slot, proto_ev, EV_KEY, KEY_LEFTSHIFT, 1);
            push_sync(slot, proto_ev);
            slot->synth_shift_emitted = true;
        }
    } else {
        if (slot->synth_shift_emitted) {
            push_event(slot, proto_ev, EV_KEY, KEY_LEFTSHIFT, 0);
            push_sync(slot, proto_ev);
            slot->synth_shift_emitted = false;
        }
    }
}

static void process_input_event(fd_slot_t* slot, const struct input_event* ev) {
    /* Drop EV_MSC scan codes; SDL_EVDEV ignores them and we synthesize clean EV_KEYs */
    if (ev->type == EV_MSC) {
        return;
    }

    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        push_sync(slot, ev);
        return;
    }

    if (ev->type != EV_KEY || ev->code > KEY_MAX) {
        push_event(slot, ev, ev->type, ev->code, ev->value);
        return;
    }

    uint16_t in_code = ev->code;
    int32_t val = ev->value; /* 0: release, 1: press, 2: autorepeat */

    /* Swallow CardputerZero's Sym (KEY_CD) & Fn (KEY_DVD) indicator keycodes */
    if (in_code == KEY_CD) {
        slot->raw_sym_down = (val != 0);
        return;
    }
    if (in_code == KEY_DVD || in_code == KEY_RESERVED) {
        return;
    }

    /* Track physical Left/Right Shift (Aa key) */
    if (in_code == KEY_LEFTSHIFT || in_code == KEY_RIGHTSHIFT) {
        slot->phys_shift_down = (val != 0);
        if (val != 0 && slot->synth_shift_emitted) {
            /* Physical shift took over */
            slot->synth_shift_emitted = false;
        }
        push_event(slot, ev, EV_KEY, in_code, val);
        return;
    }

    /* Release of any key */
    if (val == 0) {
        active_key_t* ak = &slot->active_in[in_code];
        if (!ak->active) {
            /* Unmatched release: forward as-is if not a dummy keymap1 code */
            if (g_keymap1_table[in_code].code == 0)
                push_event(slot, ev, EV_KEY, in_code, 0);
            return;
        }

        uint16_t out_code = ak->out_code;
        bool used_shift = ak->used_synth_shift;
        bool already_rel = ak->released_early;
        memset(ak, 0, sizeof(*ak));

        if (!already_rel) {
            if (slot->out_refcount[out_code] > 0)
                slot->out_refcount[out_code]--;
            if (slot->out_refcount[out_code] == 0) {
                push_event(slot, ev, EV_KEY, out_code, 0);
            }
        }

        if (used_shift) {
            if (slot->synth_shift_refcount > 0)
                slot->synth_shift_refcount--;
            if (slot->synth_shift_refcount == 0 && slot->synth_shift_emitted) {
                push_sync(slot, ev);
                push_event(slot, ev, EV_KEY, KEY_LEFTSHIFT, 0);
                slot->synth_shift_emitted = false;
            }
        }
        return;
    }

    /* Autorepeat (val is 2) */
    if (val == 2) {
        active_key_t* ak = &slot->active_in[in_code];
        if (ak->active && !ak->released_early) {
            push_event(slot, ev, EV_KEY, ak->out_code, 2);
        }
        return;
    }

    /* Key press (val is 1): determine if this is a Sym-mapped key */
    sym_target_t target = g_keymap1_table[in_code];
    if (target.code == 0 && slot->raw_sym_down) {
        target = g_raw_sym_table[in_code];
    }

    uint16_t out_code = (target.code != 0) ? target.code : in_code;
    bool want_synth_shift = (target.code != 0) ? target.shift : false;

    if (g_debug && target.code != 0) {
        fprintf(stderr, "[cz_sdl2_sym] in_code=%u -> out_code=%u shift=%d\n",
                in_code, out_code, want_synth_shift ? 1 : 0);
    }

    /*
     * Handle fast key rollover where another held key already pressed the same
     * output keycode (e.g. rolling from O `{` to U `[`, or E `_` to R `-`).
     */
    if (slot->out_refcount[out_code] > 0) {
        for (int k = 0; k <= KEY_MAX; ++k) {
            if (slot->active_in[k].active &&
                !slot->active_in[k].released_early &&
                slot->active_in[k].out_code == out_code) {
                slot->active_in[k].released_early = true;
            }
        }
        slot->out_refcount[out_code] = 0;
        push_event(slot, ev, EV_KEY, out_code, 0);
        push_sync(slot, ev);
    }

    ensure_synth_shift(slot, ev, want_synth_shift);

    slot->active_in[in_code].active = true;
    slot->active_in[in_code].out_code = out_code;
    slot->active_in[in_code].used_synth_shift = want_synth_shift;
    slot->active_in[in_code].released_early = false;

    if (want_synth_shift)
        slot->synth_shift_refcount++;
    if (slot->out_refcount[out_code] < UINT8_MAX)
        slot->out_refcount[out_code]++;

    push_event(slot, ev, EV_KEY, out_code, 1);
}

static size_t drain_pending(fd_slot_t* slot, struct input_event* dst, size_t max_events) {
    size_t n = (slot->count < max_events) ? slot->count : max_events;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = slot->pending[slot->head];
        slot->head = (slot->head + 1) % PENDING_CAP;
    }
    slot->count -= n;
    if (slot->count == 0)
        slot->head = 0;
    return n;
}

static ssize_t shim_read_impl(int fd, void* buf, size_t count) {
    resolve_libc_symbols();

    if (count < sizeof(struct input_event)) {
        return real_read_fn(fd, buf, count);
    }

    pthread_mutex_lock(&g_lock);
    fd_kind_t kind = classify_fd_locked(fd);
    if (kind != FD_STATE_KEYPAD) {
        pthread_mutex_unlock(&g_lock);
        return real_read_fn(fd, buf, count);
    }

    fd_slot_t* slot = &g_fds[fd];
    size_t max_out = count / sizeof(struct input_event);

    if (slot->count > 0) {
        size_t n = drain_pending(slot, (struct input_event*)buf, max_out);
        pthread_mutex_unlock(&g_lock);
        return (ssize_t)(n * sizeof(struct input_event));
    }
    pthread_mutex_unlock(&g_lock);

    /*
     * Read raw events outside the lock so blocking fds don't hold g_lock.
     * Loop if all read events were swallowed (e.g. standalone Sym press/release)
     * so non-blocking fds cleanly return -1/EAGAIN instead of 0 (EOF).
     */
    for (;;) {
        struct input_event raw[32];
        size_t ask = max_out < 32 ? max_out : 32;
        ssize_t r = real_read_fn(fd, raw, ask * sizeof(struct input_event));
        if (r <= 0) {
            return r;
        }

        size_t n_raw = (size_t)r / sizeof(struct input_event);
        if (n_raw == 0) {
            return r;
        }

        pthread_mutex_lock(&g_lock);
        slot = &g_fds[fd];
        for (size_t i = 0; i < n_raw; ++i) {
            process_input_event(slot, &raw[i]);
        }

        if (slot->count > 0) {
            size_t n = drain_pending(slot, (struct input_event*)buf, max_out);
            pthread_mutex_unlock(&g_lock);
            return (ssize_t)(n * sizeof(struct input_event));
        }
        pthread_mutex_unlock(&g_lock);
    }
}

/*
 * Standard US 104-key ASCII table for keycodes 2..53 so that even if `loadkeys`
 * loaded tca8418_keypad_m5stack_keymap.map onto the active Linux VT (which
 * remaps keycode 26 -> `!`, 27 -> `@`, etc.), SDL_evdev_kbd's KDGKBENT lookup
 * still resolves standard US punctuation characters.
 */
static bool us104_ascii_for_kdgkbent(uint8_t table, uint8_t keycode, unsigned short* out_val) {
    if (table != 0 && table != (1 << KG_SHIFT))
        return false;
    bool shift = (table == (1 << KG_SHIFT));

    unsigned char ch = 0;
    switch (keycode) {
        case KEY_1:          ch = shift ? '!' : '1'; break;
        case KEY_2:          ch = shift ? '@' : '2'; break;
        case KEY_3:          ch = shift ? '#' : '3'; break;
        case KEY_4:          ch = shift ? '$' : '4'; break;
        case KEY_5:          ch = shift ? '%' : '5'; break;
        case KEY_6:          ch = shift ? '^' : '6'; break;
        case KEY_7:          ch = shift ? '&' : '7'; break;
        case KEY_8:          ch = shift ? '*' : '8'; break;
        case KEY_9:          ch = shift ? '(' : '9'; break;
        case KEY_0:          ch = shift ? ')' : '0'; break;
        case KEY_MINUS:      ch = shift ? '_' : '-'; break;
        case KEY_EQUAL:      ch = shift ? '+' : '='; break;
        case KEY_LEFTBRACE:  ch = shift ? '{' : '['; break;
        case KEY_RIGHTBRACE: ch = shift ? '}' : ']'; break;
        case KEY_SEMICOLON:  ch = shift ? ':' : ';'; break;
        case KEY_APOSTROPHE: ch = shift ? '"' : '\''; break;
        case KEY_GRAVE:      ch = shift ? '~' : '`'; break;
        case KEY_BACKSLASH:  ch = shift ? '|' : '\\'; break;
        case KEY_COMMA:      ch = shift ? '<' : ','; break;
        case KEY_DOT:        ch = shift ? '>' : '.'; break;
        case KEY_SLASH:      ch = shift ? '?' : '/'; break;
        default:
            return false;
    }

    *out_val = K(KT_LATIN, ch);
    return true;
}

/* Exported libc overrides */

__attribute__((visibility("default")))
ssize_t read(int fd, void* buf, size_t count) {
    return shim_read_impl(fd, buf, count);
}

__attribute__((visibility("default")))
ssize_t __read_chk(int fd, void* buf, size_t nbytes, size_t buflen) {
    if (nbytes > buflen) {
        resolve_libc_symbols();
        if (real_read_chk_fn)
            return real_read_chk_fn(fd, buf, nbytes, buflen);
    }
    return shim_read_impl(fd, buf, nbytes);
}

__attribute__((visibility("default")))
int close(int fd) {
    resolve_libc_symbols();
    pthread_mutex_lock(&g_lock);
    reset_fd_slot_locked(fd);
    pthread_mutex_unlock(&g_lock);
    return real_close_fn(fd);
}

__attribute__((visibility("default")))
int dup2(int oldfd, int newfd) {
    resolve_libc_symbols();
    pthread_mutex_lock(&g_lock);
    if (oldfd != newfd)
        reset_fd_slot_locked(newfd);
    pthread_mutex_unlock(&g_lock);
    return real_dup2_fn(oldfd, newfd);
}

__attribute__((visibility("default")))
int dup3(int oldfd, int newfd, int flags) {
    resolve_libc_symbols();
    pthread_mutex_lock(&g_lock);
    if (oldfd != newfd)
        reset_fd_slot_locked(newfd);
    pthread_mutex_unlock(&g_lock);
    return real_dup3_fn(oldfd, newfd, flags);
}

__attribute__((visibility("default")))
int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    resolve_libc_symbols();

    int ready_buffered = 0;
    pthread_mutex_lock(&g_lock);
    for (nfds_t i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS &&
            g_fds[fd].kind == FD_STATE_KEYPAD &&
            g_fds[fd].count > 0 &&
            (fds[i].events & POLLIN)) {
            ready_buffered++;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (ready_buffered == 0) {
        return real_poll_fn(fds, nfds, timeout);
    }

    int rc = real_poll_fn(fds, nfds, 0);
    if (rc < 0)
        return rc;

    pthread_mutex_lock(&g_lock);
    for (nfds_t i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS &&
            g_fds[fd].kind == FD_STATE_KEYPAD &&
            g_fds[fd].count > 0 &&
            (fds[i].events & POLLIN) &&
            !(fds[i].revents & POLLIN)) {
            fds[i].revents |= POLLIN;
            rc++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

__attribute__((visibility("default")))
int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* tmo_p, const sigset_t* sigmask) {
    resolve_libc_symbols();

    int ready_buffered = 0;
    pthread_mutex_lock(&g_lock);
    for (nfds_t i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS &&
            g_fds[fd].kind == FD_STATE_KEYPAD &&
            g_fds[fd].count > 0 &&
            (fds[i].events & POLLIN)) {
            ready_buffered++;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (ready_buffered == 0) {
        return real_ppoll_fn(fds, nfds, tmo_p, sigmask);
    }

    struct timespec zero_ts = {0, 0};
    int rc = real_ppoll_fn(fds, nfds, &zero_ts, sigmask);
    if (rc < 0)
        return rc;

    pthread_mutex_lock(&g_lock);
    for (nfds_t i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS &&
            g_fds[fd].kind == FD_STATE_KEYPAD &&
            g_fds[fd].count > 0 &&
            (fds[i].events & POLLIN) &&
            !(fds[i].revents & POLLIN)) {
            fds[i].revents |= POLLIN;
            rc++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

__attribute__((visibility("default")))
int ioctl(int fd, unsigned long request, ...) {
    resolve_libc_symbols();

    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    int rc = real_ioctl_fn(fd, request, arg);
    if (rc == 0 && request == KDGKBENT && arg != NULL) {
        const char* keep = getenv("CZ_SYM_KEEP_VT_KEYMAP");
        if (!keep || keep[0] == '\0' || strcmp(keep, "0") == 0) {
            struct kbentry* kbe = (struct kbentry*)arg;
            unsigned short clean_val = 0;
            if (us104_ascii_for_kdgkbent(kbe->kb_table, kbe->kb_index, &clean_val)) {
                kbe->kb_value = clean_val;
            }
        }
    }
    return rc;
}
