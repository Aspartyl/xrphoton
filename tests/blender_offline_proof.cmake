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
    SMOOTH_SPHERE_OUTPUT
    LEAF_CARD_BLEND
    LEAF_CARD_OUTPUT
    LEAF_TEXTURE_ROOT
    LEAF_TEXTURE_DDS)
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
set(leaf_card_candidate_1 "${LEAF_CARD_OUTPUT}.proof-candidate-1")
set(leaf_card_candidate_2 "${LEAF_CARD_OUTPUT}.proof-candidate-2")
get_filename_component(leaf_card_output_directory "${LEAF_CARD_OUTPUT}" DIRECTORY)
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef leaf_alias_nonce)
set(leaf_alias_texture_root
    "${leaf_card_output_directory}/.xrphoton-leaf-alias-${leaf_alias_nonce}")
set(leaf_alias_texture_dds
    "${leaf_alias_texture_root}/trees/trees_new_vetka_green.dds")
if(EXISTS "${leaf_alias_texture_root}")
    message(FATAL_ERROR
        "random Blender proof temporary already exists: ${leaf_alias_texture_root}")
endif()

function(cleanup_proof_candidates)
    file(REMOVE
        "${pyramid_candidate_1}"
        "${pyramid_candidate_2}"
        "${sphere_candidate_1}"
        "${sphere_candidate_2}"
        "${smooth_sphere_candidate_1}"
        "${smooth_sphere_candidate_2}"
        "${leaf_card_candidate_1}"
        "${leaf_card_candidate_2}")
    file(REMOVE_RECURSE "${leaf_alias_texture_root}")
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
cmake_path(ABSOLUTE_PATH LEAF_CARD_OUTPUT NORMALIZE
    OUTPUT_VARIABLE leaf_card_output_absolute)
set(alias_root_path "${leaf_alias_texture_root}")
cmake_path(ABSOLUTE_PATH alias_root_path NORMALIZE
    OUTPUT_VARIABLE leaf_alias_texture_root_absolute)
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
set(candidate_path "${leaf_card_candidate_1}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE leaf_card_candidate_1_absolute)
set(candidate_path "${leaf_card_candidate_2}")
cmake_path(ABSOLUTE_PATH candidate_path NORMALIZE
    OUTPUT_VARIABLE leaf_card_candidate_2_absolute)
set(mutable_paths
    "${pyramid_output_absolute}"
    "${sphere_output_absolute}"
    "${smooth_sphere_output_absolute}"
    "${leaf_card_output_absolute}"
    "${leaf_alias_texture_root_absolute}"
    "${pyramid_candidate_1_absolute}"
    "${pyramid_candidate_2_absolute}"
    "${sphere_candidate_1_absolute}"
    "${sphere_candidate_2_absolute}"
    "${smooth_sphere_candidate_1_absolute}"
    "${smooth_sphere_candidate_2_absolute}"
    "${leaf_card_candidate_1_absolute}"
    "${leaf_card_candidate_2_absolute}")
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
        PYRAMID_BLEND SPHERE_BLEND SMOOTH_SPHERE_BLEND LEAF_CARD_BLEND
        LEAF_TEXTURE_ROOT LEAF_TEXTURE_DDS)
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
        PYRAMID_BLEND SPHERE_BLEND SMOOTH_SPHERE_BLEND LEAF_CARD_BLEND
        LEAF_TEXTURE_DDS)
    if(NOT EXISTS "${${input_variable}}")
        proof_failure(
            "${input_variable} does not exist: ${${input_variable}}")
    endif()
    if(IS_DIRECTORY "${${input_variable}}")
        proof_failure(
            "${input_variable} must identify a file: ${${input_variable}}")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${LEAF_TEXTURE_ROOT}")
    proof_failure(
        "LEAF_TEXTURE_ROOT must identify an existing directory: "
        "${LEAF_TEXTURE_ROOT}")
endif()

# The verifier must pin the exact DDS from which the exporter derives the
# output's canonical logical reference, not a separately selected lookalike.
set(leaf_texture_resolved_path
    "${LEAF_TEXTURE_ROOT}/trees/trees_new_vetka_green.dds")
if(NOT EXISTS "${leaf_texture_resolved_path}"
    OR IS_DIRECTORY "${leaf_texture_resolved_path}")
    proof_failure(
        "the leaf texture root does not resolve the expected source DDS: "
        "${leaf_texture_resolved_path}")
endif()
file(REAL_PATH "${leaf_texture_resolved_path}" leaf_texture_resolved_real)
file(REAL_PATH "${LEAF_TEXTURE_DDS}" leaf_texture_dds_real)
if(NOT leaf_texture_resolved_real STREQUAL leaf_texture_dds_real)
    proof_failure(
        "LEAF_TEXTURE_DDS must be the same file resolved by LEAF_TEXTURE_ROOT: "
        "expected ${leaf_texture_resolved_real}, found ${leaf_texture_dds_real}")
