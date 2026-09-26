# SPDX-License-Identifier: Apache-2.0
#
# nn_osal — OS Abstraction Layer for the nn project.
#
# Apps and libraries depend on nn_osal headers + the active backend
# implementation.  Currently only the Zephyr backend ships; future
# backends (posix, freertos, baremetal) drop into src/<name>/ and are
# selected via CONFIG_NN_OSAL_BACKEND_*.

if(CONFIG_NN_OSAL)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(CONFIG_NN_OSAL_BACKEND_ZEPHYR)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/thread.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/sync.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/work.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/time.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/log.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/gpio.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/storage.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/system.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/socket.c
            # P6b — new surfaces
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/uart.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/watchdog.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/shell.c
        )
    endif()
endif()
