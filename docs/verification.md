# Manual Verification Checklists & Platform Status

This document is the source of truth for what has been verified automatically vs. manually across operating systems, hardware, and MCU families. Do not check a box until the step has been executed on a real machine (and real hardware where applicable) and observed to match the expected result.

---

## Status Table

### Platforms

| Platform | Automated Build & Tests | Packaging & Smoke Test | Manual App + Hardware Verification | Current Notes |
|---|---|---|---|---|
| **Linux** (`x86_64`, Ubuntu 24.04+) | **Verified locally** (`22/22` CTest suites, `35/35` UI tests + `svelte-check`, `2/2` sidecar script tests, and portability checks pass on this branch) | **Configured** (`.deb` in `.github/workflows/release.yml`, `targets: "all"` in `tauri.conf.json`) | **Partial** (pre-Stage-2 baseline verified on ESP32-D0WD-V3; Stage 2 `devices`/`families` refactor, drag-and-drop, real `pkexec`, and "Flash this board" click still unverified) | Frontend Vitest/Svelte check verified via local `node_modules/.bin`; AppImage is enabled under `"all"` targets in `tauri.conf.json` while `.deb` is packaged in `release.yml`. |
| **macOS** (`aarch64`, macOS 13.3+) | **Configured in CI** (`macos-latest` in `.github/workflows/ci.yml`; not yet run locally on Mac hardware) | **Configured in CI** (`.dmg` in `.github/workflows/release.yml`, ad-hoc signed) | **Not yet run** | IOKit port/USB enumeration (`ports_macos.cpp`, `usb_macos.cpp`) and `IOSSIOSPEED` baud handling await manual verification on real macOS hardware. |
| **Windows** (`x86_64`, Windows 10 1809+ / 11) | **Configured in CI** (`windows-latest` in `.github/workflows/ci.yml`; `os/*_win.cpp` backends written, awaiting remote MSVC CI run) | **Configured in CI** (`build-sidecar.mjs`, `tauri.conf.json` `"all"` + `downloadBootstrapper`, and `release.yml` `nsis` job configured; awaiting remote build) | **Not yet run** | `COM10+`, non-ASCII/space profile path fallback (`C:\.malgrind\projects`), handle inheritance (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), and ESP32 flow await Windows CI and physical testing. |

### MCU Families & Modes

| Family / Mode | Engine & Build Support | Automated Tests | Hardware Verification | Current Notes |
|---|---|---|---|---|
| **ESP32 (ESP-IDF)** (`esp32`, Xtensa / RISC-V) | Implemented (`host/src/families/esp32.cpp`, `agent/esp32/`) | **Verified** (`families`, `pio`, `flash`, `devices`, `serve`, `session`, `cli` pass on Linux) | **Partial** (baseline `0.4.0` run/attach/backup/`verify_flash` verified on ESP32-D0WD-V3; Stage 2 checklists below still open) | Native USB (`VID 303a`) hints `esp32` tentatively; generic bridges (CH340/CP210x/FTDI) remain neutral until probed. |
| **STM32 / RP2040** | Not started (stub docs in `platforms/stm32/README.md`) | — | — | Planned for P10. |
| **Debug probe (OpenOCD / SWD / JTAG)** | USB probe discovery only (`find_probes()`); tiles disabled ("in development") | **Verified** (probe ID & USB enumeration unit tests on fake sysfs) | **Not yet implemented** | Planned for P9. |

---

## 1. Linux Checklist

Target environment: Ubuntu 24.04+ (`x86_64`), real ESP32 board (both USB-UART bridge such as CP210x/CH340 and, if available, ESP32-S3/C3 native USB `VID 303a`).

- [ ] **Install package (`.deb` / AppImage)**: Install `malgrind_<version>_amd64.deb` via `sudo apt install ./malgrind_<version>_amd64.deb` (and test AppImage execution if an AppImage bundle is produced). *Expected:* `malgrind` launches from the desktop/shell and `malgrind-cli --version` prints `<version>`; bundled agent sources exist under `/usr/lib/malgrind/agent-src/agent/esp32/family.json`.
- [ ] **First start (clean home / clean `$XDG_DATA_HOME`)**: Launch `malgrind` with no existing `~/.local/share/malgrind/`. *Expected:* Start screen shows the **New project** entry cleanly with empty **Projects** and **Runs** tabs and no console/sidecar errors.
- [ ] **New project (folder, GitHub, no code)**:
  - Add a local ESP-IDF project via *Choose folder…* — *Expected:* Opens in place without modifying any file in the project directory.
  - Paste a public GitHub ESP-IDF repository URL — *Expected:* Clones shallowly into `~/.local/share/malgrind/projects/`, shows the untrusted banner, and blocks *Build & flash* until **Trust** is clicked (while still allowing *Attach*).
  - Create **No code, just a board** (with and without an `.elf`) — *Expected:* Project is created; without `.elf` only raw addresses show; with `.elf` functions symbolize.
