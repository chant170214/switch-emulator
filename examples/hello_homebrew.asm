; Demonstration program showing the expanded instruction set and GPU helpers

.section data
.org 0x500
color_gradient:
    .word 0x000008 0x00000F 0x00001F 0x00003F

.org 0x600
rect_desc:
    .word 12 8 42 22 0xFF1188CC
line_desc:
    .word 0 0 127 71 0xFFFFFFFF
tri_desc:
    .word 20 52 60 24 96 40 0xFFFFAA33 1

checker_pixels:
    .word 0xFFFF0000 0x00000000 0xFFFF0000 0x00000000
    .word 0x00000000 0xFFFF0000 0x00000000 0xFFFF0000
    .word 0xFFFF0000 0x00000000 0xFFFF0000 0x00000000
    .word 0x00000000 0xFFFF0000 0x00000000 0xFFFF0000

tex_desc:
    .word 4 4 checker_pixels

sprite_desc:
    .word 0 80 16 4 4
    .byte 0 0

dma_desc:
    .word checker_pixels 4 4 4 48 30

.section code
    ; Paint background with a subtle gradient
    MOVI r0, 0x001020
    GPUFILL r0

    MOVI r1, rect_desc
    GPURECT r1

    MOVI r1, line_desc
    GPULINE r1

    MOVI r1, tri_desc
    GPUTRI r1

    ; Upload the checkerboard texture and draw it as a sprite
    MOVI r10, tex_desc
    GPUTEX r10, r2
    MOVI r11, sprite_desc
    MOVI r3, 0
    ADD r3, r2, r3
    MOVI r4, 8
    STOREB r3, r11, 0
    SHR r3, r3, r4
    STOREB r3, r11, 1
    SHR r3, r3, r4
    STOREB r3, r11, 2
    SHR r3, r3, r4
    STOREB r3, r11, 3
    GPUSPRITE r11

    ; DMA blit another copy elsewhere on the framebuffer
    MOVI r1, dma_desc
    DMABLT r1

    HALT
