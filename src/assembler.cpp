#include "assembler.h"

#include "instruction_set.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim(std::string value) {
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(),
                value.end());
    return value;
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

bool is_blank(const std::string& line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_string = false;
    for (char ch : line) {
        if (ch == '"') {
            in_string = !in_string;
            current.push_back(ch);
            continue;
        }
        if (!in_string && (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string strip_quotes(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::uint32_t pack_word(long long value, std::size_t line_no) {
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Line " + std::to_string(line_no) + ": .word value out of 32-bit range");
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

class Assembler {
public:
    AssembledProgram run(std::istream& input, const std::string& source_name);

private:
    enum class Section { Code, Data };

    struct Fixup {
        enum class Type { Branch, Call, Movi };
        std::size_t instruction_index;
        std::string label;
        std::size_t line;
        Type type;
        std::uint8_t opcode;
        std::uint8_t rd;
    };

    void assemble_stream(std::istream& input, const fs::path& current_path);
    void parse_line(const std::string& raw_line);
    void emit_instruction(std::uint32_t inst);
    void handle_directive(const std::vector<std::string>& tokens);
    void handle_instruction(const std::vector<std::string>& tokens);
    std::uint8_t parse_register(const std::string& token) const;
    long long parse_value(const std::string& token) const;
    void validate_range(long long value, int bits, std::size_t line_no, const std::string& context) const;
    void flush_data_patch();
    void ensure_data_patch();
    void apply_fixups();

    Section section_ = Section::Code;
    std::size_t line_no_ = 0;
    fs::path current_file_;
    AssembledProgram program_;
    DataPatch current_patch_{};
    bool patch_active_ = false;
    std::unordered_map<std::string, std::size_t> label_to_word_;
    std::unordered_map<std::string, long long> symbols_;
    std::vector<Fixup> fixups_;
    std::size_t next_data_address_ = 0;
};

AssembledProgram Assembler::run(std::istream& input, const std::string& source_name) {
    assemble_stream(input, fs::path(source_name));
    flush_data_patch();
    apply_fixups();
    for (const auto& [name, value] : symbols_) {
        if (value >= 0) {
            program_.symbols[name] = static_cast<std::size_t>(value);
        }
    }
    return program_;
}

void Assembler::assemble_stream(std::istream& input, const fs::path& current_path) {
    const fs::path previous_file = current_file_;
    current_file_ = current_path;
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        ++line_no_;
        std::string line = raw_line;
        const auto comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) {
            line.erase(comment_pos);
        }
        const auto slash_comment = line.find("//");
        if (slash_comment != std::string::npos) {
            line.erase(slash_comment);
        }
        line = trim(line);
        if (line.empty() || is_blank(line)) {
            continue;
        }
        parse_line(line);
    }
    current_file_ = previous_file;
}

void Assembler::parse_line(const std::string& raw_line) {
    std::string line = raw_line;
    const auto colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
        std::string label = trim(line.substr(0, colon_pos));
        if (label.empty()) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": empty label definition");
        }
        if (label_to_word_.count(label) || symbols_.count(label)) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": duplicate label '" + label + "'");
        }
        if (section_ == Section::Code) {
            label_to_word_[label] = program_.code.size();
            symbols_[label] = static_cast<long long>(program_.code.size() * sizeof(std::uint32_t));
        } else {
            ensure_data_patch();
            symbols_[label] = static_cast<long long>(current_patch_.address + current_patch_.bytes.size());
        }
        line = trim(line.substr(colon_pos + 1));
        if (line.empty()) {
            return;
        }
    }

    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
        return;
    }

    if (tokens[0][0] == '.') {
        handle_directive(tokens);
    } else if (tokens.size() >= 1) {
        handle_instruction(tokens);
    }
}

void Assembler::emit_instruction(std::uint32_t inst) {
    if (section_ != Section::Code) {
        throw std::runtime_error("Line " + std::to_string(line_no_) + ": instructions must be in the code section");
    }
    program_.code.push_back(inst);
}

