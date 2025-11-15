#include <iostream>
#include "cpu.h"

int main() {
    CPU cpu;

    cpu.reset();
    std::cout << "Switch Emulator Started.\n";

    // simple test: write value to register 0
    cpu.set_reg(0, 12345);
    std::cout << "R0 = " << cpu.get_reg(0) << "\n";

    return 0;
}
