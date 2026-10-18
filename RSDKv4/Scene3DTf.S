# RSDKv4-ps1 (docs/43 C4): the special stage's GTE transform loop (PS1TransformProject3D, RSDKv4/Scene3D.cpp) in
# assembly: three vertices a command (RTPT), relative to the frame's origin, as the C loop: a triple with a coordinate
# past +-2^20 from the origin, a GTE flag (bit 31) or a depth under 256 (units of 32) is left to the C code.
#
#   int PS1TransformAsm(PS1TfState *s, int v)   (layout: Scene3D.cpp, PS1TfState; offsets below)
# From vertex v (a multiple of 3), every triple whose three vertices the GTE can do gets .u = SXY, .v = SZ << 5 in
# g_ps1Proj, exactly as the C loop. Returns the index of the first triple it did not do (the C code does it, then calls
# back) or end (the last whole triple's end). The GTE's matrix, translation, offset and H are set by the caller.
# MIPS I rules as Scene3DOT.S: no load result used by the next instruction, mfc2 / cfc2 the same, two instructions
# between an mtc2 and the command, no GTE instruction or load in a delay slot.

    .set    noreorder
    .set    noat
    .text
    .globl  PS1TransformAsm
    .type   PS1TransformAsm, @function

T_VB        = 0     # const Vertex *: vertex 0 (20 bytes: x, y, z, u, v)
T_PJ        = 4     # PS1ProjEntry *: g_ps1Proj (8 bytes: u, v)
T_END       = 8     # the last whole triple's end (count - count % 3)
T_OX        = 12    # the origin (vertex 0's position)
T_OY        = 16
T_OZ        = 20

# registers: s0 state, s1 vertex, s2 vertex end, s3 projection entry, s4 / s5 / s6 origin x / y / z, s7 2^20, fp index;
# per triple t0-t8 the nine coordinates (then their offsets), a0-a3 / v0 / v1 / t9 / at scratch.

PS1TransformAsm:
    addiu   $sp, $sp, -40
    sw      $s0, 0($sp)
    sw      $s1, 4($sp)
    sw      $s2, 8($sp)
    sw      $s3, 12($sp)
    sw      $s4, 16($sp)
    sw      $s5, 20($sp)
    sw      $s6, 24($sp)
    sw      $s7, 28($sp)
    sw      $fp, 32($sp)
    move    $s0, $a0
    move    $fp, $a1
    lw      $s1, T_VB($s0)
    lw      $s3, T_PJ($s0)
    lw      $t1, T_END($s0)
    sll     $t0, $a1, 2             # v * 4
    sll     $t2, $a1, 4             # v * 16
    addu    $t2, $t2, $t0           # v * 20
    addu    $s1, $s1, $t2           # &vb[v]
    sll     $t0, $a1, 3
    addu    $s3, $s3, $t0           # &pj[v]
    sll     $t0, $t1, 2
    sll     $t2, $t1, 4
    addu    $t2, $t2, $t0
    lw      $t3, T_VB($s0)
    lw      $s4, T_OX($s0)
    addu    $s2, $t3, $t2           # &vb[end]
    lw      $s5, T_OY($s0)
    lw      $s6, T_OZ($s0)
    lui     $s7, 0x10               # 2^20
    sltu    $at, $s1, $s2
    beqz    $at, T_ret
    nop

    .balign 16
T_tri:
    lw      $t0, 0($s1)             # x0
    lw      $t1, 4($s1)             # y0
    lw      $t2, 8($s1)             # z0
    subu    $t0, $t0, $s4           # dx0
    lw      $t3, 20($s1)            # x1
    subu    $t1, $t1, $s5           # dy0
    lw      $t4, 24($s1)            # y1
    subu    $t2, $t2, $s6           # dz0
    lw      $t5, 28($s1)            # z1
    subu    $t3, $t3, $s4           # dx1
    lw      $t6, 40($s1)            # x2
    subu    $t4, $t4, $s5           # dy1
    lw      $t7, 44($s1)            # y2
    subu    $t5, $t5, $s6           # dz1
    lw      $t8, 48($s1)            # z2
    subu    $t6, $t6, $s4           # dx2
    subu    $t7, $t7, $s5           # dy2
    subu    $t8, $t8, $s6           # dz2
    # every offset within +-2^20: (d + 2^20) < 2^21 for all nine (unsigned, as the C loop's OR)
    addu    $a0, $t0, $s7
    addu    $a1, $t1, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t2, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t3, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t4, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t5, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t6, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t7, $s7
    or      $a0, $a0, $a1
    addu    $a1, $t8, $s7
    or      $a0, $a0, $a1
    srl     $a0, $a0, 21
    bnez    $a0, T_ret              # a coordinate past 2^20: the C code (the CPU) does this triple
    # the GTE's input in units of 32: VXYn = (dx >> 5) & 0xFFFF | (dy >> 5) << 16, VZn = dz >> 5
    sra     $t0, $t0, 5
    sra     $t1, $t1, 5
    andi    $t0, $t0, 0xFFFF
    sll     $t1, $t1, 16
    or      $t0, $t0, $t1
    sra     $t2, $t2, 5
    mtc2    $t0, $0                 # VXY0
    mtc2    $t2, $1                 # VZ0
    sra     $t3, $t3, 5
    sra     $t4, $t4, 5
    andi    $t3, $t3, 0xFFFF
    sll     $t4, $t4, 16
    or      $t3, $t3, $t4
    sra     $t5, $t5, 5
    mtc2    $t3, $2                 # VXY1
    mtc2    $t5, $3                 # VZ1
    sra     $t6, $t6, 5
    sra     $t7, $t7, 5
    andi    $t6, $t6, 0xFFFF
    sll     $t7, $t7, 16
    or      $t6, $t6, $t7
    sra     $t8, $t8, 5
    mtc2    $t6, $4                 # VXY2
    mtc2    $t8, $5                 # VZ2
    nop
    nop
    cop2    0x0280030               # RTPT
    cfc2    $a0, $31                # FLAG
    mfc2    $a1, $17                # SZ1
    mfc2    $a2, $18                # SZ2
    mfc2    $a3, $19                # SZ3
    bltz    $a0, T_ret              # a GTE flag: the C code redoes the triple one vertex at a time
    sltiu   $t0, $a1, 256
    sltiu   $t1, $a2, 256
    sltiu   $t2, $a3, 256
    or      $t0, $t0, $t1
    or      $t0, $t0, $t2
    bnez    $t0, T_ret              # a vertex near the camera: the same
    sll     $a1, $a1, 5
    swc2    $12, 0($s3)             # .u = SXY0
    swc2    $13, 8($s3)
    swc2    $14, 16($s3)
    sll     $a2, $a2, 5
    sll     $a3, $a3, 5
    sw      $a1, 4($s3)             # .v = SZ << 5
    sw      $a2, 12($s3)
    sw      $a3, 20($s3)
    addiu   $s1, $s1, 60
    addiu   $s3, $s3, 24
    bne     $s1, $s2, T_tri
    addiu   $fp, $fp, 3
T_ret:
    move    $v0, $fp
    lw      $s0, 0($sp)
    lw      $s1, 4($sp)
    lw      $s2, 8($sp)
    lw      $s3, 12($sp)
    lw      $s4, 16($sp)
    lw      $s5, 20($sp)
    lw      $s6, 24($sp)
    lw      $s7, 28($sp)
    lw      $fp, 32($sp)
    jr      $ra
    addiu   $sp, $sp, 40
    .size   PS1TransformAsm, .-PS1TransformAsm