- [ ] **Drag and drop**: Drag a project folder onto the **New project** window, then test dragging a `.elf` file and an unsupported archive (`.zip`). *Expected:* Folder creates a `folder` project; `.elf` creates a `none` project with that ELF; `.zip` shows `"unpack the archive first, then drop its folder"`.
- [ ] **Device tiles & USB metadata**: Plug in an ESP32 board and open **Profile**. *Expected:* Tile displays the USB adapter title, short port name (`ttyUSB0` / `ttyACM0`), USB socket location (e.g. `USB 3-3`), and adapter serial number (if present). Native USB (`303a`) displays `probably ESP32 (ESP-IDF)`; generic USB-UART bridge shows no guessed chip before probing.
- [ ] **Two identical boards**: Plug in two identical ESP32 boards at the same time. *Expected:* Two separate tiles appear with distinct `device_id`s (by serial if different, or by USB location if serials are absent/identical).
- [ ] **Unplug / replug persistence**: Note the selected device tile, unplug the board, wait 2 s, plug it back into the same USB port, and press **Rescan**. *Expected:* Even if the kernel renumbers `/dev/ttyUSB0` → `/dev/ttyUSB1` or `/dev/ttyACM0` → `/dev/ttyACM1`, the board retains the same `device_id` and stays identifiable.
- [ ] **udev / port permissions (`pkexec`)**: Run `malgrind` as a Linux user **not** in `dialout` (or `uucp`) with no udev ACL on `/dev/ttyUSB0`. *Expected:* The board tile shows `"No permission to use this port: you are not in the group \"dialout\""` and **Grant access…**. Clicking **Grant access…** opens the real system `pkexec` password prompt, grants immediate ACL access (`setfacl`) so **Rescan** immediately sees the port without restarting the session, and adds the user to `dialout` so access persists across replug after re-login.
- [ ] **Attach without reset**: With the malgrind agent already running on the board, open **Profile → Attach → Attach**. *Expected:* Tile initially reports `● malgrind agent running · <arch> · <ncores> cores · <cpu_mhz> MHz`; attaching streams live flamegraph & function table without resetting the MCU (`uptime`/log output continues uninterrupted), and stopping leaves the board running.
- [ ] **Build & flash with backup**: On a board running non-malgrind firmware, open **Profile → Build & flash**, confirm the board name/port in the settings panel, and click **Flash this board**. *Expected:* Pre-flash backup is written to `~/.local/share/malgrind/backups/<board-mac>/<timestamp>.bin`, `.pio/malgrind/` builds without modifying user source files, firmware flashes, and live profiling starts automatically.
- [ ] **Remove agent & verify restore**: Click **Remove agent… → Restore & flash**. *Expected:* Live profiling stops, the pre-agent `.bin` backup is flashed back, the original firmware resumes running, and `esptool.py --port /dev/ttyUSB0 verify_flash 0x0 <backup.bin>` passes byte-for-byte.
- [ ] **Install PlatformIO from the app**: On a clean `HOME` without `pio` on `PATH` (with `python3` + `python3-venv` installed), click **Install PlatformIO** in the Setup banner. *Expected:* Live log streams `venv` + `pip install platformio` into `~/.platformio/penv` without `sudo`, then the banner clears and **Build & flash** becomes available.

---

## 2. macOS Checklist

Target environment: Apple Silicon (`aarch64`, macOS 13.3+), real ESP32 board (USB-UART bridge and/or native USB).

