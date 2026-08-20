# Building on Linux

The Linux build produces a native `ac6recomp` binary that renders through
**Vulkan** and windows through **GTK 3**. Windows builds use Direct3D 12 and
Win32 instead, so the two platforms exercise different backends — see
[Platform differences](#platform-differences) before reporting a bug that only
appears on one of them.

> [!NOTE]
> The Linux port is newer than the Windows one and some features are still
> Windows-only. [Platform differences](#platform-differences) lists what is
> missing today.

---

## Requirements

| | |
|---|---|
| **CPU** | Must support AVX2 — the build compiles with `-march=x86-64-v3` |
| **GPU** | Vulkan 1.2 capable, with working drivers (`vulkaninfo` should succeed) |
| **Compiler** | Clang **20** or newer. GCC is not supported. |
| **Build tools** | CMake 3.25+, Ninja |
| **Game data** | Your own legally obtained copy of Ace Combat 6 (US region only) |

### System packages

On Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config \
                 libgtk-3-dev libx11-xcb-dev \
                 libasound2-dev libpulse-dev \
                 libvulkan-dev vulkan-tools \
                 extract-xiso
```

On Fedora:

```bash
sudo dnf install cmake ninja-build pkgconf-pkg-config \
                 gtk3-devel libX11-devel libxcb-devel \
                 alsa-lib-devel pulseaudio-libs-devel \
                 vulkan-loader-devel vulkan-tools
```

`extract-xiso` is packaged on some distributions and otherwise builds in a
minute from <https://github.com/XboxDev/extract-xiso>.

### Getting Clang 20

The CMake preset asks for `clang-20` / `clang++-20` on `PATH`. If your
distribution ships them, nothing more is needed.

If it does not, download an upstream LLVM release and point CMake at it
directly. Unpack it anywhere — the commands below assume
`toolchain/LLVM-20.1.8-Linux-X64/` beside the repository — and pass the two
compiler paths on the configure line as shown in
[Option B](#option-b-explicit-toolchain).

---

## Extracting the game data

The build needs the game's `default.xex`, so the ISO must be extracted before
codegen runs:

```bash
mkdir -p assets
extract-xiso -d assets /path/to/your/AC6.iso
ls assets/default.xex     # must exist before you continue
```

This mirrors what `setup_and_build.bat` does automatically on Windows; there is
no equivalent script for Linux yet, so the steps are manual.

---

## Building

Codegen runs as a **separate first pass** that generates the recompiled
PowerPC sources, and the main configure step needs to see those files. That is
why `cmake --preset` is run twice — the second run picks up the generated
sources. Skipping the re-configure is the most common build failure.

### Option A: distribution Clang 20

```bash
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo --target ac6recomp_codegen
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo
```

### Option B: explicit toolchain

Use this when `clang-20` is not on `PATH`, or when the linker needs overriding
(see [Troubleshooting](#troubleshooting)):

```bash
TOOLCHAIN=$PWD/../toolchain/LLVM-20.1.8-Linux-X64

cmake --preset linux-amd64-relwithdebinfo \
  -DCMAKE_C_COMPILER="$TOOLCHAIN/bin/clang" \
  -DCMAKE_CXX_COMPILER="$TOOLCHAIN/bin/clang++" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"

cmake --build --preset linux-amd64-relwithdebinfo --target ac6recomp_codegen
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo
```

The binary lands at `out/build/linux-amd64-relwithdebinfo/ac6recomp`.

Other presets: `linux-amd64-debug` and `linux-amd64-release`. RelWithDebInfo is
recommended — a release build is hard to get a usable backtrace out of, and the
recompiled sources make debug builds very large and slow.

### Running

Copy the binary next to your game data and run it:

```bash
cp out/build/linux-amd64-relwithdebinfo/ac6recomp /path/to/gamedir/
cd /path/to/gamedir
./ac6recomp
```

The game directory needs either the `.iso` or an extracted `assets/` folder,
and optionally a `dlc/` folder holding extracted DLC packages. On first run
`ac6recomp.toml` is created beside the binary.

Settings, keybinds, and the in-game overlay keys are identical to Windows and
are documented in the [README](../README.md).

---

## Platform differences

Behaviour that differs from the Windows build today:

| Area | Windows | Linux |
|---|---|---|
| Graphics backend | Direct3D 12 | Vulkan |
| Windowing | Win32 | GTK 3 |
| Keyboard, mouse buttons, wheel (`ac6_kbm_enabled`) | Supported | Supported |
| Mouse **steering** | Supported | **Not yet** — see below |
| Controllers | Supported | Supported |

### Keyboard and mouse

`ac6_kbm_enabled` and `ac6_input.toml` work the same on both platforms, so the
bindings and every `ac6_kbm_*` setting are shared — nothing platform-specific to
configure:

```toml
ac6_kbm_enabled = true
```

> [!IMPORTANT]
> `ac6recomp.toml` is read as **top-level keys only**. Putting a setting under a
> `[section]` header folds the section name into the cvar name and it is
> silently ignored.

Enabling `ac6_kbm_enabled` switches `mnk_mode` off automatically — the two are
different keyboard implementations and only one can own the keyboard.

The platforms get their key state differently, which is invisible in the config
but matters if you are debugging input. Windows polls the OS with
`GetAsyncKeyState` and hooks the scroll wheel with `SetWindowsHookExW`. There is
no equivalent poll elsewhere, so every other platform accumulates key, mouse
button, and wheel state from the SDK window's `WindowInputListener` events, and
takes the input gate's focus check from window focus rather than from the
foreground window.

One consequence worth knowing: GTK reports only side-agnostic modifiers, so
`LeftControl` and `RightControl` both arrive as plain `Control`. Bindings that
name a side — the stock `Left Ctrl` for the machine gun, `Left Alt` for camera
control — are folded onto the generic key, so either side of the keyboard
triggers them.

**Mouse steering is still Windows-only.** It works by pinning the cursor to the
window centre on every poll and reading the delta, and `GTKWindow` implements
neither cursor warping nor mouse capture — it overrides only fullscreen, title,
and menu, so the cursor calls are no-ops. Supporting it needs those primitives
in the GTK backend, and on Wayland warping the pointer is not permitted at all:
that path needs the pointer-constraints and relative-pointer protocols, which is
a different implementation from X11. Until then, set the mouse mode to something
other than `steer` in `ac6_input.toml`, or fly with the keyboard or a pad.

Controllers work on both platforms with no configuration.

---

## Troubleshooting

**`clang-20` not found during configure.** The preset hard-codes that name. Use
[Option B](#option-b-explicit-toolchain) to give CMake explicit compiler paths.

**Link fails looking for `LLVMgold.so`.** An upstream LLVM tarball does not ship
the gold plugin, so the default BFD linker cannot do LTO. Configure with
`-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"` to use LLVM's own linker.

**ALSA or PulseAudio headers not found.** Install `libasound2-dev` and
`libpulse-dev`. If you cannot install them system-wide, unpack the `-dev`
packages into a local sysroot and point CMake at it:

```bash
cmake --preset linux-amd64-relwithdebinfo \
  -DCMAKE_PREFIX_PATH=/path/to/sysroot/usr
```

**Generated sources missing / codegen symbols undefined.** The second
`cmake --preset` was skipped. Re-run it after the codegen target and build
again.

**Stale configure after changing compilers.** Delete
`out/build/linux-amd64-relwithdebinfo` and configure from scratch; CMake caches
the compiler identity and will not switch underneath you.

---

## Debugging

Full trace logging makes every file open and read visible, with paths and
status codes, which is what most content and save problems come down to:

```toml
log_level = "trace"
```

> [!IMPORTANT]
> `--log_level=trace` on the **command line does not work**. The app applies a
> session default of `debug` when `ac6_performance_mode` is off, and the
> command-line value is not yet marked user-set at that point, so the session
> default wins. A value in `ac6recomp.toml` does win. At `debug` level only
> disc-image paths and outright failures are logged.

`ac6_performance_mode` forces `log_level = error`, so pass
`--no-ac6_performance_mode` when you want any logging at all.

Logs are written to `ac6recomp.log` beside the binary and rotate at
`log_max_file_size_mb` (default 5 MB) into `ac6recomp.N.log`; concatenate them
in order (`ls -tr ac6recomp*.log`) before analysing a long session, or raise the
limit for a single run.

### Attaching a debugger

If `/proc/sys/kernel/yama/ptrace_scope` is `1`, only a parent process may
attach. Either run the game under the debugger from the start, or have the
process opt in by preloading a small library that calls
`prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)`.

A hang with one thread at 100% CPU and the rest idle is usually a fault inside
recompiled guest code rather than a deadlock. Sampling that thread's registers
repeatedly tells the two apart: if every register is identical across samples
seconds apart, it is pinned on one instruction, not looping slowly.
