if(NOT DEFINED ENGINE OR ENGINE STREQUAL "")
    message(FATAL_ERROR "ENGINE must name the xrPhoton executable")
endif()
if(NOT DEFINED TEST_DIR OR TEST_DIR STREQUAL "")
    message(FATAL_ERROR "TEST_DIR must name a dedicated capture-proof directory")
endif()
if(NOT DEFINED BINARY_DIR OR BINARY_DIR STREQUAL "")
    message(FATAL_ERROR "BINARY_DIR must name the configured build directory")
endif()

# This path is configured inside the binary tree. Pin recursive cleanup to the
# dedicated leaf even if somebody invokes the script manually with bad arguments.
cmake_path(ABSOLUTE_PATH TEST_DIR NORMALIZE OUTPUT_VARIABLE normalized_test_dir)
cmake_path(ABSOLUTE_PATH BINARY_DIR NORMALIZE OUTPUT_VARIABLE normalized_binary_dir)
cmake_path(GET normalized_test_dir FILENAME test_dir_name)
cmake_path(GET normalized_test_dir PARENT_PATH test_dir_parent)
if(NOT test_dir_name STREQUAL "capture-proof"
    OR NOT test_dir_parent STREQUAL normalized_binary_dir)
    message(FATAL_ERROR
        "TEST_DIR must be the capture-proof leaf directly under BINARY_DIR")
endif()
set(TEST_DIR "${normalized_test_dir}")

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_capture label output_path)
    execute_process(
        COMMAND "${ENGINE}" --capture 8 "${output_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 60
    )
    file(WRITE "${TEST_DIR}/${label}.log" "${stdout}${stderr}")

    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label}: capture failed with ${result}; see ${TEST_DIR}/${label}.log")
    endif()

    string(REGEX MATCH
        "Capture start: extent=([0-9]+x[0-9]+) requestedFrames=8"
        start_line
        "${stdout}")
    if(start_line STREQUAL "")
        message(FATAL_ERROR "${label}: missing capture start extent")
    endif()
    set(start_extent "${CMAKE_MATCH_1}")

    string(REGEX MATCH
        "Capture complete: extent=([0-9]+x[0-9]+) successfulFrames=8 frameIndex=7 hash=0x([0-9a-f]+)"
        summary
        "${stdout}")
    if(summary STREQUAL "")
        message(FATAL_ERROR "${label}: missing successful capture summary")
    endif()
    set(final_extent "${CMAKE_MATCH_1}")
    set(hash "${CMAKE_MATCH_2}")

    string(REGEX MATCH
        "traceMedianMs=([0-9]+[.][0-9]+) traceSamples=8 traceTiming=diagnostic"
        timing_summary
        "${stdout}")
    if(timing_summary STREQUAL "")
        message(FATAL_ERROR "${label}: missing diagnostic trace timing summary")
    endif()

    if(NOT start_extent STREQUAL final_extent)
        message(FATAL_ERROR
            "${label}: extent changed from ${start_extent} to ${final_extent}")
    endif()
    string(LENGTH "${hash}" hash_length)
    if(NOT hash_length EQUAL 16)
        message(FATAL_ERROR "${label}: hash is not 16 lowercase hexadecimal digits")
    endif()

    if(NOT EXISTS "${output_path}")
        message(FATAL_ERROR "${label}: PPM was not created")
    endif()
    file(SIZE "${output_path}" output_size)
    if(output_size EQUAL 0)
        message(FATAL_ERROR "${label}: PPM is empty")
    endif()

    set("${label}_extent" "${final_extent}" PARENT_SCOPE)
    set("${label}_hash" "${hash}" PARENT_SCOPE)
endfunction()

set(first_output "${TEST_DIR}/capture-a.ppm")
set(second_output "${TEST_DIR}/capture-b.ppm")
run_capture(first "${first_output}")
run_capture(second "${second_output}")

if(NOT first_extent STREQUAL second_extent)
    message(FATAL_ERROR
        "independent captures changed extent: ${first_extent} vs ${second_extent}")
endif()
if(NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR
        "independent captures changed hash: ${first_hash} vs ${second_hash}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${first_output}" "${second_output}"
    RESULT_VARIABLE compare_result
)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "independent capture PPM files are not byte-identical")
endif()

# An existing directory cannot be opened as a regular PPM file. This deliberately
# reaches publication only after one successful render/readback, proving the runtime
# propagates checked output failure instead of reporting a completed capture.
execute_process(
    COMMAND "${ENGINE}" --capture 1 "${TEST_DIR}"
    RESULT_VARIABLE unwritable_result
    OUTPUT_VARIABLE unwritable_stdout
    ERROR_VARIABLE unwritable_stderr
    TIMEOUT 60
)
file(WRITE
    "${TEST_DIR}/unwritable.log"
    "${unwritable_stdout}${unwritable_stderr}")
if(unwritable_result EQUAL 0)
    message(FATAL_ERROR "directory output path unexpectedly succeeded")
endif()
string(CONCAT unwritable_log "${unwritable_stdout}" "${unwritable_stderr}")
if(NOT unwritable_log MATCHES "Failed to publish capture PPM")
    message(FATAL_ERROR "directory output failure did not reach checked publication")
endif()

message(STATUS
    "Capture baseline: extent=${first_extent} frameIndex=7 hash=0x${first_hash}")