- [ ] **DMG install & Gatekeeper**: Mount `malgrind_<version>_aarch64.dmg`, drag `malgrind.app` to `/Applications`, and launch it (allowing the ad-hoc signed app via *System Settings → Privacy & Security → Open Anyway* or `xattr -dr com.apple.quarantine /Applications/malgrind.app`). *Expected:* App launches; `/Applications/malgrind.app/Contents/MacOS/malgrind-cli --version` matches `<version>` and `Contents/Resources/agent-src/agent/esp32/family.json` exists.
- [ ] **First start**: Launch `malgrind` with clean `~/Library/Application Support/malgrind/` and `~/.malgrind/projects/`. *Expected:* Start screen loads with **New project**, **Projects**, and **Runs** tabs and no sidecar errors.
- [ ] **New project (folder, GitHub, no code) & drag and drop**: Exercise folder picker, GitHub URL clone + **Trust** banner, **No code, just a board**, plus dragging a project folder and a `.elf` onto the window. *Expected:* All four project flows work; Finder drops resolve properly.
- [ ] **`/dev/cu.*` enumeration & baud rate**: Open **Profile** with an ESP32 connected (and Bluetooth enabled on the Mac). *Expected:* Only USB callout devices (`/dev/cu.usbserial-*`, `/dev/cu.usbmodem*`, `/dev/cu.SLAB_USBtoUART`, etc.) appear—never `/dev/cu.Bluetooth-Incoming-Port` or `/dev/tty.*`. Tile shows adapter title, `cu.*` name, `USB 0x<locationID>`, and serial (if present). Non-standard `921600` baud works via `IOSSIOSPEED`.
- [ ] **Two identical boards**: Plug in two identical ESP32 boards. *Expected:* Two tiles appear with distinct `device_id`s (`usb-<vid>-<pid>-sn-...` or `usb-<vid>-<pid>-at-0x<locationID>`).
- [ ] **Location-based replug**: Unplug the board and plug it back into the **same** USB port/hub socket. *Expected:* The `locationID` (`0x...`) and `device_id` stay identical after **Rescan**, even if `/dev/cu.usbmodem*` suffix changes.
- [ ] **Attach without reset**: Attach to a running agent board. *Expected:* 2 s listen detects the running agent without resetting the board; live flamegraph and function table stream at `921600` baud; stopping leaves the board running.
- [ ] **Build & flash with backup**: Run **Profile → Build & flash → Flash this board** on an ESP-IDF project. *Expected:* Backup saved under `~/Library/Application Support/malgrind/backups/<board-mac>/`, PlatformIO builds inside `.pio/malgrind/`, board flashes, and live profiling begins.
- [ ] **Remove agent & verify restore**: Run **Remove agent… → Restore & flash**. *Expected:* Original firmware is restored and `esptool.py --port /dev/cu.<port> verify_flash 0x0 <backup.bin>` succeeds.
- [ ] **Install PlatformIO from the app**: On a machine without `pio` on `PATH` (with Xcode CLT / `python3` available), click **Install PlatformIO**. *Expected:* Installs into `~/.platformio/penv` without admin privileges and refreshes board methods.

---

## 3. Windows Checklist

Target environment: Windows 10 (1809+) or Windows 11 (`x86_64`), real ESP32 board (CP210x / CH340 / FTDI / native USB-JTAG `VID 303a`).

