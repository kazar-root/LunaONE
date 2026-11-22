function(lunaone_streaming_dependencies)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(LIBSRTP_TEST_APPS OFF)
    # Each slice of a universal build uses the same portable Opus source list.
    set(OPUS_DISABLE_INTRINSICS ON)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(BUILD_TESTING OFF)

    set(sources "${PROJECT_SOURCE_DIR}/third_party/streaming")
    FetchContent_Declare(libsrtp
        URL "${sources}/libsrtp-2.7.0.tar.gz"
        URL_HASH SHA256=54facb1727a557c2a76b91194dcb2d0a453aaf8e2d0cbbf1e3c2848c323e28ad)
    FetchContent_Declare(opus
        URL "${sources}/opus-1.6.1.tar.gz"
        URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1)
    FetchContent_MakeAvailable(libsrtp opus)
    # Xcode enables narrowing diagnostics that this vendored release promotes to errors.
    target_compile_options(srtp2 PRIVATE "$<$<C_COMPILER_ID:AppleClang>:-Wno-error=shorten-64-to-32>")
    if(WIN32)
        # Winsock's ntohl returns unsigned long, which libsrtp logs with %x.
        target_compile_options(srtp2 PRIVATE "$<$<C_COMPILER_ID:Clang,GNU>:-Wno-error=format>")
    endif()
endfunction()
lunaone_streaming_dependencies()

set(peer_src "${PROJECT_SOURCE_DIR}/third_party/libpeer/src")
add_library(lunaone_peer STATIC
    "${peer_src}/address.c" "${peer_src}/agent.c" "${peer_src}/base64.c"
    "${peer_src}/dtls_srtp.c" "${peer_src}/ice.c" "${peer_src}/mdns.c"
    "${peer_src}/peer.c" "${peer_src}/peer_connection.c" "${peer_src}/ports.c"
    "${peer_src}/rtcp.c" "${peer_src}/rtp.c" "${peer_src}/sctp.c" "${peer_src}/transport_feedback.c"
    "${peer_src}/sdp.c" "${peer_src}/socket.c" "${peer_src}/stun.c" "${peer_src}/utils.c")
target_include_directories(lunaone_peer PUBLIC "${peer_src}")
target_compile_definitions(lunaone_peer PRIVATE CONFIG_USE_USRSCTP=0 DISABLE_PEER_SIGNALING=1 LOG_LEVEL=LEVEL_WARN)
target_link_libraries(lunaone_peer PRIVATE mbedtls mbedx509 mbedcrypto srtp2)

if(WIN32)
    target_compile_definitions(lunaone_peer PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(lunaone_peer PRIVATE ws2_32 iphlpapi)
endif()
