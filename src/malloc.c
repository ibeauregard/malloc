#include "malloc.h"

#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

/*
 * The smallest unit of memory a client can request: 8 bytes, or 64 bits. We want to stay 8-byte aligned.
 * This number HAS TO be a power of two.
 */
#define MEM_UNIT (uint8_t) 8 // bytes

/*
 * The base-2 logarithm of the maximum number of memory mappings that can be stored by this program. Here a memory
 * mapping means an area of memory mapped by the mmap(2) function (https://man7.org/linux/man-pages/man2/mmap.2.html).
 * Note that contiguous mappings will be counted as one single mapping.
 *
 * Be careful before changing the value of this constant. There will be an impact on the header_t data structure.
 */
#define LOG2_NUM_MAPPINGS (uint8_t) 15

/*
 * A 24-byte data structure that precedes all managed memory blocks. The first 48 bits represent the size
 * of the block, including its metadata (header and footer). The next 15 bits represent the index of the memory mapping
 * where the block is located. (Memory mappings are created by calls to mmap(2).) The least significant bit of the first
 * 8 bytes is 1 if the block is free, 0 otherwise.
 *
 * The next and prev members point to the next and previous blocks, respectively, stored in the bucket of free blocks
 * where this block is stored. Note that buckets store blocks in ascending order of size, not in order of memory
 * address.
 *
 * The `next` and `prev` members are only used when the block is free. At the moment of allocation, the address of the
 * `next` member is returned, such that only the first 8 bytes of this data structure are expected to be preserved
 * while the block is in use.
 */
typedef struct header header_t;
struct header {
    uint64_t size:48;
    uint64_t mapping:LOG2_NUM_MAPPINGS;
    uint64_t free:1;
    header_t* next;
    header_t* prev;
};

/*
 * This is used to get the offset between the beginning of a block and the beginning of the memory zone where
 * the client can write. The client can write over the `next` and `prev` pointers while the block is in use.
 */
#define METADATA_OFFSET (sizeof (uint64_t))

/*
 * An 8-byte, single-member data structure that is kept at the end of every managed memory block. Like the header, it
 * stores the size of the block, including its metadata (header and footer). The purpose of this structure is to allow
 * any block in memory to access the size of its previous block. Knowing its size, we can travel back to
 * the header to know whether that previous block is free. Note that we could easily have stored the free flag here
 * also, but it seems more cautious to store it in a single place.
 *
 * Note also that we could have typedef-ed footer_t as an alias for uint64_t, or we could have avoided any typedef
 * altogether here, but this definition makes the program clearer without adding any performance overhead.
 */
typedef struct footer {
    uint64_t size;
} footer_t;

/*
 * The smallest block size that this program can manage.
 */
#define MIN_ALLOC (sizeof (header_t) + sizeof (footer_t))

/*
 * The smallest unit of memory one can request from the OS.
 */
#define MMAP_UNIT ((1 << 5) * sysconf(_SC_PAGESIZE))

/* The number of free memory block buckets. */
#define NUM_BUCKETS (uint8_t) 166

/*
 * Memory blocks available to the client are stored in buckets, each of which is associated with a certain size range.
 * Here is the index-to-size mapping :
 * 0:   0
 * 1:   8
 * 2:   16
 * 3:   24
 * 4:   32
 * 5:   40
 * ...  ...
 * n (n < 128):   8*n
 * ...
 * 127: 1016
 * 128: {1024, 1032, 1040, ..., 2032, 2040} == {8*k : 2^7 <= k < 2^8}
 * 129: {8*k : 2^8 <= k < 2^9}
 * 130: {8*k : 2^9 <= k < 2^10}
 * ...  ...
 * n (128 <= n < 166):   {8*k : 2^(n - 121) <= k < 2^(n - 120)}
 * ...
 * 165: {8*k : 2^44 <= k < 2^45}
 *
 * We assume that the address space is 48-bit long, which is why we stop at index 165. In other words, it is impossible,
 * even theoretically, to allocate a memory block of 2^48 bytes or more.
 * See https://stackoverflow.com/questions/6716946/why-do-x86-64-systems-have-only-a-48-bit-virtual-address-space.
 *
 * Inside each bucket, available blocks are kept sorted in ascending size order. When searching for a free block to
 * allocate to the client, the appropriate bucket is searched, and the first block with a sufficient size is taken.
 * Because the blocks within a bucket are sorted, the search is a best-fit one.
 */
static header_t buckets[NUM_BUCKETS];

/*
 * Flag to indicate whether the buckets are initialized. Will be and remain set to true after the first call to malloc_.
 */
