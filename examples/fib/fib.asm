; fib(n) - compute nth Fibonacci number (recursive)
; Input:  edi = n
; Output: eax = fib(n)

global fib

section .text

fib:
    cmp edi, 1
    jle .base

    push rbx
    mov ebx, edi

    ; fib(n-1)
    lea edi, [rbx-1]
    call fib

    push rax
    lea edi, [rbx-2]
    call fib

    pop rcx
    add eax, ecx

    pop rbx
    ret

.base:
    mov eax, edi
    ret
