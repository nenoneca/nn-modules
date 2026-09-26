# SPDX-License-Identifier: Apache-2.0
#
# nn_pal — Platform Abstraction Layer for protocol subsystems.
# Sister to nn_osal — wraps BLE / DFU / OpenThread / WiFi / mDNS so
# apps + libs don't reach into Zephyr (or any other vendor SDK)
# protocol stacks directly.
#
# Currently only the Zephyr backend ships under src/zephyr/.  Future
# backends (NimBLE on ESP-IDF, etc.) drop into src/<name>/ and are
# selected via CONFIG_NN_PAL_BACKEND_*.

if(CONFIG_NN_PAL)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(CONFIG_NN_PAL_BACKEND_ZEPHYR)
        if(CONFIG_NN_PAL_MDNS)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/mdns.c)
        endif()
        if(CONFIG_NN_PAL_WIFI)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/wifi.c)
        endif()
        if(CONFIG_NN_PAL_OPENTHREAD)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/openthread.c)
        endif()
        if(CONFIG_NN_PAL_DFU)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/dfu.c)
        endif()
        if(CONFIG_NN_PAL_BLE)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/ble.c)
        endif()
        if(CONFIG_NN_PAL_LOG_SINK)
            target_sources(app PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/log_sink.c)
        endif()
    endif()
endif()
