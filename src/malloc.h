#ifndef MY_MALLOC_MALLOC_H
#define MY_MALLOC_MALLOC_H

#include <stddef.h>

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);

#endif
