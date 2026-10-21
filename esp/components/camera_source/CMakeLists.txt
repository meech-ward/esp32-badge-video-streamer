idf_component_register(SRCS "camera_source.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__esp32-camera protocol
                       PRIV_REQUIRES esp_timer esp_driver_i2c freertos log)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(camera_library espressif__esp32-camera COMPONENT_LIB)
    idf_component_get_property(camera_directory espressif__esp32-camera COMPONENT_DIR)
    idf_build_get_property(camera_python PYTHON)

    function(streamer_stage_camera_source original staged)
        execute_process(COMMAND "${camera_python}" "${CMAKE_CURRENT_LIST_DIR}/prepare_driver.py"
            "${original}" "${staged}" ${ARGN}
            RESULT_VARIABLE camera_prepare_result)
        if(NOT camera_prepare_result EQUAL 0)
            message(FATAL_ERROR "Camera driver staging failed")
        endif()
        get_target_property(camera_sources ${camera_library} SOURCES)
        set(camera_replacements 0)
        set(camera_staged_sources)
        foreach(source IN LISTS camera_sources)
            if(source STREQUAL original)
                list(APPEND camera_staged_sources "${staged}")
                math(EXPR camera_replacements "${camera_replacements} + 1")
            else()
                list(APPEND camera_staged_sources "${source}")
            endif()
        endforeach()
        if(NOT camera_replacements EQUAL 1)
            message(FATAL_ERROR "Camera component source list changed; review driver staging")
        endif()
        set_property(TARGET ${camera_library} PROPERTY SOURCES "${camera_staged_sources}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${original}" "${CMAKE_CURRENT_LIST_DIR}/prepare_driver.py")
    endfunction()

    # Every profile needs the explicit wait budget, including the two-slot
    # fallback. Preserve the upstream public API and its four-second default.
    target_include_directories(${camera_library} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    streamer_stage_camera_source("${camera_directory}/driver/esp_camera.c"
        "${CMAKE_CURRENT_BINARY_DIR}/camera_driver/esp_camera.c" --timed-getter)

    # Every profile must trim at the first structural JPEG endpoint. With two
    # buffers the one-entry queue is already the upstream queue depth.
    streamer_stage_camera_source("${camera_directory}/driver/cam_hal.c"
        "${CMAKE_CURRENT_BINARY_DIR}/camera_driver/cam_hal.c")
endif()
