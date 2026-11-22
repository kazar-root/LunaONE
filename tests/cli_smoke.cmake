cmake_minimum_required(VERSION 3.25)

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(state "${CMAKE_CURRENT_BINARY_DIR}/cli-smoke-${suffix}")

function(check_cli expected pattern)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "LUNAONE_DATA_DIR=${state}" "${CLI}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result STREQUAL "${expected}" OR NOT "${output}\n${error}" MATCHES "${pattern}")
        file(REMOVE_RECURSE "${state}")
        message(FATAL_ERROR "lunaone ${ARGN} returned ${result}\n${output}\n${error}")
    endif()
endfunction()

check_cli(0 "Usage: lunaone" --help)
check_cli(0 "LunaNX 3\\.1" --version)
check_cli(0 "fr[\t]France" markets)
check_cli(2 "Unknown market" login invalid-market)
check_cli(1 "No saved Amazon device" catalog)
check_cli(1 "No saved Amazon device" catalog --all)
check_cli(1 "No saved Amazon device" search "Alan Wake 2")
check_cli(2 "Usage: lunaone" search)
# A new installation follows the device: 1080p and the Maximum mode on a Mac.
check_cli(0 "Automatic · 1080p.*25 Mbps.*Maximum quality.*60 FPS" settings)
check_cli(0 "1080p.*25 Mbps.*Maximum quality.*60 FPS" settings 1080 25 max 60)
check_cli(2 "Choose auto, stable or max" settings 1080 25 clear 60)
check_cli(0 "1080p.*15 Mbps.*Stable.*30 FPS" settings 1080 15 smooth 30)
check_cli(0 "Bitrate mode" settings --list)
check_cli(0 "^1080p.*15 Mbps.*Stable.*30 FPS" settings)
check_cli(0 "Swap A/B" settings --list)
if(PLAYER)
    check_cli(2 "Seconds must be between" play "Example Game" --seconds 0)
    check_cli(1 "No saved Amazon device" play "Example Game")
endif()

# An invalid stale account file must never initiate a second registration.
file(WRITE "${state}/amazon_device.json" "{}")
check_cli(1 "device file already exists" login fr)
check_cli(0 "Local account data removed" logout)
if(EXISTS "${state}/amazon_device.json")
    file(REMOVE_RECURSE "${state}")
    message(FATAL_ERROR "logout left the local account file")
endif()
file(REMOVE_RECURSE "${state}")
