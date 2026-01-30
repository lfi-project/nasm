; Example functions demonstrating virtual register usage under LFI.
; With --lfi-vregs, user code may reference r11, r14, and r15 as
; virtual registers backed by memory at offsets from r15.

global vreg_store_load
global vreg_arithmetic
global vreg_swap
global vreg_push_pop
global vreg_addressing
global vreg_sub_sizes
global vreg_accumulate

section .text

; vreg_store_load(long val) -> long
; Store a value into virtual r14, then load it back.
; Tests basic mov to/from a reserved register.
vreg_store_load:
    mov r14, rdi
    mov rax, r14
    ret

; vreg_arithmetic(long a, long b) -> long
; Compute (a + b) * 2 - b using virtual r14 and r11.
; Tests read-modify-write on reserved registers.
vreg_arithmetic:
    mov r14, rdi        ; r14 = a
    add r14, rsi        ; r14 = a + b
    mov r11, r14        ; r11 = a + b
    add r14, r11        ; r14 = (a + b) * 2
    sub r14, rsi        ; r14 = (a + b) * 2 - b
    mov rax, r14
    ret

; vreg_swap(long a, long b) -> long
; Swap two values using xchg with a virtual register.
; Returns the original b (which ends up in r14 after the swap).
vreg_swap:
    mov r14, rdi        ; r14 = a
    mov rax, rsi        ; rax = b
    xchg r14, rax       ; r14 = b, rax = a
    mov rax, r14        ; return b
    ret

; vreg_push_pop(long val) -> long
; Push a virtual register onto the stack, then pop it back.
; Tests push/pop with reserved registers.
vreg_push_pop:
    mov r14, rdi
    push r14
    xor r14, r14        ; clear r14
    pop r14             ; restore from stack
    mov rax, r14
    ret

; vreg_addressing(long *arr, long idx) -> long
; Use a virtual register as a base in an addressing mode.
; Tests reserved register in memory operand base.
vreg_addressing:
    mov r14, rdi                ; r14 = arr
    mov rax, [r14 + rsi*8]     ; load arr[idx]
    ret

; vreg_sub_sizes(int val) -> long
; Store a 32-bit value into a virtual register (zero-extends to 64),
; then read back the full 64-bit value.
; Tests Case 8: 32-bit writes with zero-extension.
vreg_sub_sizes:
    mov r14, -1         ; fill r14 with all 1s
    mov r14d, edi       ; 32-bit write: zero-extends upper 32 bits
    mov rax, r14        ; read back full 64-bit value
    ret

; vreg_accumulate(long *arr, int n) -> long
; Sum array elements using virtual r14 as the accumulator.
; Tests virtual registers in a loop with memory accesses.
vreg_accumulate:
    xor r14, r14        ; clear the virtual accumulator (RMW: load, xor, store)
    test esi, esi
    jle .done
    xor ecx, ecx
.loop:
    add r14, [rdi + rcx*8]
    inc ecx
    cmp ecx, esi
    jl .loop
.done:
    mov rax, r14
    ret
