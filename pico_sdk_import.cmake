# Locates the Pico SDK from PICO_SDK_PATH (CMake variable or environment).
# The VS Code Raspberry Pi Pico extension sets this up automatically.
if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
if (NOT PICO_SDK_PATH OR NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Pico SDK 2.1 or newer checkout")
endif ()
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
