# This is a copy of <FREERTOS_KERNEL_PATH>/portable/ThirdParty/GCC/RP2040/FREERTOS_KERNEL_import.cmake

# This can be dropped into an external project to help locate the FreeRTOS kernel
# It should be include()ed prior to project(). Alternatively this file may
# or the CMakeLists.txt in this directory may be included or added via add_subdirectory
# respectively.

if (DEFINED ENV{FREERTOS_KERNEL_PATH} AND (NOT FREERTOS_KERNEL_PATH))
    set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
    message("Using FREERTOS_KERNEL_PATH from environment ('${FREERTOS_KERNEL_PATH}')")
endif ()

if (NOT FREERTOS_KERNEL_PATH)
    set (FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_LIST_DIR}/kernel")
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is undefined.")
endif ()

if (NOT FREERTOS_KERNEL_PATH)
    if (PICO_SDK_PATH)
        set(FREERTOS_KERNEL_PATH ${PICO_SDK_PATH}/../FreeRTOS-Kernel)
        message("Defaulting FREERTOS_KERNEL_PATH as sibling of PICO_SDK_PATH: ${FREERTOS_KERNEL_PATH}")
    endif ()
endif ()

if (NOT FREERTOS_KERNEL_PATH)
    message(FATAL_ERROR "FreeRTOS location was not specified. Please set FREERTOS_KERNEL_PATH.")
endif()

set(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH}" CACHE PATH "Path to the FreeRTOS Kernel")

get_filename_component(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if (NOT EXISTS ${FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "Directory '${FREERTOS_KERNEL_PATH}' not found")
endif()

set(FREERTOS_KERNEL_RP2040_RELATIVE_PATH "portable/ThirdParty/GCC/RP2040")
set(FREERTOS_KERNEL_RP2040_ABSOLUTE_PATH ${FREERTOS_KERNEL_PATH}/${FREERTOS_KERNEL_RP2040_RELATIVE_PATH})

if (NOT EXISTS ${FREERTOS_KERNEL_RP2040_ABSOLUTE_PATH}/CMakeLists.txt)
    message(FATAL_ERROR "Directory '${FREERTOS_KERNEL_PATH}' does not contain an RP2040 port here: ${FREERTOS_KERNEL_RP2040_RELATIVE_PATH}")
endif()
set(FREERTOS_KERNEL_PATH ${FREERTOS_KERNEL_PATH} CACHE PATH "Path to the FreeRTOS_KERNEL" FORCE)

add_subdirectory(${FREERTOS_KERNEL_RP2040_ABSOLUTE_PATH} FREERTOS_KERNEL)