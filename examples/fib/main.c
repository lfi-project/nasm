#include <stdio.h>
#include <stdlib.h>

int fib(int n);

int main(int argc, char *argv[]) {
    int n = 10;
    if (argc > 1)
        n = atoi(argv[1]);
    printf("fib(%d) = %d\n", n, fib(n));
    return 0;
}
