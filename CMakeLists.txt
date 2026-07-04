cmake_minimum_required(VERSION 3.24)

project(xrPhoton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Whether to request the Khronos validation layer and debug messenger. Even when ON,
# validation is best-effort at runtime: if the layer is not installed (machines without
# the Vulkan SDK), the program warns and continues rather than refusing to start.
option(XRPHOTON_ENABLE_VALIDATION "Request the Vulkan validation layer and debug messenger" ON)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

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
set(SHADER_HEADER ${SHADER_HEADER_DIR}/triangle_spv.h)
add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_HEADER_DIR}
    COMMAND ${XRPHOTON_SLANGC}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.slang
            -target spirv
            -fvk-use-entrypoint-name
            -source-embed-style u32
            -source-embed-name triangle_spv
            -o ${SHADER_HEADER}
    # slangc's embed output uses uint32_t/size_t without including their headers;
    # prepend the includes so the generated header is self-contained. The C
    # spellings keep it parseable in both C and C++ mode (IDEs analyzing a bare
    # .h often pick C).
    COMMAND sed -i -e "1i #include <stddef.h>" -e "1i #include <stdint.h>"
            ${SHADER_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.slang
    COMMENT "Compiling triangle.slang to SPIR-V header"
    VERBATIM
)

add_custom_target(xrPhotonShaders DEPENDS ${SHADER_HEADER})

add_executable(xrPhoton
    src/main.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/acceleration_structure.cpp
    src/rt_pipeline.cpp
)

if(XRPHOTON_ENABLE_VALIDATION)
    target_compile_definitions(xrPhoton PRIVATE XRPHOTON_ENABLE_VALIDATION)
endif()

add_dependencies(xrPhoton xrPhotonShaders)
target_include_directories(xrPhoton PRIVATE ${SHADER_HEADER_DIR})

target_compile_options(xrPhoton PRIVATE -Wall -Wextra)

target_link_libraries(xrPhoton
    PRIVATE
        Vulkan::Vulkan
        glfw
)
