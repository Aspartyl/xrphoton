foreach(required_variable ASSET_COMPILER VERIFIER SOURCE_OGF OUTPUT_OGFX)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

set(candidate_output "${OUTPUT_OGFX}.proof-candidate")
file(REMOVE "${candidate_output}")

if(NOT EXISTS "${SOURCE_OGF}")
    message(FATAL_ERROR
        "M4a corpus source not found: ${SOURCE_OGF}\n"
        "Place plitka1.ogf in the documented legacy OGF corpus path or configure "
        "XRPHOTON_M4A_CORPUS_OGF.")
endif()

file(SIZE "${SOURCE_OGF}" source_size)
file(SHA256 "${SOURCE_OGF}" source_hash)
set(expected_source_size 64392)
set(expected_source_hash
    fca71a14064c7453fb6b940949e2c407d8613cf4b22fac2cd0fafc2ca43d21be)
if(NOT source_size EQUAL expected_source_size
    OR NOT source_hash STREQUAL expected_source_hash)
    message(FATAL_ERROR
        "M4a corpus identity mismatch: expected ${expected_source_size} bytes / "
        "${expected_source_hash}, found ${source_size} bytes / ${source_hash}")
endif()

execute_process(
    COMMAND "${ASSET_COMPILER}" convert-ogf "${SOURCE_OGF}" "${candidate_output}"
    RESULT_VARIABLE convert_result
    OUTPUT_VARIABLE convert_output
    ERROR_VARIABLE convert_error)
if(NOT convert_result EQUAL 0)
    file(REMOVE "${candidate_output}")
    message(FATAL_ERROR
        "M4a conversion failed with exit ${convert_result}\n"
        "stdout: ${convert_output}\nstderr: ${convert_error}")
endif()

execute_process(
    COMMAND "${VERIFIER}" --verify-corpus-output
        "${SOURCE_OGF}" "${candidate_output}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
    file(REMOVE "${candidate_output}")
    message(FATAL_ERROR
        "M4a output verification failed with exit ${verify_result}\n"
        "stdout: ${verify_output}\nstderr: ${verify_error}")
endif()

file(SIZE "${candidate_output}" output_size)
file(SHA256 "${candidate_output}" output_hash)
set(expected_output_size 71328)
set(expected_output_hash
    cdeb620332585767ed4cf04aeebd46e508df1fdbfbba3d630ffc0b32727feca0)
if(NOT output_size EQUAL expected_output_size
    OR NOT output_hash STREQUAL expected_output_hash)
    file(REMOVE "${candidate_output}")
    message(FATAL_ERROR
        "M4a output identity mismatch: expected ${expected_output_size} bytes / "
        "${expected_output_hash}, found ${output_size} bytes / ${output_hash}")
endif()

file(RENAME "${candidate_output}" "${OUTPUT_OGFX}" RESULT publish_result)
if(NOT publish_result STREQUAL "0")
    file(REMOVE "${candidate_output}")
    message(FATAL_ERROR
        "M4a proof succeeded but publishing its output failed: ${publish_result}")
endif()

message(STATUS
    "M4a offline proof passed: ${output_size} bytes, SHA-256 ${output_hash}")
message(STATUS "Persistent proof output: ${OUTPUT_OGFX}")
