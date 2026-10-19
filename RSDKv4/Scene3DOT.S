# RSDKv4-ps1 (docs/43 C1): the faded fast path of PS1Draw3DSceneOT (RSDKv4/Scene3D.cpp) in assembly.
# GCC's loop spilled ~10 values to the stack every face and padded the GTE accesses with nops (~157 instructions, 23
# loads, 11 stores for a drawn face). This keeps the pass's state in registers. Model: GTA3-ps1 ps1/re3/tripass.S.
#
#   int PS1OTFadedAsm(PS1OTFastState *s, int i)   (layout: Scene3D.cpp, PS1OTFastState; offsets below)
# From face i to the last (returns count): every face that takes the fast path (flag FACE_FLAG_FADED, all four corners
# projected by the GTE) is done exactly as the C code did it: its depth (the sum of the four .v >> 2) into the draw
# list's record and lo / hi, upstream's fog and DrawFadedFace early-outs, the degenerate test, the strip order from the
# same NCLIP side tests, the fog colour, the flat quad written at s->w (unlinked; record .first = its header word). Any
# other face gets its depth, lo / hi and record here and goes to C (PS1OTOtherC: closes the open run, the general
# paths); a quad ready to write while s->w == s->wEnd (no open run, or a full one) has PS1OTOpenRun open one first.
# Writes back lo, hi, the colour cache, w and the culled count. Also here: PS1OTLinkAsm, the second pass.
#
# Exact shortcuts over the C loop (same results):
# - the fog colour is a pure function of (colour, alpha): two cache entries instead of one (the pipe alternates two
#   colours: one entry missed half the faces);
# - a pair of side tests passes when the two NCLIPs have opposite signs, both nonzero: the sign of their 64-bit
#   product (mult, HI);
# - s1 = s3 = 0 with a != c puts b and d on the line a-c: all four corners are collinear, every later side test is 0,
#   so the order is a b c d without the other four NCLIPs (a fifth of the drawn faces: tiny distant ones).
#
# MIPS I rules kept by hand (noreorder inserts nothing): a load's result is not used by the next instruction; mfc2's
# neither; two instructions between an mtc2 and the GTE command that reads it; two instructions between an mfhi / mflo
# and the next mult; no GTE instruction and no load in a branch delay slot (the arm64 dynarec bug of docs/34: a load in
# a delay slot).

    .set    noreorder
    .set    noat
    .text
    .globl  PS1OTFadedAsm
    .type   PS1OTFadedAsm, @function

# state offsets (static_assert'ed in Scene3D.cpp)
S_FB        = 0     # const Face *: face 0 (24 bytes: a, b, c, d, color, flag)
S_REC       = 4     # PS1OTRec *: record 0 (8 bytes: depth, first)
S_PJ        = 8     # const PS1ProjEntry *: g_ps1Proj (8 bytes: u = SXY, v = depth z)
S_COUNT     = 12
S_LO        = 16
S_HI        = 20
S_CCOLOR    = 24    # colour cache entry 1: face colour, alpha, GP0 28h command word
S_CALPHA    = 28
S_CCMD      = 32
S_W         = 36    # uint32_t *: the open bulk run's next primitive (its first word; the header is at -4)
S_WEND      = 40
S_FOGMAX    = 44    # fogStrength
S_SKIPAT    = 48    # draw ? 255 : INT_MIN: a face is skipped when its clamped fog >= this (!draw || alpha < 1)
S_QXN       = 52    # -(SCREEN_XSIZE + 1)
S_QYN       = 56    # -((SCREEN_YSIZE + 1) << 16) (the asm uses the constant: SCREEN_YSIZE is 240)
S_FOGRB     = 60    # the fog colour's 5-bit R | B << 16
S_FOGG      = 64
S_CULLED    = 68
S_PENDING   = 72
S_PW        = 76    # 5 words: command, then the strip's four corners
S_C2COLOR   = 96    # colour cache entry 2: colour, alpha << 24 | the command's low 24 bits (alpha 0: empty)
S_C2AR      = 100
S_RUN       = 104   # C only (PS1OTOpenRun / PS1OTOtherC): the open bulk run, the quads written in closed runs, the
S_DRAWN     = 108   # sheet and draw flag PS1OTOtherFace takes
QYN_HI      = 0xFF0F    # -(241 << 16) >> 16

