cmake_minimum_required(VERSION 3.15)
project(SwitchEmulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(switch-emulator
    src/main.cpp
    src/cpu.cpp
    src/gpu.cpp
    src/gui.cpp
    src/assembler.cpp
    src/homebrew_package.cpp
    src/nro_package.cpp
)

find_package(X11)
if (X11_FOUND)
    target_link_libraries(switch-emulator PRIVATE X11::X11)
    target_compile_definitions(switch-emulator PRIVATE SWITCH_EMU_HAS_X11=1)
else()
    message(WARNING "X11 not found; --gui will be unavailable")
endif()

enable_testing()
add_test(
    NAME emulator_smoke_test
    COMMAND ${CMAKE_SOURCE_DIR}/tests/smoke_test.sh ${CMAKE_BINARY_DIR}
)