void Assembler::handle_directive(const std::vector<std::string>& tokens) {
    const std::string directive = to_upper(tokens[0]);
    if (directive == ".SECTION") {
        if (tokens.size() != 2) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .section requires an argument");
        }
        const std::string section_name = to_upper(tokens[1]);
        if (section_name == "CODE" || section_name == "TEXT") {
            flush_data_patch();
            section_ = Section::Code;
        } else if (section_name == "DATA") {
            flush_data_patch();
            section_ = Section::Data;
        } else {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": unknown section '" + tokens[1] + "'");
        }
        return;
    }

    if (directive == ".ORG") {
        if (tokens.size() != 2) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .org expects one argument");
        }
        const long long address = parse_value(tokens[1]);
        if (address < 0) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .org address must be non-negative");
        }
        if (section_ == Section::Code) {
            if (address % 4 != 0) {
                throw std::runtime_error("Line " + std::to_string(line_no_) + ": .org address must be word-aligned");
            }
            const std::size_t target_words = static_cast<std::size_t>(address) / sizeof(std::uint32_t);
            if (target_words < program_.code.size()) {
                throw std::runtime_error("Line " + std::to_string(line_no_) + ": .org cannot move backwards");
            }
            while (program_.code.size() < target_words) {
                emit_instruction(isa::encode_r(isa::OP_NOP, 0, 0, 0));
            }
        } else {
            flush_data_patch();
            current_patch_.address = static_cast<std::size_t>(address);
            current_patch_.bytes.clear();
            patch_active_ = true;
            next_data_address_ = static_cast<std::size_t>(address);
        }
        return;
    }

    if (directive == ".DATA") {
        if (tokens.size() < 3) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .data requires an address and values");
        }
        const long long address = parse_value(tokens[1]);
        if (address < 0) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .data address must be non-negative");
        }
        DataPatch patch;
        patch.address = static_cast<std::size_t>(address);
        patch.bytes.reserve(tokens.size() - 2);
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const long long byte_value = parse_value(tokens[i]);
            if (byte_value < 0 || byte_value > 0xFF) {
                throw std::runtime_error("Line " + std::to_string(line_no_) + ": .data byte out of range");
            }
            patch.bytes.push_back(static_cast<std::uint8_t>(byte_value));
        }
        next_data_address_ = static_cast<std::size_t>(address) + patch.bytes.size();
        program_.data_segments.push_back(std::move(patch));
        return;
    }

    if (directive == ".WORD") {
        if (section_ != Section::Data) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .word only valid in data section");
        }
        ensure_data_patch();
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::uint32_t word = pack_word(parse_value(tokens[i]), line_no_);
            current_patch_.bytes.push_back(static_cast<std::uint8_t>(word & 0xFF));
            current_patch_.bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFF));
            current_patch_.bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFF));
            current_patch_.bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFF));
        }
        return;
    }

    if (directive == ".BYTE") {
        if (section_ != Section::Data) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .byte only valid in data section");
        }
        ensure_data_patch();
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const long long value = parse_value(tokens[i]);
            if (value < std::numeric_limits<std::int8_t>::min() || value > std::numeric_limits<std::uint8_t>::max()) {
                throw std::runtime_error("Line " + std::to_string(line_no_) + ": .byte value out of range");
            }
            current_patch_.bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        }
        return;
    }

    if (directive == ".INCLUDE") {
        if (tokens.size() != 2) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .include expects a path");
        }
        const fs::path path = current_file_.empty() ? fs::path(strip_quotes(tokens[1]))
                                                    : current_file_.parent_path() / strip_quotes(tokens[1]);
        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": failed to include '" + path.string() + "'");
        }
        assemble_stream(file, path);
        return;
    }

    if (directive == ".EQU" || directive == ".MACRO") {
        if (tokens.size() != 3) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": .equ/.macro expects name and value");
        }
        const std::string name = tokens[1];
        if (symbols_.count(name)) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": symbol already defined: '" + name + "'");
        }
        symbols_[name] = parse_value(tokens[2]);
        return;
    }

    throw std::runtime_error("Line " + std::to_string(line_no_) + ": unknown directive '" + tokens[0] + "'");
}

