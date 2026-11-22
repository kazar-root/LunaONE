# LunaNX 3.1 source audit

Audit date 2026-11-18. The local release worktree matched tag `v3.1` at commit `a794d274a9e48fca6b26639bee8e56946f5cc660`. LunaCore was extracted into a separate directory from that tag. The source audit used the shipped code, build definitions, dependency tree, provenance metadata and host tests. [LunaNX source at the audited commit](https://github.com/kazar-root/LunaNX/tree/a794d274a9e48fca6b26639bee8e56946f5cc660)

## Application modules

| LunaNX source | Role and dependencies | LunaCore 0.1 |
| --- | --- | --- |
| `app/src/main.cpp`, `ui/`, `resources/` | Borealis UI, Switch applet/I/O startup, software keyboard, browser applet, phone callback listener, QR code, GPU-backed image cache and ROMFS assets | Native CLI uses the existing device-code flow on Amazon's confirmation page. The standalone image URL helper is available to the artwork test. |
| `core/http_client.*`, `core/i18n.*`, `core/runtime_journal.*` | libcurl, C++ translations and runtime diagnostics | Extracted. Apple builds use HTTP/1.1 because compressed responses from the Luna proxy failed over HTTP/2 with the macOS SDK's libcurl. The upstream TLS checks are used. |
| `core/app_paths.*`, `core/atomic_file_replace.hpp` | SD-card home directory and storage replacement with a FAT fallback | The port resolves the user data directory on the host. The file replacement helper is reused. |
| Other `core/` modules | Version, update flow, stream settings, generated provenance and libnx/mbedTLS compatibility | Origin markers and notices retained. The NRO version, updater, stream settings, generated NRO metadata and Switch compatibility shims stay in LunaNX. |
| `luna/amazon_auth.*`, `cookie_jar.*`, `device_code_policy.hpp` | Device-code challenge, confirmation and registration, PKCE alternative, cookie refresh, deregistration and persistence | Extracted as-is. The CLI invokes the device-code path. Jansson and Mbed TLS satisfy the existing JSON and PKCE calls. |
| `luna/luna_client.*` and its models and policies | Access token and app configuration, profile login, regional routing, shelves and widget retrieval, history, search, entitlement labels, PIN requirement, session creation and polling | Extracted as-is. Catalog is exposed by the CLI. Session jobs are available in the library. |
| `luna/luna_sdp.*` | Builds the Luna offer from libpeer ICE/DTLS parameters and normalizes the answer | Extracted. The H.264 payload lookup now checks the codec as well as the RTP payload number. SSRC generation uses one random engine per thread. |
| `input/` | Protobuf-like controller/startup messages and delivery policy, plus libnx rumble | Encoders and portable policies extracted. Native controller discovery, USB/Bluetooth OS events and haptics belong in host adapters. |
| `webrtc/` | `LunaStreamSession` coordinates the session job, libpeer, RTP callbacks, data channels, video queue, audio, rendering, metrics and input. It includes Switch-specific rendering and rumble. | Portable WebRTC helpers are compiled. Media transport and the orchestration class remain work for the streaming milestone. |
| `stream/` | CPU-independent queue, RTP and metrics policies, plus an FFmpeg/libav decoder, deko3d renderer and libnx audio pipeline | Portable policies extracted. The media implementation depends on Switch APIs. |

## Build, media and platform dependencies

- LunaNX CMake selects the devkitPro toolchain, devkitA64, Switch portlibs, libnx, Borealis, deko3d, `uam` shaders and ROMFS/NRO packaging. LunaCore builds a C++20 static library using CMake, threads, native libcurl and the vendored JSON/PKCE sources. On macOS, Xcode builds the CLI and tests against the SDK.
- LunaNX's libpeer subtree includes ICE, DTLS-SRTP, SCTP, RTP/RTCP, H.264 depacketization and Luna-specific recovery and data-channel code. It builds Mbed TLS, libsrtp, usrsctp and cJSON and includes coreHTTP and coreMQTT. The Switch CMake modifies the Mbed TLS configuration in place and enables null entropy for its console build. The Apple transport needs a host build with platform entropy and verification of the negotiated peer fingerprint before real session tests. LunaCore 0.1 uses the vendored Base64/SHA-256 subset solely for PKCE and relies on libcurl for HTTP TLS.
- The Switch media path uses an FFmpeg NVTEGRA build with NVDEC, a deko3d renderer and GLSL shaders. The inherited decoder contains a VideoToolbox branch behind an FFmpeg build flag. The CMake tree ships Switch archives and headers. The macOS media adapter needs native video output, hardware capability checks and a fallback for Apple Silicon and Intel.
- `AudioPipeline` uses `hwopus` and `audout`. The Apple path can reuse RTP timing and buffering policies with a native Opus decode and audio sink. `GameController` belongs in the Apple application layer, feeding the existing controller encoder and the WebRTC data channels.
- `provenance/`, `scripts/`, `docs/` and `tests/` document the Switch release process, including source manifests and NRO integrity records. Six host suites were ported. LunaCore adds a native CLI smoke test and an SDP regression suite.

## License review

LunaCore retains GNU GPL v3, the upstream creator notice and per-file origin markers. The GPL requires a prominent modification notice for conveyed versions and licensing of the covered work as a whole. Distribution of a binary also requires a compliant way to provide the corresponding source for the exact build, including relevant build scripts and dependency sources. [GNU GPL v3, sections 1, 5 and 6](https://www.gnu.org/licenses/gpl-3.0.html)

Keeping the project in a separate directory and using permissive dependencies leaves the license of the derived work under GPL v3. An MIT release of the derived code would require a separate grant from all applicable rights holders and an inventory of upstream contributions, especially if Moonlight-Switch or the FFmpeg path is carried over. Review the distribution terms and signing arrangements for the chosen Apple distribution channel before shipping an iPadOS build.

## macOS 0.1 validation

Tests ran on Apple Silicon under macOS 26.5.2 with Xcode 26.6, the macOS 26.5 SDK and CMake 4.4.4.

| Check | Result |
| --- | --- |
| Xcode Debug for macOS arm64 | Build succeeded. All 8 CTest suites passed. |
| Xcode universal Release | Build succeeded with arm64 and x86_64 slices and a macOS 13 deployment target. All 8 CTest suites passed. |
| x86_64 execution | Seven C++ test executables and the CLI version and markets commands passed under Rosetta. |
| Live Amazon device sign-in | Amazon France issued a code and accepted the confirmation, then Luna issued an access token and a player profile token. |
| Live catalog | The native Xcode CLI fetched 100 catalog entries across 12 shelves from the signed-in account. |
| Upstream integrity | The LunaNX release worktree was clean at the start and end of the audit. Of the 42 copied application files, 38 matched the tag byte for byte. |

The first live request to the Luna configuration endpoint reproduced libcurl error 23 with compressed HTTP/2 responses. The same request succeeded with compression over HTTP/1.1. The final Apple build uses HTTP/1.1 for those requests and retains the Amazon authentication and Luna API logic. The registration is stored in the local LunaCore application data directory.

The follow-up port of the media path and its live validation have a separate report. [macOS streaming](MACOS-STREAMING.md)
