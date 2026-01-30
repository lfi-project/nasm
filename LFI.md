# LFI Support in NASM

NASM supports Lightweight Fault Isolation (LFI) for x86-64 through the
`--lfi` command-line option. When enabled, NASM automatically rewrites
instructions to confine code within a 4 GiB sandbox region.

LFI works by applying assembly-level transformations during the assembly
step. All control flow and memory accesses are rewritten so that they
cannot escape the sandbox. The transformations use 32-byte bundle
alignment to ensure that sandboxing sequences cannot be bypassed by
jumping into the middle of a multi-instruction sequence.

## Usage

    nasm --lfi -f elf64 input.asm -o output.o

The `--lfi` flag implies 64-bit mode. Additional flags control which
memory accesses are sandboxed:

| Flag | Description |
|------|-------------|
| `--lfi` | Enable LFI mode |
| `--no-lfi-loads` | Do not sandbox load instructions |
| `--no-lfi-stores` | Do not sandbox store instructions |
| `--no-lfi-segue` | Do not use the `gs` segment for memory sandboxing |
| `--no-lfi-align-labels` | Do not align labels to bundle boundaries |
| `--lfi-vregs` | Enable virtual register file for reserved registers |

Use `--no-lfi-loads` for a stores-only sandbox that may read outside the
sandbox but cannot write outside it.

Use `--no-lfi-loads --no-lfi-stores` for a jumps-only sandbox that may
read and write outside the sandbox but cannot transfer control outside
it (e.g., cannot execute system calls directly). This is useful in
combination with another form of memory sandboxing such as Intel MPK.

Use `--no-lfi-segue` to avoid using the `gs` segment register for
memory sandboxing. In this mode, memory accesses are sandboxed using
scratch register sequences instead.

## Reserved Registers

The LFI ABI reserves the following registers:

| Register | Purpose |
|----------|---------|
| `r14` | Sandbox base address (must never be modified) |
| `gs` | Segment register set to the sandbox base address |
| `rsp` | Stack pointer (always holds a valid sandbox address) |
| `r11` | Scratch register used by the rewriter |
| `r15` | Thread-local virtual register file pointer |

Any instruction that writes to `r14` or directly uses `gs` in the
source will produce a fatal error.

## Assembly Rewrites

### Control Flow

**Indirect jump via register:**

```
; Original:       ; Rewritten:
jmp rax           .bundle_lock
                  and eax, 0xffffffe0
                  add rax, r14
                  jmp rax
                  .bundle_unlock
```

**Indirect call via register:**

```
; Original:       ; Rewritten:
call rax          .bundle_lock align_to_end
                  and eax, 0xffffffe0
                  add rax, r14
                  call rax
                  .bundle_unlock
```

**Indirect jump/call via memory:**

The memory operand is first loaded into `r11` (with memory sandboxing
applied to the load), then the indirect branch sequence is applied to
`r11`.

**Return:**

```
; Original:       ; Rewritten:
ret               pop r11
                  .bundle_lock
                  and r11d, 0xffffffe0
                  add r11, r14
                  jmp r11
                  .bundle_unlock
```

**Direct call:**

```
; Original:       ; Rewritten:
call label        .bundle_lock align_to_end
                  call label
                  .bundle_unlock
```

Direct calls are placed at the end of a bundle so that the return
address is bundle-aligned.

### Memory Accesses

Memory operands are rewritten to use the `gs` segment prefix with 32-bit
registers, which zeroes the upper 32 bits and makes the access relative
to the sandbox base:

```
; Original:              ; Rewritten:
mov rax, [rcx]           mov rax, [gs:ecx]
mov rax, [rcx+rdx*4+16] mov rax, [gs:ecx+edx*4+16]
```

The following addressing modes are safe and not rewritten:

- `rsp`-based (stack pointer is always valid)
- `rip`-relative (code is within the sandbox)
- `lea` instructions (no memory access)

With `--no-lfi-segue`, memory sandboxing uses a scratch register instead:

```
; Original:              ; Rewritten (simple):
mov rax, [rcx]           .bundle_lock
                         mov ecx, ecx
                         mov rax, [r14 + rcx]
                         .bundle_unlock

; Original:              ; Rewritten (complex addressing):
mov rax, [rcx+rdx*4+16] .bundle_lock
                         lea r11d, [rcx+rdx*4+16]
                         mov rax, [r14 + r11]
                         .bundle_unlock
```

### Stack Modification

Any instruction that modifies `rsp` is demoted to its 32-bit form and
then re-guarded:

```
; Original:       ; Rewritten:
sub rsp, 8        .bundle_lock
                  sub esp, 8
                  lea rsp, [rsp + r14]
                  .bundle_unlock
```

