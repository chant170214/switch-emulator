#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "assembler.h"
#include "cpu.h"
#include "gpu.h"
#include "gui.h"
#include "homebrew_package.h"
#include "nro_package.h"

namespace {

enum class TraceMode { None, Stdout, File };

struct Options {
    std::optional<std::string> homebrew_path;
    bool read_stdin = false;
    std::optional<std::string> dump_path;
    bool debug = false;
    bool trace_stdout = false;
    std::optional<std::string> trace_path;
    std::vector<std::string> breakpoint_specs;
    std::vector<std::string> watch_specs;
    std::size_t max_steps = 1'000'000;
    bool treat_as_package = false;
    std::optional<std::string> emit_package_path;
    std::optional<std::string> emit_nro_path;
    bool show_ascii = false;
    bool ascii_color = true;
    std::size_t ascii_width = 80;
    bool show_gui = false;
};

struct TraceState {
    TraceMode preferred = TraceMode::None;
    TraceMode current = TraceMode::None;
    std::string file_path;
    std::ofstream file;
};

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
    return value;
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [program.(asm|thb|nro) | --stdin] [--dump framebuffer.ppm] [--debug] [--trace [path]]"
                         " [--break symbol] [--watch spec] [--max-steps N] [--package] [--emit-homebrew file]"
                         " [--emit-nro file] [--show-ansi] [--show-ansi-mono] [--show-ansi-width cols] [--gui]\n";
            return std::nullopt;
        } else if (arg == "--dump") {
            if (i + 1 >= argc) {
                std::cerr << "--dump requires a path argument\n";
                return std::nullopt;
            }
            opts.dump_path = argv[++i];
        } else if (arg == "--debug" || arg == "--step") {
            opts.debug = true;
        } else if (arg == "--trace") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opts.trace_path = argv[++i];
            } else {
                opts.trace_stdout = true;
            }
        } else if (arg == "--trace-file") {
            if (i + 1 >= argc) {
                std::cerr << "--trace-file requires a path\n";
                return std::nullopt;
            }
            opts.trace_path = argv[++i];
        } else if (arg == "--break") {
            if (i + 1 >= argc) {
                std::cerr << "--break requires a symbol or address\n";
                return std::nullopt;
            }
            opts.breakpoint_specs.push_back(argv[++i]);
        } else if (arg == "--watch") {
            if (i + 1 >= argc) {
                std::cerr << "--watch requires a spec (symbol[:len])\n";
                return std::nullopt;
            }
            opts.watch_specs.push_back(argv[++i]);
        } else if (arg == "--max-steps") {
            if (i + 1 >= argc) {
                std::cerr << "--max-steps requires a numeric value\n";
                return std::nullopt;
            }
            try {
                const std::size_t value = std::stoull(argv[++i]);
                opts.max_steps = value;
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --max-steps\n";
                return std::nullopt;
            }
        } else if (arg == "--package" || arg == "--binary") {
            opts.treat_as_package = true;
        } else if (arg == "--emit-homebrew") {
            if (i + 1 >= argc) {
                std::cerr << "--emit-homebrew requires a path\n";
                return std::nullopt;
            }
            opts.emit_package_path = argv[++i];
        } else if (arg == "--emit-nro") {
            if (i + 1 >= argc) {
                std::cerr << "--emit-nro requires a path\n";
                return std::nullopt;
            }
            opts.emit_nro_path = argv[++i];
        } else if (arg == "--show-ansi") {
            opts.show_ascii = true;
            opts.ascii_color = true;
        } else if (arg == "--show-ansi-mono") {
            opts.show_ascii = true;
            opts.ascii_color = false;
        } else if (arg == "--show-ansi-width") {
            if (i + 1 >= argc) {
                std::cerr << "--show-ansi-width requires a numeric column count\n";
                return std::nullopt;
            }
            try {
                std::size_t width = std::stoull(argv[++i]);
                if (width == 0) {
                    std::cerr << "--show-ansi-width must be greater than zero\n";
                    return std::nullopt;
                }
                opts.ascii_width = width;
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --show-ansi-width\n";
                return std::nullopt;
            }
        } else if (arg == "--stdin") {
            if (opts.homebrew_path) {
                std::cerr << "Cannot combine --stdin with an explicit program path.\n";
                return std::nullopt;
            }
            if (opts.read_stdin) {
                std::cerr << "--stdin specified multiple times.\n";
                return std::nullopt;
            }
            opts.read_stdin = true;
        } else if (arg == "--gui") {
            opts.show_gui = true;
        } else if (!opts.homebrew_path) {
            opts.homebrew_path = arg;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }
    return opts;
}

