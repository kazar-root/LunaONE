function(lunaone_test name)
    add_executable(${name} "${name}_test.cpp")
    target_link_libraries(${name} PRIVATE LunaONE::Core)
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

lunaone_test(amazon_device_sign_in)
lunaone_test(catalog_artwork)
lunaone_test(luna_recent_catalog)
lunaone_test(menu_and_metrics)
lunaone_test(playback_interface)
lunaone_test(recent_games)
lunaone_test(search_match)
lunaone_test(luna_sdp)
lunaone_test(virtual_keyboard)
lunaone_test(library_scene)
lunaone_test(preferences_storage)

target_link_libraries(amazon_device_sign_in PRIVATE jansson)
target_link_libraries(luna_recent_catalog PRIVATE jansson)
target_link_libraries(recent_games PRIVATE jansson)

if(TARGET lunaone_peer)
    lunaone_test(peer_packet_headers)
    target_link_libraries(peer_packet_headers PRIVATE lunaone_peer)
endif()

if(TARGET lunaone)
    add_test(NAME cli_smoke
        COMMAND "${CMAKE_COMMAND}" -DCLI=$<TARGET_FILE:lunaone>
                -DPLAYER=${LUNACORE_BUILD_MACOS_PLAYER}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake")
endif()
