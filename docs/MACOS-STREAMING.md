# macOS streaming prototype

LunaCore uses LunaNX 3.1's session jobs, SDP, libpeer RTP and RTCP code, embedded SCTP transport, display configuration and gamepad encoder. The macOS CLI selects a title from the authenticated catalog and opens a native AppKit window. The C++ session keeps Apple framework types in the host adapters.

The native libpeer build uses Mbed TLS with platform entropy, an ephemeral ECDSA certificate and SHA-256 verification of the peer against the fingerprint in the Luna SDP answer. It negotiates the SRTP profile implemented by the installed packet policies. The session requires Luna's launch permission. The live `ALLOW` response is handled explicitly, and required or unrecognized PIN states stop the launch.

VideoToolbox accepts the H.264 access units from libpeer and requests hardware decoding. A sample-buffer display layer presents the decoded images. Opus decodes the audio into PCM for an Audio Queue. GameController supplies axes, analog triggers and buttons to the existing Luna input channel. [VideoToolbox hardware decoding](https://developer.apple.com/documentation/VideoToolbox/kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder) · [GameController extended gamepad](https://developer.apple.com/documentation/gamecontroller/gcextendedgamepad)

The three dependency source archives are pinned by SHA-256 in CMake. [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7) · [libSRTP 2.7.0](https://github.com/cisco/libsrtp/releases/tag/v2.7.0) · [Opus 1.6.1](https://opus-codec.org/release/stable/2026/01/14/libopus-1_6_1.html)

## Live validation on Apple Silicon

On 2026-11-18, three real Sonic Mania Plus sessions connected to `eu-west-3` and exited normally.

| Measurement | First stream | Stability test | USB controller test |
| --- | ---: | ---: | ---: |
| Playback time | 20 s | 90 s | 70 s |
| H.264 access units decoded | 1,206 | 5,404 | 4,191 |
| Pictures submitted to the display layer | 1,190 | 5,340 | 4,132 |
| Submission rate | 59.4 fps | 59.3 fps | 59.0 fps |
| Longest interval between submitted pictures | — | 132 ms | 299 ms |
| VideoToolbox hardware decoding | Active | Active | Active |
| Audio packets / buffers returned by Audio Queue | 1,044 / 1,034 | 4,520 / 4,510 | 3,516 / 3,506 |
| Session start to first picture | 11.8 s | 9.0 s | 8.7 s |
| WebRTC RTT at shutdown / p95 | 7 / 11 ms | 9 / 12 ms | 8 / 12 ms |
| Peak sampled resident memory | 85.2 MiB | 83.9 MiB | 85.0 MiB |
| Input reports / changed states sent | 42 / 0 | 181 / 0 | 149 / 12 |

In the stability test, macOS detected the USB Xbox Series X controller but no button or axis changes reached the encoder. After the player began accepting GameController events throughout playback and preferred the most recently used controller, the USB retest recorded 12 changed states and the tester confirmed that the game responded. [GameController event delivery](https://developer.apple.com/documentation/gamecontroller/gccontroller/shouldmonitorbackgroundevents)

Network round-trip time measures the transport. Controller-to-screen latency remains unmeasured. Picture counts measure submissions to the display layer.

Xcode Debug and universal Release passed all eight CTest suites. The player CLI also started under x86_64 Rosetta. The core-only CMake target built independently of the player.

## Bluetooth controller validation

On 2026-11-19, the native Xcode Debug player ran Sonic Mania Plus in `eu-west-3` for 60 seconds. macOS reported the Xbox Wireless Controller over Bluetooth Low Energy. All 37 transport checks during the run showed the Xbox Bluetooth device with the USB cable disconnected.

The tester confirmed that the D-pad and buttons controlled the game. LunaCore sent 455 reports, including 393 state changes. VideoToolbox hardware decoding was active and the player submitted 3,562 pictures at 59.4 fps. The WebRTC RTT was 9 ms at shutdown and 14 ms at the 95th percentile. The player exited normally.
