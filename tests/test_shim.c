/*
 * Copyright 2026 @macario_moreno
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_shim.c - Automated test suite for libcz_sdl2_sym.so
 *
 * Simulates the exact evdev packet stream produced by CardputerZero's
 * tca8418_keypad_m5stack.ko driver over a non-blocking pipe with LD_PRELOAD
 * active, then decodes the resulting US 104-key EV_KEY + Shift stream the same
 * way SDL2 (SDL_EVDEV_Poll + SDL_evdev_kbd) and TIC-80 do.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef KEY_CD
#define KEY_CD 0x17f
#endif
#ifndef KEY_DVD
#define KEY_DVD 0x185
#endif

static void emit_raw(int wfd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t n = write(wfd, &ev, sizeof(ev));
    assert(n == (ssize_t)sizeof(ev));
}

static void emit_tca8418_key(int wfd, uint32_t scan, uint16_t keycode, int32_t value) {
    emit_raw(wfd, EV_MSC, MSC_SCAN, (int32_t)scan);
    emit_raw(wfd, EV_KEY, keycode, value);
    emit_raw(wfd, EV_SYN, SYN_REPORT, 0);
}

/* Standard US 104-key decoder matching SDL2's default_key_maps + scancode state */
static char decode_us104(uint16_t code, bool shift) {
    switch (code) {
        case KEY_1:          return shift ? '!' : '1';
        case KEY_2:          return shift ? '@' : '2';
        case KEY_3:          return shift ? '#' : '3';
        case KEY_4:          return shift ? '$' : '4';
        case KEY_5:          return shift ? '%' : '5';
        case KEY_6:          return shift ? '^' : '6';
        case KEY_7:          return shift ? '&' : '7';
        case KEY_8:          return shift ? '*' : '8';
        case KEY_9:          return shift ? '(' : '9';
        case KEY_0:          return shift ? ')' : '0';
        case KEY_MINUS:      return shift ? '_' : '-';
        case KEY_EQUAL:      return shift ? '+' : '=';
        case KEY_LEFTBRACE:  return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_SEMICOLON:  return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE:      return shift ? '~' : '`';
        case KEY_BACKSLASH:  return shift ? '|' : '\\';
        case KEY_COMMA:      return shift ? '<' : ',';
        case KEY_DOT:        return shift ? '>' : '.';
        case KEY_SLASH:      return shift ? '?' : '/';
        case KEY_Q:          return shift ? 'Q' : 'q';
        case KEY_E:          return shift ? 'E' : 'e';
        case KEY_SPACE:      return ' ';
        default:             return '\0';
    }
}

typedef struct {
    bool keystate[KEY_MAX + 1];
    char typed[256];
    size_t typed_len;
} sim_sdl_state_t;

