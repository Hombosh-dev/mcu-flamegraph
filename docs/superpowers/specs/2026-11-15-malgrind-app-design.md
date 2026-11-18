# malgrind: desktop app design

Status: draft for approval, 2026-11-15. Supersedes the CLI-plus-browser shape of `mulcan`.

## 1. Intent

mulcan today: an ESP32 sampling profiler with a `mulcan run|attach|detect` CLI and a live flamegraph in the browser.
malgrind turns it into a **desktop application** (`.deb`, `.dmg`): "valgrind for microcontrollers". The user opens it,
presses **New run** (or attaches to a board that is already running their program), and watches live analysis of
their own firmware: flamegraph, function table, heap over time, FreeRTOS tasks, memory errors. Old runs are kept
and can be reopened, rewound, compared and exported.

Why: this is a Modern C++ course project that should also be a tool an embedded developer would really use.
At least ~60% of the logic must be C++ (measured by lines at the end, see section 10); the rest may use any language.
No tight deadline; everything below is expected before the defense.

## 2. Goals, non-goals, anti-goals

**Goals**
- A packaged app for Linux (`.deb`), macOS (`.dmg`, built in CI, verified by the user on a Mac), and Windows (`.exe` installer built by CI, verified by the user).
- Live-profile a program that is already running on the board, without resetting it (attach).
- Four analyses, all live and all replayable from history: callgrind-like, massif-like, FreeRTOS tasks, memcheck-like.
- History with summary, time scrubbing, diff of two runs, name and notes, export.
- Remove the agent from a board again (section 6).
- A terminal mode in the same package: `malgrind-cli run|attach|detect` with a localhost live page (the old tool), on the same engine and the same run history. The app is `malgrind`, the CLI `malgrind-cli`.
- Overhead: about 0% while the agent sleeps; about 1.5% per 1 kHz per core while sampling (today's figure).
  Heap checking costs more and the UI says so.

**Non-goals (this version)**
- Any MCU except ESP32 classic (the core and port structure stay chip-neutral, no new ports now).
- QEMU emulation. (JTAG/SWD probe attach moved from here to a planned stage, section 5a.)
- Own build system or flasher: building and flashing is PlatformIO's job (`pio`).
- Several boards at once; a background daemon (closing the window ends the run); cloud or sharing.
- Uninitialised-memory and arbitrary invalid-read/write detection (needs emulation; this is not real memcheck).
- Code signing and notarization. The `.dmg` is unsigned, so macOS shows a Gatekeeper warning.

**Anti-goals (failure even if it "works")**
- Changing the user's source files.
- Leaving an agent in the firmware without the user being told.
- Two readers on one serial port silently splitting the bytes (the port stays opened exclusively).
- Analysis logic duplicated in TypeScript. The UI renders; the C++ sidecar computes.

## 3. Approaches considered

**Shell/UI stack.** (a) **Tauri**: thin Rust shell, TypeScript UI, C++ sidecar. Bundles deb/dmg/AppImage, native
menu and dialogs, small. Costs one more toolchain (Rust). (b) C++ with `webview/webview` and CPack: all C++, but
menus, dialogs and the macOS `.app` bundle are hand work. (c) Qt 6/QML: native C++ UI, but the flamegraph must be
rewritten and packaging is heavy. **Chosen: (a)**, user's decision. Rust stays about 150 lines (spawn sidecar, window,
menu, folder/save dialogs).

**Data source.** (a) **Sleeping agent** in the firmware, injected at build time. (b) JTAG probe: needs ESP-Prog, ~14
samples/s, no stacks, no heap view. (c) QEMU: exact like real valgrind but not the user's board. **Chosen: (a)**, user's
decision. The probe stays a later option for ARM, where it is fast.

## 4. Architecture

```
Tauri window (TypeScript UI)  <--HTTP/JSON, localhost + token-->  malgrind sidecar (C++20)  <--UART-->  ESP32 + agent
        |                                                              |
   Rust shell: window, menu,                                 serial, build orchestration (pio), symbols (addr2line),
   folder/save dialogs, spawn sidecar                        aggregation, analyses, run store, replay, export
```

- **Sidecar** = today's `host/` tool, named `malgrind-cli` (the app executable is `malgrind`), gaining a `serve` mode: starts the HTTP API on
  `127.0.0.1` with a random port and a random token that the shell passes to the UI. Every request must carry
  the token (other local pages or users must not be able to read the API or start a flash).
- **UI** polls twice a second for live data, as the page does today. Time series and flamegraph both get their data
  from the same endpoints for live and for history (a replay is the stored stream fed through the live path).
- **Agent** = today's `core/` plus the ESP32 port, extended (section 5). It is a PlatformIO library the app ships in its resources.
- Packaging: the sidecar is bundled as a Tauri external binary; the agent sources as app resources.
  CMake builds the sidecar; Tauri produces the bundles.