endif()

foreach(output_variable
        PYRAMID_OUTPUT SPHERE_OUTPUT SMOOTH_SPHERE_OUTPUT LEAF_CARD_OUTPUT)
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

file(SIZE "${LEAF_TEXTURE_DDS}" leaf_texture_size)
file(SHA256 "${LEAF_TEXTURE_DDS}" leaf_texture_hash)
set(expected_leaf_texture_size 174904)
set(expected_leaf_texture_hash
    f6d6ad3e53890ed4614ad0b3c486a3196945bac9a27cee88ba71fc9e048985a5)
if(NOT leaf_texture_size EQUAL expected_leaf_texture_size
    OR NOT leaf_texture_hash STREQUAL expected_leaf_texture_hash)
    proof_failure(
        "leaf DDS identity mismatch: expected ${expected_leaf_texture_size} "
        "bytes / SHA-256 ${expected_leaf_texture_hash}, found "
        "${leaf_texture_size} bytes / SHA-256 ${leaf_texture_hash}")
endif()
execute_process(
    COMMAND "${VERIFIER}" --verify-leaf-texture "${LEAF_TEXTURE_DDS}"
    RESULT_VARIABLE leaf_texture_verify_result
    OUTPUT_VARIABLE leaf_texture_verify_output
    ERROR_VARIABLE leaf_texture_verify_error)
if(NOT "${leaf_texture_verify_result}" STREQUAL "0")
    proof_failure(
        "leaf DDS alpha verification failed with exit "
        "${leaf_texture_verify_result}\nstdout: ${leaf_texture_verify_output}\n"
        "stderr: ${leaf_texture_verify_error}")
endif()

cleanup_proof_candidates()

# A textured export gains another irreplaceable input. Exercise its output-alias
# guard against a disposable DDS copy so a regression cannot overwrite the
# owner-local source texture.
file(MAKE_DIRECTORY "${leaf_alias_texture_root}/trees")
file(COPY_FILE "${LEAF_TEXTURE_DDS}" "${leaf_alias_texture_dds}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "XRPHOTON_LEAF_ALIAS_PATH=${leaf_alias_texture_dds}"
        "${BLENDER_EXECUTABLE}"
        --background
        --factory-startup
        --disable-autoexec
        --python-exit-code 1
        "${LEAF_CARD_BLEND}"
        --python-expr
        "setattr(__import__('bpy').data.images['trees_new_vetka_green.dds'], 'filepath', __import__('os').environ['XRPHOTON_LEAF_ALIAS_PATH'])"
        --python "${EXPORT_SCRIPT}"
        --
        --compiler "${ASSET_COMPILER}"
        --output "${leaf_alias_texture_dds}"
        --object test_leaf_card
        --texture-root "${leaf_alias_texture_root}"
    RESULT_VARIABLE alias_rejection_result
    OUTPUT_VARIABLE alias_rejection_output
    ERROR_VARIABLE alias_rejection_error)
if("${alias_rejection_result}" STREQUAL "0")
    proof_failure(
        "leaf material-image output alias unexpectedly passed validation")
endif()
string(CONCAT alias_rejection_diagnostic
    "${alias_rejection_output}" "${alias_rejection_error}")
if(NOT alias_rejection_diagnostic MATCHES
    "output path must not identify the material image")
    proof_failure(
        "leaf material-image alias failed without the expected diagnostic:\n"
        "${alias_rejection_diagnostic}")
endif()
file(SIZE "${leaf_alias_texture_dds}" alias_texture_size)
file(SHA256 "${leaf_alias_texture_dds}" alias_texture_hash)
if(NOT alias_texture_size EQUAL expected_leaf_texture_size
    OR NOT alias_texture_hash STREQUAL expected_leaf_texture_hash)
    proof_failure(
        "leaf material-image alias rejection modified its protected input")
endif()
file(REMOVE_RECURSE "${leaf_alias_texture_root}")

