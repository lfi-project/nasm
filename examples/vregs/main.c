#include <stdio.h>

long vreg_store_load(long val);
long vreg_arithmetic(long a, long b);
long vreg_swap(long a, long b);
long vreg_push_pop(long val);
long vreg_addressing(long *arr, long idx);
long vreg_sub_sizes(int val);
long vreg_accumulate(long *arr, int n);

int main(void) {
    /* store/load */
    printf("vreg_store_load(42) = %ld\n", vreg_store_load(42));

    /* arithmetic */
    printf("vreg_arithmetic(3, 7) = %ld\n", vreg_arithmetic(3, 7));

    /* swap */
    printf("vreg_swap(10, 20) = %ld\n", vreg_swap(10, 20));

    /* push/pop */
    printf("vreg_push_pop(99) = %ld\n", vreg_push_pop(99));

    /* addressing */
    long arr[] = {100, 200, 300};
    printf("vreg_addressing(arr, 0) = %ld\n", vreg_addressing(arr, 0));
    printf("vreg_addressing(arr, 2) = %ld\n", vreg_addressing(arr, 2));

    /* sub-register sizes */
    printf("vreg_sub_sizes(5) = %ld\n", vreg_sub_sizes(5));

    /* accumulate */
    long arr2[] = {10, 20, 30, 40};
    printf("vreg_accumulate(arr2, 4) = %ld\n", vreg_accumulate(arr2, 4));

    return 0;
}
