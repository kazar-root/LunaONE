# LunaONE on Linux and Windows

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

The local ARM64 archive targets Alpine 3.23 and uses musl. On Alpine, install `gtk+3.0 sdl2 ffmpeg-libs libcurl font-dejavu` before opening the app. Use the Linux preset to build against another distribution's libraries.

## Windows

Extract `LunaONE-Windows-x64.zip` for an Intel or AMD PC, or `LunaONE-Windows-arm64.zip` for a Windows ARM PC. Keep the files in the extracted folder together and double-click `LunaONE.exe`.

Choose the country for your Amazon account, create a code and scan the QR to open the Amazon confirmation page. Return to LunaONE after confirming. The window loads the catalog, searches Luna by title and can explore linked collections for more titles. Use **Se déconnecter** to remove the saved account from the PC.

To start a stream, select a title and click **Jouer**, double-click the title, or press Enter with the title selected. The Windows player uses the shared WebRTC engine, Media Foundation H.264 decoding, Opus audio and an XInput controller. Playback diagnostics are written to `runtime.log` in the app’s local data directory.

Use an x64 or ARM64 compiler environment with CMake 3.25 or newer, Ninja and libcurl for the same architecture. If libcurl comes from vcpkg, pass its CMake toolchain file to the configure command.

```powershell
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
.\build\windows\LunaONE.exe
.\build\windows\LunaONE-cli.exe --version
.\build\windows\LunaONE-cli.exe search "Alan Wake 2"
```

The local cross-build directories under `build/desktop` contain the x64 and ARM64 executables and their runtime DLLs. Keep the executable beside those DLLs. The tests that run executables need the same DLLs on `PATH` or beside the test binaries.

Open `LunaONE-CLI.cmd` to display the CLI help and keep the console open. In PowerShell, run `.\LunaONE-cli.exe --version` from the folder containing the executable and DLLs to check that the CLI starts.

## Library and controls

The Windows and Linux windows show the Google TV library. The layout, panels, settings and on-screen keyboard live in `app/src/ui/library_scene.cpp`; Windows draws them with Direct2D and DirectWrite, Linux with Cairo and Pango. The Mac client draws the same screen with AppKit.

| Action | Mouse and keyboard | Controller |
| --- | --- | --- |
| Move, select | Arrows, Enter; a click selects, a second click or a double-click plays | D-pad or left stick, A |
| Back, close a panel | Backspace, Esc | B |
| Settings | Click **Réglages** | Menu or Start |
| Section (Top 10, Reprendre, Tous les jeux) | Click the section icons | LB or RB, pressed alone |
| Cancel a game launch | Click **Annuler le lancement du jeu**, then **Oui** | B |
| Search keyboard | Type in the field | LB + LT + RB + RT (L1 + L2 + R1 + R2) |
| Full screen | F11 (Alt + Enter on Windows, ⌃⌘F on Mac); Esc returns to the window | |
| Leave a game | Close the game window; Esc in a window | LS + RS + RB (L3 + R3 + R1) |

**Profil de qualité** in Réglages › Général sets the H.264 profile, picture and sound together: Faible (Baseline, 720p 30 FPS), Moyen (Main, 720p 60 FPS), Élevé (High, 1080p 60 FPS), Ultra (High, 1080p 60 FPS, full-rate audio), Automatique (follows the screen) or Personnalisé (the rows of Réglages avancés). Luna's server has so far always answered with Constrained Baseline. Transport-wide arrival feedback stays on the Mac only: on the Google TV over Wi-Fi it raised losses and picture delay without raising the bitrate.

Full screen is borderless and keeps the display mode. The choice is saved and applies to the library and to games. Hints and settings name the buttons as printed on the controller in use: Xbox, PlayStation or Nintendo.

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
