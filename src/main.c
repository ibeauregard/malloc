#include "malloc.h"

#include <stdlib.h>
#include <time.h>
#include <stdio.h>

#define NUM_POINTERS (1 << 10)
#define NUM_CYCLES (1 << 10)
#define BLOCK_SIZE_UPPER_BOUND (1 << 16)

typedef void* calloc_func(size_t num, size_t size);
typedef void* realloc_func(void* ptr, size_t size);
typedef void free_func(void* ptr);

static void benchmark(calloc_func* calloc, realloc_func* realloc, free_func* free);
int main()
{
    printf("%s\n", "*** Benchmarking of built-in memory allocation functions ***");
    benchmark(&calloc, &realloc, &free);
    puts("");

    printf("%s\n", "*** Benchmarking of custom memory allocation functions ***");
    benchmark(&calloc_, &realloc_, &free_);

    return EXIT_SUCCESS;
}

void benchmark(calloc_func* calloc, realloc_func* realloc, free_func* free)
{
    void* pointers[NUM_POINTERS];
    clock_t start = clock();
    for (unsigned short i = 0; i < NUM_CYCLES; i++) {
        for (unsigned short j = 0; j < NUM_POINTERS; j++) {
            pointers[j] = calloc(1, rand() % BLOCK_SIZE_UPPER_BOUND);
        }
        for (unsigned short j = 0; j < NUM_POINTERS; j++) {
            pointers[j] = realloc(pointers[j], rand() % BLOCK_SIZE_UPPER_BOUND);
        }
        for (unsigned short j = 0; j < NUM_POINTERS; j++) {
            free(pointers[j]);
        }
    }
    printf("Completed in %f seconds\n",
           (float) (clock() - start) / CLOCKS_PER_SEC);
}
