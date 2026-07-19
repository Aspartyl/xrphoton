foreach(required_variable ASSET_COMPILER FIXTURE_TOOL TEST_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

# This path is configured by CMake inside its binary tree. Keep cleanup pinned to
# the test-only leaf even if this script is invoked manually with bad arguments.
if(NOT TEST_DIR MATCHES "/blender-cli$")
    message(FATAL_ERROR "TEST_DIR must end in /blender-cli")
endif()

function(run_success label)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label} failed with exit ${result}\nstdout: ${output}\nstderr: ${error}")
    endif()
endfunction()

function(run_failure_with_input label input_file)
    execute_process(
        COMMAND ${ARGN}
        INPUT_FILE "${input_file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
set(source "${TEST_DIR}/source.xrbm")
set(first "${TEST_DIR}/piped.ogfx")
set(second "${TEST_DIR}/from-input-file.ogfx")

# Exercise the actual no-intermediate Blender path: the fixture's binary stdout
# is connected directly to the asset compiler's stdin by execute_process.
execute_process(
    COMMAND "${FIXTURE_TOOL}" --write-cli-fixture -
    COMMAND "${ASSET_COMPILER}" convert-blender
        "synthetic.blend::object test_asymmetric" "${first}"
    RESULT_VARIABLE piped_result
    OUTPUT_VARIABLE piped_output
    ERROR_VARIABLE piped_error)
if(NOT piped_result EQUAL 0)
    message(FATAL_ERROR
        "piped Blender conversion failed with exit ${piped_result}\n"
        "stdout: ${piped_output}\nstderr: ${piped_error}")
endif()

# Also pin ordinary binary stdin behavior independently of CMake's command
# pipeline, matching how a producer can provide a prebuilt diagnostic fixture.
run_success(
    "synthetic XRBM file generation"
    "${FIXTURE_TOOL}" --write-cli-fixture "${source}")
execute_process(
    COMMAND "${ASSET_COMPILER}" convert-blender
        "synthetic.blend::object test_asymmetric" "${second}"
    INPUT_FILE "${source}"
    RESULT_VARIABLE file_result
    OUTPUT_VARIABLE file_output
    ERROR_VARIABLE file_error)
if(NOT file_result EQUAL 0)
    message(FATAL_ERROR
        "INPUT_FILE Blender conversion failed with exit ${file_result}\n"
        "stdout: ${file_output}\nstderr: ${file_error}")
endif()

run_success(
    "Blender CLI output verification"
    "${FIXTURE_TOOL}" --verify-cli-outputs "${first}" "${second}")

# Parsing must complete before publication begins. A malformed stdin stream must
# leave a preexisting destination byte-for-byte untouched.
set(invalid "${TEST_DIR}/invalid.xrbm")
set(preserved "${TEST_DIR}/preserved.ogfx")
file(WRITE "${invalid}" "not an XRBM stream")
file(WRITE "${preserved}" "existing destination sentinel")
run_failure_with_input(
    "invalid Blender stream conversion"
    "${invalid}"
    "${ASSET_COMPILER}" convert-blender
    "invalid.blend::object broken" "${preserved}")
file(READ "${preserved}" preserved_contents)
if(NOT preserved_contents STREQUAL "existing destination sentinel")
    message(FATAL_ERROR "a failed Blender conversion changed its destination")
endif()

# Publication must never replace the running compiler, even when a producer
# supplies a valid stream and mistakenly selects that executable as output.
file(SHA256 "${ASSET_COMPILER}" compiler_hash_before)
run_failure_with_input(
    "asset-compiler output alias rejection"
    "${source}"
    "${ASSET_COMPILER}" convert-blender
    "synthetic.blend::object test_asymmetric" "${ASSET_COMPILER}")
file(SHA256 "${ASSET_COMPILER}" compiler_hash_after)
if(NOT compiler_hash_before STREQUAL compiler_hash_after)
    message(FATAL_ERROR "output alias rejection changed the asset compiler")
endif()

# A shell normally invokes installed tools by a bare PATH lookup, where argv[0]
# does not identify the executable relative to the working directory. The Linux
# /proc guard must still recognize and protect the running binary.
get_filename_component(compiler_directory "${ASSET_COMPILER}" DIRECTORY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${compiler_directory}:$ENV{PATH}"
        xrPhotonAssetCompiler convert-blender
        "synthetic.blend::object test_asymmetric" "${ASSET_COMPILER}"
    INPUT_FILE "${source}"
    WORKING_DIRECTORY "${TEST_DIR}"
    RESULT_VARIABLE path_alias_result
    OUTPUT_VARIABLE path_alias_output
    ERROR_VARIABLE path_alias_error)
if(path_alias_result EQUAL 0)
    message(FATAL_ERROR "bare-PATH compiler output alias unexpectedly succeeded")
endif()
file(SHA256 "${ASSET_COMPILER}" compiler_hash_after_path_alias)
if(NOT compiler_hash_before STREQUAL compiler_hash_after_path_alias)
    message(FATAL_ERROR "bare-PATH output alias rejection changed the asset compiler")
endif()

file(GLOB_RECURSE unexpected_temporaries
    LIST_DIRECTORIES false
    "${TEST_DIR}/*.tmp.*")
if(unexpected_temporaries)
    message(FATAL_ERROR "Blender CLI tests left unexpected temporary outputs")
endif()