# registers: s0 state, s1 face, s2 face end, s3 record, s4 g_ps1Proj, s5 lo, s6 hi, s7 / fp / t8 cache entry 1 (colour,
# alpha, command), a3 / ra cache entry 2, t9 w, v1 wEnd, a1 fogMax, a2 skipAt, a0 qxn, v0 culled; per face t0-t3 the
# corners pa pb pc pd (packed SXY), t4 depth then the first side then the command to write, t5 colour, t6 the second
# side, t7 alpha, at scratch. The writes store (t1, t0, t2, t3): the strip order of the first test (b a c d); the other
# orders swap registers.

PS1OTFadedAsm:
    addiu   $sp, $sp, -88
    sw      $s0, 16($sp)
    sw      $s1, 20($sp)
    sw      $s2, 24($sp)
    sw      $s3, 28($sp)
    sw      $s4, 32($sp)
    sw      $s5, 36($sp)
    sw      $s6, 40($sp)
    sw      $s7, 44($sp)
    sw      $fp, 48($sp)
    sw      $ra, 52($sp)
    move    $s0, $a0
    lw      $s1, S_FB($s0)
    lw      $s3, S_REC($s0)
    lw      $s2, S_COUNT($s0)
    sll     $t0, $a1, 3             # i * 8
    sll     $t1, $a1, 4             # i * 16
    addu    $s3, $s3, $t0           # &rec[i]
    addu    $t1, $t1, $t0           # i * 24
    addu    $s1, $s1, $t1           # &face[i]
    sll     $t0, $s2, 3
    sll     $t1, $s2, 4
    addu    $t1, $t1, $t0
    lw      $t2, S_FB($s0)
    lw      $s4, S_PJ($s0)
    addu    $s2, $t2, $t1           # &face[count]
    lw      $s5, S_LO($s0)
    lw      $s6, S_HI($s0)
    lw      $s7, S_CCOLOR($s0)
    lw      $fp, S_CALPHA($s0)
    lw      $t8, S_CCMD($s0)
    lw      $a3, S_C2COLOR($s0)
    lw      $ra, S_C2AR($s0)
    lw      $t9, S_W($s0)
    lw      $v1, S_WEND($s0)
    lw      $a1, S_FOGMAX($s0)
    lw      $a2, S_SKIPAT($s0)
    lw      $a0, S_QXN($s0)
    lw      $v0, S_CULLED($s0)
    sltu    $at, $s1, $s2
    beqz    $at, L_ret
    nop

    .balign 16
L_face:
    lw      $t0, 0($s1)             # f.a
    lw      $t1, 4($s1)             # f.b
    lw      $t2, 8($s1)             # f.c
    sll     $t0, $t0, 3
    lw      $t3, 12($s1)            # f.d
    sll     $t1, $t1, 3
    addu    $t0, $s4, $t0
    sll     $t2, $t2, 3
    addu    $t1, $s4, $t1
    lw      $t4, 4($t0)             # A.v
    addu    $t2, $s4, $t2
    lw      $t5, 4($t1)             # B.v
    sll     $t3, $t3, 3
    lw      $t6, 4($t2)             # C.v
    addu    $t3, $s4, $t3
    lw      $t7, 4($t3)             # D.v
    lw      $t0, 0($t0)             # pa = A.u
    lw      $t1, 0($t1)             # pb
    lw      $t2, 0($t2)             # pc
    lw      $t3, 0($t3)             # pd
    addu    $t4, $t4, $t5
    lw      $t5, 20($s1)            # f.flag
    addu    $t4, $t4, $t6
    addu    $t4, $t4, $t7           # A.v + B.v + C.v + D.v
    # fast path: flag FADED and every corner on the GTE (x + 1024 has bits 11-15 clear; a CPU corner's x is 0x8000)
    addiu   $t6, $t0, 1024
    addiu   $t7, $t1, 1024
    or      $t6, $t6, $t7
    addiu   $t7, $t2, 1024
    or      $t6, $t6, $t7
    addiu   $t7, $t3, 1024
    or      $t6, $t6, $t7
    andi    $t6, $t6, 0xF800
    addiu   $t5, $t5, -4            # FACE_FLAG_FADED
    or      $t6, $t6, $t5
    bnez    $t6, L_other            # not fast: the C code does this face (PS1OTOtherC)
    sra     $t4, $t4, 2             # depth (Sort3DDrawList's)
    mtc2    $t0, $12                # SXY0 = pa
    mtc2    $t2, $13                # SXY1 = pc
    mtc2    $t1, $14                # SXY2 = pb
    slt     $at, $t4, $s5
    beqz    $at, 1f
    sw      $t4, 0($s3)             # rec.depth
    move    $s5, $t4                # lo
