cmake_minimum_required(VERSION 3.25)
project(LunaONE VERSION 0.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

function(add_lunacore_jansson)
    # Jansson 2.14 predates CMake 4. Its build policy applies only to the dependency.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(JANSSON_BUILD_DOCS OFF)
    set(JANSSON_BUILD_SHARED_LIBS OFF)
    set(JANSSON_EXAMPLES OFF)
    set(JANSSON_WITHOUT_TESTS ON)
    set(JANSSON_INSTALL OFF)
    FetchContent_Declare(jansson
        URL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jansson/jansson-2.14.tar.gz"
        URL_HASH SHA256=c739578bf6b764aa0752db9a2fdadcfe921c78f1228c7ec0bb47fa804c55d17b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(jansson)
    target_include_directories(jansson INTERFACE "$<BUILD_INTERFACE:${jansson_BINARY_DIR}/include>")
endfunction()
add_lunacore_jansson()

set(lunaone_is_macos OFF)
set(lunaone_is_ios OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(lunaone_is_macos ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(lunaone_is_ios ON)
endif()
set(lunaone_cli_default ON)
if(lunaone_is_ios)
    set(lunaone_cli_default OFF)
    enable_language(OBJCXX)
    set(lunaone_http app/ipados/http_client.mm)
elseif(ANDROID)
    set(lunaone_cli_default OFF)
    set(lunaone_http app/android/src/main/cpp/http_client.cpp)
else()
    find_package(CURL REQUIRED)
    set(lunaone_http app/src/core/http_client.cpp)
endif()

option(LUNACORE_BUILD_CLI "Build the desktop command-line client" ${lunaone_cli_default})
set(lunaone_player_default OFF)
if(lunaone_is_macos AND LUNACORE_BUILD_CLI)
    set(lunaone_player_default ON)
endif()
option(LUNACORE_BUILD_MACOS_PLAYER "Build the native macOS streaming player" ${lunaone_player_default})
option(LUNACORE_BUILD_IPADOS_APP "Build the native iPadOS application" ${lunaone_is_ios})
option(LUNACORE_BUILD_WINDOWS_APP "Build the native Windows application" ${WIN32})
option(LUNACORE_BUILD_ANDROID_APP "Build the native Android application library" ${ANDROID})
option(LUNACORE_BUILD_LINUX_APP "Build the native Linux GTK application" OFF)
option(LUNACORE_BUILD_FREEBSD_APP "Build the native FreeBSD GTK application" OFF)
option(LUNACORE_BUILD_STREAM "Build the shared native WebRTC streaming library" OFF)
if(LUNACORE_BUILD_LINUX_APP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        message(FATAL_ERROR "Select a Linux toolchain to build the Linux application.")
    endif()
    set(lunaone_gtk_target lunaone_linux)
endif()
if(LUNACORE_BUILD_FREEBSD_APP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        message(FATAL_ERROR "Select a FreeBSD toolchain to build the FreeBSD application.")
    endif()
    set(lunaone_gtk_target lunaone_freebsd)
endif()
if(lunaone_gtk_target)
    set(LUNACORE_BUILD_STREAM ON)
endif()
if(LUNACORE_BUILD_MACOS_PLAYER AND (NOT lunaone_is_macos OR NOT LUNACORE_BUILD_CLI))
    message(FATAL_ERROR "The streaming player requires the macOS CLI.")
endif()
if(LUNACORE_BUILD_IPADOS_APP AND NOT lunaone_is_ios)
    message(FATAL_ERROR "Select the iOS toolchain to build the iPadOS application.")
endif()
if(LUNACORE_BUILD_WINDOWS_APP AND NOT WIN32)
    message(FATAL_ERROR "Select a Windows toolchain to build the Windows application.")
endif()
if(LUNACORE_BUILD_ANDROID_APP AND NOT ANDROID)
    message(FATAL_ERROR "Select the Android NDK toolchain to build the Android application.")
endif()
if(ANDROID AND LUNACORE_BUILD_CLI)
    message(FATAL_ERROR "The Android HTTPS client requires the application JVM.")
endif()
if(LUNACORE_BUILD_MACOS_PLAYER)
    enable_language(OBJCXX)
endif()
if(WIN32 OR LUNACORE_BUILD_STREAM OR LUNACORE_BUILD_MACOS_PLAYER OR LUNACORE_BUILD_IPADOS_APP OR LUNACORE_BUILD_ANDROID_APP)
    # Windows also needs Mbed TLS's platform code for the authentication helpers.
    include(cmake/MbedTLS.cmake)
    set(lunaone_crypto mbedcrypto)
else()
    set(mbedtls_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls")
    add_library(lunaone_mbedcrypto STATIC
        "${mbedtls_dir}/library/base64.c"
        "${mbedtls_dir}/library/constant_time.c"
        "${mbedtls_dir}/library/platform_util.c"
        "${mbedtls_dir}/library/sha256.c")
    target_include_directories(lunaone_mbedcrypto PUBLIC "${mbedtls_dir}/include" "${mbedtls_dir}")
    target_compile_definitions(lunaone_mbedcrypto PUBLIC MBEDTLS_CONFIG_FILE="lunacore_mbedtls_config.h")
    set(lunaone_crypto lunaone_mbedcrypto)
endif()

if(LUNACORE_BUILD_STREAM OR LUNACORE_BUILD_MACOS_PLAYER OR LUNACORE_BUILD_IPADOS_APP OR LUNACORE_BUILD_WINDOWS_APP OR LUNACORE_BUILD_ANDROID_APP)
    include(cmake/StreamingDependencies.cmake)
endif()

add_library(lunaone_core STATIC
    app/src/core/app_paths.cpp
    app/src/core/playback_preferences.cpp
    app/src/core/update_check.cpp
    app/src/core/self_update.cpp
    ${lunaone_http}
    app/src/core/i18n.cpp
    app/src/core/runtime_journal.cpp
    app/src/input/luna_client_messages.cpp
    app/src/input/luna_input_encoder.cpp
    app/src/input/controller_labels.cpp
    app/src/luna/amazon_auth.cpp
    app/src/luna/cookie_jar.cpp
    app/src/luna/luna_client.cpp
    app/src/luna/luna_sdp.cpp
    app/src/luna/recent_games.cpp
    app/src/ui/device_qr.cpp
    app/src/ui/library_scene.cpp
    app/src/ui/library_sections.cpp
    app/src/ui/virtual_keyboard.cpp
    third_party/qrcodegen/qrcodegen.c
    app/src/webrtc/shared.cpp)
add_library(LunaONE::Core ALIAS lunaone_core)
target_include_directories(lunaone_core PUBLIC app/src)
target_include_directories(lunaone_core PRIVATE third_party/qrcodegen)
target_compile_features(lunaone_core PUBLIC cxx_std_20)
target_compile_definitions(lunaone_core PRIVATE LUNAONE_VERSION="${PROJECT_VERSION}")
target_link_libraries(lunaone_core PRIVATE Threads::Threads jansson ${lunaone_crypto})
if(lunaone_is_ios)
    find_library(LUNACORE_Foundation Foundation REQUIRED)
    find_library(LUNACORE_Network Network REQUIRED)
    set_property(SOURCE ${lunaone_http} APPEND PROPERTY COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(lunaone_core PRIVATE ${LUNACORE_Foundation} ${LUNACORE_Network})
elseif(NOT ANDROID)
    target_link_libraries(lunaone_core PRIVATE CURL::libcurl)
endif()

if(LUNACORE_BUILD_CLI)
    add_executable(lunaone app/cli/main.cpp)
    target_compile_definitions(lunaone PRIVATE LUNACORE_VERSION="${PROJECT_VERSION}")
    if(MINGW)
        target_link_options(lunaone PRIVATE -municode)
    endif()
    if(WIN32)
        set_target_properties(lunaone PROPERTIES OUTPUT_NAME LunaONE-cli)
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:lunaone>/LunaONE-CLI.cmd"
            INPUT "${CMAKE_CURRENT_SOURCE_DIR}/app/cli/LunaONE-CLI.cmd"
            NEWLINE_STYLE CRLF)
    endif()
    target_link_libraries(lunaone PRIVATE CURL::libcurl)
    if(NOT LUNACORE_BUILD_MACOS_PLAYER)
        target_link_libraries(lunaone PRIVATE LunaONE::Core)
    endif()
endif()

if(LUNACORE_BUILD_WINDOWS_APP)
    # SDL2 reads the controllers: Xbox, PlayStation, Nintendo and 8BitDo, over
    # USB or Bluetooth. Only its controller parts are built, linked statically.
    function(lunaone_windows_sdl2)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(SDL_SHARED OFF)
        set(SDL_STATIC ON)
        set(SDL_TEST OFF)
        set(SDL2_DISABLE_SDL2MAIN ON)
        set(SDL2_DISABLE_INSTALL ON)
        set(SDL2_DISABLE_UNINSTALL ON)
        # waveOut plays the sound and Direct2D draws the windows.
        set(SDL_AUDIO OFF)
        set(SDL_VIDEO OFF)
        set(SDL_RENDER OFF)
        set(SDL_HAPTIC OFF)
        set(SDL_POWER OFF)
        FetchContent_Declare(sdl2
            URL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl2/SDL2-2.32.10.tar.gz"
            URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(sdl2)
    endfunction()
    lunaone_windows_sdl2()

    enable_language(RC)
    add_executable(lunaone_windows WIN32 app/windows/main.cpp app/windows/player.cpp app/windows/artwork.cpp
        app/windows/video_decoder.cpp app/windows/LunaONE.rc)
    set_target_properties(lunaone_windows PROPERTIES OUTPUT_NAME LunaONE)
    set_property(SOURCE app/windows/LunaONE.rc APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/windows/LunaONE.manifest;${CMAKE_CURRENT_SOURCE_DIR}/app/resources/LunaONE.ico")
    target_include_directories(lunaone_windows PRIVATE app/windows)
    target_compile_definitions(lunaone_windows PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 SDL_MAIN_HANDLED)
    target_link_libraries(lunaone_windows PRIVATE lunaone_stream opus CURL::libcurl Threads::Threads SDL2::SDL2-static
        comctl32 shell32 gdi32 user32 ole32 uuid mfuuid wmcodecdspuuid winmm iphlpapi ws2_32 windowscodecs d2d1 dwrite)
    # The library shows the logo from LunaONE.png beside the executable.
    add_custom_command(TARGET lunaone_windows POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/LunaONE.png" "$<TARGET_FILE_DIR:lunaone_windows>/LunaONE.png")
    if(MINGW)
        target_link_options(lunaone_windows PRIVATE -municode)
    elseif(MSVC)
        target_compile_options(lunaone_windows PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
        target_link_options(lunaone_windows PRIVATE /MANIFEST:NO)
    endif()
endif()

if(LUNACORE_BUILD_STREAM OR LUNACORE_BUILD_MACOS_PLAYER OR LUNACORE_BUILD_IPADOS_APP OR LUNACORE_BUILD_WINDOWS_APP OR LUNACORE_BUILD_ANDROID_APP)
    add_library(lunaone_stream STATIC app/src/webrtc/luna_stream_session.cpp)
    add_library(LunaONE::Stream ALIAS lunaone_stream)
    target_link_libraries(lunaone_stream PUBLIC LunaONE::Core PRIVATE lunaone_peer Threads::Threads opus)
    if(WIN32)
        # timeBeginPeriod keeps the network loop's 1 ms waits at 1 ms.
        target_link_libraries(lunaone_stream PRIVATE winmm)
    endif()
endif()

if(LUNACORE_BUILD_ANDROID_APP)
    add_library(lunaone_android SHARED
        app/android/src/main/cpp/native_bridge.cpp
        app/android/src/main/cpp/player.cpp)
    target_link_libraries(lunaone_android PRIVATE LunaONE::Stream opus android mediandk aaudio)
endif()

if(lunaone_gtk_target)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LUNAONE_GTK REQUIRED IMPORTED_TARGET gtk+-3.0 libavcodec libavutil libswscale sdl2)
    add_executable(${lunaone_gtk_target} app/linux/main.cpp app/linux/player.cpp app/linux/platform.cpp)
    set_target_properties(${lunaone_gtk_target} PROPERTIES OUTPUT_NAME LunaONE)
    target_link_libraries(${lunaone_gtk_target} PRIVATE LunaONE::Stream opus CURL::libcurl PkgConfig::LUNAONE_GTK)
    add_custom_command(TARGET ${lunaone_gtk_target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/LunaONE.png" "$<TARGET_FILE_DIR:${lunaone_gtk_target}>/LunaONE.png")
endif()

if(LUNACORE_BUILD_FREEBSD_APP)
    install(TARGETS lunaone_freebsd RUNTIME DESTINATION . COMPONENT LunaONE)
    if(LUNACORE_BUILD_CLI)
        install(TARGETS lunaone RUNTIME DESTINATION . COMPONENT LunaONE)
    endif()
    install(FILES app/resources/LunaONE.png LICENSE NOTICE THIRD_PARTY_NOTICES.md DESTINATION . COMPONENT LunaONE)
    install(DIRECTORY licenses DESTINATION . COMPONENT LunaONE)
    set(CPACK_GENERATOR TGZ)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};LunaONE;/")
    set(CPACK_PACKAGE_FILE_NAME "LunaONE-V${PROJECT_VERSION}-FreeBSD-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_STRIP_FILES ON)
    include(CPack)
endif()

if(LUNACORE_BUILD_MACOS_PLAYER OR LUNACORE_BUILD_IPADOS_APP)
    add_library(lunaone_apple_media STATIC
        app/apple/playback_support.mm
        app/apple/video_decoder.mm
        app/apple/metal_video_renderer.mm
        app/apple/audio_output.cpp)
    set_property(SOURCE app/apple/playback_support.mm app/apple/video_decoder.mm app/apple/metal_video_renderer.mm
        APPEND PROPERTY COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(lunaone_apple_media PUBLIC lunaone_stream opus)
    target_include_directories(lunaone_apple_media PUBLIC app/apple)
    foreach(framework IN ITEMS AVFoundation AudioToolbox CoreMedia CoreVideo GameController Metal Network QuartzCore VideoToolbox)
        find_library(LUNACORE_${framework} ${framework} REQUIRED)
        target_link_libraries(lunaone_apple_media PUBLIC "${LUNACORE_${framework}}")
    endforeach()
    if(lunaone_is_ios)
        find_library(LUNACORE_CoreTelephony CoreTelephony REQUIRED)
        target_link_libraries(lunaone_apple_media PUBLIC "${LUNACORE_CoreTelephony}")
    else()
        find_library(LUNACORE_IOKit IOKit REQUIRED)
        target_link_libraries(lunaone_apple_media PUBLIC "${LUNACORE_IOKit}")
    endif()
endif()

if(LUNACORE_BUILD_MACOS_PLAYER)
    add_library(lunaone_macos_player STATIC app/apple/macos_player.mm app/apple/macos_window.mm)
    target_compile_options(lunaone_macos_player PRIVATE -fobjc-arc)
    find_library(LUNACORE_AppKit AppKit REQUIRED)
    target_link_libraries(lunaone_macos_player PUBLIC lunaone_apple_media PRIVATE ${LUNACORE_AppKit})
    target_include_directories(lunaone_macos_player PUBLIC app)
    target_compile_definitions(lunaone PRIVATE LUNACORE_MACOS_PLAYER)
    target_include_directories(lunaone PRIVATE app)
    target_link_libraries(lunaone PRIVATE lunaone_macos_player)
    set(player_info "${CMAKE_CURRENT_SOURCE_DIR}/app/apple/PlayerInfo.plist")
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(lunaone PROPERTIES
            XCODE_ATTRIBUTE_INFOPLIST_FILE "${player_info}"
            XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
            XCODE_ATTRIBUTE_CREATE_INFOPLIST_SECTION_IN_BINARY YES)
    else()
        target_link_options(lunaone PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${player_info}")
    endif()
    set_property(TARGET lunaone APPEND PROPERTY LINK_DEPENDS "${player_info}")
    add_custom_command(TARGET lunaone POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/LunaONE.png" "$<TARGET_FILE_DIR:lunaone>/LunaONE.png")
    add_executable(lunaone_macos MACOSX_BUNDLE app/apple/macos_catalog.mm app/resources/LunaONE.icns)
    set_source_files_properties(app/resources/LunaONE.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(lunaone_macos PROPERTIES
        OUTPUT_NAME LunaONE
        MACOSX_BUNDLE_GUI_IDENTIFIER app.lunaone.macos
        MACOSX_BUNDLE_BUNDLE_NAME LunaONE
        MACOSX_BUNDLE_ICON_FILE LunaONE.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    target_compile_options(lunaone_macos PRIVATE -fobjc-arc)
    find_library(LUNAONE_QuartzCore QuartzCore REQUIRED)
    target_link_libraries(lunaone_macos PRIVATE lunaone_macos_player CURL::libcurl ${LUNACORE_AppKit} ${LUNAONE_QuartzCore})
endif()

if(LUNACORE_BUILD_IPADOS_APP)
    set(LUNACORE_APPLE_TEAM_ID "" CACHE STRING "Apple Development team for the iPadOS target")
    set(LUNACORE_IPADOS_BUNDLE_ID "app.lunacore.ipados" CACHE STRING "Development bundle identifier")
    find_library(LUNACORE_UIKit UIKit REQUIRED)
    find_library(LUNACORE_CoreGraphics CoreGraphics REQUIRED)
    add_executable(LunaONE-iOS MACOSX_BUNDLE
        app/ipados/main.mm
        app/ipados/catalog_controller.mm
        app/ipados/player_controller.mm
        app/ipados/watch_mirror.mm
        app/ipados/Assets.xcassets
        app/resources/LunaONE.png)
    set_source_files_properties(app/ipados/Assets.xcassets app/resources/LunaONE.png PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(LunaONE-iOS PROPERTIES
        OUTPUT_NAME "LunaONE"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/ipados/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${LUNACORE_IPADOS_BUNDLE_ID}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "12"
        XCODE_GENERATE_SCHEME ON
        XCODE_SCHEME_LAUNCH_CONFIGURATION RelWithDebInfo
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LUNACORE_APPLE_TEAM_ID}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LUNACORE_IPADOS_BUNDLE_ID}"
        XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST NO)
    target_compile_options(LunaONE-iOS PRIVATE -fobjc-arc)
    find_library(LUNAONE_QuartzCore QuartzCore REQUIRED)
    find_library(LUNAONE_WatchConnectivity WatchConnectivity REQUIRED)
    find_library(LUNAONE_CoreImage CoreImage REQUIRED)
    find_library(LUNAONE_ImageIO ImageIO REQUIRED)
    target_link_libraries(LunaONE-iOS PRIVATE lunaone_apple_media ${LUNACORE_UIKit} ${LUNACORE_CoreGraphics} ${LUNAONE_QuartzCore}
        ${LUNAONE_WatchConnectivity} ${LUNAONE_CoreImage} ${LUNAONE_ImageIO})

    # Apple Watch mirror: the game played on the iPhone, shown on the paired
    # watch. Needs the watchOS platform in Xcode; embedded in the iPhone app.
    option(LUNACORE_BUILD_WATCH_APP "Embed the Apple Watch mirror in the iPhone app" OFF)
    if(LUNACORE_BUILD_WATCH_APP)
        enable_language(Swift)
        set(lunaone_watch_dir "${CMAKE_BINARY_DIR}/watch")
        add_executable(LunaONE-Watch MACOSX_BUNDLE
            app/watchos/LunaONEWatchApp.swift
            app/watchos/MirrorSession.swift
            app/watchos/MirrorView.swift
            app/watchos/Assets.xcassets)
        set_source_files_properties(app/watchos/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(LunaONE-Watch PROPERTIES
            OUTPUT_NAME "LunaONEWatch"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/watchos/Info.plist.in"
            XCODE_ATTRIBUTE_SDKROOT watchos
            XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "watchos watchsimulator"
            XCODE_ATTRIBUTE_WATCHOS_DEPLOYMENT_TARGET 10.0
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 4
            XCODE_ATTRIBUTE_ARCHS "$(ARCHS_STANDARD)"
            XCODE_ATTRIBUTE_SKIP_INSTALL YES
            XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LUNACORE_APPLE_TEAM_ID}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LUNACORE_IPADOS_BUNDLE_ID}.watchkitapp"
            XCODE_ATTRIBUTE_CONFIGURATION_BUILD_DIR "${lunaone_watch_dir}/$(CONFIGURATION)")
        add_dependencies(LunaONE-iOS LunaONE-Watch)
        # Into LunaONE.app/Watch before Xcode signs the iPhone app.
        add_custom_command(TARGET LunaONE-iOS POST_BUILD
            COMMAND rm -rf "$<TARGET_BUNDLE_DIR:LunaONE-iOS>/Watch"
            COMMAND mkdir -p "$<TARGET_BUNDLE_DIR:LunaONE-iOS>/Watch"
            COMMAND ditto "${lunaone_watch_dir}/$<CONFIG>/LunaONEWatch.app"
                    "$<TARGET_BUNDLE_DIR:LunaONE-iOS>/Watch/LunaONEWatch.app"
            COMMENT "Embedding the Apple Watch app")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
