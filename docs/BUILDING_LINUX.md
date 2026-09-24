# Building on Linux

This guide takes you from nothing to a running game, one step at a time. Every
command goes in a terminal; copy each one exactly, press Enter, and wait for it
to finish before starting the next.

The Linux build produces a native `ac6recomp` program that renders through
**Vulkan** and draws its window with **GTK 3**. Windows builds use Direct3D 12
and Win32 instead, so a few things differ — see
[Platform differences](#platform-differences).

> [!NOTE]
> Nothing here downloads the game. You need your own copy of Ace Combat 6 as a
> disc image (`.iso`). **Only the US release works** — the European and
> Japanese discs are not supported.

**Contents**

1. [Before you start](#before-you-start)
2. [Install the tools](#step-1-install-the-tools)
3. [Install extract-xiso](#step-2-install-extract-xiso)
4. [Download the source code](#step-3-download-the-source-code)
5. [Extract the game files](#step-4-extract-the-game-files)
6. [Build](#step-5-build)
7. [Run the game](#step-6-run-the-game)
8. [Updating to a newer version](#updating-to-a-newer-version)
9. [Settings](#settings) · [Platform differences](#platform-differences) ·
   [Troubleshooting](#troubleshooting) · [Debugging](#debugging)

---

## Before you start

| | |
|---|---|
| **CPU** | Must support AVX2: Intel 4th-gen Core (2013) or newer, AMD Zen (2017) or newer |
| **GPU** | Vulkan 1.2 capable, with working drivers |
| **Distribution** | Ubuntu 24.04 or newer, a current Fedora, or Arch. Others work if they can provide the same packages |
| **Disk space** | About 15 GB free: roughly 5 GB for the extracted game, a few GB for the build, plus the `.iso` itself |
| **Game** | Your own `.iso` of Ace Combat 6: Fires of Liberation (US) |

To check the CPU, run this. If it prints `avx2`, you are fine; if it prints
nothing, this computer cannot run the game:

```bash
grep -o -m1 avx2 /proc/cpuinfo
```

The build itself takes a while — expect anywhere from ten minutes to an hour
depending on the machine. That is normal.

---

## Step 1: Install the tools

Pick the block for your distribution and run it. It asks for your password
because it installs system packages.

**Ubuntu / Debian / Linux Mint / Pop!_OS:**

```bash
sudo apt update
sudo apt install git build-essential cmake ninja-build pkg-config \
                 clang-20 lld-20 \
                 libgtk-3-dev libx11-xcb-dev \
                 libasound2-dev libpulse-dev \
                 libvulkan-dev vulkan-tools
```

**Fedora:**

```bash
sudo dnf install git gcc-c++ cmake ninja-build pkgconf-pkg-config \
                 clang lld \
                 gtk3-devel libX11-devel libxcb-devel \
                 alsa-lib-devel pulseaudio-libs-devel \
                 vulkan-loader-devel vulkan-tools
```

**Arch / Manjaro / EndeavourOS:**

```bash
sudo pacman -S --needed git base-devel cmake ninja pkgconf \
                 clang20 lld \
                 gtk3 libx11 libxcb \
                 alsa-lib libpulse \
                 vulkan-headers vulkan-icd-loader vulkan-tools
```

> [!IMPORTANT]
> Do not skip the audio packages (`libasound2-dev` and `libpulse-dev`, or your
> distribution's equivalents). Without them the build still succeeds, but it
> quietly has no working sound, and **the game then crashes at startup**. If you
> install them after a build, see
> [Troubleshooting](#troubleshooting) — a plain rebuild is not enough.

Now check two things.

**The graphics driver.** This should print a line naming your graphics card. If
it prints an error instead, your Vulkan driver is missing or broken; fix that
first (on most distributions, install the Mesa Vulkan drivers, or the NVIDIA
driver for NVIDIA cards).

```bash
vulkaninfo --summary | grep deviceName
```

**The compiler version.** Run the line for your distribution. The first line of
output must say version **20 or higher**.

```bash
clang-20 --version                  # Ubuntu / Debian
clang --version                     # Fedora
/usr/lib/llvm20/bin/clang --version # Arch
```

If your distribution has no Clang 20 at all, see
[Using a downloaded Clang](#using-a-downloaded-clang).

---

## Step 2: Install extract-xiso

`extract-xiso` unpacks the Xbox 360 disc image. Most distributions do not
package it.

**Arch users** can install it from the AUR with an AUR helper, for example
`yay -S extract-xiso` (run it **without** `sudo`), and skip to Step 3.

**Everyone else** builds it from source. This takes about a minute:

```bash
cd ~
git clone https://github.com/XboxDev/extract-xiso.git
cmake -S extract-xiso -B extract-xiso/build -G Ninja
cmake --build extract-xiso/build
sudo cp extract-xiso/build/extract-xiso /usr/local/bin/
```

Check it worked — this should print a usage message, not "command not found":

```bash
extract-xiso -h
```

---

## Step 3: Download the source code

This downloads the project into a folder called `AC6_recomp` in your home
folder, and then moves the terminal into it:

```bash
cd ~
git clone --branch linux-port https://github.com/Tetragramm/AC6_recomp.git
cd AC6_recomp
```

**Every command from here on must be run from inside this folder.** If you
close the terminal and come back later, start with `cd ~/AC6_recomp` first.

---

## Step 4: Extract the game files

The build reads the game's program file (`default.xex`) to generate code from
it, so the disc has to be unpacked into an `assets` folder first.

Replace `/path/to/your/AC6.iso` with where your disc image actually is. A quick
way to get the path right: type the start of the command, then drag the `.iso`
file from your file manager into the terminal window.

```bash
mkdir -p assets
extract-xiso -d assets /path/to/your/AC6.iso
```

This takes a minute or two. Then check the one file that matters is there:

```bash
ls assets/default.xex
```

It should print `assets/default.xex`. If it says "No such file or directory",
the extraction did not work — check the `.iso` path and try again.

After this the `.iso` is no longer needed for playing; you can keep it or
delete it.

---

## Step 5: Build

First, tell the build which compiler to use. Run the **one** block for your
distribution:

```bash
# Ubuntu / Debian
export CC=clang-20 CXX=clang++-20
```

```bash
# Fedora
export CC=clang CXX=clang++
```

```bash
# Arch
export CC=/usr/lib/llvm20/bin/clang CXX=/usr/lib/llvm20/bin/clang++
```

> [!NOTE]
> `export` only lasts until the terminal is closed. If you close it partway
> through, run `cd ~/AC6_recomp` and your `export` line again before
> continuing.

Now build. These are four separate commands; run them **in order, one at a
time**. The first and third are the same on purpose (see below).

```bash
cmake --preset linux-amd64-relwithdebinfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"
```

```bash
cmake --build --preset linux-amd64-relwithdebinfo --target ac6recomp_codegen
```

```bash
cmake --preset linux-amd64-relwithdebinfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"
```

```bash
cmake --build --preset linux-amd64-relwithdebinfo
```

What these do:

1. **Configure** — checks your tools and prepares the build.
2. **Codegen** — reads `assets/default.xex` and generates C++ source code from
   the game's Xbox 360 program.
3. **Configure again** — makes the build notice the code that step 2 just
   generated. **Skipping this is the most common build failure.**
4. **Build** — compiles everything. This is the long one. Lots of text scrolls
   past; that is normal. It is finished when you get your prompt back and the
   last lines do not say `FAILED` or `error`.

When it succeeds, the program is at
`out/build/linux-amd64-relwithdebinfo/ac6recomp`.

> [!TIP]
> If the computer becomes unresponsive during step 4, or the build stops with
> `Killed`, it ran out of memory. Run step 4 again with fewer parallel jobs:
> `cmake --build --preset linux-amd64-relwithdebinfo -j 4`. It picks up where it
> left off.

Other presets exist — `linux-amd64-release` and `linux-amd64-debug` — but
RelWithDebInfo is recommended: it runs at full speed and still gives a usable
crash report. A debug build is very large and far too slow to play.

---

## Step 6: Run the game

The game looks for its files next to the program, so copy the program into the
project folder, where `assets/` already is:

```bash
cp out/build/linux-amd64-relwithdebinfo/ac6recomp .
./ac6recomp
```

To start the game later, open a terminal and run:

```bash
cd ~/AC6_recomp
./ac6recomp
```

Always start it from inside its own folder like this — the shader cache is
stored relative to the folder you launch from.

On the first run:

- A settings file, `ac6recomp.toml`, is created next to the program. See
  [Settings](#settings).
- The first time each scene is shown it may stutter briefly while shaders are
  prepared. They are saved in `cache/shaders/`, so later runs are smooth.

**DLC (optional).** Make a folder called `dlc` next to the program and put your
DLC packages in it. The packages work as they are (no extraction needed);
already-extracted packages work too.

**Using a separate game folder instead.** If you would rather keep the game
somewhere else, that folder needs the `ac6recomp` program plus either the
`assets/` folder or the `.iso` (the game can run straight from the disc image),
and optionally `dlc/`.

---

## Updating to a newer version

```bash
cd ~/AC6_recomp
git pull
```

Then run your `export` line from [Step 5](#step-5-build), all four build
commands again, and copy the new program over the old one:

```bash
cp out/build/linux-amd64-relwithdebinfo/ac6recomp .
```

Rebuilding after an update is much faster than the first build. Your
`ac6recomp.toml`, key bindings, saves and shader cache are kept.

If the build fails after an update in a way it did not before, delete the build
folder and build from scratch:

```bash
rm -rf out/build/linux-amd64-relwithdebinfo
```

---

## Settings

Settings, the in-game overlay keys (`F3`, `F4`, `F11`) and the key bindings are
the same as on Windows and are documented in the [README](../README.md). A few
things are Linux-specific.

> [!IMPORTANT]
> `ac6recomp.toml` is read as **top-level keys only**. Putting a setting under a
> `[section]` header folds the section name into the setting name and it is
> silently ignored. And **use forward slashes in paths** — a backslash is an
> escape character and one bad line voids the whole file.

- **`ac6_fps_target = 0` (auto) does not work on Linux** — it cannot read the
  monitor's refresh rate yet and falls back to `ac6_max_sim_fps`. Set a number
  instead (the default, `60`, is what the physics is validated for).
- **`ac6_widescreen` and `ac6_cursor_hide_seconds` do nothing on Linux** —
  see [Platform differences](#platform-differences).

### Vulkan performance options

These were added with the Linux port to make the Vulkan backend fast enough for
60 fps at high resolution scales. They are all **on by default** and should stay
that way. Each exists so a visual problem can be traced: if something looks
wrong, turn one off, restart, and see whether the problem goes away — and please
report which one it was.

| setting | default | what it does |
|---|---|---|
| `ac6_swap_flip_on_fence` | `true` | Finish each frame as soon as the GPU is done with it, instead of waiting for the next 60 Hz tick. Removes a large source of lost frames |
| `ac6_clear_elides_edram_transfer` | `true` | Skip copying render-target contents that a clear is about to erase |
| `ac6_quad_elides_edram_transfer` | `true` | The same for full-screen post-processing passes that overwrite everything |
| `ac6_edram_skip_stencil_transfers` | `true` | Don't carry stencil data between render targets. Turn off if masked effects look wrong |
| `ac6_wide_world_target` | `true` | Draw the 3D world in one pass instead of two halves. Removes about a quarter of the draws in a frame |
| `ac6_skip_no_effect_draws` | `true` | Skip the draws the game issues that cannot change any pixel |
| `vulkan_resolve_to_texture_image` | `true` | Copy rendered images straight into the textures that read them |
| `vulkan_resolve_to_texture_msaa` | `true` | The same, for anti-aliased images |
| `vulkan_resolve_to_texture_compute` | `true` | The same, with a compute shader, for the cases a plain copy cannot handle |
| `vulkan_edram_stencil_transfer_compute` | `true` | Faster stencil copies on GPUs without the stencil-export extension |
| `vulkan_reuse_texture_descriptor_sets` | `true` | Reuse the previous draw's texture bindings when nothing changed |
| `shared_memory_keep_uploaded_pages_valid` | `true` | Stop re-uploading unchanged game memory to the GPU every frame |
| `texture_cache_resolve_destination_index` | `true` | Find the target texture of each copy by address instead of searching the whole cache |

One fix is also Vulkan-only so far:

| setting | default | what it fixes |
|---|---|---|
| `ac6_fix_water_bottom_band` | `true` | A dark band across the sea along the bottom of the screen |

---

## Platform differences

| Area | Windows | Linux |
|---|---|---|
| Graphics backend | Direct3D 12 | Vulkan |
| Windowing | Win32 | GTK 3 |
| Controllers | Supported | Supported |
| Keyboard, mouse buttons, wheel (`ac6_kbm_enabled`) | Supported | Supported |
| Mouse **steering** | Supported | **Not yet** — see below |
| Ultrawide (`ac6_widescreen`) | Supported | **Not yet** — the setting has no effect |
| Hiding an idle cursor (`ac6_cursor_hide_seconds`) | Supported | Not yet |
| Auto frame-rate target (`ac6_fps_target = 0`) | Matches the monitor | Falls back to `ac6_max_sim_fps` |
| Vulkan performance options above | Not used | On |

### Keyboard and mouse

`ac6_kbm_enabled` and `ac6_input.toml` work the same on both platforms, so the
bindings and every `ac6_kbm_*` setting are shared:

```toml
ac6_kbm_enabled = true
```

Enabling `ac6_kbm_enabled` switches `mnk_mode` off automatically — the two are
different keyboard implementations and only one can own the keyboard.

GTK reports only side-agnostic modifier keys, so `LeftControl` and
`RightControl` both arrive as plain `Control`. Bindings that name a side — the
stock `Left Ctrl` for the machine gun, `Left Alt` for camera control — are
folded onto the generic key, so either side of the keyboard triggers them.

**Mouse steering is still Windows-only.** Until it is ported, set the mouse mode
to something other than `steer` in `ac6_input.toml`, or fly with the keyboard or
a controller.

<details>
<summary>Why mouse steering is missing (for developers)</summary>

Steering pins the cursor to the window centre on every poll and reads the delta.
`GTKWindow` implements neither cursor warping nor mouse capture — it overrides
only fullscreen, title, and menu, so the cursor calls are no-ops. Supporting it
needs those primitives in the GTK backend, and on Wayland warping the pointer is
not permitted at all: that path needs the pointer-constraints and
relative-pointer protocols, which is a different implementation from X11.

Key state is also gathered differently. Windows polls the OS with
`GetAsyncKeyState` and hooks the scroll wheel with `SetWindowsHookExW`. Other
platforms accumulate key, mouse button, and wheel state from the SDK window's
`WindowInputListener` events, and take the input gate's focus check from window
focus rather than from the foreground window.

</details>

Controllers work on both platforms with no configuration.

---

## Troubleshooting

**`clang-20: command not found`, or CMake says it cannot find the compiler.**
The `export` line from [Step 5](#step-5-build) was not run in this terminal, or
Clang is not installed. Run the version check from [Step 1](#step-1-install-the-tools).
If you already ran a configure with the wrong compiler, delete the build folder
(`rm -rf out/build/linux-amd64-relwithdebinfo`) — CMake remembers the first
compiler it saw and will not switch.

**CMake says the source directory has no `CMakePresets.json`, or the preset does
not exist.** You are not in the project folder. Run `cd ~/AC6_recomp`.

**Codegen fails, or complains it cannot open `assets/default.xex`.** The game
was not extracted into the right place. Redo [Step 4](#step-4-extract-the-game-files)
from inside `~/AC6_recomp`.

**The build ends with many "undefined reference" / undefined symbol errors.**
The second configure (command 3 in Step 5) was skipped. Run it, then command 4
again.

**Link fails looking for `LLVMgold.so` or `ld.lld`.** The `lld` package is
missing, or the `-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"` part of the configure
line was left off. Install `lld` (`lld-20` on Ubuntu/Debian), then configure and
build again.

**The game crashes straight away, and `ac6recomp.log` mentions
`No available audio device`.** The program was built without sound support
because the audio development packages were missing (see
[Step 1](#step-1-install-the-tools)). Install them, then **delete the build
folder** — the sound check is remembered from the first configure — and build
again:

```bash
rm -rf out/build/linux-amd64-relwithdebinfo
```

As a stopgap you can start the game with no sound at all:
`SDL_AUDIO_DRIVER=dummy ./ac6recomp`.

**Sound crackles or stutters.** Sound goes through PulseAudio (which PipeWire
systems also provide). SDL's native PipeWire backend is turned off on purpose
because it crackled badly in testing; don't turn it back on with
`-DSDL_PIPEWIRE=ON`. To force PulseAudio on a build that already has PipeWire
compiled in, start the game with `SDL_AUDIO_DRIVER=pulseaudio ./ac6recomp`.

**"No game data was found" when the game starts.** The `assets/` folder (or the
`.iso`) is not next to the `ac6recomp` program. See
[Step 6](#step-6-run-the-game).

**The game starts but there is no sound and a mission intro never ends.** Check
the game's own volume settings in its options menu. The mission intro waits for
the radio voice line to finish, so with the in-game volume at zero it never
does.

**ALSA or PulseAudio headers cannot be installed system-wide.** Unpack the
`-dev` packages into a local folder and point CMake at it by adding
`-DCMAKE_PREFIX_PATH=/path/to/sysroot/usr` to both configure commands.

### Using a downloaded Clang

If your distribution has no Clang 20 or newer, download a prebuilt LLVM release
for Linux x86-64 from <https://github.com/llvm/llvm-project/releases>, unpack
it anywhere, and use its full paths in Step 5 — for example:

```bash
export CC=$HOME/LLVM-20.1.8-Linux-X64/bin/clang CXX=$HOME/LLVM-20.1.8-Linux-X64/bin/clang++
```

The download includes `lld`, so nothing else is needed.

---

## Debugging

This section is for tracking down bugs; you don't need it to play.

Logs are written to `ac6recomp.log` next to the program and rotate at
`log_max_file_size_mb` (default 5 MB) into `ac6recomp.N.log`; concatenate them
in order (`ls -tr ac6recomp*.log`) before analysing a long session, or raise the
limit for a single run.

`ac6_performance_mode` (on by default) forces `log_level = error`, so turn it
off to get any other logging. Full trace logging makes every file open and read
visible, with paths and status codes, which is what most content and save
problems come down to:

```toml
ac6_performance_mode = false
log_level = "trace"
```

> [!IMPORTANT]
> `--log_level=trace` on the **command line does not work**. The app applies a
> session default of `debug` when `ac6_performance_mode` is off, and the
> command-line value is not yet marked user-set at that point, so the session
> default wins. A value in `ac6recomp.toml` does win. At `debug` level only
> disc-image paths and outright failures are logged.

### Diagnostic settings

All off by default. Most need `ac6_performance_mode = false` to show anything.

| setting | what it does |
|---|---|
| `profiling` | Log a per-frame CPU time breakdown every `profiling_interval_s` seconds. Logs at error level, so it works with `ac6_performance_mode` on |
| `gpu_timestamps` | With `profiling`, add per-frame GPU milliseconds per pass, transfer and resolve |
| `ac6_timing_trace` | Log the FPS unlock's timing once a second: measured frame time, the frame delta the game received, the physics step ratio |
| `narrate_frame_every_s` | Write one whole frame to the log as `[NARR]` lines (every draw, transfer, texture and resolve) this many seconds apart |
| `probe_shader_hash`, `probe_register`, `probe_scale` | Make one pixel shader output a register's value instead of its colour, to see what it is really computing |
| `ac6_present_pacing` | Set `false` to stop the FPS unlock from holding back the game's frame submission, keeping the rest of the unlock |

Leave `ac6_freerun_vblank_hz` at `0`. Any value above the frame rate corrupts
the game's own frame timing and makes the simulation crawl.

### Attaching a debugger

If `/proc/sys/kernel/yama/ptrace_scope` is `1`, only a parent process may
attach. Either run the game under the debugger from the start, or have the
process opt in by preloading a small library that calls
`prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)`.

A hang with one thread at 100% CPU and the rest idle is usually a fault inside
recompiled guest code rather than a deadlock. Sampling that thread's registers
repeatedly tells the two apart: if every register is identical across samples
seconds apart, it is pinned on one instruction, not looping slowly.