1:  cop2    0x1400006               # NCLIP: s1 = side(pa, pc, pb)
    slt     $at, $s6, $t4
    beqz    $at, 2f
    addiu   $t5, $t4, -32768        # depth - 0x8000
    move    $s6, $t4                # hi
2:  sra     $t5, $t5, 8             # fogStr
    sra     $t6, $t5, 31
    nor     $t6, $t6, $zero
    and     $t5, $t5, $t6           # max(fogStr, 0)
    subu    $t6, $t5, $a1
    sra     $t7, $t6, 31
    and     $t6, $t6, $t7
    addu    $t5, $a1, $t6           # min(fogStr, fogMax)
    mfc2    $t4, $24                # s1 = MAC0 (the depth is stored: t4 is free)
    mtc2    $t3, $14                # SXY2 = pd
    slt     $at, $t5, $a2
    beqz    $at, L_skip             # !draw || alpha < 1
    li      $t7, 255
    cop2    0x1400006               # NCLIP: s3 = side(pa, pc, pd)
    bltz    $t5, L_neg              # fogStr < 0 (fogMax < 0): alpha > 255 -> 255
    subu    $t7, $t7, $t5           # alpha = 255 - fogStr
L_alpha:
    # A face wholly off the 320 x 240 screen is written anyway: its corners are within the GTE's -1024..1023, so the GPU
    # clips it to the drawing area (nothing drawn, a few GPU cycles); the CPU tests cost ~25 instructions a face (docs/43
    # C1: pictures identical, chain words not). Degenerate faces (all x or all y equal) are culled in L_notA: such a face
    # has all four corners collinear, so s1 = s3 = 0 (the test only runs then).
    mfc2    $t6, $24                # s3 = MAC0
    lw      $t5, 16($s1)            # f.color
    mult    $t4, $t6                # strip order: diagonal a-c when s1 * s3 < 0: b a c d
    mfhi    $at
    bgez    $at, L_notA
    nop
L_ccheck:
    bne     $t5, $s7, L_c2
    move    $t4, $t8                # entry 1's command (the write stores t4)
    bne     $t7, $fp, L_c2
    nop
L_write:
    beq     $t9, $v1, L_pending     # no open run / the run is full: PS1OTOpenRun opens one
    addiu   $t6, $t9, -4            # the primitive's header word
    sw      $t4, 0($t9)
    sw      $t1, 4($t9)
    sw      $t0, 8($t9)
    sw      $t2, 12($t9)
    sw      $t3, 16($t9)
    sw      $t6, 4($s3)             # rec.first
    addiu   $t9, $t9, 24
L_next:
    addiu   $s1, $s1, 24
    bne     $s1, $s2, L_face
    addiu   $s3, $s3, 8
L_ret:
    # return the face index (s3 - rec) / 8; write the state back
    lw      $t0, S_REC($s0)
    sw      $s5, S_LO($s0)
    subu    $t0, $s3, $t0
    sw      $s6, S_HI($s0)
    sw      $s7, S_CCOLOR($s0)
    sw      $fp, S_CALPHA($s0)
    sw      $t8, S_CCMD($s0)
    sw      $a3, S_C2COLOR($s0)
    sw      $ra, S_C2AR($s0)
    sw      $t9, S_W($s0)
    sw      $v0, S_CULLED($s0)
    sra     $v0, $t0, 3
    lw      $s0, 16($sp)
    lw      $s1, 20($sp)
    lw      $s2, 24($sp)
    lw      $s3, 28($sp)
    lw      $s4, 32($sp)
    lw      $s5, 36($sp)
    lw      $s6, 40($sp)
    lw      $s7, 44($sp)
    lw      $fp, 48($sp)
    lw      $ra, 52($sp)
    nop
    jr      $ra
    addiu   $sp, $sp, 88