## 5. The agent

**Injection, so the user's code is unchanged.** `New run` runs `pio run -t upload` for the user's project with the
agent added through PlatformIO environment overrides (extra lib dir, build flags, sdkconfig additions) and a
separate build directory (`.pio/malgrind/`). The user's normal `.pio/build/` is never overwritten. The agent starts
itself during system init (no `mulcan_start()` call in user code). *Builder to confirm the exact hook
(`ESP_SYSTEM_INIT_FN` vs a constructor plus `-u` symbol) in a spike; the requirement is "no user source change".*

**Sleeping.** After boot the agent does nothing but send a tiny **hello beacon** about once a second (arch, cores, CPU
Hz, agent version, capability bits) and listen on UART RX. When malgrind sends *start* (with rate and which
analyses), it begins; *stop* puts it back to sleep. The beacon is also how attach detects "already running
firmware with the agent" without a reset: open the port without toggling DTR/RTS, listen about 2 s, and fall back
to esptool chip-id only if nothing arrives and the user agrees (esptool resets the board).

**Data it can send** (new frame kinds in the existing envelope `MLC1 | len | body | crc32`; body version bumps to 2
and gains a kind byte):
- stack samples (exists today);
- heap stats: free, minimum free, largest free block (fragmentation), per capability region;
- FreeRTOS tasks: CPU share, priority, stack high-water mark;
- allocation events for leak tracking: address, size, call stack on malloc; address on free (via linker `--wrap` of
  the heap allocation entry points, bounded depth 16, batched);
- error events: heap corruption (from comprehensive heap poisoning), stack overflow (FreeRTOS stack check), allocation failure.

**Build option.** "Check memory errors" (heap poisoning, stack-overflow check, allocation hooks) is a checkbox at
New run, off by default, because it changes sdkconfig and costs time and RAM. The beacon's capability bits tell the
UI which tabs have data. *Builder to measure and show the overhead in the UI.*

**Console sharing.** The agent shares UART0 with the user's `ESP_LOG` text, as today; the host separates frames from text.

## 5a. Sources and methods (added 2026-11-16)

