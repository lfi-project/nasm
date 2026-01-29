#include <stdio.h>
#include <string.h>

int array_sum(int *arr, int n);
void array_scale(int *arr, int n, int factor);
int indirect_call(int (*fn)(int, int), int a, int b);
void memcpy_simple(void *dst, const void *src, int n);
void swap(int *a, int *b);
void *table_lookup(void **table, int index);
long struct_access(void *ptr, int offset);

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }

int main(void) {
    /* array_sum */
    int arr[] = {1, 2, 3, 4, 5};
    printf("array_sum = %d\n", array_sum(arr, 5));

    /* array_scale */
    int arr2[] = {1, 2, 3, 4};
    array_scale(arr2, 4, 10);
    printf("array_scale = {%d, %d, %d, %d}\n",
           arr2[0], arr2[1], arr2[2], arr2[3]);

    /* indirect_call */
    printf("indirect_call(add, 3, 4) = %d\n", indirect_call(add, 3, 4));
    printf("indirect_call(mul, 3, 4) = %d\n", indirect_call(mul, 3, 4));

    /* memcpy_simple */
    char src[] = "hello";
    char dst[6];
    memcpy_simple(dst, src, 6);
    printf("memcpy_simple = \"%s\"\n", dst);

    /* swap */
    int a = 10, b = 20;
    swap(&a, &b);
    printf("swap: a=%d b=%d\n", a, b);

    /* table_lookup */
    void *table[] = { "zero", "one", "two" };
    printf("table_lookup[1] = \"%s\"\n", (char *)table_lookup(table, 1));

    /* struct_access */
    struct { long x; long y; } s = { 42, 99 };
    printf("struct_access(s, 0) = %ld\n", struct_access(&s, 0));
    printf("struct_access(s, 8) = %ld\n", struct_access(&s, 8));

    return 0;
}
