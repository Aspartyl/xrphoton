cmake_minimum_required(VERSION 3.24)

project(xray_photon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(xray_photon
    src/main.cpp
)
