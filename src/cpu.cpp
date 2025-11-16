#include "cpu.h"

#include "gpu.h"
#include "instruction_set.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t OPCODE_SHIFT = 26;
constexpr std::uint32_t OPCODE_MASK = 0x3Fu << OPCODE_SHIFT;
constexpr std::uint32_t REGISTER_MASK = 0x1Fu;
constexpr std::uint32_t IMM21_MASK = 0x1FFFFFu;
constexpr std::uint32_t IMM11_MASK = 0x7FFu;

constexpr std::size_t STACK_POINTER = 31;
constexpr std::size_t LINK_REGISTER = 30;
constexpr std::size_t INTERRUPT_VECTOR_BASE = 0x100;
constexpr std::size_t INTERRUPT_VECTOR_STRIDE = 16;
constexpr std::size_t GPU_MMIO_BASE = 0x001F0000;
constexpr std::size_t GPU_MMIO_SIZE = 0x100;

inline std::int32_t sign_extend(std::uint32_t value, int bits) {
    const std::uint32_t mask = 1u << (bits - 1);
    value &= (1u << bits) - 1;
    if (value & mask) {
        value |= ~((1u << bits) - 1);
    }
    return static_cast<std::int32_t>(value);
}

inline std::uint32_t to_u32(std::uint64_t value) {
    return static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
}

inline std::string hex64(std::uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

}  // namespace

enum class CPU::Opcode : std::uint8_t {
    NOP = isa::OP_NOP,
    ADD = isa::OP_ADD,
    SUB = isa::OP_SUB,
    MOVI = isa::OP_MOVI,
    LOAD = isa::OP_LOAD,
    STORE = isa::OP_STORE,
    GPUDRAW = isa::OP_GPUDRAW,
    GPUFILL = isa::OP_GPUFILL,
    BRANCH = isa::OP_BRANCH,
    MUL = isa::OP_MUL,
    DIV = isa::OP_DIV,
    AND = isa::OP_AND,
    OR = isa::OP_OR,
    XOR = isa::OP_XOR,
    NOT = isa::OP_NOT,
    SHL = isa::OP_SHL,
    SHR = isa::OP_SHR,
    ADDI = isa::OP_ADDI,
    SUBI = isa::OP_SUBI,
    LOADB = isa::OP_LOADB,
    STOREB = isa::OP_STOREB,
    LOADW = isa::OP_LOADW,
    STOREW = isa::OP_STOREW,
    BRANCHZ = isa::OP_BRANCHZ,
    BRANCHNZ = isa::OP_BRANCHNZ,
    BRANCHGT = isa::OP_BRANCHGT,
    BRANCHLT = isa::OP_BRANCHLT,
    PUSH = isa::OP_PUSH,
    POP = isa::OP_POP,
    CALL = isa::OP_CALL,
    RET = isa::OP_RET,
    INT = isa::OP_INT,
    GPURECT = isa::OP_GPURECT,
    GPULINE = isa::OP_GPULINE,
    GPUTRI = isa::OP_GPUTRI,
    GPUSETTARGET = isa::OP_GPUSETTARGET,
    GPUFLIP = isa::OP_GPUFLIP,
    GPUTEX = isa::OP_GPUTEX,
    DMABLT = isa::OP_DMABLT,
    CMP = isa::OP_CMP,
    GPUSPRITE = isa::OP_GPUSPRITE,
    HALT = isa::OP_HALT,
};

