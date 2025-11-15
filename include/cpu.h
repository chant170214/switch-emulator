#pragma once
#include <array>
#include <cstdint>

class CPU {
public:
    CPU();
    void reset();

    uint64_t get_reg(int index) const;
    void set_reg(int index, uint64_t value);

private:
    std::array<uint64_t, 32> regs;  // ARM64 has 31 + XZR, but we use 32 slots
};
