NASM = ../../nasm
CC = x86_64_lfi-linux-musl-clang

fib: main.c fib.o
	$(CC) main.c fib.o -o fib -static-pie -O2

fib.o: fib.asm
	$(NASM) --lfi -f elf64 fib.asm -o fib.o

clean:
	rm -f fib fib.o

.PHONY: clean
