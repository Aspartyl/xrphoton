foreach(required_variable ASSET_COMPILER VERIFIER SOURCE_OGF TEXTURE_DDS OUTPUT_OGFX)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

set(first_output "${OUTPUT_OGFX}.proof-first")
set(second_output "${OUTPUT_OGFX}.proof-second")
file(REMOVE "${first_output}" "${second_output}")

if(NOT EXISTS "${SOURCE_OGF}")
    message(FATAL_ERROR
        "Alpha-tested tail source not found: ${SOURCE_OGF}\n"
        "Place item_psevdodog_tail.ogf in the documented ignored SoC source "
        "tree or configure XRPHOTON_ALPHA_TAIL_CORPUS_OGF.")
endif()

if(NOT EXISTS "${TEXTURE_DDS}")
    message(FATAL_ERROR
        "Alpha-tested tail texture not found: ${TEXTURE_DDS}\n"
        "Place act_pseudodog_fur.dds in the documented ignored SoC texture "
        "tree or configure XRPHOTON_ALPHA_TAIL_TEXTURE_DDS.")
endif()

file(SIZE "${SOURCE_OGF}" source_size)
file(SHA256 "${SOURCE_OGF}" source_hash)
set(expected_source_size 60922)
set(expected_source_hash
    68d204cf13c028ea0987dad37f834272010d47f1c659a783327008423f4f69ed)
if(NOT source_size EQUAL expected_source_size
    OR NOT source_hash STREQUAL expected_source_hash)
    message(FATAL_ERROR
        "Alpha-tested tail source identity mismatch: expected "
        "${expected_source_size} bytes / ${expected_source_hash}, found "
        "${source_size} bytes / ${source_hash}")
endif()

# The mesh compiler preserves a logical texture reference and does not ingest image
# bytes. Pin the external DDS here because the acceptance claim depends on the exact
# texture resolved by the gallery and sampled by any-hit.
file(SIZE "${TEXTURE_DDS}" texture_size)
file(SHA256 "${TEXTURE_DDS}" texture_hash)
set(expected_texture_size 21992)
set(expected_texture_hash
    c58f047a1b3c004de845d5d61de68b28fba2660558ac35222a07cf19b603d9bd)
if(NOT texture_size EQUAL expected_texture_size
    OR NOT texture_hash STREQUAL expected_texture_hash)
    message(FATAL_ERROR
        "Alpha-tested tail texture identity mismatch: expected "
        "${expected_texture_size} bytes / ${expected_texture_hash}, found "
        "${texture_size} bytes / ${texture_hash}")
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
            "Alpha-tested tail conversion failed with exit ${convert_result}\n"
            "stdout: ${convert_output}\nstderr: ${convert_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${first_output}" "${second_output}"
    RESULT_VARIABLE compare_result)
if(NOT compare_result EQUAL 0)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR
        "Two alpha-tested tail conversions were not byte-identical")
endif()

execute_process(
    COMMAND "${VERIFIER}" --verify-alpha-corpus-output
        "${SOURCE_OGF}" "${first_output}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR
        "Alpha-tested tail output verification failed with exit ${verify_result}\n"
        "stdout: ${verify_output}\nstderr: ${verify_error}")
endif()

file(SIZE "${first_output}" output_size)
file(SHA256 "${first_output}" output_hash)
set(expected_output_size 34921)
set(expected_output_hash
    b5fc918b3e5a9f11dcdf596360361824719999c850282e30ce0f6dd97b5fc0dd)
if(NOT output_size EQUAL expected_output_size
    OR NOT output_hash STREQUAL expected_output_hash)
    file(REMOVE "${first_output}" "${second_output}")
    message(FATAL_ERROR
        "Alpha-tested tail output identity mismatch: expected "
        "${expected_output_size} bytes / ${expected_output_hash}, found "
        "${output_size} bytes / ${output_hash}")
endif()

file(REMOVE "${second_output}")
file(RENAME "${first_output}" "${OUTPUT_OGFX}" RESULT publish_result)
if(NOT publish_result STREQUAL "0")
    file(REMOVE "${first_output}")
    message(FATAL_ERROR
        "Alpha-tested tail proof succeeded but publishing its output failed: "
        "${publish_result}")
endif()

message(STATUS
    "Alpha-tested tail offline proof passed: ${output_size} bytes, SHA-256 "
    "${output_hash}")
message(STATUS "Persistent proof output: ${OUTPUT_OGFX}")
