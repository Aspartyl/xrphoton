if(NOT DEFINED ENGINE OR ENGINE STREQUAL "")
    message(FATAL_ERROR "ENGINE must name the xrPhoton executable")
endif()
if(NOT DEFINED COMPARATOR OR COMPARATOR STREQUAL "")
    message(FATAL_ERROR "COMPARATOR must name xrPhotonReferenceCompare")
endif()
if(NOT DEFINED TEST_DIR OR TEST_DIR STREQUAL "")
    message(FATAL_ERROR "TEST_DIR must name the reference-proof directory")
endif()
if(NOT DEFINED BINARY_DIR OR BINARY_DIR STREQUAL "")
    message(FATAL_ERROR "BINARY_DIR must name the configured build directory")
endif()
if(NOT DEFINED SAMPLE_COUNT OR SAMPLE_COUNT STREQUAL "")
    set(SAMPLE_COUNT 256)
endif()

# Constrain recursive cleanup to the dedicated build-tree leaf, matching the ordinary
# capture proof's publication-directory contract.
cmake_path(ABSOLUTE_PATH TEST_DIR NORMALIZE OUTPUT_VARIABLE normalized_test_dir)
cmake_path(ABSOLUTE_PATH BINARY_DIR NORMALIZE OUTPUT_VARIABLE normalized_binary_dir)
cmake_path(GET normalized_test_dir FILENAME test_dir_name)
cmake_path(GET normalized_test_dir PARENT_PATH test_dir_parent)
if(NOT test_dir_name STREQUAL "reference-proof"
    OR NOT test_dir_parent STREQUAL normalized_binary_dir)
    message(FATAL_ERROR
        "TEST_DIR must be the reference-proof leaf directly under BINARY_DIR")
endif()
set(TEST_DIR "${normalized_test_dir}")

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
foreach(estimator IN ITEMS mis nee bsdf)
    execute_process(
        COMMAND "${ENGINE}" --reference "${SAMPLE_COUNT}"
            --estimator "${estimator}" --time 0
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 900
    )
    file(WRITE "${TEST_DIR}/${estimator}.log" "${stdout}${stderr}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${estimator} reference failed; see ${TEST_DIR}/${estimator}.log")
    endif()
endforeach()

execute_process(
    COMMAND "${COMPARATOR}"
        "${TEST_DIR}/mis.log"
        "${TEST_DIR}/nee.log"
        "${TEST_DIR}/bsdf.log"
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_stdout
    ERROR_VARIABLE compare_stderr
)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR
        "Reference estimator gate failed:\n${compare_stdout}${compare_stderr}")
endif()
message(STATUS "${compare_stdout}")

# The isolated furnace uses BSDF-only transport so P3's intentionally approximate
# straight-line Glass shadow rule cannot enter the energy-conservation proof. The
# executable applies the per-case linear-HDR two-percent/three-sigma gate itself.
execute_process(
    COMMAND "${ENGINE}" --reference "${SAMPLE_COUNT}"
        --estimator bsdf --furnace
    RESULT_VARIABLE furnace_result
    OUTPUT_VARIABLE furnace_stdout
    ERROR_VARIABLE furnace_stderr
    TIMEOUT 900
)
file(WRITE "${TEST_DIR}/furnace.log" "${furnace_stdout}${furnace_stderr}")
if(NOT furnace_result EQUAL 0)
    message(FATAL_ERROR
        "White-furnace energy proof failed; see ${TEST_DIR}/furnace.log")
endif()
message(STATUS "White-furnace energy proof passed")