CPU::CPU(GPU* gpu_ptr) : gpu(gpu_ptr) {
    reset();
    if (gpu) {
        map_region({GPU_MMIO_BASE,
                    GPU_MMIO_SIZE,
                    [this](std::size_t offset) -> std::uint64_t {
                        switch (offset) {
                        case 0x00:
                            return gpu_regs_.x;
                        case 0x08:
                            return gpu_regs_.y;
                        case 0x10:
                            return gpu_regs_.width;
                        case 0x18:
                            return gpu_regs_.height;
                        case 0x20:
                            return gpu_regs_.color;
                        case 0x28:
                            return gpu_regs_.texture_id;
                        case 0x30:
                            return gpu_regs_.mode;
                        default:
                            return 0;
                        }
                    },
                    [this](std::size_t offset, std::uint64_t value) {
                        switch (offset) {
                        case 0x00:
                            gpu_regs_.x = value;
                            break;
                        case 0x08:
                            gpu_regs_.y = value;
                            break;
                        case 0x10:
                            gpu_regs_.width = value;
                            break;
                        case 0x18:
                            gpu_regs_.height = value;
                            break;
                        case 0x20:
                            gpu_regs_.color = value;
                            break;
                        case 0x28:
                            gpu_regs_.texture_id = value;
                            break;
                        case 0x30:
                            gpu_regs_.mode = value;
                            break;
                        case 0x38:
                            if (!gpu) {
                                throw std::runtime_error("GPU command issued without connected GPU");
                            }
                            switch (value) {
                            case 1:
                                gpu->draw_pixel(static_cast<int>(gpu_regs_.x), static_cast<int>(gpu_regs_.y),
                                                static_cast<std::uint32_t>(gpu_regs_.color));
                                break;
                            case 2:
                                gpu->fill(static_cast<std::uint32_t>(gpu_regs_.color));
                                break;
                            case 3:
                                gpu->submit_rectangle(static_cast<int>(gpu_regs_.x), static_cast<int>(gpu_regs_.y),
                                                      static_cast<int>(gpu_regs_.width),
                                                      static_cast<int>(gpu_regs_.height),
                                                      static_cast<std::uint32_t>(gpu_regs_.color));
                                break;
                            case 4:
                                gpu->draw_line(static_cast<int>(gpu_regs_.x), static_cast<int>(gpu_regs_.y),
                                               static_cast<int>(gpu_regs_.width), static_cast<int>(gpu_regs_.height),
                                               static_cast<std::uint32_t>(gpu_regs_.color));
                                break;
                            case 5:
                                gpu->draw_triangle(static_cast<int>(gpu_regs_.x), static_cast<int>(gpu_regs_.y),
                                                   static_cast<int>(gpu_regs_.width),
                                                   static_cast<int>(gpu_regs_.height),
                                                   static_cast<int>(gpu_regs_.texture_id >> 32),
                                                   static_cast<int>(gpu_regs_.texture_id & 0xFFFFFFFFu),
                                                   static_cast<std::uint32_t>(gpu_regs_.color));
                                break;
                            case 6:
                                gpu->set_render_target(static_cast<std::size_t>(gpu_regs_.mode));
                                break;
                            case 7:
                                gpu->swap_buffers();
                                break;
                            case 8: {
                                const bool depth = (gpu_regs_.mode & 0x1u) != 0;
                                gpu->set_depth_test(depth);
                                if (gpu_regs_.mode & 0x2u) {
                                    gpu->clear_depth();
                                }
                                break;
                            }
                            default:
                                throw std::runtime_error("Unknown GPU MMIO command");
                            }
                            break;
                        default:
                            break;
                        }
                    }});
    }
}

void CPU::reset() {
    regs.fill(0);
    memory.fill(0);
    pc = 0;
    program_end = 0;
    halted = false;
    step_mode_ = false;
    last_cmp_ = 0;
    last_cmp_negative_ = false;
    interrupt_pending_ = false;
    interrupt_vector_ = 0;
    regs[STACK_POINTER] = MEMORY_SIZE - 16;
    regs[LINK_REGISTER] = 0;
}

void CPU::connect_gpu(GPU* gpu_ptr) noexcept {
    gpu = gpu_ptr;
}

void CPU::load_program(const std::vector<std::uint32_t>& program) {
    if (program.size() * sizeof(std::uint32_t) > memory.size()) {
        throw std::runtime_error("Program does not fit in memory");
    }

    std::fill(memory.begin(), memory.end(), 0);
    for (std::size_t i = 0; i < program.size(); ++i) {
        write_u32(i * sizeof(std::uint32_t), program[i]);
    }

    pc = 0;
    program_end = program.size() * sizeof(std::uint32_t);
    halted = false;
}

