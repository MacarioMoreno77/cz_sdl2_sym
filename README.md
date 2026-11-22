# cz_sdl2_sym

**cz_sdl2_sym** is a lightweight `LD_PRELOAD` shim (`libcz_sdl2_sym.so`) for the **M5Stack CardputerZero** that provides standard 104-key `evdev` keyboard events to **SDL2** applications reading raw `/dev/input/event*` devices directly (for example, when running under the `kmsdrm` video backend without a Wayland/X11 compositor).

On the CardputerZero, the built-in `tca8418` keypad driver maps `Sym` combinations (`_`, `-`, `[`, `]`, `{`, `}`, `!`–`)`, etc.) to repurposed Linux keycodes intended for a custom system keymap, which raw-`evdev` SDL2 apps ignore or misinterpret. By intercepting `evdev` reads from the internal keypad and translating those codes into standard US 104-key `EV_KEY` events (with synthetic `Shift` state where needed), **cz_sdl2_sym** restores full symbol input in fantasy consoles and standalone SDL2 apps such as **TIC-80** and **PICO-8**—supporting multiple CardputerZero keyboard revisions and layouts (`v0.5`, `v0.3`, and early prototype layouts) without recompiling `libsdl2`, modifying kernel modules, or affecting external USB/Bluetooth keyboards.

## Root Cause (from M5Stack's Kernel Driver)