long long parse_symbol_term(const std::string& term, const AssembledProgram& program) {
    auto sym = program.symbols.find(term);
    if (sym != program.symbols.end()) {
        return static_cast<long long>(sym->second);
    }
    std::size_t consumed = 0;
    long long value = std::stoll(term, &consumed, 0);
    if (consumed != term.size()) {
        throw std::runtime_error("Unrecognized symbol term: " + term);
    }
    return value;
}

std::optional<std::size_t> parse_address(const std::string& spec, const AssembledProgram& program) {
    long long value = 0;
    int sign = 1;
    std::string token;
    for (char ch : spec) {
        if (ch == '+' || ch == '-') {
            if (token.empty()) {
                return std::nullopt;
            }
            value += sign * parse_symbol_term(token, program);
            token.clear();
            sign = (ch == '+') ? 1 : -1;
        } else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) {
        value += sign * parse_symbol_term(token, program);
    }
    if (value < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

void print_registers(const CPU& cpu) {
    std::cout << "Registers:" << std::hex << std::setfill('0');
    for (int i = 0; i < 32; i += 4) {
        std::cout << "\n R" << std::setw(2) << i << ": 0x" << std::setw(16) << cpu.get_reg(i)
                  << "  R" << std::setw(2) << i + 1 << ": 0x" << std::setw(16) << cpu.get_reg(i + 1)
                  << "  R" << std::setw(2) << i + 2 << ": 0x" << std::setw(16) << cpu.get_reg(i + 2)
                  << "  R" << std::setw(2) << i + 3 << ": 0x" << std::setw(16) << cpu.get_reg(i + 3);
    }
    std::cout << std::dec << std::setfill(' ') << "\n";
}

void dump_memory(const CPU& cpu, std::size_t address, std::size_t length) {
    const auto& mem = cpu.memory_snapshot();
    if (address + length > mem.size()) {
        length = mem.size() - address;
    }
    for (std::size_t offset = 0; offset < length; offset += 16) {
        std::cout << std::hex << std::setw(6) << std::setfill('0') << (address + offset) << ": ";
        std::string ascii;
        for (std::size_t i = 0; i < 16 && offset + i < length; ++i) {
            const std::uint8_t byte = mem[address + offset + i];
            std::cout << std::setw(2) << static_cast<int>(byte) << ' ';
            ascii.push_back(std::isprint(byte) ? static_cast<char>(byte) : '.');
        }
        std::cout << " |" << ascii << "|\n";
    }
    std::cout << std::dec;
}

void debug_repl(CPU& cpu, GPU& gpu, std::set<std::size_t>& breakpoints, const AssembledProgram& program,
                std::size_t max_steps, TraceState& trace_state) {
    cpu.enable_step_mode(true);
    bool running = true;
    while (running && !cpu.is_halted()) {
        std::cout << "(emu) " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string command;
        iss >> command;
        if (command == "help") {
            std::cout << "Commands: help, step/s, continue/c, run, regs, mem <addr> [len], break <addr>, watch <addr> <len>,"
                         " trace on/off/stdout/file <path>, trace status, frame, quit\n";
        } else if (command == "step" || command == "s") {
            cpu.step_debug();
            std::cout << "PC=0x" << std::hex << cpu.program_counter() << std::dec << "\n";
        } else if (command == "continue" || command == "c" || command == "run") {
            cpu.enable_step_mode(false);
            cpu.run(max_steps);
            if (!cpu.is_halted()) {
                cpu.enable_step_mode(true);
                std::cout << "Paused at PC=0x" << std::hex << cpu.program_counter() << std::dec << "\n";
            }
        } else if (command == "regs") {
            print_registers(cpu);
        } else if (command == "mem") {
            std::string addr_token;
            iss >> addr_token;
            if (addr_token.empty()) {
                std::cout << "mem requires address\n";
                continue;
            }
            std::size_t length = 64;
            if (!(iss >> length)) {
                length = 64;
            }
            auto address = parse_address(addr_token, program);
            if (!address) {
                std::cout << "Invalid address spec\n";
                continue;
            }
            dump_memory(cpu, *address, length);
        } else if (command == "break") {
            std::string spec;
            iss >> spec;
            if (spec.empty()) {
                std::cout << "break requires address or symbol\n";
                continue;
            }
            auto addr = parse_address(spec, program);
            if (!addr) {
                std::cout << "Invalid breakpoint spec\n";
                continue;
            }
            breakpoints.insert(*addr);
            cpu.set_breakpoints(breakpoints);
            std::cout << "Breakpoint added at 0x" << std::hex << *addr << std::dec << "\n";
        } else if (command == "watch") {
            std::string spec;
            std::size_t length = 8;
            iss >> spec >> length;
            if (spec.empty()) {
                std::cout << "watch requires address and optional length\n";
                continue;
            }
            auto addr = parse_address(spec, program);
            if (!addr) {
                std::cout << "Invalid watch spec\n";
                continue;
            }
            cpu.add_watch(*addr, length);
            std::cout << "Watch added at 0x" << std::hex << *addr << std::dec << " length " << length << " bytes\n";
        } else if (command == "trace") {
            std::string mode;
            iss >> mode;
            if (mode == "off") {
                cpu.set_trace(nullptr);
                trace_state.current = TraceMode::None;
                std::cout << "Instruction trace disabled\n";
            } else if (mode == "on" || mode == "resume") {
                if (trace_state.preferred == TraceMode::Stdout) {
                    cpu.set_trace(&std::cout);
                    trace_state.current = TraceMode::Stdout;
                    std::cout << "Instruction trace enabled on stdout\n";
                } else if (trace_state.preferred == TraceMode::File) {
                    if (!trace_state.file.is_open() && !trace_state.file_path.empty()) {
                        trace_state.file.clear();
                        trace_state.file.open(trace_state.file_path.c_str());
                    }
                    if (trace_state.file.is_open()) {
                        cpu.set_trace(&trace_state.file);
                        trace_state.current = TraceMode::File;
                        std::cout << "Instruction trace writing to '" << trace_state.file_path << "'\n";
                    } else {
                        std::cout << "Unable to reopen trace file. Specify a new path via 'trace file <path>'.\n";
                    }
                } else {
                    std::cout << "No previous trace destination. Use 'trace stdout' or 'trace file <path>'.\n";
                }
            } else if (mode == "stdout") {
                cpu.set_trace(&std::cout);
                trace_state.preferred = TraceMode::Stdout;
                trace_state.current = TraceMode::Stdout;
                std::cout << "Instruction trace enabled on stdout\n";
            } else if (mode == "file") {
                std::string path;
                iss >> path;
                if (path.empty()) {
                    if (trace_state.file_path.empty()) {
                        std::cout << "trace file requires a path\n";
                        continue;
                    }
                    path = trace_state.file_path;
                }
                trace_state.file.close();
                trace_state.file.clear();
                trace_state.file.open(path.c_str());
                if (!trace_state.file) {
                    std::cout << "Failed to open trace file '" << path << "'\n";
                    trace_state.current = TraceMode::None;
                    continue;
                }
                trace_state.file_path = path;
                cpu.set_trace(&trace_state.file);
                trace_state.preferred = TraceMode::File;
                trace_state.current = TraceMode::File;
                std::cout << "Instruction trace writing to '" << path << "'\n";
            } else if (mode == "status") {
                switch (trace_state.current) {
                case TraceMode::Stdout:
                    std::cout << "Trace active on stdout\n";
                    break;
                case TraceMode::File:
                    std::cout << "Trace active on file '" << trace_state.file_path << "'\n";
                    break;
                case TraceMode::None:
                default:
                    std::cout << "Trace disabled";
                    if (trace_state.preferred == TraceMode::Stdout) {
                        std::cout << "; last destination stdout";
                    } else if (trace_state.preferred == TraceMode::File && !trace_state.file_path.empty()) {
                        std::cout << "; last destination '" << trace_state.file_path << "'";
                    }
                    std::cout << "\n";
                    break;
                }
            } else {
                std::cout << "Usage: trace on/off/stdout/file <path>/status\n";
            }
        } else if (command == "quit" || command == "exit") {
            running = false;
        } else if (command == "frame") {
            std::cout << "Framebuffer checksum: 0x" << std::hex << gpu.checksum() << std::dec << "\n";
        } else {
            std::cout << "Unknown command. Type 'help' for a list of commands.\n";
        }
    }
}

AssembledProgram builtin_demo() {
    const char* source = R"(;
.section data
.org 0x200
palette:
    .word 0xFF101020 0xFF3060A0 0xFF60C0FF 0xFFFFCC33

.org 0x400
rect_desc:
    .word 8 6 28 16 0xFF3060A0
line_desc:
    .word 0 0 63 35 0xFFFFFFFF
tri_desc:
    .word 6 30 24 12 50 20 0xFFFFCC33 1

sprite_pixels:
    .word 0xFFFF0000 0x00000000 0xFFFF0000 0x00000000
    .word 0x00000000 0xFFFF0000 0x00000000 0xFFFF0000
    .word 0xFFFF0000 0x00000000 0xFFFF0000 0x00000000
    .word 0x00000000 0xFFFF0000 0x00000000 0xFFFF0000

tex_desc:
    .word 4 4 sprite_pixels

sprite_desc:
    .word 0 40 10 4 4
    .byte 0 0

dma_desc:
    .word sprite_pixels 4 4 4 30 22

.section code
    MOVI R0, 0x001020
    GPUFILL R0

    MOVI R1, rect_desc
    GPURECT R1

    MOVI R1, line_desc
    GPULINE R1

    MOVI R1, tri_desc
    GPUTRI R1

    MOVI R10, tex_desc
    GPUTEX R10, R2

    MOVI R11, sprite_desc
    MOVI R3, 0
    ADD R3, R2, R3
    MOVI R4, 8
    STOREB R3, R11, 0
    SHR R3, R3, R4
    STOREB R3, R11, 1
    SHR R3, R3, R4
    STOREB R3, R11, 2
    SHR R3, R3, R4
    STOREB R3, R11, 3
    GPUSPRITE R11

    MOVI R1, dma_desc
    DMABLT R1

    HALT
)";
    std::istringstream input(source);
    return assemble_program(input);
}

}  // namespace

