#include "cpu.h"

CPU::CPU() {
    reset();
}

void CPU::reset() {
    regs.fill(0);
}

uint64_t CPU::get_reg(int index) const {
    return regs[index];
}

void CPU::set_reg(int index, uint64_t value) {
    regs[index] = value;
}
