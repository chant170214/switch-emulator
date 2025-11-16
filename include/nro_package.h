#pragma once

#include <string>

#include "assembler.h"

namespace toy_nro {

bool is_package_path(const std::string& path);
AssembledProgram load_package(const std::string& path);
void save_package(const AssembledProgram& program, const std::string& path, const std::string& name = "toy_homebrew");

}  // namespace toy_nro
