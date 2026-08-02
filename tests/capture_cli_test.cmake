if(NOT DEFINED ENGINE OR ENGINE STREQUAL "")
    message(FATAL_ERROR "ENGINE must name the xrPhoton executable")
endif()

function(expect_capture_parse_failure label)
    execute_process(
        COMMAND "${ENGINE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: malformed capture command unexpectedly succeeded")
    endif()

    string(CONCAT combined "${stdout}" "${stderr}")
    if(NOT combined MATCHES "Capture frame count must be a positive 32-bit integer")
        message(FATAL_ERROR "${label}: missing capture-count diagnostic:\n${combined}")
    endif()
    if(combined MATCHES "Initialized GLFW")
        message(FATAL_ERROR "${label}: malformed CLI reached GLFW initialization")
    endif()
endfunction()

function(expect_spp_parse_failure label)
    execute_process(
        COMMAND "${ENGINE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: malformed SPP command unexpectedly succeeded")
    endif()

    string(CONCAT combined "${stdout}" "${stderr}")
    if(NOT combined MATCHES "Samples per pixel must be 1, 2, 4, 8, or 16")
        message(FATAL_ERROR "${label}: missing SPP diagnostic:\n${combined}")
    endif()
    if(combined MATCHES "Initialized GLFW")
        message(FATAL_ERROR "${label}: malformed CLI reached GLFW initialization")
    endif()
endfunction()

expect_capture_parse_failure(zero_count --capture 0 ignored.ppm)
expect_capture_parse_failure(invalid_count --capture eight ignored.ppm)
expect_spp_parse_failure(unsupported_spp --spp 3)