static void pump_events(int rfd, sim_sdl_state_t* st, size_t chunk_events) {
    struct input_event buf[32];
    assert(chunk_events >= 1 && chunk_events <= 32);

    for (;;) {
        ssize_t r = read(rfd, buf, chunk_events * sizeof(struct input_event));
        if (r < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        assert(r > 0 && (r % (ssize_t)sizeof(struct input_event)) == 0);
        size_t n = (size_t)r / sizeof(struct input_event);
        for (size_t i = 0; i < n; ++i) {
            if (buf[i].type != EV_KEY)
                continue;
            uint16_t c = buf[i].code;
            int32_t v = buf[i].value;
            assert(c != KEY_CD && c != KEY_DVD);
            if (v == 1) {
                st->keystate[c] = true;
                if (c != KEY_LEFTSHIFT && c != KEY_RIGHTSHIFT) {
                    bool sh = st->keystate[KEY_LEFTSHIFT] || st->keystate[KEY_RIGHTSHIFT];
                    char ch = decode_us104(c, sh);
                    assert(ch != '\0');
                    assert(st->typed_len + 1 < sizeof(st->typed));
                    st->typed[st->typed_len++] = ch;
                    st->typed[st->typed_len] = '\0';
                }
            } else if (v == 0) {
                st->keystate[c] = false;
            }
        }
    }
}

static bool any_key_held(const sim_sdl_state_t* st) {
    for (int i = 0; i <= KEY_MAX; ++i) {
        if (st->keystate[i])
            return true;
    }
    return false;
}

int main(void) {
    int pfd[2];
    if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC) != 0) {
        perror("pipe2");
        return 1;
    }

    /* Point the preloaded shim at our synthetic evdev read end BEFORE first read() */
    char fd_str[32];
    snprintf(fd_str, sizeof(fd_str), "%d", pfd[0]);
    setenv("CZ_SYM_FORCE_FD", fd_str, 1);

    sim_sdl_state_t st;
    memset(&st, 0, sizeof(st));

    /* 1. Swallowing standalone Sym (KEY_CD) & Fn (KEY_DVD) must return -1/EAGAIN, never 0 (EOF) */
    emit_tca8418_key(pfd[1], 0x43, KEY_CD, 1);
    emit_tca8418_key(pfd[1], 0x43, KEY_CD, 0);
    emit_tca8418_key(pfd[1], 0x42, KEY_DVD, 1);
    emit_tca8418_key(pfd[1], 0x42, KEY_DVD, 0);

    struct input_event tmp;
    errno = 0;
    ssize_t rc = read(pfd[0], &tmp, sizeof(tmp));
    assert(rc == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    printf("[PASS] Swallowed KEY_CD/KEY_DVD cleanly with EAGAIN\n");

    /* 2. All 32 CardputerZero v0.5 Sym keymap1 codes in exact layout order */
    static const struct {
        uint16_t km1_code;
        char expected;
    } v5_cases[] = {
        /* Number row: 1! 2@ 3# 4$ 5% 6^ 7& 8* 9( 0) */
        { 26, '!' }, { 27, '@' }, { 39, '#' }, { 40, '$' }, { 41, '%' },
        { 43, '^' }, { 51, '&' }, { 52, '*' }, { 53, '(' }, { 94, ')' },
        /* Q..P row: Q~ W` E_ R- T+ Y= U[ I] O{ P} */
        { 55, '~' }, { 69, '`' }, { 70, '_' }, { 71, '-' }, { 72, '+' },
        { 73, '=' }, { 74, '[' }, { 75, ']' }, { 76, '{' }, { 77, '}' },
        /* A..L row: A; S: D' G" H< J> K\ L| */
        { 79, ';' }, { 80, ':' }, { 81, '\'' }, { 82, '"' },
        { 83, '<' }, { 85, '>' }, { 86, '\\' }, { 89, '|' },
        /* V..M row: V, B. N/ M? */
        { 90, ',' }, { 91, '.' }, { 92, '/' }, { 93, '?' },
    };

    char expected_str[64];
    for (size_t i = 0; i < sizeof(v5_cases) / sizeof(v5_cases[0]); ++i) {
        expected_str[i] = v5_cases[i].expected;
        emit_tca8418_key(pfd[1], 0x43, KEY_CD, 1);
        emit_tca8418_key(pfd[1], 0x10, v5_cases[i].km1_code, 1);
        pump_events(pfd[0], &st, 32);

        emit_tca8418_key(pfd[1], 0x10, v5_cases[i].km1_code, 0);
        emit_tca8418_key(pfd[1], 0x43, KEY_CD, 0);
        pump_events(pfd[0], &st, 32);

        assert(!any_key_held(&st));
    }
    expected_str[sizeof(v5_cases) / sizeof(v5_cases[0])] = '\0';

    if (strcmp(st.typed, expected_str) != 0) {
        fprintf(stderr, "Mismatch!\n  got:      %s\n  expected: %s\n", st.typed, expected_str);
        return 1;
    }
    printf("[PASS] All 32 CardputerZero Sym symbols decoded accurately: %s\n", st.typed);

    /* 3. Fast rollover between two symbols sharing the same 104-key scancode:
     *    Hold O (`{`, shifted `[`), press U (`[`, unshifted `[`) BEFORE releasing O,
     *    then release O, then release U. Both `{` and `[` must register, and no stuck Shift!
     */
    memset(&st, 0, sizeof(st));
    emit_tca8418_key(pfd[1], 0x18, 76, 1); /* O -> { */
    pump_events(pfd[0], &st, 32);
    emit_tca8418_key(pfd[1], 0x16, 74, 1); /* U -> [ while O still held */
    pump_events(pfd[0], &st, 32);
    emit_tca8418_key(pfd[1], 0x18, 76, 0); /* release O */
    pump_events(pfd[0], &st, 32);
    emit_tca8418_key(pfd[1], 0x16, 74, 0); /* release U */
    pump_events(pfd[0], &st, 32);

    assert(strcmp(st.typed, "{[") == 0);
    assert(!any_key_held(&st));
    printf("[PASS] Overlapping rollover `{` -> `[` produced \"%s\" with zero stuck keys\n", st.typed);

    /* 4. Single-event read() + poll() readiness when synthetic Shift expands 1 packet into >1 */
    memset(&st, 0, sizeof(st));
    emit_tca8418_key(pfd[1], 0x12, 70, 1); /* E -> _ (expands into LSHIFT down, SYN, MINUS down, SYN) */

    /* Read just 1 event (LSHIFT down); pipe is now drained, but shim ring buffer still has events! */
    struct input_event single;
    rc = read(pfd[0], &single, sizeof(single));
    assert(rc == (ssize_t)sizeof(single));
    assert(single.type == EV_KEY && single.code == KEY_LEFTSHIFT && single.value == 1);
    st.keystate[single.code] = true;

    struct pollfd p = { .fd = pfd[0], .events = POLLIN, .revents = 0 };
    int prc = poll(&p, 1, 0);
    assert(prc == 1 && (p.revents & POLLIN));

    /* Drain remaining events 1 by 1 */
    pump_events(pfd[0], &st, 1);
    emit_tca8418_key(pfd[1], 0x12, 70, 0);
    pump_events(pfd[0], &st, 1);

    assert(strcmp(st.typed, "_") == 0);
    assert(!any_key_held(&st));
    printf("[PASS] Single-event read() + poll() buffering works cleanly\n");

    /* 5. Normal base typing + physical Shift (Aa) passthrough */
    memset(&st, 0, sizeof(st));
    emit_tca8418_key(pfd[1], 0x41, KEY_LEFTSHIFT, 1);
    emit_tca8418_key(pfd[1], 0x10, KEY_Q, 1);
    emit_tca8418_key(pfd[1], 0x10, KEY_Q, 0);
    emit_tca8418_key(pfd[1], 0x41, KEY_LEFTSHIFT, 0);
    emit_tca8418_key(pfd[1], 0x12, KEY_E, 1);
    emit_tca8418_key(pfd[1], 0x12, KEY_E, 0);
    pump_events(pfd[0], &st, 32);

    assert(strcmp(st.typed, "Qe") == 0);
    assert(!any_key_held(&st));
    printf("[PASS] Normal keys and physical Shift (`Aa`) pass through untouched\n");

    close(pfd[0]);
    close(pfd[1]);
    printf("All tests passed!\n");
    return 0;
}
