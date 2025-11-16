#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

struct DataPatch {
    std::size_t address;
    std::vector<std::uint8_t> bytes;
};

struct AssembledProgram {
    std::vector<std::uint32_t> code;
    std::vector<DataPatch> data_segments;
    std::unordered_map<std::string, std::size_t> symbols;
};

AssembledProgram assemble_program(const std::string& source);
AssembledProgram assemble_program(std::istream& input);
