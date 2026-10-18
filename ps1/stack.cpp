#include "stack.hh"

// See stack.hh (CD-092). The array is 8-byte aligned (o32 stack alignment) and lives in .bss, so the heap
// (__heap_start .. __stack_start) never contains the stack.
alignas(8) uint32_t g_ps1Stack[PS1_STACK_BYTES / 4];
volatile uint32_t g_ps1StackOverflow = 0;

static constexpr uint32_t STACK_PAINT = 0x57ACC0DEu;
static constexpr int GUARD_WORDS      = 4; // the bottom 16 bytes: never reached unless the stack overflowed

// int PS1RunOnStack(int (*fn)(), void *top): a 24-byte frame at the top of the new stack (o32 argument area 0-15, the
// caller's $ra at 16, its $sp at 20), call fn there, restore both. top is 8-byte aligned, so the frame is too.
__asm__(R"(
    .text
    .globl  _Z13PS1RunOnStackPFivEPv
    .type   _Z13PS1RunOnStackPFivEPv, @function
    .set    push
    .set    noreorder
_Z13PS1RunOnStackPFivEPv:
    addiu   $t0, $a1, -24
    sw      $ra, 16($t0)
    sw      $sp, 20($t0)
    jalr    $a0
    move    $sp, $t0
    lw      $ra, 16($sp)
    lw      $sp, 20($sp)
    jr      $ra
    nop
    .set    pop
    .size   _Z13PS1RunOnStackPFivEPv, .-_Z13PS1RunOnStackPFivEPv
)");

void PS1StackPaint() {
    for (auto &w : g_ps1Stack) w = STACK_PAINT;
}

bool PS1StackGuardBroken() {
    for (int i = 0; i < GUARD_WORDS; i++)
        if (g_ps1Stack[i] != STACK_PAINT)
            return true;
    return false;
}

uint32_t PS1StackLowWater() {
    unsigned i = GUARD_WORDS;
    while (i < PS1_STACK_BYTES / 4 && g_ps1Stack[i] == STACK_PAINT) i++;
    return (uint32_t)(uintptr_t)&g_ps1Stack[i];
}