void CPU::load_data(std::size_t address, const std::vector<std::uint8_t>& data) {
    if (address + data.size() > memory.size()) {
        throw std::runtime_error("Data load exceeds memory bounds");
    }
    std::copy(data.begin(), data.end(), memory.begin() + static_cast<std::ptrdiff_t>(address));
}

void CPU::set_breakpoints(std::set<std::size_t> points) {
    breakpoints_ = std::move(points);
}

void CPU::add_watch(std::size_t address, std::size_t length) {
    watchpoints_.push_back({address, length});
}

void CPU::set_trace(std::ostream* out) {
    trace_stream_ = out;
}

void CPU::enable_step_mode(bool enabled) {
    step_mode_ = enabled;
}

void CPU::step() {
    if (halted) {
        return;
    }
    if (interrupt_pending_) {
        enter_interrupt();
    }
    if (pc + sizeof(std::uint32_t) > memory.size()) {
        throw std::runtime_error("Program counter out of bounds");
    }

    const std::size_t current_pc = pc;
    const std::uint32_t instruction = fetch();
    pc += sizeof(std::uint32_t);
    trace_instruction(instruction);
    execute(instruction);

    if (breakpoints_.count(pc) > 0) {
        step_mode_ = true;
    }

    if (current_pc == pc && !halted) {
        // Prevent infinite loops caused by instructions failing to advance PC
        pc += sizeof(std::uint32_t);
    }
}

void CPU::step_debug() {
    const bool prev = step_mode_;
    step_mode_ = false;
    step();
    step_mode_ = prev;
}

void CPU::run(std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps && !halted; ++i) {
        if (step_mode_) {
            return;
        }
        if (breakpoints_.count(pc) > 0) {
            step_mode_ = true;
            return;
        }
        step();
    }
}

bool CPU::is_halted() const noexcept {
    return halted;
}

uint64_t CPU::get_reg(int index) const {
    validate_reg(index);
    return regs[static_cast<std::size_t>(index)];
}

void CPU::set_reg(int index, uint64_t value) {
    validate_reg(index);
    regs[static_cast<std::size_t>(index)] = value;
}

void CPU::map_region(MMIORegion region) {
    mmio_regions_.push_back(std::move(region));
}

void CPU::unmap_region(std::size_t start, std::size_t length) {
    mmio_regions_.erase(std::remove_if(mmio_regions_.begin(), mmio_regions_.end(), [&](const MMIORegion& region) {
                                   return region.start == start && region.length == length;
                               }),
                        mmio_regions_.end());
}

void CPU::request_interrupt(std::uint8_t vector) {
    interrupt_pending_ = true;
    interrupt_vector_ = vector;
}

uint32_t CPU::fetch() const {
    return read_u32(pc);
}