int main(int argc, char** argv) {
    const auto parsed = parse_args(argc, argv);
    if (!parsed) {
        return 0;
    }

    GPU gpu(128, 72);
    CPU cpu(&gpu);
    cpu.reset();

    AssembledProgram program;

    if (parsed->read_stdin && parsed->treat_as_package) {
        std::cerr << "--package cannot be combined with --stdin because stdin always supplies raw assembly.\n";
        return 1;
    }

    if (parsed->read_stdin) {
        std::stringstream source;
        source << std::cin.rdbuf();
        std::istringstream input(source.str());
        try {
            program = assemble_program(input);
        } catch (const std::exception& ex) {
            std::cerr << "Assembly from stdin failed: " << ex.what() << "\n";
            return 1;
        }
        std::cout << "Loaded homebrew program from stdin with " << program.code.size() << " instructions.\n";
    } else if (parsed->homebrew_path) {
        const std::string& homebrew_path = *parsed->homebrew_path;
        const bool path_is_thb = homebrew::is_package_path(homebrew_path);
        const bool path_is_nro = toy_nro::is_package_path(homebrew_path);
        const bool treat_as_package = parsed->treat_as_package || path_is_thb || path_is_nro;
        if (treat_as_package) {
            std::vector<std::string> load_errors;
            auto try_load_thb = [&]() -> bool {
                try {
                    program = homebrew::load_package(homebrew_path);
                    std::cout << "Loaded Toy Homebrew Binary '" << homebrew_path << "' with " << program.code.size()
                              << " instructions and " << program.data_segments.size() << " data patches.\n";
                    return true;
                } catch (const std::exception& ex) {
                    load_errors.emplace_back(std::string("THB loader: ") + ex.what());
                    return false;
                }
            };
            auto try_load_nro = [&]() -> bool {
                try {
                    program = toy_nro::load_package(homebrew_path);
                    std::cout << "Loaded Toy NRO '" << homebrew_path << "' with " << program.code.size()
                              << " instructions and " << program.data_segments.size() << " data patches.\n";
                    return true;
                } catch (const std::exception& ex) {
                    load_errors.emplace_back(std::string("NRO loader: ") + ex.what());
                    return false;
                }
            };

            bool loaded = false;
            if (path_is_nro) {
                loaded = try_load_nro();
            }
            if (!loaded && path_is_thb) {
                loaded = try_load_thb();
            }
            if (!loaded && !path_is_nro && !path_is_thb) {
                loaded = try_load_thb();
                if (!loaded) {
                    loaded = try_load_nro();
                }
            }

            if (!loaded) {
                std::cerr << "Failed to load packaged homebrew '" << homebrew_path
                          << "'. Tried the following loaders:\n";
                for (const auto& err : load_errors) {
                    std::cerr << "  - " << err << "\n";
                }
                return 1;
            }
        } else {
            std::ifstream file(homebrew_path.c_str());
            if (!file) {
                std::cerr << "Failed to open homebrew file: " << homebrew_path << "\n";
                return 1;
            }
            try {
                program = assemble_program(file);
            } catch (const std::exception& ex) {
                std::cerr << "Assembly failed: " << ex.what() << "\n";
                return 1;
            }
            std::cout << "Loaded homebrew program from '" << homebrew_path << "' with " << program.code.size()
                      << " instructions.\n";
        }
    } else {
        std::cout << "No program specified; running built-in showcase.\n";
        program = builtin_demo();
    }

    if (parsed->emit_package_path) {
        try {
            homebrew::save_package(program, *parsed->emit_package_path);
            std::cout << "Wrote Toy Homebrew Binary to '" << *parsed->emit_package_path << "'.\n";
        } catch (const std::exception& ex) {
            std::cerr << "Failed to write Toy Homebrew Binary: " << ex.what() << "\n";
            return 1;
        }
    }

    if (parsed->emit_nro_path) {
        try {
            toy_nro::save_package(program, *parsed->emit_nro_path);
            std::cout << "Wrote Toy NRO to '" << *parsed->emit_nro_path << "'.\n";
        } catch (const std::exception& ex) {
            std::cerr << "Failed to write Toy NRO: " << ex.what() << "\n";
            return 1;
        }
    }

    cpu.load_program(program.code);
    for (const auto& patch : program.data_segments) {
        cpu.load_data(patch.address, patch.bytes);
    }

    std::set<std::size_t> breakpoint_addresses;
    for (const auto& spec : parsed->breakpoint_specs) {
        try {
            auto addr = parse_address(spec, program);
            if (!addr) {
                std::cerr << "Invalid breakpoint spec: " << spec << "\n";
                continue;
            }
            breakpoint_addresses.insert(*addr);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to parse breakpoint '" << spec << "': " << ex.what() << "\n";
        }
    }
    cpu.set_breakpoints(breakpoint_addresses);

    for (const auto& spec : parsed->watch_specs) {
        const auto colon = spec.find(':');
        std::string addr_part = spec.substr(0, colon);
        std::size_t length = 8;
        if (colon != std::string::npos) {
            try {
                length = static_cast<std::size_t>(std::stoull(spec.substr(colon + 1), nullptr, 0));
            } catch (const std::exception&) {
                std::cerr << "Invalid watch length in spec: " << spec << "\n";
                continue;
            }
        }
        try {
            auto addr = parse_address(addr_part, program);
            if (!addr) {
                std::cerr << "Invalid watch spec: " << spec << "\n";
                continue;
            }
            cpu.add_watch(*addr, length);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to parse watch '" << spec << "': " << ex.what() << "\n";
        }
    }

    TraceState trace_state;
    if (parsed->trace_path) {
        trace_state.file_path = *parsed->trace_path;
        trace_state.file.open(trace_state.file_path.c_str());
        if (!trace_state.file) {
            std::cerr << "Failed to open trace file: " << trace_state.file_path << "\n";
        } else {
            cpu.set_trace(&trace_state.file);
            trace_state.preferred = TraceMode::File;
            trace_state.current = TraceMode::File;
        }
    } else if (parsed->trace_stdout) {
        cpu.set_trace(&std::cout);
        trace_state.preferred = TraceMode::Stdout;
        trace_state.current = TraceMode::Stdout;
    }

    if (parsed->debug) {
        debug_repl(cpu, gpu, breakpoint_addresses, program, parsed->max_steps, trace_state);
    } else {
        cpu.run(parsed->max_steps);
    }

    std::cout << "Program halted: " << std::boolalpha << cpu.is_halted() << "\n";
    const auto checksum = gpu.checksum();
    std::cout << "Framebuffer checksum: 0x" << std::hex << checksum << std::dec << "\n";

    if (parsed->dump_path) {
        try {
            const std::string path = gpu.dump_to_ppm(*parsed->dump_path);
            std::cout << "Framebuffer dumped to " << path << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "Failed to dump framebuffer: " << ex.what() << "\n";
            return 1;
        }
    }

    if (parsed->show_ascii) {
        std::cout << "\n[Framebuffer preview]\n";
        std::cout << gpu.render_ascii_art(parsed->ascii_width, parsed->ascii_color);
    }

    if (parsed->show_gui) {
        GuiViewer viewer(gpu.width(), gpu.height());
        if (!viewer.present(gpu.framebuffer())) {
            std::cerr << "GUI preview failed. Ensure an X11 server is available.\n";
        }
    }

    return 0;
}