void Assembler::handle_instruction(const std::vector<std::string>& tokens) {
    const std::string mnemonic_upper = to_upper(tokens[0]);

    auto require_operands = [&](std::size_t expected) {
        if (tokens.size() != expected + 1) {
            throw std::runtime_error("Line " + std::to_string(line_no_) + ": instruction '" + tokens[0] +
                                     "' expects " + std::to_string(expected) + " operands");
        }
    };

    auto parse_branch_target = [&](std::uint8_t opcode, std::uint8_t rd, const std::string& label) {
        auto it = label_to_word_.find(label);
        if (it != label_to_word_.end()) {
            const long long target_words = static_cast<long long>(it->second);
            const long long current = static_cast<long long>(program_.code.size());
            const long long offset = target_words - (current + 1);
            validate_range(offset, 21, line_no_, tokens[0]);
            emit_instruction(isa::encode_i(opcode, rd, static_cast<std::int32_t>(offset)));
        } else {
            emit_instruction(isa::encode_i(opcode, rd, 0));
            fixups_.push_back({program_.code.size() - 1, label, line_no_, Fixup::Type::Branch, opcode, rd});
        }
    };

    if (mnemonic_upper == "NOP") {
        emit_instruction(isa::encode_r(isa::OP_NOP, 0, 0, 0));
        return;
    }

    if (mnemonic_upper == "HALT") {
        emit_instruction(isa::encode_i(isa::OP_HALT, 0, 0));
        return;
    }

    if (mnemonic_upper == "RET" || mnemonic_upper == "GPUFLIP" || mnemonic_upper == "DMABLT") {
        require_operands(mnemonic_upper == "RET" ? 0 : 1);
        std::uint8_t rd = 0;
        if (tokens.size() == 2) {
            rd = parse_register(tokens[1]);
        }
        std::uint8_t opcode = (mnemonic_upper == "RET") ? isa::OP_RET :
                               (mnemonic_upper == "GPUFLIP") ? isa::OP_GPUFLIP : isa::OP_DMABLT;
        emit_instruction(isa::encode_single(opcode, rd));
        return;
    }

    if (mnemonic_upper == "INT") {
        require_operands(1);
        const std::uint8_t vector_reg = parse_register(tokens[1]);
        emit_instruction(isa::encode_single(isa::OP_INT, vector_reg));
        return;
    }

    if (mnemonic_upper == "PUSH" || mnemonic_upper == "POP" || mnemonic_upper == "GPUFILL" ||
        mnemonic_upper == "GPURECT" || mnemonic_upper == "GPULINE" || mnemonic_upper == "GPUTRI" ||
        mnemonic_upper == "GPUSPRITE") {
        require_operands(1);
        const std::uint8_t rd = parse_register(tokens[1]);
        std::uint8_t opcode = 0;
        if (mnemonic_upper == "PUSH") {
            opcode = isa::OP_PUSH;
        } else if (mnemonic_upper == "POP") {
            opcode = isa::OP_POP;
        } else if (mnemonic_upper == "GPUFILL") {
            opcode = isa::OP_GPUFILL;
        } else if (mnemonic_upper == "GPURECT") {
            opcode = isa::OP_GPURECT;
        } else if (mnemonic_upper == "GPULINE") {
            opcode = isa::OP_GPULINE;
        } else if (mnemonic_upper == "GPUTRI") {
            opcode = isa::OP_GPUTRI;
        } else {
            opcode = isa::OP_GPUSPRITE;
        }
        emit_instruction(isa::encode_single(opcode, rd));
        return;
    }

    if (mnemonic_upper == "GPUSETTARGET") {
        require_operands(1);
        const std::uint8_t rd = parse_register(tokens[1]);
        emit_instruction(isa::encode_single(isa::OP_GPUSETTARGET, rd));
        return;
    }

    if (mnemonic_upper == "GPUDRAW" || mnemonic_upper == "ADD" || mnemonic_upper == "SUB" ||
        mnemonic_upper == "MUL" || mnemonic_upper == "DIV" || mnemonic_upper == "AND" ||
        mnemonic_upper == "OR" || mnemonic_upper == "XOR" || mnemonic_upper == "SHL" ||
        mnemonic_upper == "SHR" || mnemonic_upper == "CMP" || mnemonic_upper == "GPUTEX") {
        require_operands(mnemonic_upper == "GPUTEX" ? 2 : 3);
        const std::uint8_t rd = parse_register(tokens[1]);
        const std::uint8_t rn = parse_register(tokens[2]);
        const std::uint8_t rm = (tokens.size() > 3) ? parse_register(tokens[3]) : 0;
        std::uint8_t opcode = 0;
        if (mnemonic_upper == "GPUDRAW") {
            opcode = isa::OP_GPUDRAW;
        } else if (mnemonic_upper == "ADD") {
            opcode = isa::OP_ADD;
        } else if (mnemonic_upper == "SUB") {
            opcode = isa::OP_SUB;
        } else if (mnemonic_upper == "MUL") {
            opcode = isa::OP_MUL;
        } else if (mnemonic_upper == "DIV") {
            opcode = isa::OP_DIV;
        } else if (mnemonic_upper == "AND") {
            opcode = isa::OP_AND;
        } else if (mnemonic_upper == "OR") {
            opcode = isa::OP_OR;
        } else if (mnemonic_upper == "XOR") {
            opcode = isa::OP_XOR;
        } else if (mnemonic_upper == "SHL") {
            opcode = isa::OP_SHL;
        } else if (mnemonic_upper == "SHR") {
            opcode = isa::OP_SHR;
        } else if (mnemonic_upper == "CMP") {
            opcode = isa::OP_CMP;
        } else {
            opcode = isa::OP_GPUTEX;
        }
        emit_instruction(isa::encode_r(opcode, rd, rn, rm));
        if (mnemonic_upper == "GPUTEX") {
            // Expect rd: descriptor pointer, rn: destination register
            // rm unused
        }
        return;
    }

    if (mnemonic_upper == "NOT") {
        require_operands(2);
        const std::uint8_t rd = parse_register(tokens[1]);
        const std::uint8_t rn = parse_register(tokens[2]);
        emit_instruction(isa::encode_r(isa::OP_NOT, rd, rn, 0));
        return;
    }

    if (mnemonic_upper == "MOVI") {
        require_operands(2);
        const std::uint8_t rd = parse_register(tokens[1]);
        const std::string& value_token = tokens[2];
        auto symbol_it = symbols_.find(value_token);
        if (symbol_it != symbols_.end()) {
            const long long value = symbol_it->second;
            validate_range(value, 21, line_no_, tokens[0]);
            emit_instruction(isa::encode_i(isa::OP_MOVI, rd, static_cast<std::int32_t>(value)));
        } else {
            try {
                const long long imm = parse_value(value_token);
                validate_range(imm, 21, line_no_, tokens[0]);
                emit_instruction(isa::encode_i(isa::OP_MOVI, rd, static_cast<std::int32_t>(imm)));
            } catch (const std::exception&) {
                emit_instruction(isa::encode_i(isa::OP_MOVI, rd, 0));
                fixups_.push_back({program_.code.size() - 1, value_token, line_no_, Fixup::Type::Movi, isa::OP_MOVI, rd});
            }
        }
        return;
    }

    if (mnemonic_upper == "ADDI" || mnemonic_upper == "SUBI") {
        require_operands(2);
        const std::uint8_t rd = parse_register(tokens[1]);
        const long long imm = parse_value(tokens[2]);
        validate_range(imm, 21, line_no_, tokens[0]);
        const std::uint8_t opcode = (mnemonic_upper == "ADDI") ? isa::OP_ADDI : isa::OP_SUBI;
        emit_instruction(isa::encode_i(opcode, rd, static_cast<std::int32_t>(imm)));
        return;
    }

    if (mnemonic_upper == "LOAD" || mnemonic_upper == "STORE" || mnemonic_upper == "LOADB" ||
        mnemonic_upper == "STOREB" || mnemonic_upper == "LOADW" || mnemonic_upper == "STOREW") {
        require_operands(3);
        const std::uint8_t rd = parse_register(tokens[1]);
        const std::uint8_t rn = parse_register(tokens[2]);
        const long long imm = parse_value(tokens[3]);
        validate_range(imm, 11, line_no_, tokens[0]);
        std::uint8_t opcode = 0;
        if (mnemonic_upper == "LOAD") {
            opcode = isa::OP_LOAD;
        } else if (mnemonic_upper == "STORE") {
            opcode = isa::OP_STORE;
        } else if (mnemonic_upper == "LOADB") {
            opcode = isa::OP_LOADB;
        } else if (mnemonic_upper == "STOREB") {
            opcode = isa::OP_STOREB;
        } else if (mnemonic_upper == "LOADW") {
            opcode = isa::OP_LOADW;
        } else {
            opcode = isa::OP_STOREW;
        }
        emit_instruction(isa::encode_mem(opcode, rd, rn, static_cast<std::int32_t>(imm)));
        return;
    }

    if (mnemonic_upper == "BRANCH" || mnemonic_upper == "BRANCHZ" || mnemonic_upper == "BRANCHNZ" ||
        mnemonic_upper == "BRANCHGT" || mnemonic_upper == "BRANCHLT") {
        require_operands((mnemonic_upper == "BRANCH") ? 1 : 2);
        std::uint8_t rd = 0;
        std::size_t label_index = (mnemonic_upper == "BRANCH") ? 1 : 2;
        if (label_index == 2) {
            rd = parse_register(tokens[1]);
        }
        std::uint8_t opcode = 0;
        if (mnemonic_upper == "BRANCH") {
            opcode = isa::OP_BRANCH;
        } else if (mnemonic_upper == "BRANCHZ") {
            opcode = isa::OP_BRANCHZ;
        } else if (mnemonic_upper == "BRANCHNZ") {
            opcode = isa::OP_BRANCHNZ;
        } else if (mnemonic_upper == "BRANCHGT") {
            opcode = isa::OP_BRANCHGT;
        } else {
            opcode = isa::OP_BRANCHLT;
        }
        parse_branch_target(opcode, rd, tokens[label_index]);
        return;
    }

    if (mnemonic_upper == "CALL") {
        require_operands(1);
        const std::string& label = tokens[1];
        auto it = label_to_word_.find(label);
        if (it != label_to_word_.end()) {
            const long long target_words = static_cast<long long>(it->second);
            const long long current = static_cast<long long>(program_.code.size());
            const long long offset = target_words - (current + 1);
            validate_range(offset, 21, line_no_, tokens[0]);
            emit_instruction(isa::encode_call(isa::OP_CALL, static_cast<std::int32_t>(offset)));
        } else {
            emit_instruction(isa::encode_call(isa::OP_CALL, 0));
            fixups_.push_back({program_.code.size() - 1, label, line_no_, Fixup::Type::Call, isa::OP_CALL, 0});
        }
        return;
    }

    throw std::runtime_error("Line " + std::to_string(line_no_) + ": unknown mnemonic '" + tokens[0] + "'");
}

