# Third-party notices

LunaCore is based on LunaNX 3.1 by KazaR-RooT. The contribution notices and origin markers travel with the extracted files. [LunaNX v3.1 third-party notices](https://github.com/kazar-root/LunaNX/blob/v3.1/THIRD_PARTY_NOTICES.md)

| Component | Use in LunaCore | License and origin |
| --- | --- | --- |
| libcurl | HTTP and TLS for Amazon registration and Luna requests | System SDK or platform package. [curl license](licenses/curl.txt) |
| Jansson 2.14 | JSON parsing and serialization | MIT. Original source archive in `third_party/jansson/`. [License](licenses/Jansson-MIT.txt) |
| Mbed TLS 3.4.0 | Base64 and SHA-256 for the inherited PKCE code | Apache-2.0. Unmodified component files from LunaNX 3.1's libpeer tree, with a LunaCore build configuration. [License](third_party/mbedtls/LICENSE) |
| OpenNOW-Switch | Upstream basis for stream queue and audio policies | MIT. [License](licenses/OpenNOW-Switch-MIT.txt) |
| libpeer | ICE, DTLS, SCTP, RTP and RTCP | MIT. LunaNX's fork with Apple and Windows port adaptations. [License](third_party/libpeer/LICENSE) |
| Mbed TLS 3.6.7 | PKCE and DTLS in native player builds | Apache-2.0 option of the dual license. [License](licenses/MbedTLS-3.6.7.txt) |
| libSRTP 2.7.0 | SRTP protection | BSD-3-Clause. [License](licenses/libSRTP-2.7.0.txt) |
| qrcodegen | Local Amazon sign-in QR rendering | MIT. Reused from the original LunaNX 3.1 source. [License](licenses/qrcodegen-MIT.txt) |
| Opus 1.6.1 | Audio decoding | BSD-3-Clause. [License](licenses/Opus-1.6.1.txt) |
| SDL 2.32.10 | Controllers on Windows (Xbox, PlayStation, Nintendo, 8BitDo; USB and Bluetooth), statically linked. Linux and FreeBSD use the system SDL2 for audio and controllers | zlib. Original source archive in `third_party/sdl2/`. [License](licenses/SDL2-zlib.txt) |

The LunaNX Switch binary has additional notices for Borealis, Moonlight-Switch, FFmpeg, libnx, deko3d and other components. Use the original notice list when bringing over their code or runtime dependencies.