- [ ] **Installer (NSIS / MSI)**: Run the Windows installer (`malgrind_<version>_x64-setup.exe` or `.msi` if enabled; also test silent install `/S /D=<temp-dir>`). *Expected:* Installer completes (installing/bootstrapping WebView2 Runtime if absent), Start Menu shortcut launches `malgrind`, `malgrind-cli.exe --version` prints `<version>`, and `agent-src\agent\esp32\family.json` is present in app resources.
- [ ] **First start**: Launch `malgrind` on a clean `%LOCALAPPDATA%\malgrind\`. *Expected:* Sidecar starts cleanly with no visible console window (`CREATE_NO_WINDOW`) and shows **New project**.
- [ ] **New project (folder, GitHub, no code) & drag and drop**: Test *Choose folder…*, GitHub URL + **Trust**, **No code, just a board**, and Explorer drag-and-drop of a folder and a `.elf` file. *Expected:* All paths work using UTF-8/UTF-16 round-tripping; cloned repo builds only after **Trust**.
- [ ] **`COM*` & `COM10+` port handling**: Test an adapter assigned to `COM1`–`COM9` and reassign/use an adapter mapped to `COM10` or higher (via Device Manager → Port Settings → Advanced). *Expected:* Tile shows `COM10` (opened internally via `\\.\COM10`), adapter title, `USB Port_#...Hub_#...` location, and serial number (including composite `&MI_00` parent serial lookup).
- [ ] **Non-ASCII + space user profile path (`Іван Петренко`)**: Run as a Windows user whose profile directory contains non-ASCII characters and a space (e.g. `C:\Users\Іван Петренко`). *Expected:*
  - Projects root automatically falls back to `C:\.malgrind\projects` (`<SystemDrive>\.malgrind\projects`) so PlatformIO ESP-IDF builds do not fail on spaces.
  - `%LOCALAPPDATA%\malgrind\runs\` and `backups\` retain the user profile path intact in `meta.json`, logs, and child process arguments (`os::to_utf8` / `os::from_utf8`).
  - `C:\.platformio` (when `PLATFORMIO_CORE_DIR` or fallback is used) and child `pio` / `esptool` processes run without mangled characters.
- [ ] **Long paths warning / behavior**: Place or test a deep project/toolchain path approaching 260 characters on a system with `LongPathsEnabled = 0` vs `1`. *Expected:* Either the app/engine warns clearly before PlatformIO/CMake fails on `MAX_PATH`, or long-path failure surfaces cleanly in the build log rather than hanging or silently truncating.
- [ ] **No handle inheritance while serial is open**: Start a live attach/profile session on `COMx` (so `malgrind-cli.exe` holds `\\.\COMx` open exclusively), then trigger another `pio`/`git`/`esptool` subprocess from the app (or inspect `malgrind-cli` child process handles with Process Explorer / `handle.exe`). *Expected:* Child processes created via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` inherit **only** their pipe/NUL handles—never the `COMx` serial handle; closing/reopening or another process probing the port observes clean `Busy` (`ERROR_ACCESS_DENIED`) only from the session holder, never leaked to children.
- [ ] **Two identical boards & replug**: Plug in two identical ESP32 boards; verify distinct `device_id`s; unplug and replug into the same USB port. *Expected:* Same `device_id` is preserved across replug.
- [ ] **Attach without reset**: Attach to a running agent board on `COMx`. *Expected:* Agent detected without resetting the board; live flamegraph & functions stream cleanly; stopping closes the handle cleanly so another tool can immediately open `COMx`.
- [ ] **Build & flash with backup**: Run **Profile → Build & flash → Flash this board**. *Expected:* Pre-flash backup saved, PlatformIO builds into `.pio\malgrind\`, `esptool` flashes without handle contention, and live profiling begins.
- [ ] **Remove agent & verify restore**: Run **Remove agent… → Restore & flash**. *Expected:* Original firmware is restored and `esptool verify_flash` passes byte-for-byte.
- [ ] **Install PlatformIO from the app**: On a Windows machine without `pio` (with standard Python 3 installed, and even with the Microsoft Store `WindowsApps\python.exe` stub present on `PATH`), click **Install PlatformIO**. *Expected:* Ignores the `WindowsApps` stub, creates `penv\Scripts\python.exe` + `pip.exe`, installs `platformio`, and enables **Build & flash**.

---

## 4. Cross-Platform Edge Cases (Review Focus)

Verify these edge cases across **Linux, macOS, and Windows**:

- [ ] **Re-plugging / re-enumerating during upload (`open_after_upload`)**: Flash an ESP32 (especially a native USB board such as ESP32-S3/C3 that switches between bootloader CDC and runtime CDC, or unplug/replug right as upload completes). *Expected:* `open_after_upload` re-lists devices and resolves the same `device_id` across Linux `ttyACM` renumbering, Windows COM port re-enumeration/reuse, and macOS `cu.usbmodem<location>`, then opens the live stream without user intervention.
- [ ] **Two identical serial-less USB-UART adapters swapped between ports**: Plug in two CH340 boards with identical `VID:PID` and no `iSerial`, note their `USB <location>` labels, then swap their physical USB sockets and click **Rescan**. *Expected:* Each board gets a location-based ID (`usb-1a86-7523-at-<location>`), swapping sockets swaps which ID maps to which physical board, and clicking **Build & flash** names the target adapter + port/location in the settings/confirmation panel before flashing.
- [ ] **Legacy `0.4.0` (wire v1) runs & `<mac>/` backups**: Copy a recorded run from `0.4.0` (wire v1 frames, `meta.json` lacking `device_id`) into `runs/` and an existing backup under `backups/<mac>/<timestamp>.bin`. *Expected:* The `0.4.0` run appears in **Runs**, opens and replays its flamegraph/functions cleanly, and **Remove agent… → Restore & flash** locates and restores the `<mac>/` backup.
- [ ] **Cross-version agent / host compatibility (once Agent v2 / P6 lands)**: Attach a v1-agent board to a v2 host (no `HELLO` beacon; samples start immediately), and attach a v2-agent board compiled with `MALGRIND_START_SAMPLING=y` to a `0.4.0` host. *Expected:* Both combinations stream and decode valid frames.
- [ ] **OS & chip boundary hygiene (automated)**: Run `node scripts/check-portability.mjs` and `node --test scripts/check-portability.test.mjs`. *Expected:* Exits `0` with `"os": []` and `"chip": []` in `scripts/portability-allow.json` (no OS `#ifdef`/headers outside `host/src/os/`, no chip names outside `host/src/families/`, `agent/`, and `platforms/`).