std::uint8_t Assembler::parse_register(const std::string& token) const {
    if (token.size() < 2 || (token[0] != 'R' && token[0] != 'r')) {
        throw std::runtime_error("Line " + std::to_string(line_no_) + ": expected register token, got '" + token + "'");
    }
    const std::string digits = token.substr(1);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), ::isdigit)) {
        throw std::runtime_error("Line " + std::to_string(line_no_) + ": malformed register token '" + token + "'");
    }
    const int value = std::stoi(digits);
    if (value < 0 || value > 31) {
        throw std::runtime_error("Line " + std::to_string(line_no_) + ": register index out of range");
    }
    return static_cast<std::uint8_t>(value);
}

long long Assembler::parse_value(const std::string& token) const {
    auto sym_it = symbols_.find(token);
    if (sym_it != symbols_.end()) {
        return sym_it->second;
    }
    try {
        std::size_t consumed = 0;
        long long value = std::stoll(token, &consumed, 0);
        if (consumed == token.size()) {
            return value;
        }
    } catch (const std::exception&) {
    }
    throw std::runtime_error("Line " + std::to_string(line_no_) + ": unknown symbol or value '" + token + "'");
}

void Assembler::validate_range(long long value, int bits, std::size_t line_no, const std::string& context) const {
    const long long min = -(1ll << (bits - 1));
    const long long max = (1ll << (bits - 1)) - 1;
    if (value < min || value > max) {
        throw std::runtime_error("Line " + std::to_string(line_no) + ": immediate for " + context + " out of range");
    }
}

