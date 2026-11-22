function(lunaone_add_mbedtls)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(ENABLE_TESTING OFF)
    set(ENABLE_PROGRAMS OFF)
    set(MBEDTLS_FATAL_WARNINGS OFF)

    set(sources "${PROJECT_SOURCE_DIR}/third_party/streaming")
    FetchContent_Declare(mbedtls
        URL "${sources}/mbedtls-3.6.7.tar.bz2"
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
    FetchContent_MakeAvailable(mbedtls)
    foreach(target IN ITEMS mbedtls mbedx509 mbedcrypto)
        target_compile_definitions(${target} PUBLIC MBEDTLS_USER_CONFIG_FILE="lunacore_dtls_config.h")
        target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${sources}>")
    endforeach()
endfunction()

lunaone_add_mbedtls()