function(require_rejected_leaf_mutation
        case_label python_expression diagnostic_pattern candidate_path)
    execute_process(
        COMMAND "${BLENDER_EXECUTABLE}"
            --background
            --factory-startup
            --disable-autoexec
            --python-exit-code 1
            "${LEAF_CARD_BLEND}"
            --python-expr "${python_expression}"
            --python "${EXPORT_SCRIPT}"
            --
            --compiler "${ASSET_COMPILER}"
            --output "${candidate_path}"
            --object test_leaf_card
            --texture-root "${LEAF_TEXTURE_ROOT}"
        RESULT_VARIABLE rejection_result
        OUTPUT_VARIABLE rejection_output
        ERROR_VARIABLE rejection_error)
    if("${rejection_result}" STREQUAL "0")
        proof_failure(
            "${case_label} unexpectedly passed Blender source validation")
    endif()
    if(EXISTS "${candidate_path}")
        proof_failure(
            "${case_label} rejection left an output candidate: ${candidate_path}")
    endif()
    string(CONCAT rejection_diagnostic
        "${rejection_output}" "${rejection_error}")
    if(NOT rejection_diagnostic MATCHES "${diagnostic_pattern}")
        proof_failure(
            "${case_label} failed without the expected diagnostic "
            "'${diagnostic_pattern}':\n${rejection_diagnostic}")
    endif()
endfunction()

# Blender can display a result that disagrees with the raw BC1 runtime unless
# the image interpretation is pinned. Prove both ambiguous settings fail before
# running the deterministic positive exports.
require_rejected_leaf_mutation(
    "leaf alpha-mode mutation"
    "setattr(__import__('bpy').data.images['trees_new_vetka_green.dds'], 'alpha_mode', 'NONE')"
    "material image alpha mode"
    "${leaf_card_candidate_1}")
require_rejected_leaf_mutation(
    "leaf color-space mutation"
    "setattr(__import__('bpy').data.images['trees_new_vetka_green.dds'].colorspace_settings, 'name', 'Linear Rec.709')"
    "material image color space"
    "${leaf_card_candidate_2}")
require_rejected_leaf_mutation(
    "leaf muted-image-node mutation"
    "setattr(next(node for node in __import__('bpy').data.materials['test_leaf_card_alpha_clip'].node_tree.nodes if node.bl_idname == 'ShaderNodeTexImage'), 'mute', True)"
    "material node mute state"
    "${leaf_card_candidate_2}")
require_rejected_leaf_mutation(
    "leaf texture-mapping mutation"
    "setattr(next(node for node in __import__('bpy').data.materials['test_leaf_card_alpha_clip'].node_tree.nodes if node.bl_idname == 'ShaderNodeTexImage').texture_mapping, 'translation', (0.25, 0.0, 0.0))"
    "material texture mapping"
    "${leaf_card_candidate_1}")
require_rejected_leaf_mutation(
    "leaf color-mapping mutation"
    "setattr(next(node for node in __import__('bpy').data.materials['test_leaf_card_alpha_clip'].node_tree.nodes if node.bl_idname == 'ShaderNodeTexImage').color_mapping, 'brightness', 2.0)"
    "material color mapping"
    "${leaf_card_candidate_2}")

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
            ${ARGN}
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
export_candidate(
    "test_leaf_card first pass"
    "${LEAF_CARD_BLEND}"
    "test_leaf_card"
    "${leaf_card_candidate_1}"
    --texture-root "${LEAF_TEXTURE_ROOT}")
export_candidate(
    "test_leaf_card second pass"
    "${LEAF_CARD_BLEND}"
    "test_leaf_card"
    "${leaf_card_candidate_2}"
    --texture-root "${LEAF_TEXTURE_ROOT}")

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
require_identical(
    "test_leaf_card" "${leaf_card_candidate_1}" "${leaf_card_candidate_2}")

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
verify_candidate(
    "test_leaf_card"
    "--verify-leaf-card-output"
    "${leaf_card_candidate_1}")

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
file(SIZE "${leaf_card_candidate_1}" leaf_card_size)
file(SHA256 "${leaf_card_candidate_1}" leaf_card_hash)

# The duplicate candidates are no longer needed. Each remaining candidate is
# replaced into its destination with one same-directory atomic rename.
file(REMOVE
    "${pyramid_candidate_2}"
    "${sphere_candidate_2}"
    "${smooth_sphere_candidate_2}"
    "${leaf_card_candidate_2}")
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
file(RENAME
    "${leaf_card_candidate_1}" "${LEAF_CARD_OUTPUT}"
    RESULT leaf_card_publish_result)
if(NOT leaf_card_publish_result STREQUAL "0")
    proof_failure(
        "test_leaf_card proof succeeded but publishing its output failed: "
        "${leaf_card_publish_result}")
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
    "Blender offline proof passed: test_leaf_card ${leaf_card_size} "
    "bytes / SHA-256 ${leaf_card_hash}; DDS mip 0 has 153894 transparent "
    "texels")
message(STATUS
    "Persistent Blender proof outputs: ${PYRAMID_OUTPUT}; ${SPHERE_OUTPUT}; "
    "${SMOOTH_SPHERE_OUTPUT}; ${LEAF_CARD_OUTPUT}")