void Assembler::flush_data_patch() {
    if (patch_active_ && !current_patch_.bytes.empty()) {
        next_data_address_ = current_patch_.address + current_patch_.bytes.size();
        program_.data_segments.push_back(current_patch_);
    }
    current_patch_ = DataPatch{};
    patch_active_ = false;
}

void Assembler::ensure_data_patch() {
    if (!patch_active_) {
        current_patch_.address = next_data_address_;
        current_patch_.bytes.clear();
        patch_active_ = true;
    }
}

void Assembler::apply_fixups() {
    for (const auto& fixup : fixups_) {
        auto sym_it = symbols_.find(fixup.label);
        if (sym_it == symbols_.end()) {
            throw std::runtime_error("Line " + std::to_string(fixup.line) + ": unknown label '" + fixup.label + "'");
        }
        const long long target_address = sym_it->second;
        switch (fixup.type) {
        case Fixup::Type::Branch: {
            const long long current = static_cast<long long>(fixup.instruction_index + 1);
            const long long target_words = target_address / 4;
            const long long offset = target_words - current;
            validate_range(offset, 21, fixup.line, "branch");
            program_.code[fixup.instruction_index] =
                isa::encode_i(fixup.opcode, fixup.rd, static_cast<std::int32_t>(offset));
            break;
        }
        case Fixup::Type::Call: {
            const long long current = static_cast<long long>(fixup.instruction_index + 1);
            const long long target_words = target_address / 4;
            const long long offset = target_words - current;
            validate_range(offset, 21, fixup.line, "call");
            program_.code[fixup.instruction_index] =
                isa::encode_call(fixup.opcode, static_cast<std::int32_t>(offset));
            break;
        }
        case Fixup::Type::Movi: {
            validate_range(target_address, 21, fixup.line, "immediate");
            std::uint32_t encoded = isa::encode_i(fixup.opcode, fixup.rd, static_cast<std::int32_t>(target_address));
            const std::uint32_t existing = program_.code[fixup.instruction_index];
            const std::uint32_t rn_bits = existing & 0x03E0000u;
            program_.code[fixup.instruction_index] = encoded | rn_bits;
            break;
        }
        }
    }
}

AssembledProgram assemble_program(const std::string& source) {
    std::istringstream input(source);
    return assemble_program(input);
}

AssembledProgram assemble_program(std::istream& input) {
    Assembler assembler;
    return assembler.run(input, "<memory>");
}