void CPU::execute(uint32_t instruction) {
    const auto opcode = static_cast<Opcode>((instruction & OPCODE_MASK) >> OPCODE_SHIFT);
    const std::uint8_t rd = static_cast<std::uint8_t>((instruction >> 21) & REGISTER_MASK);
    const std::uint8_t rn = static_cast<std::uint8_t>((instruction >> 16) & REGISTER_MASK);
    const std::uint8_t rm = static_cast<std::uint8_t>((instruction >> 11) & REGISTER_MASK);

    auto reg = [&](std::uint8_t index) -> uint64_t& {
        validate_reg(index);
        return regs[index];
    };

    auto read_s32 = [&](std::size_t address) {
        return static_cast<std::int32_t>(read_u32(address));
    };

    switch (opcode) {
    case Opcode::NOP:
        break;
    case Opcode::ADD:
        reg(rd) = reg(rn) + reg(rm);
        break;
    case Opcode::SUB:
        reg(rd) = reg(rn) - reg(rm);
        break;
    case Opcode::MUL:
        reg(rd) = reg(rn) * reg(rm);
        break;
    case Opcode::DIV:
        if (reg(rm) == 0) {
            throw std::runtime_error("Division by zero");
        }
        reg(rd) = reg(rn) / reg(rm);
        break;
    case Opcode::AND:
        reg(rd) = reg(rn) & reg(rm);
        break;
    case Opcode::OR:
        reg(rd) = reg(rn) | reg(rm);
        break;
    case Opcode::XOR:
        reg(rd) = reg(rn) ^ reg(rm);
        break;
    case Opcode::NOT:
        reg(rd) = ~reg(rn);
        break;
    case Opcode::SHL:
        reg(rd) = reg(rn) << (reg(rm) & 63);
        break;
    case Opcode::SHR:
        reg(rd) = reg(rn) >> (reg(rm) & 63);
        break;
    case Opcode::ADDI: {
        const std::int32_t imm = sign_extend(instruction & IMM21_MASK, 21);
        reg(rd) = reg(rd) + static_cast<std::int64_t>(imm);
        break;
    }
    case Opcode::SUBI: {
        const std::int32_t imm = sign_extend(instruction & IMM21_MASK, 21);
        reg(rd) = reg(rd) - static_cast<std::int64_t>(imm);
        break;
    }
    case Opcode::MOVI: {
        const std::int32_t imm = sign_extend(instruction & IMM21_MASK, 21);
        reg(rd) = static_cast<std::uint64_t>(imm);
        break;
    }
    case Opcode::LOAD: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        reg(rd) = read_u64(static_cast<std::size_t>(address));
        break;
    }
    case Opcode::STORE: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        write_u64(static_cast<std::size_t>(address), reg(rd));
        break;
    }
    case Opcode::LOADB: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        reg(rd) = read_u8(static_cast<std::size_t>(address));
        break;
    }
    case Opcode::STOREB: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        write_u8(static_cast<std::size_t>(address), static_cast<std::uint8_t>(reg(rd) & 0xFFu));
        break;
    }
    case Opcode::LOADW: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        reg(rd) = read_u16(static_cast<std::size_t>(address));
        break;
    }
    case Opcode::STOREW: {
        const std::int32_t offset = sign_extend(instruction & IMM11_MASK, 11);
        const std::uint64_t address = reg(rn) + static_cast<std::int64_t>(offset);
        write_u16(static_cast<std::size_t>(address), static_cast<std::uint16_t>(reg(rd) & 0xFFFFu));
        break;
    }
    case Opcode::GPUDRAW: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        gpu->draw_pixel(static_cast<int>(reg(rd)), static_cast<int>(reg(rn)),
                        static_cast<std::uint32_t>(reg(rm)));
        break;
    }
    case Opcode::GPUFILL: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        gpu->fill(static_cast<std::uint32_t>(reg(rd)));
        break;
    }
    case Opcode::GPURECT: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        const int x = read_s32(address);
        const int y = read_s32(address + 4);
        const int w = read_s32(address + 8);
        const int h = read_s32(address + 12);
        const std::uint32_t color = read_u32(address + 16);
        gpu->submit_rectangle(x, y, w, h, color);
        break;
    }
    case Opcode::GPULINE: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        const int x0 = read_s32(address);
        const int y0 = read_s32(address + 4);
        const int x1 = read_s32(address + 8);
        const int y1 = read_s32(address + 12);
        const std::uint32_t color = read_u32(address + 16);
        gpu->draw_line(x0, y0, x1, y1, color);
        break;
    }
    case Opcode::GPUTRI: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        const int x0 = read_s32(address);
        const int y0 = read_s32(address + 4);
        const int x1 = read_s32(address + 8);
        const int y1 = read_s32(address + 12);
        const int x2 = read_s32(address + 16);
        const int y2 = read_s32(address + 20);
        const std::uint32_t color = read_u32(address + 24);
        const int fill = read_s32(address + 28);
        gpu->draw_triangle(x0, y0, x1, y1, x2, y2, color, fill != 0);
        break;
    }
    case Opcode::GPUSPRITE: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        SpriteDescriptor sprite;
        sprite.texture_id = static_cast<std::size_t>(read_u32(address));
        sprite.x = read_s32(address + 4);
        sprite.y = read_s32(address + 8);
        sprite.width = read_s32(address + 12);
        sprite.height = read_s32(address + 16);
        sprite.flip_x = read_u8(address + 20) != 0;
        sprite.flip_y = read_u8(address + 21) != 0;
        gpu->draw_sprite(sprite);
        break;
    }
    case Opcode::GPUSETTARGET: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        gpu->set_render_target(static_cast<std::size_t>(reg(rd)));
        break;
    }
    case Opcode::GPUFLIP: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        gpu->swap_buffers();
        break;
    }
    case Opcode::GPUTEX: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        const std::size_t width = static_cast<std::size_t>(read_u32(address));
        const std::size_t height = static_cast<std::size_t>(read_u32(address + 4));
        const std::size_t data_addr = static_cast<std::size_t>(read_u32(address + 8));
        std::vector<std::uint32_t> pixels(width * height);
        for (std::size_t i = 0; i < width * height; ++i) {
            pixels[i] = read_u32(data_addr + i * 4);
        }
        reg(rn) = gpu->upload_texture(width, height, pixels);
        break;
    }
    case Opcode::DMABLT: {
        if (!gpu) {
            throw std::runtime_error("GPU not connected");
        }
        const std::size_t address = static_cast<std::size_t>(reg(rd));
        const std::size_t src_addr = static_cast<std::size_t>(read_u32(address));
        const std::size_t stride = static_cast<std::size_t>(read_u32(address + 4));
        const std::size_t width = static_cast<std::size_t>(read_u32(address + 8));
        const std::size_t height = static_cast<std::size_t>(read_u32(address + 12));
        const int dst_x = read_s32(address + 16);
        const int dst_y = read_s32(address + 20);
        std::vector<std::uint32_t> buffer(stride * height);
        for (std::size_t row = 0; row < height; ++row) {
            for (std::size_t col = 0; col < stride; ++col) {
                buffer[row * stride + col] = read_u32(src_addr + (row * stride + col) * 4);
            }
        }
        gpu->dma_blit(buffer, stride, dst_x, dst_y, width, height);
        break;
    }
    case Opcode::BRANCH: {
        const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
        const std::int64_t byte_offset = static_cast<std::int64_t>(offset_words) * static_cast<std::int64_t>(sizeof(std::uint32_t));
        const std::int64_t next_pc = static_cast<std::int64_t>(pc) + byte_offset;
        if (next_pc < 0 || static_cast<std::size_t>(next_pc) > program_end) {
            throw std::runtime_error("BRANCH target out of bounds");
        }
        pc = static_cast<std::size_t>(next_pc);
        break;
    }
    case Opcode::BRANCHZ: {
        if (reg(rd) == 0) {
            const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
            const std::int64_t byte_offset = static_cast<std::int64_t>(offset_words) * 4;
            pc = static_cast<std::size_t>(static_cast<std::int64_t>(pc) + byte_offset);
        }
        break;
    }
    case Opcode::BRANCHNZ: {
        if (reg(rd) != 0) {
            const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
            const std::int64_t byte_offset = static_cast<std::int64_t>(offset_words) * 4;
            pc = static_cast<std::size_t>(static_cast<std::int64_t>(pc) + byte_offset);
        }
        break;
    }
    case Opcode::BRANCHGT: {
        if (!last_cmp_negative_ && last_cmp_ != 0) {
            const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
            pc = static_cast<std::size_t>(static_cast<std::int64_t>(pc) + static_cast<std::int64_t>(offset_words) * 4);
        }
        break;
    }
    case Opcode::BRANCHLT: {
        if (last_cmp_negative_) {
            const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
            pc = static_cast<std::size_t>(static_cast<std::int64_t>(pc) + static_cast<std::int64_t>(offset_words) * 4);
        }
        break;
    }
    case Opcode::PUSH: {
        std::uint64_t& sp = reg(STACK_POINTER);
        sp -= 8;
        write_u64(static_cast<std::size_t>(sp), reg(rd));
        break;
    }
    case Opcode::POP: {
        std::uint64_t& sp = reg(STACK_POINTER);
        reg(rd) = read_u64(static_cast<std::size_t>(sp));
        sp += 8;
        break;
    }
    case Opcode::CALL: {
        const std::int32_t offset_words = sign_extend(instruction & IMM21_MASK, 21);
        const std::int64_t target = static_cast<std::int64_t>(pc) + static_cast<std::int64_t>(offset_words) * 4;
        if (target < 0 || static_cast<std::size_t>(target) >= memory.size()) {
            throw std::runtime_error("CALL target out of bounds");
        }
        regs[LINK_REGISTER] = pc;
        pc = static_cast<std::size_t>(target);
        break;
    }
    case Opcode::RET:
        pc = static_cast<std::size_t>(regs[LINK_REGISTER]);
        break;
    case Opcode::INT:
        request_interrupt(static_cast<std::uint8_t>(rd));
        break;
    case Opcode::CMP: {
        const std::int64_t lhs = static_cast<std::int64_t>(reg(rn));
        const std::int64_t rhs = static_cast<std::int64_t>(reg(rm));
        const std::int64_t result = lhs - rhs;
        last_cmp_ = static_cast<std::uint64_t>(result);
        last_cmp_negative_ = result < 0;
        break;
    }
    case Opcode::HALT:
        halted = true;
        break;
    }
}