Why: a board may run firmware we have no source for and must not lose (the user's water simulator). Flashing an agent
is then impossible, and a debug probe is the only way to look inside. The app must also grow beyond ESP32.

**Source.** Everything that produces samples `{time, core, stack of PCs}` is a *source*. What comes after it (symbols,
aggregation, run store, replay, UI) is shared and knows nothing about the source.

| source | needs | overhead on the chip | gives |
|---|---|---|---|
| agent over UART (exists, ESP32) | firmware project, PlatformIO, flash | ~1.5% per kHz per core | stacks; later heap, tasks, memcheck |
| debug probe via OpenOCD (planned) | probe + 4 wires, or a chip with built-in USB-JTAG | Cortex-M3/M4/M7: ~0 (DWT_PCSR is read without halting); M0/M0+ and ESP32: the core halts briefly per sample | PCs; stacks unwound on the host from the ELF; no heap or memcheck views |
| emulator (later) | firmware, no board | emulation | exact, but not the real board |

Probe sample rates are unmeasured; the first probe stage starts with a spike on real hardware (Nucleo/ST-Link or a Pico
running debugprobe) before anything is promised.

**One interchange format.** The existing MLC1 wire frame is already chip-neutral (`Arch`, cores, counted stacks). A probe
source encodes its samples into the same frames with the core encoder, so the run store, replay and aggregator stay as
they are. `Source` stays a concrete serial reader until the second implementation exists; then it becomes an interface
(same size of change now or then, so no speculative abstraction today). The UI shows only the views the source provides.

**Methods report.** When a board is plugged in, the app lists every way to profile it and, for each one that is not
possible, says what is missing:

| method | available when | else it says |
|---|---|---|
| attach | the agent's frames arrive | "no agent running on this board" |
| new run | PlatformIO found, project is ESP-IDF on espressif32 | "PlatformIO not found" / the project problem |
| without flashing | a debug probe is found | "needs a debug probe (JTAG/SWD); not supported yet" |

**Safe flash.** The first time the agent is flashed onto a board, the whole flash is read into
`<data>/backups/<board MAC>/<time>.bin` (a failed backup stops the flash). **Remove agent** by default writes that
backup back, byte for byte (esptool verifies the hash); "rebuild my project" remains as the second choice. A board
that already runs the agent is not backed up again (the image would be ours), recognised by a marker string in the
agent. Not supported: patching an agent into a dumped image (needs the binary's symbols, free IRAM, no flash
encryption; fragile), and profiling a board that has neither source nor a probe.

## 5b. Devices and families (added 2026-11-18)

**One OS layer.** Every OS-specific call lives in `host/src/os/`. Outside it: no OS macros (`#ifdef _WIN32/__APPLE__/__linux__`), POSIX or Win32 headers, `/dev/`, `/sys/`, `errno`, or `getenv`. This boundary is enforced by `scripts/check-portability.mjs`.

**Devices, not ports.** The UI and the protocol select a **device** by a stable, OS-neutral **device id** (`usb-1a86-7523-sn-5A7B0123`), never by parsing an OS path. The engine resolves the id to the current OS path (`/dev/ttyUSB0`, `/dev/cu.usbserial-1410`, `COM5`) at open time, and again after an upload. Device-id rules, in order:
- USB with serial: `usb-<vid>-<pid>-sn-<serial>`
- USB with location: `usb-<vid>-<pid>-at-<location>`
- Otherwise: `port-<name>`
- Debug probes use the `probe-` prefix instead of `usb-` (`probe-<vid>-<pid>-sn-<serial>` or `probe-<vid>-<pid>-at-<location>`).
- Characters outside `[a-z0-9.-]` become `-`, lower-cased, max 96 chars.

**Families, not ESP32.** Everything chip-specific (which PlatformIO platforms/frameworks are supported, how the agent is injected, how flash is backed up and restored, how the chip is identified, the agent's baud rate, `addr2line` tool names, and OpenOCD configs) sits behind one C++ interface `Family` in `host/src/families/`. ESP32 is the first implementation; STM32 and RP2040 are later files, not edits across the engine.

## 6. Flows

(Reworked 2026-11-16 after the user's own sketch. Vocabulary: a **project** is a container, its source code is optional.)

**Open the app.** First time (no projects, no runs): a big **New project** in the middle. Otherwise a wide "New project" card
sits on top and under it two tabs: **Projects** (name, where it came from, run count, last run; rename, delete) and **Runs**
(the flat history, which also holds runs made before projects existed). Theme follows the OS (light/dark).

**New project.** One panel with four ways in: drop a project folder (or a firmware `.elf`) on the window or press *Choose
folder*; paste a **GitHub link** (`https://github.com/owner/repo`, public, shallow clone into malgrind's own folder); or
**no code, just a board** (optionally with the board's `.elf`, which only gives function names). The folder of a user's own
project is used in place and never changed. A project from GitHub is **not trusted** until the user presses *Trust*, because
building runs the repository's `platformio.ini` scripts; attaching builds nothing and needs no trust.

**Project page.** Name, origin, environment, trust banner, its runs, and a big **Profile**.

**Profile, three steps.**
1. **Board**: one large tile per plugged-in serial port (USB adapter name, port, "agent running · 2 cores · 160 MHz" after a
   2 s read-only listen, an *Identify chip* button that says it resets the board) and greyed tiles for any USB debug probe
   found ("in development"). A tile never names a chip it cannot know.
2. **How** (the methods of section 5a, as cards): *Attach*, *Build & flash* (the board is backed up first), *Debug probe*
   (greyed, "in development"). An unavailable card says what is missing, for example "this project has no source code".
   The mode comes after the board because what is possible depends on board and project together.
3. **Run**: backup, build, flash with a log, then the live view.

**Run settings (0.4.0).** Choosing a card in step 2 opens a panel under the cards, before *every* run: project name, run name,
environment, sampling rate, firmware ELF, extra pio arguments, extra build flags, and under *Advanced* the exact build and upload
commands. Everything shows the detected value and can be changed; the panel starts from the project's last-used settings and has
*Reset to detected*; each run stores what it was made with. One function (`make_plan`) turns settings into commands for both the
preview and the real run, so what is shown is what runs. A typed command replaces the detected line but keeps the agent's environment
variables, and no shell is involved (the line is split into words). Settings are checked before the board is touched. The rate reaches
the agent as an sdkconfig key (`CONFIG_MALGRIND_RATE_HZ`), not as an environment variable, because ESP-IDF rebuilds the agent when the
sdkconfig changes but keeps a stale object when only the environment does; a build directory made with other agent files than the
current ones is deleted once, because PlatformIO configures CMake only once.

**Attach** records a board that already runs the agent, with the project's ELF (else the newest matching one in its folder, else raw
addresses and a notice). Stopping leaves the board running.

**Setup (0.3.0).** What the app needs from the machine is checked, and fixed where it can be fixed without surprises:
- *PlatformIO missing*: an **Install PlatformIO** button (Python venv in `~/.platformio/penv` + `pip install platformio`, no
  sudo, output streamed). Python 3 with `venv` is a prerequisite the app only names. git is only reported.
- *Port not openable* (the user is not in the port's group): the tile says so and offers **Grant access** through `pkexec`, the
  system's own password prompt: an ACL on the device (immediate) and group membership (permanent after the next login). The engine
  builds that command itself from the ports it found and from system names, passing them as positional arguments, so nothing the
  UI sends ends up in a shell command.
- *Two boards*: tiles carry the USB socket and serial number; flashing asks for confirmation naming the board; a run shows its port.

**Remove agent.** Dialog: "Restores the firmware that was on the board before malgrind last flashed the agent (a backup
taken then). Live profiling stops." **Restore & flash** writes the backup of this board (section 5a), so the original comes
back byte for byte even if its source is unknown. **Rebuild my project instead** runs a plain `pio run -t upload` of the
user's project (no overrides). Reachable from the sidebar footer (red) and from the Board menu.

**Live run.** Sidebar layout: Flamegraph | Functions | Memory | Tasks | Memcheck (red badge with the problem
count), core and window filters in the sidebar, LIVE indicator, pause, and a **time scrubber** under the graph (follows
"now"; dragging back rewinds; same control in history).

## 7. Analyses

- **Flamegraph:** as today (core filter, window, zoom).
- **Functions (callgrind-like):** table with self and inclusive time, callers and callees of the selected function,
  time-series strip, per core.
- **Memory (massif-like):** heap used / free / minimum / largest free block over time; biggest allocation sites with stacks.
- **Tasks:** CPU per task, priority, stack high-water mark with a warning near exhaustion.
- **Memcheck-like:** leaks (allocations without free at stop or at a chosen moment, grouped by allocation stack,
  with "definitely/possibly" by age), heap corruption events, stack overflow, fragmentation warning. All reported with
  symbolized stacks.

Honest limit, to be stated in the app: sampling is statistical; memcheck-like detects what heap poisoning,
allocation hooks and FreeRTOS checks can see.

## 8. History

A run is a directory under `~/.local/share/malgrind/runs/<id>/` (macOS `~/Library/Application Support/malgrind/runs/`):
`meta.json` (name, notes, board, firmware, start time, duration, agent version), `frames.mlg` (timestamped raw frames),
`firmware.elf` (copy, so old runs stay symbolizable after a rebuild). Opening a run replays `frames.mlg` through
the same aggregator.
- Summary view, scrubbing, name and notes (edit later).
- **Diff of two runs:** compare by function **name** from each run's own ELF (addresses differ between builds); show
  functions that grew or shrank, as a diff flamegraph and a table.
- Export: folded stacks, speedscope JSON, PNG of the current view.
- Delete a run. *Storage ceiling:* uncompressed frames are roughly tens of KB/s at worst; add compression or a size
  warning if runs get large (builder's call).

## 9. Rename

`mulcan` → `malgrind` for the binary, namespace, CMake targets, macros (`MALGRIND_ISR`), env vars and docs. The wire
magic `MLC1` stays (it is a format identifier, not a brand). Repo directory name is unchanged.

## 10. Language split and "C++ is 60%"

C++: wire, agent, ports, store, replay, aggregation, all analyses, symbolization, board and project detection, pio
orchestration, HTTP API, export. Rust: shell only. TypeScript: rendering and interaction only. At the end count lines
(`cloc`, excluding generated and vendored code); target at least 60% C++, and say so in the README.

UI tooling (my call): Vite + TypeScript, **Svelte** for components (compiles away, small), d3-flame-graph (already
used) for the flamegraph, **uPlot** for time series. The builder may swap these if they prove awkward.

## 11. Verification

- `ctest`: core and host unit tests, extended for new frame kinds, the run store, replay and diff.
- Sidecar integration: feed recorded or `host_runner` streams through a pty; replay golden runs.
- Real ESP32 on `/dev/ttyUSB0` (the user shares the board: check `fuser -v /dev/ttyUSB0` first, never kill their session):
  beacon detection without reset, start/stop, remove agent, overhead A/B.
- `cargo tauri build` yields the `.deb` locally; the `.dmg` is built by the CI macOS job and tested by the user.

## 12. Staging (all before the defense)

1. Done (0.1.0): rename; sidecar `serve` protocol; run store and replay; Tauri shell; start screen, history, live flamegraph and Functions; New run, attach, remove agent; `.deb`, `.dmg` in CI.
2. Stage 2+ roadmap and tasks: see `docs/superpowers/plans/2026-11-18-malgrind-stage2-crossplatform.md`.

## 13. Calls I made (please check)

- One board at a time; closing the window ends the run; runs stored under the user data directory.
- Agent injection via PlatformIO overrides and a separate build directory; remove agent = plain reflash.
- Beacon while sleeping (cheap, makes attach-without-reset possible).
- "Check memory errors" as a build-time checkbox.
- Svelte, uPlot, token-protected localhost API.
- `pio` required for New run; attach works without it.

## 14. Left to the builder

Exact ESP-IDF mechanisms (self-start hook, allocation wrapping, task stats availability, UART RX handling, avoiding
board reset on port open), frame layouts for the new kinds, storage compression, API endpoint shapes, Svelte versus
plain TS if Svelte gets in the way.