In [`m5stack/m5stack-linux-dtoverlays`](https://github.com/m5stack/m5stack-linux-dtoverlays) (`modules/tca8418-1.0/tca8418_keypad_m5stack.c` and `modules/CardputerZero/cardputerzero-v5-overlay.dts`):

1. The `tca8418_keypad_m5stack` kernel module **already runs the `Sym` state machine** (hold, one-shot tap, and double-tap lock) inside the kernel.
2. Because a Linux `matrix-keypad` device-tree table cannot attach a `Shift` modifier to individual entries, M5Stack mapped `linux,keymap1` (the `Sym` layer) to **32 repurposed Linux keycodes** (`26..94` on v0.5 hardware, e.g. `Sym+E` -> `70` `KEY_SCROLLLOCK` for `_`, `Sym+R` -> `71` `KEY_KP7` for `-`, `Sym+U` -> `74` `KEY_KPMINUS` for `[`, `Sym+O` -> `76` `KEY_KP5` for `{`), paired with a custom keymap (`/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map`).
3. Under `kmsdrm`, SDL2's `SDL_EVDEV_Poll()` reads `/dev/input/event*` directly and maps Linux keycodes through a hardcoded US 104-key scancode table—so `KEY_SCROLLLOCK`, `KEY_KP7`, `KEY_KP5`, etc. never become `_`, `-`, `[`, `{` in `SDL_KEYDOWN`, `SDL_GetKeyboardState()`, or `SDL_TEXTINPUT`.

## How This Shim Works

`libcz_sdl2_sym.so` intercepts `read()`, `poll()`, `ioctl()`, and `close()` on `/dev/input/event*` file descriptors matching the internal `tca8418` keypad (leaving external USB/Bluetooth keyboards untouched):

- Rewrites all 32 `linux,keymap1` `Sym` keycodes into standard US 104-key `EV_KEY` codes (`KEY_MINUS`, `KEY_LEFTBRACE`, `KEY_1`..`KEY_0`, etc.).
- Automatically injects and releases synthetic `KEY_LEFTSHIFT` events around shifted symbols (`_`, `{`, `}`, `+`, `~`, `:`, `"`, `<`, `>`, `|`, `?`, `!`–`)`), so `SDL_KEYDOWN`, `SDL_GetKeyboardState()`, `SDL_GetModState()`, and `SDL_TEXTINPUT` all stay synchronized (required by TIC-80).
- Swallows the raw `KEY_CD` (`Sym`) and `KEY_DVD` (`Fn`) indicator events.
- Sanitizes `KDGKBENT` lookups for US punctuation keys in case `loadkeys` modified the active VT keymap.

## Build & Install (on CardputerZero)

CardputerZero ships with `gcc` and `build-essential` preinstalled:

```bash
make
make test
sudo make install
```

*(Optional)* Build the interactive SDL2 KMSDRM key-event diagnostic tool:

```bash
make keytest
./bin/cz-sdl2-run ./build/cz_sdl2_keytest
```

## Usage

### 1. From a Terminal / SSH Session

Run **TIC-80**, **PICO-8**, or any other `kmsdrm` / raw-`evdev` SDL2 application using `LD_PRELOAD` directly (or via the included `cz-sdl2-run` helper):

```bash
# TIC-80
LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so tic80-c0

# PICO-8 (64-bit Raspberry Pi / ARM64 build)
SDL_VIDEODRIVER=kmsdrm LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so pico8_64
```

Or using `cz-sdl2-run`:

```bash
cz-sdl2-run tic80-c0
SDL_VIDEODRIVER=kmsdrm cz-sdl2-run pico8_64
```

### 2. Integrating with CardputerZero's AppLauncher (`.desktop` Files)

CardputerZero's main UI launcher (`APPLaunch`) enforces executable allowlisting on `.desktop` entries, so inline environment assignments (such as `Exec=env LD_PRELOAD=...` or `Exec=LD_PRELOAD=...`) are rejected.

Instead, put the `LD_PRELOAD` directive inside a small wrapper shell script and invoke it with **`Exec=/bin/bash <script_path>`** in the `.desktop` file:

#### TIC-80

1. Create a wrapper script (e.g., `/home/pi/.local/bin/tic80-cz.sh`):
   ```bash
   #!/bin/bash
   export LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so
   exec /usr/bin/tic80-c0 "$@"
   ```
   ```bash
   chmod +x /home/pi/.local/bin/tic80-cz.sh
   ```

2. Point the `.desktop` entry's `Exec=` line to `/bin/bash`:
   ```ini
   [Desktop Entry]
   Type=Application
   Name=TIC-80
   Comment=TIC-80 Fantasy Computer
   Exec=/bin/bash /home/pi/.local/bin/tic80-cz.sh
   Terminal=false
   Categories=Game;
   ```

#### PICO-8

1. Create a wrapper script (e.g., `/home/pi/.local/bin/pico8-cz.sh`, adjusting the path to `pico8_64` to match your install):
   ```bash
   #!/bin/bash
   export SDL_VIDEODRIVER=kmsdrm
   export LD_PRELOAD=/usr/local/lib/libcz_sdl2_sym.so
   exec /home/pi/pico-8/pico8_64 "$@"
   ```
   ```bash
   chmod +x /home/pi/.local/bin/pico8-cz.sh
   ```

2. Point the `.desktop` entry's `Exec=` line to `/bin/bash`:
   ```ini
   [Desktop Entry]
   Type=Application
   Name=PICO-8
   Comment=PICO-8 Fantasy Console
   Exec=/bin/bash /home/pi/.local/bin/pico8-cz.sh
   Terminal=false
   Categories=Game;
   ```

## Environment Variables (Optional)

| Variable | Default | Description |
|---|---|---|
| `CZ_SYM_DEVICE` | `tca8418` | Case-insensitive substring matched against `EVIOCGNAME` (`*` matches any evdev device). |
| `CZ_SYM_LAYOUT` | `v0.5` | `v0.5` matches shipped CardputerZero hardware (`E=_`, `R=-`, `U=[`, `O={`); set `proto` for the early Kickstarter render layout. |
| `CZ_SYM_DEBUG` | `0` | Set to `1` to log device attachment and translated keycodes to `stderr`. |

## Author & License

- **Author:** `@macario_moreno`
- **License:** Licensed under the [Apache License, Version 2.0](LICENSE).

## Disclaimer

This is a personal project. The views, code, and opinions expressed here are my own and do not represent those of my current or past employers. Neither those of M5Stack, with whom I am not affiliated.
