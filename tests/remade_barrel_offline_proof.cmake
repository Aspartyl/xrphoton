cmake_minimum_required(VERSION 3.24)

foreach(required_variable
        BLENDER_EXECUTABLE EXPORT_SCRIPT ASSET_COMPILER VERIFIER
        SOURCE_BLEND TEXTURE_ROOT TEXTURE_DDS OUTPUT_OGFX)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

set(candidate_1 "${OUTPUT_OGFX}.proof-candidate-1")
set(candidate_2 "${OUTPUT_OGFX}.proof-candidate-2")

function(cleanup_candidates)
    file(REMOVE "${candidate_1}" "${candidate_2}")
endfunction()

function(proof_failure)
    string(CONCAT diagnostic ${ARGV})
    cleanup_candidates()
    message(FATAL_ERROR "${diagnostic}")
endfunction()

set(mutable_paths "${OUTPUT_OGFX}" "${candidate_1}" "${candidate_2}")
set(mutable_paths_absolute)
foreach(mutable_path IN LISTS mutable_paths)
    cmake_path(ABSOLUTE_PATH mutable_path NORMALIZE
        OUTPUT_VARIABLE mutable_path_absolute)
    list(APPEND mutable_paths_absolute "${mutable_path_absolute}")
endforeach()
set(unique_mutable_paths ${mutable_paths_absolute})
list(REMOVE_DUPLICATES unique_mutable_paths)
list(LENGTH mutable_paths_absolute mutable_path_count)
list(LENGTH unique_mutable_paths unique_mutable_path_count)
if(NOT mutable_path_count EQUAL unique_mutable_path_count)
    message(FATAL_ERROR
        "remade-barrel proof output and candidates must identify different paths")
endif()

foreach(input_variable
        BLENDER_EXECUTABLE EXPORT_SCRIPT ASSET_COMPILER VERIFIER
        SOURCE_BLEND TEXTURE_ROOT TEXTURE_DDS)
    set(input_path "${${input_variable}}")
    cmake_path(ABSOLUTE_PATH input_path NORMALIZE
        OUTPUT_VARIABLE input_path_absolute)
    if(input_path_absolute IN_LIST mutable_paths_absolute)
        message(FATAL_ERROR
            "${input_variable} aliases a remade-barrel proof output: "
            "${${input_variable}}")
    endif()
endforeach()

foreach(input_variable
        BLENDER_EXECUTABLE EXPORT_SCRIPT ASSET_COMPILER VERIFIER
        SOURCE_BLEND TEXTURE_DDS)
    if(NOT EXISTS "${${input_variable}}" OR IS_DIRECTORY "${${input_variable}}")
        proof_failure(
            "${input_variable} must identify an existing file: "
            "${${input_variable}}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${TEXTURE_ROOT}")
    proof_failure(
        "TEXTURE_ROOT must identify an existing directory: ${TEXTURE_ROOT}")
endif()
if(IS_DIRECTORY "${OUTPUT_OGFX}")
    proof_failure("OUTPUT_OGFX must not identify a directory: ${OUTPUT_OGFX}")
endif()

set(expected_texture_path
    "${TEXTURE_ROOT}/xrphoton/remade_bochka_close_1_basecolor.dds")
if(NOT EXISTS "${expected_texture_path}" OR IS_DIRECTORY "${expected_texture_path}")
    proof_failure(
        "texture root does not resolve the remade barrel DDS: "
        "${expected_texture_path}")
endif()
file(REAL_PATH "${expected_texture_path}" expected_texture_real)
file(REAL_PATH "${TEXTURE_DDS}" supplied_texture_real)
if(NOT expected_texture_real STREQUAL supplied_texture_real)
    proof_failure(
        "TEXTURE_DDS must be the file resolved by TEXTURE_ROOT: expected "
        "${expected_texture_real}, found ${supplied_texture_real}")
endif()

execute_process(
    COMMAND "${BLENDER_EXECUTABLE}" --version
    RESULT_VARIABLE blender_version_result
    OUTPUT_VARIABLE blender_version_output
    ERROR_VARIABLE blender_version_error)
if(NOT "${blender_version_result}" STREQUAL "0")
    proof_failure(
        "Blender version probe failed with exit ${blender_version_result}\n"
        "stdout: ${blender_version_output}\nstderr: ${blender_version_error}")
endif()
if(NOT blender_version_output MATCHES "^Blender 5\\.([1-9]|[1-9][0-9])")
    proof_failure(
        "remade-barrel proof requires Blender 5.1 or newer within major 5; "
        "found:\n${blender_version_output}")
