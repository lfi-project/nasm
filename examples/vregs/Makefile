NASM = ../../nasm
CC = /opt/llvm22/x86_64-lfi-ctxreg-clang/bin/clang

vregs: main.c vregs.o
	$(CC) main.c vregs.o -o vregs -static-pie -O2

vregs.o: vregs.asm
	$(NASM) --lfi --lfi-vregs -f elf64 vregs.asm -o vregs.o

clean:
	rm -f vregs vregs.o

.PHONY: clean