L_skip:                             # !draw or alpha < 1: not drawn, not counted as culled
    b       L_next
    sw      $zero, 4($s3)

L_cull:
    addiu   $v0, $v0, 1
    b       L_next
    sw      $zero, 4($s3)

L_neg:
    b       L_alpha
    li      $t7, 255

# Calls into C keep the loop's state: s0-s7 / fp survive them; the caller-saved t8 (entry 1's command), a1 / a2 (fog
# max / skipAt), a3 / ra (entry 2) and v0 (culled) are kept at 56-76($sp) (a0, qxn, is no longer read). 0-15($sp) is the
# o32 argument home area a C callee may write (the saved registers sit above it).
L_pending:                          # a quad ready to write (t4, t1, t0, t2, t3) and no room: PS1OTOpenRun opens a run
    sw      $t0, 0+S_PW($s0)        # the corners and the command survive the call in the state's pending words
    sw      $t1, 4+S_PW($s0)
    sw      $t2, 8+S_PW($s0)
    sw      $t3, 12+S_PW($s0)
    sw      $t4, 16+S_PW($s0)
    sw      $t9, S_W($s0)
    sw      $t8, 56($sp)
    sw      $a1, 60($sp)
    sw      $a2, 64($sp)
    sw      $a3, 68($sp)
    sw      $v0, 72($sp)
    sw      $ra, 76($sp)
    jal     PS1OTOpenRun            # (s): closes the open run, opens one (s->w / s->wEnd, null when the arena is full)
    move    $a0, $s0
    lw      $t8, 56($sp)
    lw      $a1, 60($sp)
    lw      $a2, 64($sp)
    lw      $a3, 68($sp)
    lw      $v0, 72($sp)
    lw      $ra, 76($sp)
    lw      $t9, S_W($s0)
    lw      $v1, S_WEND($s0)
    lw      $t0, 0+S_PW($s0)
    lw      $t1, 4+S_PW($s0)
    lw      $t2, 8+S_PW($s0)
    lw      $t3, 12+S_PW($s0)
    lw      $t4, 16+S_PW($s0)
    bnez    $t9, L_write            # a run: write the quad
    nop
    b       L_next                  # no arena: not drawn (record .first stays 0)
    sw      $zero, 4($s3)

L_other:                            # not fast: depth, lo / hi and the record here, the face in C (PS1OTOtherC)
    slt     $at, $t4, $s5
    beqz    $at, 1f
    sw      $t4, 0($s3)             # rec.depth
    move    $s5, $t4                # lo
1:  slt     $at, $s6, $t4
    beqz    $at, 2f
    nop
    move    $s6, $t4                # hi
2:  sw      $t9, S_W($s0)           # the open run's end (PS1OTOtherC closes it)
    sw      $t8, 56($sp)
    sw      $a1, 60($sp)
    sw      $a2, 64($sp)
    sw      $a3, 68($sp)
    sw      $v0, 72($sp)
    sw      $ra, 76($sp)
    lw      $t0, S_REC($s0)
    move    $a2, $t4                # depth
    subu    $t0, $s3, $t0
    sra     $a1, $t0, 3             # face index
    jal     PS1OTOtherC             # (s, i, depth) -> the record's .first (tagged), null, or 1: nothing drawn and the
    move    $a0, $s0                # open run left as it was
    li      $at, 1
    beq     $v0, $at, L_orej
    nop
    sw      $v0, 4($s3)             # rec.first
    lw      $t8, 56($sp)
    lw      $a1, 60($sp)
    lw      $a2, 64($sp)
    lw      $a3, 68($sp)
    lw      $v0, 72($sp)
    lw      $ra, 76($sp)
    move    $t9, $zero              # the run was closed: the next faded face opens one
    b       L_next
    move    $v1, $zero
L_orej:                             # nothing drawn, the run still open: w as saved, its end as the state has it
    sw      $zero, 4($s3)           # rec.first
    lw      $t8, 56($sp)
    lw      $a1, 60($sp)
    lw      $a2, 64($sp)
    lw      $a3, 68($sp)
    lw      $v0, 72($sp)
    lw      $ra, 76($sp)
    lw      $t9, S_W($s0)
    lw      $v1, S_WEND($s0)
    b       L_next
    nop

