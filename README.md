# my_malloc

## Disclaimer
This project was done solely for educational purposes. It is by no means intended for use in a production environment.

Also note that the project is still a work-in-progress and is subject to change in the short term.

## Description
This project is a custom implementation of the C family of functions in charge of dynamic memory management: [malloc](https://man7.org/linux/man-pages/man3/malloc.3p.html), [calloc](https://man7.org/linux/man-pages/man3/calloc.3p.html), [realloc](https://www.man7.org/linux/man-pages/man3/realloc.3p.html) and [free](https://man7.org/linux/man-pages/man3/free.3p.html).

Each managed block carries metadata ahead of the allocated memory. There is also a footer after the allocated space to store a duplicate of the block's size. This information is used to let the program access this block's header from the next block in memory.

Free memory blocks are kept in buckets, according to a block size-to-bucket mapping. Inside each bucket, free memory blocks are kept sorted in ascending size order, to help perform a best-fit search.

Those are only the broad lines of my implementation. However, I have extensively documented my code with comments. Therefore, by reading `src/malloc.c`, you should be able to get all the details of the implementation.

## Simple benchmark
A simple benchmark comparing the custom functions with the built-in functions is provided.

To run the benchmark, first run `make` from the project's root directory. Then, run the `my_malloc` executable file.

## Potential downsides and areas of improvement
The main danger that I see with this implementation is the lack of error detection. More precisely, no checks are being made to ensure that header and footer metadata were unaltered during client use. Corruption of this metadata will break the program. A way of dealing with this would be to include some sort of checksum verification.

Also, blocks under management by this implementation have a minimum size of 32 bytes. In a  program where the client only ever needs very small blocks, i.e. blocks of 16 bytes (to store two pointers on a system with 8-byte pointers), memory allocation will suffer a 100% overhead.

Even though I had to conduct a fair deal of research to build this project, I still consider myself far from an expert – or even a knowledgeable person – in dynamic memory management.

Consequently, I am convinced that there are many more downsides and shortcomings that I fail to identify at this time.