static bool initialized = false;

/*
 * The index of the next memory mapping returned my mmap(2).
 */
static uint16_t mapping_index = 0;

/*
 * A 2-D array of size 2^LOG2_NUM_MAPPINGS x 2. For each mapping i (0 <= i < 2^LOG2_NUM_MAPPINGS),
 * mappings[i][0] and mappings[i][1] contain the lower (included) and upper (excluded) bounds, respectively,
 * of the memory mapping.
 */
static uintptr_t mappings[1 << LOG2_NUM_MAPPINGS][2];

static void initialize_buckets(void);
static size_t aligned(size_t size);
static header_t* get_block_from_buckets(size_t size);
static header_t* get_block_from_os(size_t size);
void* malloc_(size_t size)
{
    size_t aligned_size = aligned(size);
    if (size == 0 || aligned_size < size) { // aligned_size < size => overflow
        errno = EINVAL;
        return NULL;
    }
    if (!initialized) initialize_buckets();
    header_t* header = get_block_from_buckets(aligned_size);
    if (!header) header = get_block_from_os(aligned_size);
    if (!header) return NULL;
    header->free = false;
    return (void*) ((uintptr_t) header + METADATA_OFFSET);
}

static header_t* get_block_from_ptr(void* ptr);
static void insert_into_buckets(header_t* inserted);
static void coalesce(header_t* lower, header_t* higher);
void free_(void* ptr)
{
    if (!ptr) return;
    header_t* block = get_block_from_ptr(ptr);
    insert_into_buckets(block);
    uintptr_t next_block = (uintptr_t) block + block->size;

    /* If next_block >= mappings[block->mapping][1],
     * next_block does not point to a memory block under our management. */
    if (next_block < mappings[block->mapping][1] && (bool) ((header_t*) next_block)->free) {
        coalesce(block, (header_t*) next_block);
    }
    /* If block is the first block of its mapping, there is no previous block to potentially coalesce with. */
    if ((uintptr_t) block == mappings[block->mapping][0]) return;
    header_t* previous_block = (header_t*) ((uintptr_t) block - ((footer_t*) block - 1)->size);
    if (previous_block->free) {
        coalesce(previous_block, block);
    }
}

void* calloc_(size_t num, size_t size)
{
    if (num == 0 || size > (size_t) -1 / num) {
        errno = EINVAL;
        return NULL;
    }
    size_t total_size = num * size;
    void* ptr = malloc_(total_size);
    if (!ptr) return NULL;
    memset(ptr, 0, total_size);
    return ptr;
}

static header_t* adjusted_block(header_t* block, size_t size);
void* realloc_(void* ptr, size_t size)
{
    if (!ptr || !size) {
        free_(ptr);
        return malloc_(size);
    }
    header_t* block = get_block_from_ptr(ptr);
    uint64_t old_size = block->size - METADATA_OFFSET - sizeof (footer_t);
    if (size <= old_size) {
        return (void*) ((uintptr_t) adjusted_block(block, aligned(size)) + METADATA_OFFSET);
    }
    void* new_ptr = malloc_(size);
    if (new_ptr) memcpy(new_ptr, ptr, old_size);
    free_(ptr);
    return new_ptr;
}

/*
 * This function is called exactly once, during the very first call to malloc. It stores inside each bucket a copy
 * of a dummy block header specifying a block size of 0. Also, each dummy header has next and prev pointers that point
 * to itself. This is done to initialize each bucket as a circular doubly-linked list of headers. The prev pointer is
 * useful when we want to remove a block (we don't need to iterate through the list to find the block's previous block.)
 * The circular nature of the list helps manage edge cases (such as removing the tail or the head of the list).
 */
void initialize_buckets(void)
{
    static const header_t dummy_header = {.size = 0};
    for (uint8_t i = 0; i < NUM_BUCKETS; i++) {
        buckets[i] = dummy_header;
        buckets[i].next = buckets[i].prev = &buckets[i];
    }
    initialized = true;
}

static size_t round_up_power_of_two(size_t number, size_t power);
size_t aligned(size_t size)
{
    size = round_up_power_of_two(size, MEM_UNIT);
    size += METADATA_OFFSET + sizeof (footer_t);
    return size >= MIN_ALLOC ? size : MIN_ALLOC;
}

inline size_t round_up_power_of_two(size_t number, size_t power)
{
    return (number + (power - 1)) & ~(power - 1);
}

