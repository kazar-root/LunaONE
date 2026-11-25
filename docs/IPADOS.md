# LunaONE on iPhone and iPad

LunaONE for iPhone and iPad uses UIKit for sign-in, the catalog and playback. The same C++ engine handles the device-code flow, Luna catalog and jobs, WebRTC, and controller encoding. The iPad build uses an ephemeral URLSession for HTTPS and the Apple SDK's TLS trust store. The video, audio and Extended Gamepad adapters are shared with macOS.

The iPad registers as its own Amazon device. The session file lives in the app's private Application Support directory, which is excluded from backups.

## Build and install locally

LunaONE targets iOS and iPadOS 17 or later. Open the LunaCore folder in VS Code and use Xcode for compilation, automatic development signing and device deployment. Use your Apple Development team ID in the device preset.

```sh
cmake --preset ipados -DLUNACORE_APPLE_TEAM_ID=YOUR_TEAM_ID
open build/ipados/LunaONE.xcodeproj
```

Choose the `LunaONE-iOS` scheme and your paired iPad as the destination in Xcode. Enable Developer Mode on the iPad and run the scheme to install the app. The Run action uses `RelWithDebInfo` to optimize the stream while preserving debug symbols. The bundle ID defaults to `app.lunacore.ipados` and can be overridden with `LUNACORE_IPADOS_BUNDLE_ID`. [Xcode device builds and signing](https://developer.apple.com/documentation/xcode/building-and-running-an-app) · [Apple Developer Mode instructions](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)

For the simulator on an Apple Silicon Mac:

```sh
cmake --preset ipados-simulator
cmake --build --preset ipados-simulator
```

## Apple Watch mirror

On an iPhone paired with an Apple Watch, LunaONE shows the game on the watch. The iPhone streams and decodes the game as usual and sends the watch a small JPEG of the picture, about eight times a second, each one once the watch has shown the previous one. The watch's buttons join the controller connected to the iPhone, if any:

| On the watch | In the game |
| --- | --- |
| **A** and **B** on screen | A and B, held as long as your finger stays on them |
| Digital Crown | D-pad up and down |
| Long press on the picture | Menu (Start) |

watchOS gives apps no hardware video decoder, restricts the network connections a stream needs and accepts no gamepad, so the game itself always runs on the iPhone.

The watch app is a SwiftUI target embedded in the iPhone app. It needs the iOS and watchOS platforms in Xcode (Xcode › Settings › Components), and the same Apple Development team as the iPhone app:

```sh
cmake --preset ipados -DLUNACORE_APPLE_TEAM_ID=YOUR_TEAM_ID -DLUNACORE_BUILD_WATCH_APP=ON
open build/ipados/LunaONE.xcodeproj
```

Run the `LunaONE-iOS` scheme on the iPhone: the watch app installs with it on the paired watch. Its bundle ID is the iPhone app's followed by `.watchkitapp`. Open LunaONE on the watch, then start a game on the iPhone.

## Updates

**Check for updates** tells whether GitHub has a newer version. An iPhone or iPad app must be signed again for each device, so LunaONE cannot install it itself: download the new IPA from GitHub and install it with AltStore or Sideloadly.

## Test a game

Select your Amazon market and create a code. Scan the QR code from another device or use the Amazon button on the same device, then enter the displayed code. The sign-in screen uses the Mac's dark violet design. On an iPad, or an iPhone on its side, the form and the steps (then the code) sit side by side; on an iPhone held upright they are stacked, and the page scrolls when it is taller than the screen. Select a title in the catalog to start the native stream. Pair a Bluetooth gamepad from iPad settings to control the game. Use the close button to return to the catalog. The Xcode console reports picture and input counts when playback ends.

## LunaCore validation on 2026-11-19

| Check | Result |
| --- | --- |
| iPad simulator | The Xcode build compiled, installed and displayed the native Amazon sign-in screen on an iPad Pro 11-inch simulator. |
| Physical iPad binary | Xcode compiled and linked the unsigned arm64 iPadOS build. |
| Apple HTTPS adapter | A live macOS test with the same URLSession adapter and a signed-in Luna account fetched 100 catalog entries across 12 shelves. |
| macOS regression | All eight CTest suites passed after the Apple media code was shared. |
| iPad Pro connection | Xcode paired the iPad Pro with Developer Mode enabled and recognized it as an iPadOS 26.6.1 destination. |
| Development signature | Xcode used an automatically managed development profile for `app.lunacore.ipados`. The signed build passed `codesign --verify --strict`. |
| Physical iPad installation | CoreDevice installed an early LunaCore build and confirmed it in the iPad's app list. |
| Physical iPad launch | The developer opened LunaCore on the iPad Pro and confirmed that the Amazon sign-in screen appeared. |
| Amazon device code on iPad | The first build crashed while reading the HTTPS response. Xcode rebuilt the client with ARC enabled. Amazon returned HTTP 200 after reinstalling, and the developer confirmed that LunaCore displayed a code. |
| Amazon sign-in and catalog on iPad | Amazon accepted the device registration. LunaCore retrieved 99 catalog entries across 12 shelves, and the developer confirmed that the catalog appeared on the physical iPad. |
| Initial iPad streaming test | Sonic responded to the controller. The developer reported stuttering video and an audio cutout. The app counted 5,502 presented frames, 7,280 recycled audio buffers, and 24 input changes. |
| macOS audio recovery | The updated shared Audio Queue resumed processing after an injected 300 ms pause. It recycled 36 buffers with decoded audio after resumption and dropped zero packets when the test submitted Opus packets 20 ms apart. |
| Optimized iPad build before the display change | The app presented 3,523 frames in 68.6 seconds (51.4 fps) and decoded 4,085 frames with hardware VideoToolbox. |
| iPad Sonic test with display on decode | The developer confirmed smooth video and continuous sound. In 72.1 seconds, the app presented 4,273 of 4,274 decoded frames (59.3 fps), recorded 515 input changes, and measured 17 ms of transport RTT at the end of playback. |

The final test used hardware VideoToolbox and the connected controller. The audio queue recorded 45 ten-millisecond buffers that needed some silence and 23 incoming audio packets that exceeded available buffer capacity. The developer heard no interruption in that test. Stability over longer sessions remains unmeasured.

### Catalog and streaming follow-up

The iPad shows the initial shelves while the engine explores linked collections. Search combines local matches with the Luna search results page. Catalog requests stop being scheduled when a game opens.

The NXNow notes describe short decode queues, burst handling and packet recovery. The iPad player decodes synchronously on its own worker and holds up to ten compressed units through a burst. It requests a keyframe if it must discard a backlog. The stream log includes frame gaps over 50 ms, waiting for keyframes, and non-silent decoded audio packets.

The stream uses the LunaNX 3.1 720p60 display request. If video is absent after opening the control channel, LunaCore resends the request up to three times. The Apple transport uses the compiler byte-order macros for RTP and RTCP headers. The old RTP layout misread the H.264 payload type as 112. The canonical packet now reads as RTP version 2, payload type 96, with its marker bit set.

Closing a game with × asks Luna to end that session or cancel that pending launch. The request uses the identifier returned for the current game. The app also reports the service's response when a profile is already playing or a title requires an offer or save slot.

| Check | Result |
| --- | --- |
| Live Luna catalog using the macOS engine | 52 distinct titles from initial shelves and 50 additional titles from 63 linked pages. |
| Live Luna search | Found Alan Wake 2, Kingdom Come: Deliverance II, and seven Fortnite entries. |
| Live launch data | Luna returned a launch action for Alan Wake 2 and Kingdom Come: Deliverance II. The Fortnite product page returned an offer action for this account. |
| Live game switch in the macOS engine | Kingdom Come: Deliverance II reached 461 decoded frames. Luna accepted the explicit close, then Alan Wake 2 started and reached 771 decoded frames. |
| Live audio in the macOS engine | Kingdom Come's two startup samples produced 676 and 725 silent decoded packets. Alan Wake 2's last test produced 443 non-silent packets out of 764 decoded. |
| macOS build and tests | The native macOS client built successfully and all eight CTest suites passed, including search cancellation, launch-action fallback, and current-session closure fixtures. |
| Expanded catalog on the physical iPad | LunaCore displayed 102 distinct games, including 47 found by exploring 66 linked pages. |
| First iPad test after the catalog change | Kingdom Come produced audio and showed 423 frames. The image went black during the session even though 3,445 video units reached the decoder input. Alan Wake 2 produced audio and reached the no-image timeout with zero assembled video units. |
| VideoToolbox recovery test on the Mac | A 180-frame synthetic H.264 stream intentionally lost a frame. The decoder skipped 34 dependent frames, restarted on the next keyframe, and decoded all 145 remaining eligible frames in hardware with zero decode errors. |
| iPad decoder recovery build | Alan Wake 2 still timed out before the first picture. The app decoded audio and closed the abandoned Luna session. This test preceded the corrected RTP layout and display request. |
| RTP and RTCP header regression | All nine macOS CTest suites passed, including the canonical RTP and RTCP packet test. The same RTP fixture also compiled for iPadOS arm64. |
| iPad transport update | The user confirmed that the black screen was resolved. Two sessions presented 810 and 3,630 frames, with no decoder errors and active VideoToolbox hardware decoding. The longest measured frame gaps were 922 ms and 1,001 ms. |
| V0.3 sign-in screen | The iPhone and iPad app with the redesigned sign-in screen built for iOS 17 arm64 with Xcode. It has not been tried on a device or in a simulator yet. |
