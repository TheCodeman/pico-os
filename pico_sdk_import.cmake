# Locates the Pico SDK from PICO_SDK_PATH (CMake variable or environment).
# The VS Code Raspberry Pi Pico extension sets this up automatically.
if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()
# Else the newest SDK the VS Code extension installed (~/.pico-sdk/sdk/<version>).
if (NOT PICO_SDK_PATH AND DEFINED ENV{HOME})
    file(GLOB _pico_sdks LIST_DIRECTORIES true "$ENV{HOME}/.pico-sdk/sdk/*")
    if (_pico_sdks)
        list(SORT _pico_sdks COMPARE NATURAL ORDER DESCENDING)
        list(GET _pico_sdks 0 PICO_SDK_PATH)
        message(STATUS "PICO_SDK_PATH not set: using ${PICO_SDK_PATH}")
        # and its picotool, as the extension would set it
        get_filename_component(_pico_ver ${PICO_SDK_PATH} NAME)
        if (NOT DEFINED picotool_DIR AND EXISTS "$ENV{HOME}/.pico-sdk/picotool/${_pico_ver}/picotool")
            set(picotool_DIR "$ENV{HOME}/.pico-sdk/picotool/${_pico_ver}/picotool")
        endif ()
        # GCC 14.2, which the SDK is built and tested with (15.2 rejects stack_switch.c's asm)
        if (NOT DEFINED PICO_TOOLCHAIN_PATH AND NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND
            EXISTS "$ENV{HOME}/.pico-sdk/toolchain/14_2_Rel1")
            set(PICO_TOOLCHAIN_PATH "$ENV{HOME}/.pico-sdk/toolchain/14_2_Rel1")
        endif ()
    endif ()
endif ()
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
if (NOT PICO_SDK_PATH OR NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Pico SDK 2.1 or newer checkout")
endif ()
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
