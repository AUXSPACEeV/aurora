# Aurora Tasks

add_library(aurora_tasks
        freertos_scheduling.c
        watchdog_service.c
        )

target_link_libraries(aurora_tasks
        FreeRTOS-Kernel
        pico_stdlib
        hardware_watchdog
        )

target_include_directories(aurora_tasks PRIVATE
        ${AURORA_INCLUDES}
)