static uint8_t bucket_index_from_size(size_t size);
static header_t* get_block_from_bucket(header_t* bucket, size_t size);
header_t* get_block_from_buckets(size_t size)
{
    for (uint8_t i = bucket_index_from_size(size); i < NUM_BUCKETS ; i++)
    {
        header_t* block;
        if ((block = get_block_from_bucket(&buckets[i], size))) return block;
    }
    return NULL;
}

/*
 * This function maps a block size to the index of the bucket where the block should be stored when it is free.
 * See the documentation of the `buckets` variable to know the details of the size-to-index mapping.
 */
uint8_t bucket_index_from_size(size_t size)
{
    static const uint8_t index_1024 = 1024 / MEM_UNIT, log2_1024 = 10;
    if (size < 1024) return size / MEM_UNIT;
    uint8_t log2;
    for (log2 = log2_1024; size >> (log2 + 1) > 0; log2++);
    return index_1024 + (log2 - log2_1024);
}

static void remove_from_bucket(header_t* block);
header_t* get_block_from_bucket(header_t* bucket, size_t size)
{
    header_t* block = bucket;
    block = block->next;
    for (; block->size > 0; block = block->next) {
        if (block->size >= size) {
            remove_from_bucket(block);
            return adjusted_block(block, size);
        }
    }
    return NULL;
}

void remove_from_bucket(header_t* block)
{
    block->prev->next = block->next;
    block->next->prev = block->prev;
}

static void split_after(header_t* block, size_t size);
static void update_size(header_t* block, size_t size);
header_t* adjusted_block(header_t* block, size_t size)
{
    if (block->size - size < MIN_ALLOC) return block;
    split_after(block, size);
    update_size(block, size);
    return block;
}

void split_after(header_t* block, size_t size)
{
    header_t* new_block = (header_t*) ((uintptr_t) block + size);
    update_size(new_block, block->size - size);
    new_block->mapping = block->mapping;
    insert_into_buckets(new_block);
}

inline void update_size(header_t* block, size_t size)
{
    block->size = ((footer_t*) ((uintptr_t) block + size) - 1)->size = size;
}

inline header_t* get_block_from_ptr(void* ptr)
{
    return (header_t*) ((uintptr_t) ptr - METADATA_OFFSET);
}

/*
 * Buckets are kept sorted according to block size ascending order. Ties are broken with an oldest-first rule;
 * see http://gee.cs.oswego.edu/dl/html/malloc.html.
 */
void insert_into_buckets(header_t* inserted)
{
    header_t* pre_insertion = &buckets[bucket_index_from_size(inserted->size)];
    if (inserted->size < 1024) {
        /* This is a one-size bucket; insert block in last position because of the oldest-first rule. */
        pre_insertion = pre_insertion->prev;
    } else {
        for (; true; pre_insertion = pre_insertion->next) {
            uint64_t next_block_size = pre_insertion->next->size;
            if (next_block_size == 0 || next_block_size > inserted->size) break;
        }
    }
    inserted->prev = pre_insertion;
    inserted->next = pre_insertion->next;
    pre_insertion->next = inserted;
    inserted->next->prev = inserted;
    inserted->free = true;
}

static void* get_mapping(size_t size);
header_t* get_block_from_os(size_t size)
{
    size_t size_requested_from_os = round_up_power_of_two(size, MMAP_UNIT);
    if (size_requested_from_os < size) { // overflow
        errno = EINVAL;
        return NULL;
    }
    header_t* main_block = get_mapping(size_requested_from_os);
    if (!main_block) return NULL;
    update_size(main_block, size_requested_from_os);
    main_block->mapping = mapping_index++;
    return adjusted_block(main_block, size);
}

void* get_mapping(size_t size)
{
    void* mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) {
        errno = ENOMEM;
        return NULL;
    }
    /* If this mapping begins where the previous one ended, merge them */
    if (mapping_index > 0 && (uintptr_t) mapping == mappings[mapping_index - 1][1]) {
        mappings[--mapping_index][1] += size;
    } else {
        if (mapping_index == 1 << LOG2_NUM_MAPPINGS) {
            fprintf(stderr, "malloc: reached maximum number of memory mappings: %u\n", 1 << LOG2_NUM_MAPPINGS);
            return NULL;
        }
        mappings[mapping_index][0] = (uintptr_t) mapping;
        mappings[mapping_index][1] = (uintptr_t) mapping + size;
    }
    return mapping;
}

void coalesce(header_t* lower, header_t* higher)
{
    remove_from_bucket(lower);
    remove_from_bucket(higher);
    update_size(lower, lower->size + higher->size);
    insert_into_buckets(lower);
}
