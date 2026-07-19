foreach(required_variable ASSET_COMPILER VERIFIER SOURCE_OGF OUTPUT_OGFX)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

set(first_output "${OUTPUT_OGFX}.proof-first")
set(second_output "${OUTPUT_OGFX}.proof-second")
file(REMOVE "${first_output}" "${second_output}")

if(NOT EXISTS "${SOURCE_OGF}")
    message(FATAL_ERROR
        "Rigid barrel source not found: ${SOURCE_OGF}\n"
        "Place bochka_close_1.ogf in the documented ignored SoC source tree or "
        "configure XRPHOTON_RIGID_BARREL_CORPUS_OGF.")
endif()

file(SIZE "${SOURCE_OGF}" source_size)
file(SHA256 "${SOURCE_OGF}" source_hash)
set(expected_source_size 29710)
set(expected_source_hash
    87be6a577756af252496be56d29d4c50c9ebdba9443e9d57f22af212fa1af33f)
if(NOT source_size EQUAL expected_source_size
    OR NOT source_hash STREQUAL expected_source_hash)
    message(FATAL_ERROR
        "Rigid barrel source identity mismatch: expected ${expected_source_size} "
        "bytes / ${expected_source_hash}, found ${source_size} bytes / ${source_hash}")
endif()

foreach(candidate IN ITEMS "${first_output}" "${second_output}")
    execute_process(
        COMMAND "${ASSET_COMPILER}" convert-ogf "${SOURCE_OGF}" "${candidate}"
        RESULT_VARIABLE convert_result
        OUTPUT_VARIABLE convert_output
        ERROR_VARIABLE convert_error)
    if(NOT convert_result EQUAL 0)
        file(REMOVE "${first_output}" "${second_output}")
        message(FATAL_ERROR
            "Rigid barrel conversion failed with exit ${convert_result}\n"
            "stdout: ${convert_output}\nstderr: ${convert_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${first_output}" "${second_output}"
    RESULT_VARIABLE compare_result)
if(NOT compare_result EQUAL 0)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR "Two rigid barrel conversions were not byte-identical")
endif()

execute_process(
    COMMAND "${VERIFIER}" --verify-rigid-corpus-output
        "${SOURCE_OGF}" "${first_output}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR
        "Rigid barrel output verification failed with exit ${verify_result}\n"
        "stdout: ${verify_output}\nstderr: ${verify_error}")
endif()

file(SIZE "${first_output}" output_size)
file(SHA256 "${first_output}" output_hash)
set(expected_output_size 19352)
set(expected_output_hash
    eed1c06c5d975199ae96fe49517f8893e164cf5e93ce1a040421c7cb0e115060)
if(NOT output_size EQUAL expected_output_size
    OR NOT output_hash STREQUAL expected_output_hash)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR
        "Rigid barrel output identity mismatch: expected ${expected_output_size} "
        "bytes / ${expected_output_hash}, found ${output_size} bytes / ${output_hash}")
endif()

file(REMOVE "${second_output}")
file(RENAME "${first_output}" "${OUTPUT_OGFX}" RESULT publish_result)
if(NOT publish_result STREQUAL "0")
    file(REMOVE "${first_output}")
    message(FATAL_ERROR
        "Rigid barrel proof succeeded but publishing its output failed: ${publish_result}")
endif()

message(STATUS
    "Rigid barrel offline proof passed: ${output_size} bytes, SHA-256 ${output_hash}")
message(STATUS "Persistent proof output: ${OUTPUT_OGFX}")
