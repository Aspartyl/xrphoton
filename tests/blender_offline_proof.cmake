cmake_minimum_required(VERSION 3.24)

set(required_variables
    BLENDER_EXECUTABLE
    EXPORT_SCRIPT
    ASSET_COMPILER
    VERIFIER
    PYRAMID_BLEND
    PYRAMID_OUTPUT
    SPHERE_BLEND
    SPHERE_OUTPUT
    SMOOTH_SPHERE_BLEND
    SMOOTH_SPHERE_OUTPUT)
foreach(required_variable IN LISTS required_variables)
    if(NOT DEFINED ${required_variable}
        OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

# Every proof temporary is adjacent to its eventual destination. This makes the
# final rename atomic and keeps cleanup both deterministic and narrowly scoped.
set(pyramid_candidate_1 "${PYRAMID_OUTPUT}.proof-candidate-1")
set(pyramid_candidate_2 "${PYRAMID_OUTPUT}.proof-candidate-2")
set(sphere_candidate_1 "${SPHERE_OUTPUT}.proof-candidate-1")
set(sphere_candidate_2 "${SPHERE_OUTPUT}.proof-candidate-2")
set(smooth_sphere_candidate_1
    "${SMOOTH_SPHERE_OUTPUT}.proof-candidate-1")
set(smooth_sphere_candidate_2
    "${SMOOTH_SPHERE_OUTPUT}.proof-candidate-2")

function(cleanup_proof_candidates)
    file(REMOVE
        "${pyramid_candidate_1}"
        "${pyramid_candidate_2}"
        "${sphere_candidate_1}"
        "${sphere_candidate_2}"
        "${smooth_sphere_candidate_1}"
        "${smooth_sphere_candidate_2}")
endfunction()

function(proof_failure)
    string(CONCAT diagnostic ${ARGV})
    cleanup_proof_candidates()
    message(FATAL_ERROR "${diagnostic}")
endfunction()

# Reject colliding mutable paths before deleting any stale proof candidates.
cmake_path(ABSOLUTE_PATH PYRAMID_OUTPUT NORMALIZE
    OUTPUT_VARIABLE pyramid_output_absolute)
cmake_path(ABSOLUTE_PATH SPHERE_OUTPUT NORMALIZE
    OUTPUT_VARIABLE sphere_output_absolute)
cmake_path(ABSOLUTE_PATH SMOOTH_SPHERE_OUTPUT NORMALIZE
    OUTPUT_VARIABLE smooth_sphere_output_absolute)
set(candidate_path "${pyramid_candidate_1}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE pyramid_candidate_1_absolute)
set(candidate_path "${pyramid_candidate_2}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE pyramid_candidate_2_absolute)
set(candidate_path "${sphere_candidate_1}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE sphere_candidate_1_absolute)
set(candidate_path "${sphere_candidate_2}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE sphere_candidate_2_absolute)
set(candidate_path "${smooth_sphere_candidate_1}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE smooth_sphere_candidate_1_absolute)
set(candidate_path "${smooth_sphere_candidate_2}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE smooth_sphere_candidate_2_absolute)
set(mutable_paths
    "${pyramid_output_absolute}"
    "${sphere_output_absolute}"
    "${smooth_sphere_output_absolute}"
    "${pyramid_candidate_1_absolute}"
    "${pyramid_candidate_2_absolute}"
    "${sphere_candidate_1_absolute}"
    "${sphere_candidate_2_absolute}"
    "${smooth_sphere_candidate_1_absolute}"
    "${smooth_sphere_candidate_2_absolute}")
set(unique_mutable_paths ${mutable_paths})
list(REMOVE_DUPLICATES unique_mutable_paths)
list(LENGTH mutable_paths mutable_path_count)
list(LENGTH unique_mutable_paths unique_mutable_path_count)
if(NOT mutable_path_count EQUAL unique_mutable_path_count)
    message(FATAL_ERROR
        "Blender proof outputs and their candidate suffixes must identify "
        "different paths")
endif()

foreach(input_variable
        BLENDER_EXECUTABLE EXPORT_SCRIPT ASSET_COMPILER VERIFIER
        PYRAMID_BLEND SPHERE_BLEND SMOOTH_SPHERE_BLEND)
    set(input_path "${${input_variable}}")
    cmake_path(ABSOLUTE_PATH input_path NORMALIZE
        OUTPUT_VARIABLE input_path_absolute)
    if(input_path_absolute IN_LIST mutable_paths)
        message(FATAL_ERROR
            "${input_variable} aliases a Blender proof output or candidate: "
            "${${input_variable}}")
    endif()
endforeach()

foreach(input_variable
        BLENDER_EXECUTABLE EXPORT_SCRIPT ASSET_COMPILER VERIFIER
        PYRAMID_BLEND SPHERE_BLEND SMOOTH_SPHERE_BLEND)
    if(NOT EXISTS "${${input_variable}}")
        proof_failure(
            "${input_variable} does not exist: ${${input_variable}}")
    endif()
    if(IS_DIRECTORY "${${input_variable}}")
        proof_failure(
            "${input_variable} must identify a file: ${${input_variable}}")
    endif()
endforeach()

foreach(output_variable PYRAMID_OUTPUT SPHERE_OUTPUT SMOOTH_SPHERE_OUTPUT)
    if(IS_DIRECTORY "${${output_variable}}")
        proof_failure(
            "${output_variable} must not identify a directory: ${${output_variable}}")
    endif()
endforeach()

# Probe the binary before starting exports so a misconfigured executable or
# unsupported Blender generation fails without disturbing prior proof outputs.
execute_process(
    COMMAND "${BLENDER_EXECUTABLE}" --version
    RESULT_VARIABLE blender_version_result
    OUTPUT_VARIABLE blender_version_output
    ERROR_VARIABLE blender_version_error)
if(NOT "${blender_version_result}" STREQUAL "0")
    proof_failure(
        "BLENDER_EXECUTABLE failed its version probe with exit "
        "${blender_version_result}\nstdout: ${blender_version_output}\n"
        "stderr: ${blender_version_error}")
endif()
string(REGEX MATCH
    "^Blender ([0-9]+)\\.([0-9]+)(\\.([0-9]+))?"
    blender_version_match
    "${blender_version_output}")
set(blender_major "${CMAKE_MATCH_1}")
set(blender_minor "${CMAKE_MATCH_2}")
if(blender_version_match STREQUAL ""
    OR NOT blender_major STREQUAL "5"
    OR blender_minor LESS 1)
    proof_failure(
        "Blender offline proof requires Blender 5.1 or newer within major "
        "version 5; version output was:\n${blender_version_output}")
endif()

cleanup_proof_candidates()

function(export_candidate asset_label blend_path object_name candidate_path)
    execute_process(
        COMMAND "${BLENDER_EXECUTABLE}"
            --background
            --factory-startup
            --disable-autoexec
            --python-exit-code 1
            "${blend_path}"
            --python "${EXPORT_SCRIPT}"
            --
            --compiler "${ASSET_COMPILER}"
            --output "${candidate_path}"
            --object "${object_name}"
        RESULT_VARIABLE export_result
        OUTPUT_VARIABLE export_output
        ERROR_VARIABLE export_error)
    if(NOT "${export_result}" STREQUAL "0")
        proof_failure(
            "${asset_label} Blender export failed with exit ${export_result}\n"
            "stdout: ${export_output}\nstderr: ${export_error}")
    endif()
    if(NOT EXISTS "${candidate_path}" OR IS_DIRECTORY "${candidate_path}")
        proof_failure(
            "${asset_label} Blender export reported success without creating "
            "its candidate: ${candidate_path}")
    endif()
    file(SIZE "${candidate_path}" candidate_size)
    if(candidate_size EQUAL 0)
        proof_failure(
            "${asset_label} Blender export created an empty candidate: "
            "${candidate_path}")
    endif()
endfunction()

export_candidate(
    "test_pyramid first pass"
    "${PYRAMID_BLEND}"
    "test_pyramid"
    "${pyramid_candidate_1}")
export_candidate(
    "test_pyramid second pass"
    "${PYRAMID_BLEND}"
    "test_pyramid"
    "${pyramid_candidate_2}")
export_candidate(
    "test_sphere first pass"
    "${SPHERE_BLEND}"
    "test_sphere"
    "${sphere_candidate_1}")
export_candidate(
    "test_sphere second pass"
    "${SPHERE_BLEND}"
    "test_sphere"
    "${sphere_candidate_2}")
export_candidate(
    "test_smooth_sphere first pass"
    "${SMOOTH_SPHERE_BLEND}"
    "test_smooth_sphere"
    "${smooth_sphere_candidate_1}")
export_candidate(
    "test_smooth_sphere second pass"
    "${SMOOTH_SPHERE_BLEND}"
    "test_smooth_sphere"
    "${smooth_sphere_candidate_2}")

function(require_identical asset_label first_candidate second_candidate)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${first_candidate}" "${second_candidate}"
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_output
        ERROR_VARIABLE compare_error)
    if(NOT "${compare_result}" STREQUAL "0")
        proof_failure(
            "${asset_label} Blender exports were not byte-identical\n"
            "stdout: ${compare_output}\nstderr: ${compare_error}")
    endif()
endfunction()

require_identical(
    "test_pyramid" "${pyramid_candidate_1}" "${pyramid_candidate_2}")
require_identical(
    "test_sphere" "${sphere_candidate_1}" "${sphere_candidate_2}")
require_identical(
    "test_smooth_sphere"
    "${smooth_sphere_candidate_1}"
    "${smooth_sphere_candidate_2}")

function(verify_candidate asset_label verifier_flag candidate_path)
    execute_process(
        COMMAND "${VERIFIER}" "${verifier_flag}" "${candidate_path}"
        RESULT_VARIABLE verify_result
        OUTPUT_VARIABLE verify_output
        ERROR_VARIABLE verify_error)
    if(NOT "${verify_result}" STREQUAL "0")
        proof_failure(
            "${asset_label} output verification failed with exit "
            "${verify_result}\nstdout: ${verify_output}\n"
            "stderr: ${verify_error}")
    endif()
endfunction()

verify_candidate(
    "test_pyramid"
    "--verify-pyramid-output"
    "${pyramid_candidate_1}")
verify_candidate(
    "test_sphere"
    "--verify-sphere-output"
    "${sphere_candidate_1}")
verify_candidate(
    "test_smooth_sphere"
    "--verify-smooth-sphere-output"
    "${smooth_sphere_candidate_1}")

execute_process(
    COMMAND "${VERIFIER}" --verify-sphere-pair
        "${sphere_candidate_1}" "${smooth_sphere_candidate_1}"
    RESULT_VARIABLE sphere_pair_result
    OUTPUT_VARIABLE sphere_pair_output
    ERROR_VARIABLE sphere_pair_error)
if(NOT "${sphere_pair_result}" STREQUAL "0")
    proof_failure(
        "flat/smooth sphere comparison failed with exit ${sphere_pair_result}\n"
        "stdout: ${sphere_pair_output}\nstderr: ${sphere_pair_error}")
endif()

file(SIZE "${pyramid_candidate_1}" pyramid_size)
file(SHA256 "${pyramid_candidate_1}" pyramid_hash)
file(SIZE "${sphere_candidate_1}" sphere_size)
file(SHA256 "${sphere_candidate_1}" sphere_hash)
file(SIZE "${smooth_sphere_candidate_1}" smooth_sphere_size)
file(SHA256 "${smooth_sphere_candidate_1}" smooth_sphere_hash)

# The duplicate candidates are no longer needed. Each remaining candidate is
# replaced into its destination with one same-directory atomic rename.
file(REMOVE
    "${pyramid_candidate_2}"
    "${sphere_candidate_2}"
    "${smooth_sphere_candidate_2}")
file(RENAME
    "${pyramid_candidate_1}" "${PYRAMID_OUTPUT}"
    RESULT pyramid_publish_result)
if(NOT pyramid_publish_result STREQUAL "0")
    proof_failure(
        "test_pyramid proof succeeded but publishing its output failed: "
        "${pyramid_publish_result}")
endif()
file(RENAME
    "${sphere_candidate_1}" "${SPHERE_OUTPUT}"
    RESULT sphere_publish_result)
if(NOT sphere_publish_result STREQUAL "0")
    proof_failure(
        "test_sphere proof succeeded but publishing its output failed: "
        "${sphere_publish_result}")
endif()
file(RENAME
    "${smooth_sphere_candidate_1}" "${SMOOTH_SPHERE_OUTPUT}"
    RESULT smooth_sphere_publish_result)
if(NOT smooth_sphere_publish_result STREQUAL "0")
    proof_failure(
        "test_smooth_sphere proof succeeded but publishing its output failed: "
        "${smooth_sphere_publish_result}")
endif()

message(STATUS
    "Blender offline proof passed: test_pyramid ${pyramid_size} bytes / "
    "SHA-256 ${pyramid_hash}")
message(STATUS
    "Blender offline proof passed: test_sphere ${sphere_size} bytes / "
    "SHA-256 ${sphere_hash}")
message(STATUS
    "Blender offline proof passed: test_smooth_sphere ${smooth_sphere_size} "
    "bytes / SHA-256 ${smooth_sphere_hash}")
message(STATUS
    "Persistent Blender proof outputs: ${PYRAMID_OUTPUT}; ${SPHERE_OUTPUT}; "
    "${SMOOTH_SPHERE_OUTPUT}")
