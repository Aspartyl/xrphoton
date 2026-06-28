cmake_minimum_required(VERSION 3.24)

project(xrPhoton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

add_executable(xrPhoton
    src/main.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
)

target_link_libraries(xrPhoton
    PRIVATE
        Vulkan::Vulkan
        glfw
)
