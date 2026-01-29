NASM = ../../nasm
CC = x86_64_lfi-linux-musl-clang

mem: main.c mem.o
	$(CC) main.c mem.o -o mem -static-pie -O2

mem.o: mem.asm
	$(NASM) --lfi -f elf64 mem.asm -o mem.o

clean:
	rm -f mem mem.o

.PHONY: clean
