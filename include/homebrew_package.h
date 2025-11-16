#pragma once

#include <string>

#include "assembler.h"

namespace homebrew {

bool is_package_path(const std::string& path);
AssembledProgram load_package(const std::string& path);
void save_package(const AssembledProgram& program, const std::string& path);

}  // namespace homebrew
