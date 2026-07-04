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

add_executable(xrPhoton
    src/main.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/acceleration_structure.cpp
)

if(XRPHOTON_ENABLE_VALIDATION)
    target_compile_definitions(xrPhoton PRIVATE XRPHOTON_ENABLE_VALIDATION)
endif()

target_compile_options(xrPhoton PRIVATE -Wall -Wextra)

target_link_libraries(xrPhoton
    PRIVATE
        Vulkan::Vulkan
        glfw
)