endif()

file(SIZE "${TEXTURE_DDS}" texture_size)
file(SHA256 "${TEXTURE_DDS}" texture_hash)
set(expected_texture_size 1398248)
set(expected_texture_hash
    4affd371f0bdfde9ddc188735e930deee7f0b72c0a920083e700eec1e7d9307a)
if(NOT texture_size EQUAL expected_texture_size
    OR NOT texture_hash STREQUAL expected_texture_hash)
    proof_failure(
        "remade barrel DDS identity mismatch: expected ${expected_texture_size} "
        "bytes / SHA-256 ${expected_texture_hash}, found ${texture_size} bytes / "
        "SHA-256 ${texture_hash}")
endif()
execute_process(
    COMMAND "${VERIFIER}" --verify-remade-barrel-texture "${TEXTURE_DDS}"
    RESULT_VARIABLE texture_verify_result
    OUTPUT_VARIABLE texture_verify_output
    ERROR_VARIABLE texture_verify_error)
if(NOT "${texture_verify_result}" STREQUAL "0")
    proof_failure(
        "remade barrel DDS verification failed with exit ${texture_verify_result}\n"
        "stdout: ${texture_verify_output}\nstderr: ${texture_verify_error}")
endif()

cleanup_candidates()

function(export_candidate label candidate_path)
    execute_process(
        COMMAND "${BLENDER_EXECUTABLE}"
            --background
            --factory-startup
            --disable-autoexec
            --python-exit-code 1
            "${SOURCE_BLEND}"
            --python "${EXPORT_SCRIPT}"
            --
            --compiler "${ASSET_COMPILER}"
            --output "${candidate_path}"
            --object remade_bochka_close_1
            --texture-root "${TEXTURE_ROOT}"
        RESULT_VARIABLE export_result
        OUTPUT_VARIABLE export_output
        ERROR_VARIABLE export_error)
    if(NOT "${export_result}" STREQUAL "0")
        proof_failure(
            "${label} failed with exit ${export_result}\n"
            "stdout: ${export_output}\nstderr: ${export_error}")
    endif()
    if(NOT EXISTS "${candidate_path}" OR IS_DIRECTORY "${candidate_path}")
        proof_failure("${label} did not create its output candidate")
    endif()
endfunction()

export_candidate("remade barrel first export" "${candidate_1}")
export_candidate("remade barrel second export" "${candidate_2}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${candidate_1}" "${candidate_2}"
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_output
    ERROR_VARIABLE compare_error)
if(NOT "${compare_result}" STREQUAL "0")
    proof_failure(
        "remade barrel exports were not byte-identical\n"
        "stdout: ${compare_output}\nstderr: ${compare_error}")
endif()

execute_process(
    COMMAND "${VERIFIER}" --verify-remade-barrel-output "${candidate_1}"
    RESULT_VARIABLE output_verify_result
    OUTPUT_VARIABLE output_verify_output
    ERROR_VARIABLE output_verify_error)
if(NOT "${output_verify_result}" STREQUAL "0")
    proof_failure(
        "remade barrel output verification failed with exit "
        "${output_verify_result}\nstdout: ${output_verify_output}\n"
        "stderr: ${output_verify_error}")
endif()

file(SIZE "${candidate_1}" output_size)
file(SHA256 "${candidate_1}" output_hash)
set(expected_output_size 459984)
set(expected_output_hash
    1e55ea5794b9bfeb879fff05b028a701dec9e0b57c3e9e77b4f2c8cc69ace688)
if(NOT output_size EQUAL expected_output_size
    OR NOT output_hash STREQUAL expected_output_hash)
    proof_failure(
        "remade barrel OGFx identity mismatch: expected ${expected_output_size} "
        "bytes / SHA-256 ${expected_output_hash}, found ${output_size} bytes / "
        "SHA-256 ${output_hash}")
endif()

file(REMOVE "${candidate_2}")
file(RENAME "${candidate_1}" "${OUTPUT_OGFX}" RESULT publish_result)
if(NOT publish_result STREQUAL "0")
    proof_failure(
        "remade barrel proof succeeded but publishing failed: ${publish_result}")
endif()

message(STATUS
    "Remade barrel offline proof passed: ${output_size} bytes / SHA-256 "
    "${output_hash}; DDS ${texture_size} bytes / SHA-256 ${texture_hash}")
