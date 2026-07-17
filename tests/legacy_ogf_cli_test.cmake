foreach(required_variable ASSET_COMPILER FIXTURE_TOOL TEST_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not supplied")
    endif()
endforeach()

# This path is configured by CMake inside its binary tree. Keep cleanup pinned to
# the test-only leaf even if this script is invoked manually with bad arguments.
if(NOT TEST_DIR MATCHES "/legacy-ogf-cli$")
    message(FATAL_ERROR "TEST_DIR must end in /legacy-ogf-cli")
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

function(run_failure label)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded")
    endif()
endfunction()

file(REMOVE_RECURSE "${TEST_DIR}")
set(source "${TEST_DIR}/source.ogf")
set(first "${TEST_DIR}/first.ogfx")
set(second "${TEST_DIR}/created/by-cli/second.ogfx")

run_success(
    "synthetic fixture generation"
    "${FIXTURE_TOOL}" --write-cli-fixture "${source}")

# A preexisting adjacent name must neither be followed nor truncated. Exclusive
# creation makes the converter choose the next candidate and leaves this sentinel.
set(collision "${first}.tmp.0")
file(WRITE "${collision}" "preexisting temporary sentinel")
run_success(
    "first legacy conversion"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${first}")
file(READ "${collision}" collision_contents)
if(NOT collision_contents STREQUAL "preexisting temporary sentinel")
    message(FATAL_ERROR "the converter changed a temporary-name collision")
endif()
file(REMOVE "${collision}")

# Exhausting every candidate must fail without claiming or deleting the final
# preexisting name (a default-constructed stream alone does not prove open state).
set(exhausted_output "${TEST_DIR}/exhausted.ogfx")
foreach(attempt RANGE 0 255)
    file(WRITE "${exhausted_output}.tmp.${attempt}" "collision ${attempt}")
endforeach()
run_failure(
    "temporary-name exhaustion"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${exhausted_output}")
file(READ "${exhausted_output}.tmp.255" final_collision_contents)
if(NOT final_collision_contents STREQUAL "collision 255")
    message(FATAL_ERROR "temporary-name exhaustion changed its final collision")
endif()
file(GLOB exhausted_candidates "${exhausted_output}.tmp.*")
list(LENGTH exhausted_candidates exhausted_candidate_count)
if(NOT exhausted_candidate_count EQUAL 256)
    message(FATAL_ERROR "temporary-name exhaustion removed a preexisting file")
endif()
file(REMOVE ${exhausted_candidates})

# The second destination's parent does not exist; the CLI owns creating it.
run_success(
    "second legacy conversion"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${second}")
run_success(
    "CLI output verification"
    "${FIXTURE_TOOL}" --verify-cli-outputs "${first}" "${second}")

# Replacing the first output must remain canonical and deterministic.
run_success(
    "existing-output replacement"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${first}")
run_success(
    "replacement verification"
    "${FIXTURE_TOOL}" --verify-cli-outputs "${first}" "${second}")

# A parse failure cannot damage an existing destination.
set(sentinel_output "${TEST_DIR}/preserved.ogfx")
file(WRITE "${sentinel_output}" "existing destination sentinel")
run_failure(
    "invalid-source conversion"
    "${ASSET_COMPILER}" convert-ogf "${first}" "${sentinel_output}")
file(READ "${sentinel_output}" sentinel_contents)
if(NOT sentinel_contents STREQUAL "existing destination sentinel")
    message(FATAL_ERROR "a failed conversion changed its destination")
endif()

# Force publication to fail by making the destination an existing directory;
# the converter must remove only the temporary file it created.
set(blocked_output "${TEST_DIR}/blocked.ogfx")
file(MAKE_DIRECTORY "${blocked_output}")
run_failure(
    "blocked publication"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${blocked_output}")
file(GLOB blocked_temporaries "${blocked_output}.tmp.*")
if(blocked_temporaries)
    message(FATAL_ERROR "failed publication left a temporary output")
endif()

file(SHA256 "${source}" source_hash_before)
run_failure(
    "source/destination alias rejection"
    "${ASSET_COMPILER}" convert-ogf "${source}" "${source}")
file(SHA256 "${source}" source_hash_after)
if(NOT source_hash_before STREQUAL source_hash_after)
    message(FATAL_ERROR "alias rejection changed the source file")
endif()

run_failure(
    "unknown asset-compiler command"
    "${ASSET_COMPILER}" unknown-command "${source}" "${first}")

file(GLOB_RECURSE unexpected_temporaries
    LIST_DIRECTORIES false
    "${TEST_DIR}/*.tmp.*")
if(unexpected_temporaries)
    message(FATAL_ERROR "CLI tests left unexpected temporary outputs")
endif()
