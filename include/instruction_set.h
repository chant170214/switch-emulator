#pragma once

#include <cstdint>

namespace isa {

enum : std::uint8_t {
    OP_NOP = 0,
    OP_ADD = 1,
    OP_SUB = 2,
    OP_MOVI = 3,
    OP_LOAD = 4,
    OP_STORE = 5,
    OP_GPUDRAW = 6,
    OP_GPUFILL = 7,
    OP_BRANCH = 8,
    OP_MUL = 9,
    OP_DIV = 10,
    OP_AND = 11,
    OP_OR = 12,
    OP_XOR = 13,
    OP_NOT = 14,
    OP_SHL = 15,
    OP_SHR = 16,
    OP_ADDI = 17,
    OP_SUBI = 18,
    OP_LOADB = 19,
    OP_STOREB = 20,
    OP_LOADW = 21,
    OP_STOREW = 22,
    OP_BRANCHZ = 23,
    OP_BRANCHNZ = 24,
    OP_BRANCHGT = 25,
    OP_BRANCHLT = 26,
    OP_PUSH = 27,
    OP_POP = 28,
    OP_CALL = 29,
    OP_RET = 30,
    OP_INT = 31,
    OP_GPURECT = 32,
    OP_GPULINE = 33,
    OP_GPUTRI = 34,
    OP_GPUSETTARGET = 35,
    OP_GPUFLIP = 36,
    OP_GPUTEX = 37,
    OP_DMABLT = 38,
    OP_CMP = 39,
    OP_GPUSPRITE = 40,
    OP_HALT = 63,
};

constexpr std::uint32_t encode_r(std::uint8_t opcode, std::uint8_t rd, std::uint8_t rn, std::uint8_t rm) {
    return (static_cast<std::uint32_t>(opcode) << 26) |
           (static_cast<std::uint32_t>(rd & 0x1F) << 21) |
           (static_cast<std::uint32_t>(rn & 0x1F) << 16) |
           (static_cast<std::uint32_t>(rm & 0x1F) << 11);
}

constexpr std::uint32_t encode_i(std::uint8_t opcode, std::uint8_t rd, std::int32_t imm21) {
    const std::uint32_t masked = static_cast<std::uint32_t>(imm21) & 0x1FFFFF;
    return (static_cast<std::uint32_t>(opcode) << 26) |
           (static_cast<std::uint32_t>(rd & 0x1F) << 21) |
           masked;
}

constexpr std::uint32_t encode_mem(std::uint8_t opcode, std::uint8_t rd, std::uint8_t rn, std::int32_t imm11) {
    const std::uint32_t masked = static_cast<std::uint32_t>(imm11) & 0x7FFu;
    return (static_cast<std::uint32_t>(opcode) << 26) |
           (static_cast<std::uint32_t>(rd & 0x1F) << 21) |
           (static_cast<std::uint32_t>(rn & 0x1F) << 16) |
           masked;
}

constexpr std::uint32_t encode_single(std::uint8_t opcode, std::uint8_t rd) {
    return (static_cast<std::uint32_t>(opcode) << 26) |
           (static_cast<std::uint32_t>(rd & 0x1F) << 21);
}

constexpr std::uint32_t encode_call(std::uint8_t opcode, std::int32_t imm21) {
    const std::uint32_t masked = static_cast<std::uint32_t>(imm21) & 0x1FFFFF;
    return (static_cast<std::uint32_t>(opcode) << 26) | masked;
}

constexpr std::uint32_t encode_gpu_immediate(std::uint8_t opcode, std::uint8_t rd, std::uint16_t imm16) {
    return (static_cast<std::uint32_t>(opcode) << 26) |
           (static_cast<std::uint32_t>(rd & 0x1F) << 21) |
           static_cast<std::uint32_t>(imm16);
}

}  // namespace isa