L_c2:                               # entry 1 missed: entry 2?
    bne     $t5, $a3, L_miss
    srl     $at, $ra, 24
    bne     $t7, $at, L_miss
    sll     $t4, $ra, 8
    srl     $t4, $t4, 8
    lui     $at, 0x2800
    b       L_write
    or      $t4, $t4, $at           # 0x28000000 | entry 2's colour bits

L_notA:
    # s1 = s3 = 0: all four collinear. Degenerate (all x equal or all y equal: no pixel): not drawn. Else with a != c:
    # a b c d
    or      $at, $t4, $t6
    bnez    $at, L_t2
    xor     $at, $t0, $t2
    xor     $t4, $t0, $t1           # (s1 / s3 are no longer read on these paths; t5 keeps the colour)
    or      $t4, $t4, $at
    xor     $t6, $t0, $t3
    or      $t4, $t4, $t6
    andi    $t6, $t4, 0xFFFF
    beqz    $t6, L_cull
    srl     $t6, $t4, 16
    beqz    $t6, L_cull
    nop
    bnez    $at, L_swap01
    nop
L_t2:
    # diagonal b-c (or collinear): a b c d when side(b, c, a) * side(b, c, d) < 0
    mtc2    $t1, $12                # SXY0 = pb
    mtc2    $t2, $13                # SXY1 = pc
    mtc2    $t0, $14                # SXY2 = pa
    nop
    nop
    cop2    0x1400006
    mfc2    $t4, $24
    mtc2    $t3, $14                # SXY2 = pd
    nop
    nop
    cop2    0x1400006
    mfc2    $t6, $24
    nop
    mult    $t4, $t6
    mfhi    $at
    bltz    $at, L_swap01           # a b c d
    nop
    # diagonal a-d: b a d c when side(a, d, b) * side(a, d, c) < 0, else a b c d
    mtc2    $t0, $12                # SXY0 = pa
    mtc2    $t3, $13                # SXY1 = pd
    mtc2    $t1, $14                # SXY2 = pb
    nop
    nop
    cop2    0x1400006
    mfc2    $t4, $24
    mtc2    $t2, $14                # SXY2 = pc
    nop
    nop
    cop2    0x1400006
    mfc2    $t6, $24
    nop
    mult    $t4, $t6
    mfhi    $at
    bgez    $at, L_swap01
    nop
    move    $at, $t2                # b a d c: swap c and d
    move    $t2, $t3
    b       L_ccheck
    move    $t3, $at
L_swap01:
    move    $at, $t0                # a b c d: swap a and b
    move    $t0, $t1
    b       L_ccheck
    move    $t1, $at

L_miss:
    # entry 1 moves to entry 2; the new entry 1: upstream's per channel fog5 * (255 - a) >> 8 + c5 * a >> 8, R and B in
    # 16-bit lanes (as the C code). a0 (qxn) is scratch here and reloaded.
    move    $a3, $s7
    sll     $at, $fp, 24
    sll     $t6, $t8, 8
    srl     $t6, $t6, 8
    or      $ra, $at, $t6           # entry 2 = alpha1 << 24 | command1's colour bits
    move    $s7, $t5                # entry 1: colour, alpha
    move    $fp, $t7
    lw      $t6, S_FOGRB($s0)
    li      $t4, 255
    subu    $t4, $t4, $t7           # ia
    multu   $t4, $t6                # fogRB * ia
    srl     $at, $t5, 19
    andi    $at, $at, 0x1F
    srl     $a0, $t5, 3
    andi    $a0, $a0, 0x1F
    sll     $a0, $a0, 16
    or      $at, $at, $a0           # cRB
    lui     $a0, 0x1F
    ori     $a0, $a0, 0x1F          # 0x001F001F
    mflo    $t8
    srl     $t8, $t8, 8
    and     $t8, $t8, $a0           # (fogRB * ia) >> 8 & 0x001F001F
    multu   $t7, $at                # cRB * a
    lw      $t6, S_FOGG($s0)
    srl     $at, $t5, 11
    andi    $at, $at, 0x1F          # cG
    mflo    $t4                     # (ia is no longer needed: 255 - alpha again below)
    srl     $t4, $t4, 8
    and     $t4, $t4, $a0           # (cRB * a) >> 8 & 0x001F001F
    addu    $t8, $t8, $t4           # rb
    li      $t4, 255
    subu    $t4, $t4, $t7           # ia
    multu   $t4, $t6                # fogG * ia
    sll     $t8, $t8, 3
    lui     $a0, 0x2800
    or      $t8, $t8, $a0           # 0x28000000 | rb << 3
    mflo    $t6
    srl     $t6, $t6, 8             # fogG * ia >> 8
    nop
    multu   $t7, $at                # cG * a
    lw      $a0, S_QXN($s0)
    nop
    mflo    $at
    srl     $at, $at, 8             # cG * a >> 8
    addu    $t6, $t6, $at           # g
    sll     $t6, $t6, 11
    or      $t8, $t8, $t6           # | g << 11
    b       L_write
    move    $t4, $t8
    .size   PS1OTFadedAsm, .-PS1OTFadedAsm

