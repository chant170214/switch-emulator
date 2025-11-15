cmake_minimum_required(VERSION 3.15)
project(SwitchEmulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(switch-emulator
    src/main.cpp
    src/cpu.cpp
)
