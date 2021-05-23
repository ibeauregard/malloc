#ifndef MY_MALLOC_MALLOC_H
#define MY_MALLOC_MALLOC_H

#include <stddef.h>

/*
 * Implements https://man7.org/linux/man-pages/man3/malloc.3p.html
 */
void* malloc_(size_t size);

/*
 * Implements https://man7.org/linux/man-pages/man3/free.3p.html
 */
void free_(void* ptr);

/*
 * Implements https://man7.org/linux/man-pages/man3/calloc.3p.html
 */
void* calloc_(size_t num, size_t size);

/*
 * Implements https://www.man7.org/linux/man-pages/man3/realloc.3p.html
 */
void* realloc_(void* ptr, size_t size);

#endif
