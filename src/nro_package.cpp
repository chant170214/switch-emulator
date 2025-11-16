#include "nro_package.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace toy_nro {
namespace {
struct Header {
    char magic[4];
    std::uint32_t version;
    std::uint32_t name_length;
    std::uint32_t code_words;
    std::uint32_t data_segments;
    std::uint32_t symbols;
};

constexpr char MAGIC[4] = {'N', 'R', 'O', '0'};
constexpr std::uint32_t VERSION = 1;

void write_exact(std::ofstream& out, const void* data, std::size_t size) {
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error("Failed to write Toy NRO file");
    }
}

void read_exact(std::ifstream& in, void* data, std::size_t size) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("Unexpected end of Toy NRO file");
    }
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

bool is_package_path(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string ext = to_lower(path.substr(dot + 1));
    return ext == "nro";
}

AssembledProgram load_package(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open Toy NRO '" + path + "'");
    }

    Header header{};
    read_exact(in, &header, sizeof(header));
    if (!std::equal(std::begin(header.magic), std::end(header.magic), std::begin(MAGIC))) {
        throw std::runtime_error("Invalid Toy NRO magic in '" + path + "'");
    }
    if (header.version != VERSION) {
        throw std::runtime_error("Unsupported Toy NRO version in '" + path + "'");
    }

    if (header.name_length > 0) {
        std::string name(header.name_length, '\0');
        read_exact(in, name.data(), header.name_length);
    }

    AssembledProgram program;
    program.code.resize(header.code_words);
    for (std::uint32_t i = 0; i < header.code_words; ++i) {
        std::uint32_t word = 0;
        read_exact(in, &word, sizeof(word));
        program.code[i] = word;
    }

    for (std::uint32_t i = 0; i < header.data_segments; ++i) {
        DataPatch patch;
        std::uint32_t length = 0;
        std::uint64_t address = 0;
        read_exact(in, &address, sizeof(address));
        patch.address = static_cast<std::size_t>(address);
        read_exact(in, &length, sizeof(length));
        patch.bytes.resize(length);
        if (length > 0) {
            read_exact(in, patch.bytes.data(), length);
        }
        program.data_segments.push_back(std::move(patch));
    }

    for (std::uint32_t i = 0; i < header.symbols; ++i) {
        std::uint32_t name_len = 0;
        std::uint64_t value = 0;
        read_exact(in, &name_len, sizeof(name_len));
        std::string name(name_len, '\0');
        if (name_len > 0) {
            read_exact(in, name.data(), name_len);
        }
        read_exact(in, &value, sizeof(value));
        program.symbols.emplace(std::move(name), static_cast<std::size_t>(value));
    }

    return program;
}

void save_package(const AssembledProgram& program, const std::string& path, const std::string& name) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open Toy NRO for writing: " + path);
    }

    Header header{};
    std::copy(std::begin(MAGIC), std::end(MAGIC), std::begin(header.magic));
    header.version = VERSION;
    header.name_length = static_cast<std::uint32_t>(name.size());
    header.code_words = static_cast<std::uint32_t>(program.code.size());
    header.data_segments = static_cast<std::uint32_t>(program.data_segments.size());
    header.symbols = static_cast<std::uint32_t>(program.symbols.size());
    write_exact(out, &header, sizeof(header));

    if (header.name_length > 0) {
        write_exact(out, name.data(), name.size());
    }

    for (std::uint32_t word : program.code) {
        write_exact(out, &word, sizeof(word));
    }

    for (const auto& patch : program.data_segments) {
        const std::uint32_t length = static_cast<std::uint32_t>(patch.bytes.size());
        const std::uint64_t address = static_cast<std::uint64_t>(patch.address);
        write_exact(out, &address, sizeof(address));
        write_exact(out, &length, sizeof(length));
        if (length > 0) {
            write_exact(out, patch.bytes.data(), length);
        }
    }

    for (const auto& [symbol, value] : program.symbols) {
        const std::uint32_t name_len = static_cast<std::uint32_t>(symbol.size());
        const std::uint64_t encoded = static_cast<std::uint64_t>(value);
        write_exact(out, &name_len, sizeof(name_len));
        if (name_len > 0) {
            write_exact(out, symbol.data(), name_len);
        }
        write_exact(out, &encoded, sizeof(encoded));
    }
}

}  // namespace toy_nro