void CPU::validate_reg(int index) const {
    if (index < 0 || index >= static_cast<int>(regs.size())) {
        throw std::out_of_range("Invalid register index");
    }
}

std::uint64_t CPU::read_u64(std::size_t address) const {
    std::uint64_t value = 0;
    if (handle_mmio_read(address, sizeof(std::uint64_t), value)) {
        return value;
    }
    if (address + sizeof(std::uint64_t) > memory.size()) {
        throw std::runtime_error("read_u64 out of bounds");
    }
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(memory[address + static_cast<std::size_t>(i)]) << (i * 8);
    }
    check_watchpoints(address, sizeof(std::uint64_t), value, false);
    return value;
}

void CPU::write_u64(std::size_t address, std::uint64_t value) {
    if (handle_mmio_write(address, sizeof(std::uint64_t), value)) {
        return;
    }
    if (address + sizeof(std::uint64_t) > memory.size()) {
        throw std::runtime_error("write_u64 out of bounds");
    }
    for (int i = 0; i < 8; ++i) {
        memory[address + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    check_watchpoints(address, sizeof(std::uint64_t), value, true);
}

uint32_t CPU::read_u32(std::size_t address) const {
    std::uint64_t value = 0;
    if (handle_mmio_read(address, sizeof(std::uint32_t), value)) {
        return static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
    }
    if (address + sizeof(std::uint32_t) > memory.size()) {
        throw std::runtime_error("read_u32 out of bounds");
    }
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(memory[address + static_cast<std::size_t>(i)]) << (i * 8);
    }
    check_watchpoints(address, sizeof(std::uint32_t), result, false);
    return result;
}

void CPU::write_u32(std::size_t address, uint32_t value) {
    if (handle_mmio_write(address, sizeof(std::uint32_t), value)) {
        return;
    }
    if (address + sizeof(std::uint32_t) > memory.size()) {
        throw std::runtime_error("write_u32 out of bounds");
    }
    for (int i = 0; i < 4; ++i) {
        memory[address + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    check_watchpoints(address, sizeof(std::uint32_t), value, true);
}

std::uint16_t CPU::read_u16(std::size_t address) const {
    std::uint64_t value = 0;
    if (handle_mmio_read(address, sizeof(std::uint16_t), value)) {
        return static_cast<std::uint16_t>(value & 0xFFFFu);
    }
    if (address + sizeof(std::uint16_t) > memory.size()) {
        throw std::runtime_error("read_u16 out of bounds");
    }
    std::uint16_t result = 0;
    for (int i = 0; i < 2; ++i) {
        result |= static_cast<std::uint16_t>(memory[address + static_cast<std::size_t>(i)]) << (i * 8);
    }
    check_watchpoints(address, sizeof(std::uint16_t), result, false);
    return result;
}

void CPU::write_u16(std::size_t address, std::uint16_t value) {
    if (handle_mmio_write(address, sizeof(std::uint16_t), value)) {
        return;
    }
    if (address + sizeof(std::uint16_t) > memory.size()) {
        throw std::runtime_error("write_u16 out of bounds");
    }
    for (int i = 0; i < 2; ++i) {
        memory[address + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    check_watchpoints(address, sizeof(std::uint16_t), value, true);
}

std::uint8_t CPU::read_u8(std::size_t address) const {
    std::uint64_t value = 0;
    if (handle_mmio_read(address, sizeof(std::uint8_t), value)) {
        return static_cast<std::uint8_t>(value & 0xFFu);
    }
    if (address >= memory.size()) {
        throw std::runtime_error("read_u8 out of bounds");
    }
    const std::uint8_t result = memory[address];
    check_watchpoints(address, sizeof(std::uint8_t), result, false);
    return result;
}

void CPU::write_u8(std::size_t address, std::uint8_t value) {
    if (handle_mmio_write(address, sizeof(std::uint8_t), value)) {
        return;
    }
    if (address >= memory.size()) {
        throw std::runtime_error("write_u8 out of bounds");
    }
    memory[address] = value;
    check_watchpoints(address, sizeof(std::uint8_t), value, true);
}

bool CPU::handle_mmio_read(std::size_t address, std::size_t size, std::uint64_t& value) const {
    for (const auto& region : mmio_regions_) {
        if (address >= region.start && address + size <= region.start + region.length) {
            if (region.read) {
                value = region.read(address - region.start);
                return true;
            }
        }
    }
    return false;
}

bool CPU::handle_mmio_write(std::size_t address, std::size_t size, std::uint64_t value) {
    for (const auto& region : mmio_regions_) {
        if (address >= region.start && address + size <= region.start + region.length) {
            if (region.write) {
                region.write(address - region.start, value);
                return true;
            }
        }
    }
    return false;
}

void CPU::check_watchpoints(std::size_t address, std::size_t size, std::uint64_t value, bool is_write) const {
    if (!trace_stream_) {
        return;
    }
    for (const auto& [start, length] : watchpoints_) {
        const std::size_t end = start + length;
        const std::size_t access_end = address + size;
        if (!(access_end <= start || address >= end)) {
            std::ostringstream oss;
            oss << (is_write ? "WRITE" : "READ") << " @0x" << std::hex << address << " = " << hex64(value);
            (*trace_stream_) << oss.str() << '\n';
        }
    }
}

void CPU::trace_instruction(uint32_t instruction) const {
    if (!trace_stream_) {
        return;
    }
    std::ostringstream oss;
    oss << "PC=0x" << std::hex << (pc - 4) << " INST=0x" << std::setw(8) << std::setfill('0') << instruction;
    (*trace_stream_) << oss.str() << '\n';
}

void CPU::enter_interrupt() {
    interrupt_pending_ = false;
    const std::size_t vector_address = INTERRUPT_VECTOR_BASE + static_cast<std::size_t>(interrupt_vector_) * INTERRUPT_VECTOR_STRIDE;
    const std::size_t handler = static_cast<std::size_t>(read_u64(vector_address));
    regs[LINK_REGISTER] = pc;
    std::uint64_t& sp = regs[STACK_POINTER];
    sp -= 8;
    write_u64(static_cast<std::size_t>(sp), pc);
    pc = handler;
}
