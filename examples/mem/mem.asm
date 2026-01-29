; Example functions demonstrating various memory access patterns
; and indirect calls under LFI sandboxing.

global array_sum
global array_scale
global indirect_call
global memcpy_simple
global swap
global table_lookup
global struct_access

section .text

; array_sum(int *arr, int n) -> int
; Sum n elements from an array using base+index addressing.
array_sum:
    xor eax, eax
    test esi, esi
    jle .done
    xor ecx, ecx
.loop:
    add eax, [rdi + rcx*4]
    inc ecx
    cmp ecx, esi
    jl .loop
.done:
    ret

; array_scale(int *arr, int n, int factor)
; Multiply each element by factor. Demonstrates read-modify-write.
array_scale:
    test esi, esi
    jle .done
    xor ecx, ecx
.loop:
    mov eax, [rdi + rcx*4]
    imul eax, edx
    mov [rdi + rcx*4], eax
    inc ecx
    cmp ecx, esi
    jl .loop
.done:
    ret

; indirect_call(fn_ptr fn, int a, int b) -> int
; Call a function pointer. Demonstrates indirect call via register.
indirect_call:
    mov rax, rdi
    mov edi, esi
    mov esi, edx
    call rax
    ret

; memcpy_simple(void *dst, const void *src, int n)
; Byte-by-byte copy using string instruction.
memcpy_simple:
    mov rcx, rdx
    rep movsb
    ret

; swap(int *a, int *b)
; Swap two integers. Demonstrates paired load/store to different pointers.
swap:
    mov eax, [rdi]
    mov ecx, [rsi]
    mov [rdi], ecx
    mov [rsi], eax
    ret

; table_lookup(void **table, int index) -> void*
; Load a pointer from an array of pointers. Demonstrates 8-byte load
; with scaled index.
table_lookup:
    mov rax, [rdi + rsi*8]
    ret

; struct_access(void *ptr, int offset) -> long
; Load a 64-bit value at a runtime offset from a base pointer.
; Demonstrates base+offset addressing.
struct_access:
    movsxd rsi, esi
    mov rax, [rdi + rsi]
    ret
