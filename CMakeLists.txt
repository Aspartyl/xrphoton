cmake_minimum_required(VERSION 3.24)

project(xrPhoton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# Source adapters and future SDK tools share this one canonical writer. Keeping it
# in its own dependency-free target prevents the offline asset path from inheriting
# Vulkan, window-system, or renderer ABI concerns.
add_library(xrPhotonOgfx STATIC
    src/ogfx.cpp
    src/ogfx_decoder.cpp
    src/ogfx_detail.hpp
)
target_include_directories(xrPhotonOgfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(xrPhotonOgfx PRIVATE -Wall -Wextra)

# Legacy source parsing is a separate offline adapter. It populates the shared
# compiler model and never owns OGFx serialization or a runtime loading path.
add_library(xrPhotonLegacyOgf STATIC
    src/legacy_ogf.cpp
    src/legacy_ogf_rigid.cpp
)
target_include_directories(xrPhotonLegacyOgf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(xrPhotonLegacyOgf PRIVATE -Wall -Wextra)
target_link_libraries(xrPhotonLegacyOgf PUBLIC xrPhotonOgfx)

# Blender performs only source extraction. This standard-library-only adapter owns
# axis/normal/winding conversion and builds the same compiler model as every other
# source path before the canonical writer runs.
add_library(xrPhotonBlenderMesh STATIC
    src/blender_mesh.cpp
)
target_include_directories(xrPhotonBlenderMesh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(xrPhotonBlenderMesh PRIVATE -Wall -Wextra)
target_link_libraries(xrPhotonBlenderMesh PUBLIC xrPhotonOgfx)

add_executable(xrPhotonAssetCompiler
    tools/asset_compiler.cpp
)
target_compile_options(xrPhotonAssetCompiler PRIVATE -Wall -Wextra)
target_link_libraries(xrPhotonAssetCompiler PRIVATE
    xrPhotonLegacyOgf
    xrPhotonBlenderMesh
)

add_executable(xrPhotonProbeAssetCompiler EXCLUDE_FROM_ALL
    tools/compile_probe_assets.cpp
)
target_link_libraries(xrPhotonProbeAssetCompiler PRIVATE xrPhotonOgfx)
target_compile_options(xrPhotonProbeAssetCompiler PRIVATE -Wall -Wextra)

set(XRPHOTON_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/assets)
set(XRPHOTON_PROBE_ASSET_DIR ${XRPHOTON_ASSET_DIR}/probes)
set(XRPHOTON_SOC_ASSET_DIR ${XRPHOTON_ASSET_DIR}/soc)
set(XRPHOTON_BLENDER_ASSET_DIR ${XRPHOTON_ASSET_DIR}/blender)
set(XRPHOTON_TEST_QUAD_ASSET ${XRPHOTON_PROBE_ASSET_DIR}/test_quad.ogfx)
set(XRPHOTON_TEST_WEDGE_ASSET ${XRPHOTON_PROBE_ASSET_DIR}/test_wedge.ogfx)
set(XRPHOTON_TEST_PYRAMID_ASSET ${XRPHOTON_BLENDER_ASSET_DIR}/test_pyramid.ogfx)
set(XRPHOTON_TEST_SPHERE_ASSET ${XRPHOTON_BLENDER_ASSET_DIR}/test_sphere.ogfx)
set(XRPHOTON_TEST_SMOOTH_SPHERE_ASSET
    ${XRPHOTON_BLENDER_ASSET_DIR}/test_smooth_sphere.ogfx)
# Reserve the modern-content namespace even before the first Blender exporter
# output lands. Generated probe and SoC directories are created by their writers.
file(MAKE_DIRECTORY ${XRPHOTON_BLENDER_ASSET_DIR})
add_custom_command(
    OUTPUT
        ${XRPHOTON_TEST_QUAD_ASSET}
        ${XRPHOTON_TEST_WEDGE_ASSET}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XRPHOTON_PROBE_ASSET_DIR}
    COMMAND $<TARGET_FILE:xrPhotonProbeAssetCompiler>
        ${XRPHOTON_TEST_QUAD_ASSET}
        ${XRPHOTON_TEST_WEDGE_ASSET}
    DEPENDS xrPhotonProbeAssetCompiler
    COMMENT "Compiling the procedural quad and wedge probes to OGFx"
    VERBATIM
)
# Asset generation is explicit for compiler-only builds; the engine dependency below
# makes it automatic for normal builds without broadening the core-only default target.
add_custom_target(xrPhotonAssets DEPENDS
    ${XRPHOTON_TEST_QUAD_ASSET}
    ${XRPHOTON_TEST_WEDGE_ASSET}
)

if(BUILD_TESTING)
    add_executable(xrPhotonOgfxTests
        tests/ogfx_tests.cpp
    )
    target_link_libraries(xrPhotonOgfxTests PRIVATE xrPhotonOgfx)
    target_compile_options(xrPhotonOgfxTests PRIVATE -Wall -Wextra)
    add_test(NAME ogfx_core COMMAND xrPhotonOgfxTests)

    add_executable(xrPhotonOgfxDecoderTests
        tests/ogfx_decoder_tests.cpp
    )
    target_link_libraries(xrPhotonOgfxDecoderTests PRIVATE xrPhotonOgfx)
    target_compile_options(xrPhotonOgfxDecoderTests PRIVATE -Wall -Wextra)
    add_test(NAME ogfx_decoder COMMAND xrPhotonOgfxDecoderTests)

    add_executable(xrPhotonLegacyOgfTests
        tests/legacy_ogf_tests.cpp
    )
    target_link_libraries(xrPhotonLegacyOgfTests PRIVATE xrPhotonLegacyOgf)
    target_compile_options(xrPhotonLegacyOgfTests PRIVATE -Wall -Wextra)
    add_test(NAME legacy_ogf_adapter COMMAND xrPhotonLegacyOgfTests)

    add_executable(xrPhotonBlenderMeshTests
        tests/blender_mesh_tests.cpp
    )
    target_link_libraries(xrPhotonBlenderMeshTests PRIVATE
        xrPhotonBlenderMesh
        xrPhotonOgfx
    )
    target_compile_options(xrPhotonBlenderMeshTests PRIVATE -Wall -Wextra)
    add_test(NAME blender_mesh_adapter COMMAND xrPhotonBlenderMeshTests)

    add_test(
        NAME legacy_ogf_cli
        COMMAND ${CMAKE_COMMAND}
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DFIXTURE_TOOL=$<TARGET_FILE:xrPhotonLegacyOgfTests>
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/legacy-ogf-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_ogf_cli_test.cmake
    )

    add_test(
        NAME blender_mesh_cli
        COMMAND ${CMAKE_COMMAND}
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DFIXTURE_TOOL=$<TARGET_FILE:xrPhotonBlenderMeshTests>
            -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/blender-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/blender_cli_test.cmake
    )

    set(XRPHOTON_M4A_CORPUS_OGF
        "${CMAKE_SOURCE_DIR}/original_game_files/soc/meshes/objects/dynamics/plitka/plitka1.ogf"
        CACHE FILEPATH
        "Local plitka1.ogf used only by the opt-in M4a offline proof")
    set(XRPHOTON_M4A_PROOF_OUTPUT
        "${XRPHOTON_SOC_ASSET_DIR}/meshes/objects/dynamics/plitka/plitka1.ogfx")
    add_custom_target(xrPhotonM4aOfflineProof
        COMMAND ${CMAKE_COMMAND}
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DVERIFIER=$<TARGET_FILE:xrPhotonLegacyOgfTests>
            -DSOURCE_OGF=${XRPHOTON_M4A_CORPUS_OGF}
            -DOUTPUT_OGFX=${XRPHOTON_M4A_PROOF_OUTPUT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/m4a_offline_proof.cmake
        DEPENDS
            xrPhotonAssetCompiler
            xrPhotonLegacyOgfTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/m4a_offline_proof.cmake
        COMMENT "Proving the pinned legacy OGF conversion offline"
        VERBATIM
        USES_TERMINAL
    )

    set(XRPHOTON_RIGID_BARREL_CORPUS_OGF
        "${CMAKE_SOURCE_DIR}/original_game_files/soc/meshes/physics/balon/bochka_close_1.ogf"
        CACHE FILEPATH
        "Local bochka_close_1.ogf used only by the opt-in rigid OGF proof")
    set(XRPHOTON_RIGID_BARREL_PROOF_OUTPUT
        "${XRPHOTON_SOC_ASSET_DIR}/meshes/physics/balon/bochka_close_1.ogfx")
    add_custom_target(xrPhotonRigidOgfOfflineProof
        COMMAND ${CMAKE_COMMAND}
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DVERIFIER=$<TARGET_FILE:xrPhotonLegacyOgfTests>
            -DSOURCE_OGF=${XRPHOTON_RIGID_BARREL_CORPUS_OGF}
            -DOUTPUT_OGFX=${XRPHOTON_RIGID_BARREL_PROOF_OUTPUT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rigid_ogf_offline_proof.cmake
        DEPENDS
            xrPhotonAssetCompiler
            xrPhotonLegacyOgfTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rigid_ogf_offline_proof.cmake
        COMMENT "Proving deterministic rigid SoC barrel conversion offline"
        VERBATIM
        USES_TERMINAL
    )

    set(XRPHOTON_ALPHA_TAIL_CORPUS_OGF
        "${CMAKE_SOURCE_DIR}/original_game_files/soc/meshes/equipments/item_psevdodog_tail.ogf"
        CACHE FILEPATH
        "Local item_psevdodog_tail.ogf used only by the opt-in alpha OGF proof")
    set(XRPHOTON_ALPHA_TAIL_TEXTURE_DDS
        "${CMAKE_SOURCE_DIR}/original_game_files/soc/textures/act/act_pseudodog_fur.dds"
        CACHE FILEPATH
        "Local act_pseudodog_fur.dds pinned by the opt-in alpha OGF proof")
    set(XRPHOTON_ALPHA_TAIL_PROOF_OUTPUT
        "${XRPHOTON_SOC_ASSET_DIR}/meshes/equipments/item_psevdodog_tail.ogfx")
    add_custom_target(xrPhotonAlphaOgfOfflineProof
        COMMAND ${CMAKE_COMMAND}
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DVERIFIER=$<TARGET_FILE:xrPhotonLegacyOgfTests>
            -DSOURCE_OGF=${XRPHOTON_ALPHA_TAIL_CORPUS_OGF}
            -DTEXTURE_DDS=${XRPHOTON_ALPHA_TAIL_TEXTURE_DDS}
            -DOUTPUT_OGFX=${XRPHOTON_ALPHA_TAIL_PROOF_OUTPUT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/alpha_ogf_offline_proof.cmake
        DEPENDS
            xrPhotonAssetCompiler
            xrPhotonLegacyOgfTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/alpha_ogf_offline_proof.cmake
        COMMENT "Proving deterministic mixed opaque/alpha-tested SoC tail conversion offline"
        VERBATIM
        USES_TERMINAL
    )

    # Blender and the .blend inputs are owner-local tools/sources, so this proof is
    # explicit and never enters the normal asset/build/test dependency graph.
    set(XRPHOTON_BLENDER_EXECUTABLE "" CACHE FILEPATH
        "Portable Blender 5.1.x executable used by the opt-in offline proof")
    set(XRPHOTON_BLENDER_PYRAMID_BLEND
        "${CMAKE_SOURCE_DIR}/blender/test_pyramid.blend" CACHE FILEPATH
        "Local ignored pyramid .blend used by the opt-in Blender proof")
    set(XRPHOTON_BLENDER_SPHERE_BLEND
        "${CMAKE_SOURCE_DIR}/blender/test_sphere.blend" CACHE FILEPATH
        "Local ignored sphere .blend used by the opt-in Blender proof")
    set(XRPHOTON_BLENDER_SMOOTH_SPHERE_BLEND
        "${CMAKE_SOURCE_DIR}/blender/test_smooth_sphere.blend" CACHE FILEPATH
        "Local ignored smooth sphere .blend used by the opt-in Blender proof")
    add_custom_target(xrPhotonBlenderOfflineProof
        COMMAND ${CMAKE_COMMAND}
            -DBLENDER_EXECUTABLE=${XRPHOTON_BLENDER_EXECUTABLE}
            -DEXPORT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/export_ogfx.py
            -DASSET_COMPILER=$<TARGET_FILE:xrPhotonAssetCompiler>
            -DVERIFIER=$<TARGET_FILE:xrPhotonBlenderMeshTests>
            -DPYRAMID_BLEND=${XRPHOTON_BLENDER_PYRAMID_BLEND}
            -DPYRAMID_OUTPUT=${XRPHOTON_TEST_PYRAMID_ASSET}
            -DSPHERE_BLEND=${XRPHOTON_BLENDER_SPHERE_BLEND}
            -DSPHERE_OUTPUT=${XRPHOTON_TEST_SPHERE_ASSET}
            -DSMOOTH_SPHERE_BLEND=${XRPHOTON_BLENDER_SMOOTH_SPHERE_BLEND}
            -DSMOOTH_SPHERE_OUTPUT=${XRPHOTON_TEST_SMOOTH_SPHERE_ASSET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/blender_offline_proof.cmake
        DEPENDS
            xrPhotonAssetCompiler
            xrPhotonBlenderMeshTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/export_ogfx.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/blender_offline_proof.cmake
        COMMENT "Proving deterministic Blender-to-OGFx conversion offline"
        VERBATIM
        USES_TERMINAL
    )
endif()

option(XRPHOTON_BUILD_ENGINE "Build the Vulkan renderer executable" ON)
if(NOT XRPHOTON_BUILD_ENGINE)
    # Offline compiler work must remain buildable on machines with no graphics SDK.
    return()
endif()

# Optional local real-content gallery input. Empty defaults preserve a generated-
# asset-only build; once configured, a broken model or texture root is a loud runtime
# error rather than a silently skipped integration probe.
set(XRPHOTON_GALLERY_PLITKA_OGFX "" CACHE FILEPATH
    "Optional converted plitka1.ogfx shown in the runtime gallery")
set(XRPHOTON_GALLERY_BLENDER_OGFX "" CACHE FILEPATH
    "Optional converted Blender pyramid OGFx shown in the runtime gallery")
set(XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX "" CACHE FILEPATH
    "Optional converted Blender sphere OGFx shown in the runtime gallery")
set(XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX "" CACHE FILEPATH
    "Optional converted Blender smooth sphere OGFx shown in the runtime gallery")
set(XRPHOTON_GALLERY_BARREL_OGFX "" CACHE FILEPATH
    "Optional converted SoC closed barrel OGFx shown in the runtime gallery")
set(XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX "" CACHE FILEPATH
    "Optional converted SoC pseudodog tail OGFx shown in the runtime gallery")
set(XRPHOTON_GALLERY_TEXTURE_ROOT "" CACHE PATH
    "Texture root containing the DDS paths referenced by configured gallery assets")

if(XRPHOTON_GALLERY_PLITKA_OGFX)
    get_filename_component(XRPHOTON_GALLERY_PLITKA_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_PLITKA_OGFX}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_PLITKA_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_BLENDER_OGFX)
    get_filename_component(XRPHOTON_GALLERY_BLENDER_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_BLENDER_OGFX}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_BLENDER_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX)
    get_filename_component(XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX}"
        ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX)
    get_filename_component(XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX}"
        ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_BARREL_OGFX)
    get_filename_component(XRPHOTON_GALLERY_BARREL_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_BARREL_OGFX}"
        ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_BARREL_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX)
    get_filename_component(XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX_ABSOLUTE
        "${XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX}"
        ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX_ABSOLUTE "")
endif()
if(XRPHOTON_GALLERY_TEXTURE_ROOT)
    get_filename_component(XRPHOTON_GALLERY_TEXTURE_ROOT_ABSOLUTE
        "${XRPHOTON_GALLERY_TEXTURE_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
else()
    set(XRPHOTON_GALLERY_TEXTURE_ROOT_ABSOLUTE "")
endif()

# Whether to request the Khronos validation layer and debug messenger. Even when ON,
# validation is best-effort at runtime: if the layer is not installed (machines without
# the Vulkan SDK), the program warns and continues rather than refusing to start.
option(XRPHOTON_ENABLE_VALIDATION "Request the Vulkan validation layer and debug messenger" ON)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

# Current GLM packages export glm::glm; older distro packages used glm. Resolve
# that packaging variance here so the rest of the build has one dependency name.
if(TARGET glm::glm)
    set(XRPHOTON_GLM_TARGET glm::glm)
elseif(TARGET glm)
    set(XRPHOTON_GLM_TARGET glm)
else()
    message(FATAL_ERROR "The GLM package did not export a supported CMake target")
endif()

# Runtime adaptation stays Vulkan-free but maps the dependency-free decoded model
# into SceneData, whose instance type uses GLM. Keeping this as a second layer lets
# the byte decoder and malformed-file tests remain available in ogfx-core builds.
add_library(xrPhotonOgfxRuntime STATIC
    src/ogfx_loader.cpp
    src/scene_assembly.cpp
    src/texture_loader.cpp
)
target_include_directories(xrPhotonOgfxRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(xrPhotonOgfxRuntime PRIVATE -Wall -Wextra)
target_link_libraries(xrPhotonOgfxRuntime PUBLIC xrPhotonOgfx ${XRPHOTON_GLM_TARGET})
# This single value is also passed to slangc below. It is a routing ABI rather than a
# tunable: changing it requires adding the corresponding miss/hit shader variants.
set(XRPHOTON_RAY_TYPE_COUNT 1)
target_compile_definitions(xrPhotonOgfxRuntime PUBLIC
    XRPHOTON_RAY_TYPE_COUNT=${XRPHOTON_RAY_TYPE_COUNT})

if(BUILD_TESTING)
    add_executable(xrPhotonOgfxLoaderTests
        tests/ogfx_loader_tests.cpp
    )
    target_link_libraries(xrPhotonOgfxLoaderTests PRIVATE xrPhotonOgfxRuntime)
    target_compile_options(xrPhotonOgfxLoaderTests PRIVATE -Wall -Wextra)
    add_test(NAME ogfx_runtime_loader COMMAND xrPhotonOgfxLoaderTests)

    add_executable(xrPhotonSceneAssemblyTests
        tests/scene_assembly_tests.cpp
    )
    target_link_libraries(xrPhotonSceneAssemblyTests PRIVATE xrPhotonOgfxRuntime)
    target_compile_options(xrPhotonSceneAssemblyTests PRIVATE -Wall -Wextra)
    add_test(NAME scene_assembly COMMAND xrPhotonSceneAssemblyTests)

    add_executable(xrPhotonTextureLoaderTests
        tests/texture_loader_tests.cpp
    )
    target_link_libraries(xrPhotonTextureLoaderTests PRIVATE xrPhotonOgfxRuntime)
    target_compile_options(xrPhotonTextureLoaderTests PRIVATE -Wall -Wextra)
    add_test(NAME texture_loader COMMAND xrPhotonTextureLoaderTests)
endif()

# The Slang compiler is not a FindVulkan component, so locate it directly.
find_program(XRPHOTON_SLANGC slangc REQUIRED
    DOC "Slang compiler (slangc) used to build the ray tracing shaders")

# The shader module is compiled to SPIR-V at build time and embedded as a C
# header (-source-embed-style u32 emits a uint32_t array plus a _sizeInBytes
# constant), keeping the executable free of runtime file paths. Slang compiles
# every [shader(...)] entry point into the one module; -fvk-use-entrypoint-name
# preserves the source entry-point names the pipeline stages select by. Slang's
# direct SPIR-V emission targets 1.5; ray tracing needs at least 1.4.
set(SHADER_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_HEADER ${SHADER_HEADER_DIR}/raytrace_spv.h)
add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_HEADER_DIR}
    COMMAND ${XRPHOTON_SLANGC}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/raytrace.slang
            -DXRPHOTON_RAY_TYPE_COUNT=${XRPHOTON_RAY_TYPE_COUNT}
            -target spirv
            -fvk-use-entrypoint-name
            -source-embed-style u32
            -source-embed-name raytrace_spv
            -o ${SHADER_HEADER}
    # slangc's embed output uses uint32_t/size_t without including their headers;
    # prepend the includes so the generated header is self-contained. The C
    # spellings keep it parseable in both C and C++ mode (IDEs analyzing a bare
    # .h often pick C).
    COMMAND sed -i -e "1i #include <stddef.h>" -e "1i #include <stdint.h>"
            ${SHADER_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/raytrace.slang
    COMMENT "Compiling raytrace.slang to SPIR-V header"
    VERBATIM
)

add_custom_target(xrPhotonShaders DEPENDS ${SHADER_HEADER})

add_executable(xrPhoton
    src/main.cpp
    src/gallery.cpp
    src/third_party_impl.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/acceleration_structure.cpp
    src/gpu_scene.cpp
    src/camera.cpp
    src/rt_pipeline.cpp
    src/renderer.cpp
)

if(XRPHOTON_ENABLE_VALIDATION)
    target_compile_definitions(xrPhoton PRIVATE XRPHOTON_ENABLE_VALIDATION)
endif()

# The preview inputs are build-tree probes and optional owner-supplied local content,
# not packaged game assets. Embed absolute UTF-8 paths so launching from any working
# directory exercises the same configured gallery; a future asset root replaces this
# temporary bring-up policy.
target_compile_definitions(xrPhoton PRIVATE
    XRPHOTON_TEST_QUAD_ASSET_PATH=u8\"${XRPHOTON_TEST_QUAD_ASSET}\"
    XRPHOTON_TEST_WEDGE_ASSET_PATH=u8\"${XRPHOTON_TEST_WEDGE_ASSET}\"
    XRPHOTON_GALLERY_PLITKA_OGFX=u8\"${XRPHOTON_GALLERY_PLITKA_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_BLENDER_OGFX=u8\"${XRPHOTON_GALLERY_BLENDER_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX=u8\"${XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX=u8\"${XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_BARREL_OGFX=u8\"${XRPHOTON_GALLERY_BARREL_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX=u8\"${XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX_ABSOLUTE}\"
    XRPHOTON_GALLERY_TEXTURE_ROOT=u8\"${XRPHOTON_GALLERY_TEXTURE_ROOT_ABSOLUTE}\"
)

add_dependencies(xrPhoton xrPhotonShaders xrPhotonAssets)
target_include_directories(xrPhoton PRIVATE ${SHADER_HEADER_DIR})
target_include_directories(xrPhoton SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vma
)

target_compile_options(xrPhoton PRIVATE -Wall -Wextra)

target_link_libraries(xrPhoton
    PRIVATE
        xrPhotonOgfxRuntime
        Vulkan::Vulkan
        glfw
        ${XRPHOTON_GLM_TARGET}
)
