#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <set>
#include <vector>

class GPU;

class CPU {
public:
    explicit CPU(GPU* gpu = nullptr);
    void reset();

    void connect_gpu(GPU* gpu) noexcept;

    void load_program(const std::vector<std::uint32_t>& program);
    void load_data(std::size_t address, const std::vector<std::uint8_t>& data);
    void set_breakpoints(std::set<std::size_t> points);
    void add_watch(std::size_t address, std::size_t length);
    void set_trace(std::ostream* out);
    void enable_step_mode(bool enabled);

    void step();
    void step_debug();
    void run(std::size_t max_steps = 1'000'000);

    bool is_halted() const noexcept;

    uint64_t get_reg(int index) const;
    void set_reg(int index, uint64_t value);

    static constexpr std::size_t MEMORY_SIZE = 2 << 20;  // 2 MiB

    struct MMIORegion {
        std::size_t start;
        std::size_t length;
        std::function<std::uint64_t(std::size_t)> read;
        std::function<void(std::size_t, std::uint64_t)> write;
    };

    void map_region(MMIORegion region);
    void unmap_region(std::size_t start, std::size_t length);

    void request_interrupt(std::uint8_t vector);

    const std::array<std::uint8_t, MEMORY_SIZE>& memory_snapshot() const noexcept { return memory; }
    std::size_t program_counter() const noexcept { return pc; }

private:
    enum class Opcode : std::uint8_t;

    uint32_t fetch() const;
    void execute(uint32_t instruction);
    void validate_reg(int index) const;
    std::uint64_t read_u64(std::size_t address) const;
    void write_u64(std::size_t address, std::uint64_t value);
    uint32_t read_u32(std::size_t address) const;
    void write_u32(std::size_t address, uint32_t value);
    std::uint16_t read_u16(std::size_t address) const;
    void write_u16(std::size_t address, std::uint16_t value);
    std::uint8_t read_u8(std::size_t address) const;
    void write_u8(std::size_t address, std::uint8_t value);
    bool handle_mmio_read(std::size_t address, std::size_t size, std::uint64_t& value) const;
    bool handle_mmio_write(std::size_t address, std::size_t size, std::uint64_t value);
    void check_watchpoints(std::size_t address, std::size_t size, std::uint64_t value, bool is_write) const;
    void trace_instruction(uint32_t instruction) const;

    std::array<uint64_t, 32> regs{};  // Simplified register file
    std::array<std::uint8_t, MEMORY_SIZE> memory{};
    std::size_t pc = 0;
    std::size_t program_end = 0;
    bool halted = false;
    GPU* gpu = nullptr;
    std::set<std::size_t> breakpoints_;
    std::vector<std::pair<std::size_t, std::size_t>> watchpoints_;
    std::vector<MMIORegion> mmio_regions_;
    std::ostream* trace_stream_ = nullptr;
    bool step_mode_ = false;
    std::uint64_t last_cmp_ = 0;
    bool last_cmp_negative_ = false;
    bool interrupt_pending_ = false;
    std::uint8_t interrupt_vector_ = 0;
    void enter_interrupt();

    struct GPURegisterState {
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::uint64_t width = 0;
        std::uint64_t height = 0;
        std::uint64_t color = 0;
        std::uint64_t texture_id = 0;
        std::uint64_t mode = 0;
    } gpu_regs_;
};
