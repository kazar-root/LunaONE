# LunaONE on Linux, FreeBSD and Windows

The desktop CLI supports Amazon device sign-in, catalog exploration, and Luna search. `catalog` displays the initial shelves. `catalog --all` adds titles from linked collections and removes duplicates. `search` queries the authenticated Luna catalog and prints the ASIN when available.

## Linux

Use CMake 3.25 or newer, Ninja, a C++20 compiler and the development packages for libcurl, GTK 3, SDL2 and FFmpeg. The Linux preset builds `LunaONE::Core` and `LunaONE::Stream`.

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
./build/linux/LunaONE
./build/linux/lunacore login fr
./build/linux/lunacore search "Alan Wake 2"
./build/linux/lunacore catalog --all
```

The `LunaONE-V0.4-Linux-Alpine-arm64.tar.gz` archive targets Alpine 3.23 and uses musl. On Alpine, install `gtk+3.0 sdl2 ffmpeg-libs libcurl font-dejavu` before opening the app. Use the Linux preset to build against another distribution's libraries.

## FreeBSD

The FreeBSD client uses GTK 3 for the catalog, the shared WebRTC engine, FFmpeg for H.264 video, and SDL2 for audio and controllers. The release offers `LunaONE-V0.4-FreeBSD-amd64.tar.gz` and `LunaONE-V0.4-FreeBSD-aarch64.tar.gz` for FreeBSD 14.4, 14.5, 15.0 and 15.1. You can validate the first Amazon sign-in by scanning the QR code with a phone. Browse the catalog and launch games from LunaONE in your FreeBSD graphical session.

Install the build and runtime dependencies with `pkg` as root.

```sh
pkg install cmake ninja pkgconf curl gtk3 ffmpeg sdl2 dejavu
```

From the source directory, run these commands as your desktop user.

```sh
cmake --preset freebsd
cmake --build --preset freebsd
ctest --preset freebsd
./build/freebsd/LunaONE
```

To make a binary archive for the build's FreeBSD architecture, run `cpack --config build/freebsd/CPackConfig.cmake -B build/freebsd`. On the target system, install `curl gtk3 ffmpeg sdl2 dejavu` with `pkg`, extract the archive and run `./LunaONE` from the extracted directory.

### Archives for other FreeBSD releases

LunaONE uses the system's libc++, which only gains functions over time. Build a published archive against the oldest supported release: it then runs on every newer one. FreeBSD 14.4, 15.0 and 15.1 ship libc++ 19, and 14.5 ships libc++ 21. An archive built on 14.5 stops on 15.1 with `ld-elf.so.1: Undefined symbol "_ZNSt3__113__hash_memoryEPKvm"`. Build on FreeBSD 14.4, or cross-build with `cmake/FreeBSD-cross.cmake` and a FreeBSD 14.4 sysroot. The sysroot holds the release's `base.txz` and the `curl gtk3 ffmpeg sdl2 xorgproto` packages, with their dependencies, from the `FreeBSD:14` repository. The file's header gives the commands.

The first FreeBSD archives were built on 14.5 and failed on 15.1 this way. Their 14.5 builds had passed all 13 CTest tests and opened their GTK windows in virtual machines. The ARM64 build had also passed IPv4 and IPv6 UDP loopback checks and decoded a local H.264 frame.

The current archives were cross-built from a FreeBSD 14.4 sysroot with clang and lld 21. Each dynamic symbol of `LunaONE` and `lunaone` was checked against the libraries of FreeBSD 14.4, 14.5, 15.0 and 15.1. All of them resolve for amd64 and aarch64. The same check finds the missing `__hash_memory` in the first archives on 14.4, 15.0 and 15.1. These archives have not run on FreeBSD yet. Live Luna playback and physical controller input also still need testing on FreeBSD.

## Windows

Extract `LunaONE-V0.4-Windows-x64.zip` for an Intel or AMD PC, or `LunaONE-V0.4-Windows-arm64.zip` for a Windows ARM PC. Keep the files in the extracted folder together and double-click `LunaONE.exe`, the only program in the archive. The command-line client `LunaONE-cli.exe` comes from a source build.

Choose the country for your Amazon account, create a code and scan the QR to open the Amazon confirmation page. Return to LunaONE after confirming. The window loads the catalog, searches Luna by title and can explore linked collections for more titles. Use **Se déconnecter** to remove the saved account from the PC.

The sign-in screen uses the same dark violet design as the Mac: the form on the left, and on the right the three sign-in steps, replaced by the QR code and the Amazon code once one is created. Windows (Direct2D) and Linux and FreeBSD (Cairo) draw it from the shared scene in `app/src/ui/library_scene.cpp`. The page is laid out at 1280 × 720 and scaled to the window, centred on both axes, so it keeps its proportions in a small window, in full screen or on a TV connected to the PC.

To start a stream, select a title and click **Jouer**, double-click the title, or press Enter with the title selected. The Windows player uses the shared WebRTC engine, Media Foundation H.264 decoding, Opus audio and the controllers read through SDL2. Playback diagnostics are written to `runtime.log` in the app’s local data directory.

Use an x64 or ARM64 compiler environment with CMake 3.25 or newer, Ninja and libcurl for the same architecture. If libcurl comes from vcpkg, pass its CMake toolchain file to the configure command.

```powershell
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
.\build\windows\LunaONE.exe
.\build\windows\LunaONE-cli.exe --version
.\build\windows\LunaONE-cli.exe search "Alan Wake 2"
```

The local cross-build directories under `build/desktop` contain the x64 and ARM64 executables, their runtime DLLs and `LunaONE.png`, the logo shown by the library. Keep the executable beside those files. The tests that run executables need the same DLLs on `PATH` or beside the test binaries.

In a source build, open `LunaONE-CLI.cmd` to display the CLI help and keep the console open. In PowerShell, run `.\LunaONE-cli.exe --version` from the build folder to check that the CLI starts.

## Library and controls

The Windows, Linux and FreeBSD windows show the Google TV library. The layout, panels, settings and on-screen keyboard live in `app/src/ui/library_scene.cpp`. Windows draws them with Direct2D and DirectWrite, and the GTK client uses Cairo and Pango. The Mac client draws the same screen with AppKit.

| Action | Mouse and keyboard | Controller |
| --- | --- | --- |
| Move, select | Arrows, Enter; a click selects, a second click or a double-click plays | D-pad or left stick, A |
| Back, close a panel | Backspace, Esc | B |
| Settings | Click **Réglages** | Menu or Start |
| Section (Top 10, Reprendre, Tous les jeux) | Click the section icons | LB or RB, pressed alone |
| Cancel a game launch | Click **Annuler le lancement du jeu**, then **Oui** | B |
| Search keyboard | Type in the field | LB + LT + RB + RT (L1 + L2 + R1 + R2) |
| Full screen | F11 (Alt + Enter on Windows, ⌃⌘F on Mac) or the maximize button of the title bar; Esc returns to the window | |
| Leave a game | Close the game window; Esc in a window | LS + RS + RB (L3 + R3 + R1) |

Controllers connect over USB or Bluetooth. Windows, Linux and FreeBSD read them through SDL2. Windows links SDL 2.32.10 statically from `third_party/sdl2`, and Linux and FreeBSD use the system's SDL2. SDL recognizes Xbox pads, PlayStation DualShock 4 and DualSense, Nintendo Pro Controllers and Joy-Con, and 8BitDo pads. 8BitDo pads work in X-input mode like an Xbox pad, in Switch mode like a Pro Controller, and in D-input mode through SDL's mappings for 17 models. A, B, X and Y follow the button positions, as on the Mac: the bottom button is A, even where a Nintendo or 8BitDo pad prints B on it. The hints show the labels printed on the pad in hand. The keyboard, the mouse and the on-screen keyboard drive the library before a game starts, for example to search for a game. In a game session, only the controller reaches Luna. There, the keyboard and mouse only switch full screen and leave the game. Keyboard and mouse input in game is in progress.

**Profil de qualité** in Réglages › Général sets the H.264 profile, picture and sound together: Faible (Baseline, 720p 30 FPS), Moyen (Main, 720p 60 FPS), Élevé (High, 1080p 60 FPS), Ultra (High, 1080p 60 FPS, full-rate audio), Automatique (follows the screen) or Personnalisé (the rows of Réglages avancés). Luna's server has so far always answered with Constrained Baseline. Transport-wide arrival feedback stays on the Mac only: on the Google TV over Wi-Fi it raised losses and picture delay without raising the bitrate.

Full screen is borderless and keeps the display mode. On Windows the taskbar goes below the window. As with the Mac's green button, the maximize button opens full screen rather than a maximized window. On Linux the game window is not modal, because GNOME does not let a modal window go full screen. The choice is saved and applies to the library and to games. Hints and settings name the buttons as printed on the controller in use: Xbox, PlayStation or Nintendo.

## Updates

In **Settings › About**, select **Check for updates** or the version line. When GitHub has a newer release, the panel offers **Install version V…**. LunaONE then:

1. picks this computer's file in the latest release (`-macOS-universal.zip`, `-Windows-x64.zip`, `-Windows-arm64.zip`, `-Linux-Alpine-arm64.tar.gz`, `-FreeBSD-amd64.tar.gz` or `-FreeBSD-aarch64.tar.gz`), only from `github.com/kazar-root/LunaONE/releases/download/`;
2. downloads it and checks its size and its SHA-256: the one GitHub computed when the file was uploaded, or else the release's `SHA256SUMS.txt`;
3. unpacks it next to the installed copy, with `ditto` on macOS, `tar.exe` on Windows and `tar` on Linux and FreeBSD;
4. replaces the installed files and quits, and the new version starts by itself.

On macOS the app bundle moves aside and the new one takes its place; a helper waits for the old app to end, removes it and opens the new one. On Windows, a running program can be renamed but not overwritten: each file moves aside as `*.lunaone-old`, the new one is copied in, and the new version, started with `--after-update <pid>`, waits for the old one to end and deletes those files. Any failure puts the old files back. On Linux and FreeBSD each new file is copied beside the old one and renamed over it, then a helper starts the new version once the old one has ended.

LunaONE needs write access to its folder (on macOS, to the app and to the folder that holds it). Otherwise, as for a copy run from a read-only disk, the panel only links to the GitHub releases. Linux builds made against glibc also link to GitHub, since the Linux release is built on Alpine (musl). Your sign-in and settings live in the data folder and stay as they are.

## LunaCore validation on 2026-11-19

| Check | Result |
| --- | --- |
| Linux build and tests | Alpine 3.23 ARM64 ran in a Lima VM on the Apple Silicon Mac. The native CLI and WebRTC library built, and all nine CTest suites passed. |
| Live Linux login and catalog | Amazon accepted the Linux device registration. Catalog exploration found 101 distinct games. Search returned Alan Wake 2 and seven Fortnite entries. |
| Live Linux media | A temporary test linked against `LunaCore::Stream` played a real Sonic Mania Plus session into FFmpeg and Opus decoders. It decoded 720 H.264 frames and 565 Opus packets. |
| Windows cross-builds | LLVM-MinGW linked the CLI and the native GUI for x64 and ARM64. Both GUI binaries use the Windows GUI subsystem and their runtime DLLs are packaged beside them. |
| Windows x64 device test | The user opened `LunaCoreApp.exe` by double-clicking, confirmed the Amazon sign-in code, loaded the catalog and found Alan Wake 2 through the GUI search. |
| Windows player cross-builds | The stream-enabled app linked for x64 and ARM64. The Win32 subsystem, architecture and packaged runtime dependencies were verified. Live video, audio and controller input still require a Windows device test. |
| Windows x64 playback | The first Sonic launch reached the H.264 decoder and failed at Media Foundation output. The user retested the build with explicit timing, aligned buffers and a compatibility retry. Sonic displayed video and played sound on the Windows x64 PC. |
| Windows ARM64 device test | The executable and DLLs were checked during packaging. Launch and live sign-in still require a Windows ARM64 device. |
| Apple regression checks | All nine macOS CTest suites passed, and Xcode built the iPadOS app. |

The Linux media test verified reception and decoding. A desktop window, audio output, and controller input remain to be exercised with a desktop player.

## LunaONE validation on 2026-11-21

| Check | Result |
| --- | --- |
| Shared library | The `library_scene` test drives the library without a window or network: controller navigation and paging, PlayStation button names, the keyboard chord, typing and deleting, Esc, Enter to play and full screen. |
| Linux | The Alpine ARM64 build passed all 12 CTest suites. Headless renders of the sign-in screen and of a demo catalog matched the Google TV layout, with covers and backdrop. |
| Windows | LLVM-MinGW linked the x64 GUI with Direct2D and DirectWrite. It still needs a test on a Windows PC. |

## LunaONE validation on 2026-11-24

| Check | Result |
| --- | --- |
| Windows controllers | LLVM-MinGW built the x64 and ARM64 apps with SDL 2.32.10 linked statically, with its XInput, DirectInput, RawInput, Windows.Gaming.Input and HIDAPI drivers. The executables need only system DLLs besides the ones already packaged. PlayStation, Nintendo and 8BitDo pads still need a test on a Windows PC, over USB and Bluetooth. |
| Linux | The Alpine ARM64 build with the shared SDL controller code passed all 13 CTest suites. |
| macOS | The universal V0.2 app passed all 13 CTest suites. |
| Android | The V0.2 APK has version code 11 and the V0.1 signing certificate, so it installs over V0.1. |
| iPhone and iPad | Xcode's asset catalog compiler needs an iOS Simulator runtime, which this Mac lacks. The V0.2 IPA was built without the catalog and carries the V0.1 icons, which have not changed. |
| FreeBSD | The amd64 and aarch64 archives were rebuilt from the FreeBSD 14.4 sysroot. Every dynamic symbol resolves. The only new ones come from SDL2 2.32.10, which the FreeBSD 14 and 15 repositories both ship. |

## LunaONE validation on 2026-11-25 (V0.3)

| Check | Result |
| --- | --- |
| Sign-in screen | The shared scene draws the dark sign-in screen at 1280 × 720 and scales it to the window, centred. A headless Cairo render on Alpine Linux matched the macOS screen: gradient, glows, frosted cards and the three steps. The "Open the Amazon page" button is gone from the computer sign-in screen, because it stayed disabled while LunaONE waited for the code. |
| Linux | The Alpine ARM64 build passed all 13 CTest suites. |
| macOS | The universal V0.3 app passed all 13 CTest suites. |
| Windows | LLVM-MinGW built the x64 and ARM64 apps, including the Direct2D gradient backdrop. The archives hold the same files as V0.2, and the executables need only system DLLs besides the packaged ones. The new screen still needs a look on a Windows PC. |
| FreeBSD | The amd64 and aarch64 archives were built from the FreeBSD 14.4 sysroot, and every dynamic symbol resolves. The only new symbol, `cairo_pattern_create_radial`, comes from the base Cairo library. |
| Android | The V0.3 APK has version code 12 and the certificate of V0.1 and V0.2, so it installs over them. |
| iPhone and iPad | The V0.3 IPA was built without the asset catalog, as for V0.2, and carries the same icons. |

## LunaONE validation on 2026-11-25 (V0.4)

| Check | Result |
| --- | --- |
| Updates on macOS | A test build labelled 0.2, run from a scratch folder, found V0.3 on GitHub, downloaded `LunaONE-V0.3-macOS-universal.zip`, matched GitHub's SHA-256, swapped the app bundle and quit. The helper removed the old bundle and opened V0.3, whose signature verified. |
| Updates on Linux | The same test on Alpine ARM64 replaced every file of a V0.2 folder with V0.3, with modes 755 for the program and folders and 644 for the rest, then started V0.3, which stopped only because the VM has no display. |
| macOS | The universal V0.4 app passed all 13 CTest suites. |
| Linux | The Alpine ARM64 V0.4 build passed all 13 CTest suites. |
| Windows | LLVM-MinGW built the x64 and ARM64 apps with the update code (`tar.exe` unpacking, `*.lunaone-old` files, `--after-update`). The archives hold the same files as before. The update itself still needs a run on a Windows PC. |
| Android | The V0.4 APK has version code 13 and the certificate of the earlier versions. It declares `REQUEST_INSTALL_PACKAGES` and `UpdateProvider`, and its native library holds the update functions for both ABIs. The update itself still needs a run on a device. |
| iPhone and iPad | The V0.4 IPA reports a new version and links to GitHub. |
