; -------------------------------------------------------------
; Toy Switch-Style Homebrew Template
; -------------------------------------------------------------
; Copy this file to my_program.asm (or any name you like) and then
; customize the GPU calls below. Every directive used here is supported
; by the built-in assembler, so it is a great starting point for your
; own experiments.
;
; How to run:
;   1. Build the emulator:  cmake -S . -B build && cmake --build build
;   2. Execute this program: ./build/switch-emulator my_program.asm --show-ansi
;   3. Optional: pipe it directly without saving using `--stdin`:
;        cat my_program.asm | ./build/switch-emulator --stdin --show-ansi
;
; Registers ---------------------------------------------------
; R0-R29  -> general-purpose
; R30     -> link register (CALL/RET)
; R31     -> stack pointer
;
; You can define your own constants or macros here.
.section data
palette:
    .word 0xFF0B3954 ; deep blue
    .word 0xFFE4E4E4 ; light gray
    .word 0xFFFFC857 ; accent

.section code
start:
    ; Clear the framebuffer to palette[0]
    MOVI R1, 0
    LOAD R0, R1, palette
    GPUFILL R0

    ; Draw a rectangle using palette[1]
    MOVI R2, rect_desc
    GPURECT R2

    ; Set a single pixel using palette[2]
    MOVI R3, 160          ; x
    MOVI R4, 90           ; y
    LOAD R5, R1, palette+8 ; color
    GPUDRAW R3, R4, R5

    HALT

rect_desc:
    .word 64  ; x
    .word 64  ; y
    .word 96  ; width
    .word 64  ; height
    .word 0xFFE4E4E4 ; RGBA color