The `lea` form is used instead of `add` to avoid modifying flags.

### String Instructions

String instructions (`stosb`, `movsb`, `cmpsb`, etc.) have their
implicit register operands sandboxed before execution:

```
; Original:       ; Rewritten:
rep stosb         .bundle_lock
                  mov edi, edi
                  lea rdi, [r14 + rdi]
                  rep stosb
                  .bundle_unlock
```

For `movsb`/`cmpsb`, `rsi` is also sandboxed.

### System Calls

System calls are rewritten into an indirect branch to the runtime
entrypoint table stored at the sandbox base:

```
; Original:       ; Rewritten:
syscall           .bundle_lock
                  lea r11, [rel $+7]
                  jmp qword [r14]
                  .bundle_unlock
```

### Thread-Local Storage

TLS accesses via `fs:0` are rewritten to use the virtual register file:

```
; Original:            ; Rewritten:
mov rax, [fs:0]        mov rax, [r15]
```

## Virtual Registers

When `--lfi-vregs` is enabled, user assembly may reference the reserved
registers `r11`, `r14`, and `r15`. These are transparently rewritten
into loads and stores from a memory-backed virtual register file at
offsets from `r15`:

| Offset | Virtual Register |
|--------|-----------------|
| `[r15 + 40]` | `r11` |
| `[r15 + 48]` | `r14` |
| `[r15 + 56]` | `r15` |

The expansion is a pre-pass: each user instruction referencing a
reserved register is expanded into 1-3 instructions that use only
non-reserved registers, then each expanded instruction goes through
the normal LFI pipeline individually.

### Rewrite Examples

**Source substitution** (reserved register as source, no memory conflict):

```
; Original:              ; Rewritten:
mov rax, r14             mov rax, qword [r15 + 48]
add rax, r14             add rax, qword [r15 + 48]
```

**Destination substitution** (write-only):

```
; Original:              ; Rewritten:
mov r14, rax             mov qword [r15 + 48], rax
lea r14, [rax]           lea r11, [rax]
                         mov [r15 + 48], r11
```

**Read-modify-write destination** (ALU ops):

```
; Original:              ; Rewritten:
add r14, rax             mov r11, [r15 + 48]
                         add r11, rax
                         mov [r15 + 48], r11
```

**Memory operand conflict** (instruction already has a memory operand):

```
; Original:              ; Rewritten:
add [rdi], r14           mov r11, [r15 + 48]
                         add [rdi], r11

add r14, [rdi]           mov r11, [r15 + 48]
                         add r11, [rdi]
                         mov [r15 + 48], r11
```

**Reserved register in addressing mode**:

```
; Original:                ; Rewritten:
mov rax, [r14 + rbx]      mov r11, [r15 + 48]
                           mov rax, [r11 + rbx]
```

**Two reserved registers**:

```
; Original:              ; Rewritten:
add r14, r11             mov r11, [r15 + 48]
                         add r11, qword [r15 + 40]
                         mov [r15 + 48], r11
```

**push/pop**:

```
; Original:              ; Rewritten:
push r14                 mov r11, [r15 + 48]
                         push r11

pop r14                  pop r11
                         mov [r15 + 48], r11
```

**32-bit writes** (zero-extension):

```
; Original:              ; Rewritten:
mov r14d, eax            mov r11d, eax
                         mov [r15 + 48], r11
```

**8/16-bit writes** (direct memory substitution, no zero-extension):

```
; Original:              ; Rewritten:
mov r14w, ax             mov word [r15 + 48], ax
mov r14b, al             mov byte [r15 + 48], al
```

**xchg**:

```
; Original:              ; Rewritten:
xchg r14, rax            mov r11, [r15 + 48]
                         mov [r15 + 48], rax
                         mov rax, r11
```

### Flags Preservation

All expansion sequences use `mov` for load/store operations, which does
not modify the CPU flags register. This ensures that flag state is
preserved through virtual register load/store sequences.

## Bundle Alignment

All labels are aligned to 32-byte bundle boundaries so that direct
jump and call targets are always bundle-aligned. Use
`--no-lfi-align-labels` to disable label alignment. Sections have a
minimum alignment of 32 bytes.

Bundle-locked instruction groups are guaranteed to fit within a single
32-byte bundle. NOP padding is inserted before groups that would
otherwise cross a bundle boundary. Padding uses multi-byte NOP
instructions (up to 9 bytes) for efficiency.

## References

- [LFI project](https://github.com/lfi-project/)
- [LFI paper](https://zyedidia.github.io/papers/lfi_asplos24.pdf)