# void PS1OTLinkAsm(PS1OTRec *rec, int count, uint32_t *ot, unsigned hi, int shift, const DrawListEntry3D *subLast)
# PS1Draw3DSceneOT's second pass (the C loop it replaces, same order and words):
#   for (i = count - 1; i >= 0; --i) { p = rec[i].first; if (!p) continue; slot = &ot[(hi - rec[i].depth) >> shift];
#     link = *slot; tag = p & 3;
#     tag 1 (a captured sub-list): p &= ~1; *(uint32_t *)subLast[i].faceID = subLast[i].depth | link;
#     tag 2 (a textured quad): p &= ~3; *p = 9 << 24 | link;   tag 0 (a faded quad): *p = 5 << 24 | link;
#     *slot = p & 0xffffff; }
    .globl  PS1OTLinkAsm
    .type   PS1OTLinkAsm, @function
PS1OTLinkAsm:
    lw      $t8, 16($sp)            # shift
    lw      $t9, 20($sp)            # subLast
    lui     $t7, 0x0500             # 5 << 24
    lui     $t6, 0x00FF
    ori     $t6, $t6, 0xFFFF        # 0xffffff
    sll     $t0, $a1, 3
    addu    $t0, $a0, $t0           # one past the last record
    beq     $t0, $a0, K_done
    addiu   $t0, $t0, -8            # the record being linked (the loop's delay slot steps to the next one)
    .balign 16
K_loop:
    lw      $t1, 4($t0)             # first
    lw      $t2, 0($t0)             # depth
    beqz    $t1, K_next
    andi    $t3, $t1, 3             # tag
    subu    $t2, $a3, $t2
    srlv    $t2, $t2, $t8
    sll     $t2, $t2, 2
    addu    $t2, $a2, $t2           # the bucket's slot
    lw      $t4, 0($t2)             # link
    bnez    $t3, K_tag
    and     $t5, $t1, $t6           # p & 0xffffff
    or      $t4, $t4, $t7           # 5 << 24 | link
    sw      $t4, 0($t1)
    sw      $t5, 0($t2)
K_next:
    bne     $t0, $a0, K_loop
    addiu   $t0, $t0, -8
K_done:
    jr      $ra
    nop
K_tag:
    addiu   $at, $t3, -1
    bnez    $at, K_tex
    subu    $t1, $t1, $t3           # p without its tag (delay: both tags)
    subu    $at, $t0, $a0           # tag 1: subLast[i] (8 bytes an entry, as the records)
    addu    $at, $t9, $at
    lw      $t3, 0($at)             # .faceID = the sub-list's last node
    lw      $t5, 4($at)             # .depth = that node's size word
    and     $t1, $t1, $t6           # p & 0xffffff (t1 has no tag now)
    or      $t5, $t5, $t4
    sw      $t5, 0($t3)
    b       K_next
    sw      $t1, 0($t2)
K_tex:
    lui     $at, 0x0900             # tag 2: 9 << 24 | link
    or      $t4, $t4, $at
    sw      $t4, 0($t1)
    and     $t1, $t1, $t6
    b       K_next
    sw      $t1, 0($t2)
    .size   PS1OTLinkAsm, .-PS1OTLinkAsm
