#include "malloc.h"

#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

/*
 * The smallest unit of memory a user can request: 8 bytes, or 64 bits. We want to stay 8-byte aligned.
 * This number HAS TO be a power of two.
 */
#define MEM_UNIT 8 // bytes

/*
 * An 8-byte data structure that precedes all managed memory blocks. The first 63 bits represent the size
 * of the block, including its metadata (header and footer). The least significant bit is 1 if the block is free,
 * 0 otherwise.
 */
typedef struct header header_t;
struct header {
    uint64_t size:63;
    uint64_t free:1;
    header_t* next;
};

static header_t dummy_header = {.size = 0};

/*
 * This is used to get the offset between the beginning of a block and the beginning of the memory zone where
 * the user can write. The user can write over the `next` pointer while the block is in use.
 */
#define HEADER_SIZE sizeof (uint64_t)

/*
 * An 8-byte, single-member data structure that is kept at the end of every managed memory block. It also stores
 * the size of the block, including its metadata (header and footer). The purpose of this structure is to allow the
 * following block in memory to access the size of its previous block. Knowing the size, we can travel back to
 * the header to know whether the block is free. Note that we could easily have stored the free flag here also, but
 * it seems more cautious to store it in a single place.
 *
 * Note also that we could have typedef-ed footer_t as an alias for uint64_t, or we could have avoided any typedef
 * altogether here, but this definition makes the program clearer without adding any performance overhead.
 */
typedef struct footer {
    uint64_t size;
} footer_t;

#define FOOTER_SIZE sizeof (footer_t)

/*
 * The smallest block size that this program can manage.
 */
#define MIN_ALLOC (MEM_UNIT + HEADER_SIZE + FOOTER_SIZE)

/*
 * The smallest unit of memory we can request from or release to the OS.
 */
#define SYS_REQUEST_UNIT sysconf(_SC_PAGESIZE)
#define SYS_RELEASE_UNIT SYS_REQUEST_UNIT

/*
 * Memory blocks available to the user are stored in buckets, each of which is associated with a certain size range.
 * Here is the index to size mapping :
 * 0:   <empty>
 * 1:   <empty>
 * 2:   <empty>
 * 3:   24
 * 4:   32
 * 5:   40
 * ...  ...
 * n:   8*n
 * ...
 * 127: 1016
 * 128: {1024, 1032, 1040, ..., 2032, 2040} == {8*k : 2^7 <= k < 2^8}
 * 129: {8*k : 2^8 <= k < 2^9}
 * 130: {8*k : 2^9 <= k < 2^10}
 * ...  ...
 * n:   {8*k : 2^(n - 121) <= k < 2^(n - 120)}
 * ...
 * 165: {8*k : 2^44 <= k < 2^45}
 *
 * We assume that the address space is 48-bit long, which is why we stop at index 165. In other words, it is impossible,
 * even theoretically, to allocate a memory block of 2^48 bytes or more.
 * See https://stackoverflow.com/questions/6716946/why-do-x86-64-systems-have-only-a-48-bit-virtual-address-space.
 *
 * Inside each bucket, available blocks are kept sorted in ascending size order. When searching for a free block to
 * allocate to the user, the appropriate bucket is searched, and the first block with a sufficient size is taken.
 * Because the blocks within a bucket are sorted, the search is a best-fit one.
 */
#define NUM_BUCKETS 166
static header_t* buckets[NUM_BUCKETS];

static bool initialized = false;

static void initialize_buckets();
static void align_size(size_t* size);
static header_t* get_block_from_buckets(size_t size);
static header_t* get_block_from_os(size_t size);
void* malloc_(size_t size)
{
    if (size == 0) return NULL;
    if (!initialized) initialize_buckets();
    align_size(&size);
    header_t* header = get_block_from_buckets(size);
    if (!header) header = get_block_from_os(size);
    if (!header) return NULL;
    header->free = 0;
    return header + HEADER_SIZE;
}

void initialize_buckets()
{
    dummy_header.next = &dummy_header;
    /* First three buckets are empty */
    for (uint8_t i = 3; i < NUM_BUCKETS; i++) {
        buckets[i] = &dummy_header;
    }
    initialized = true;
}

void align_size(size_t* size)
{
    /* Round size up, to the closest multiple of MEM_UNIT, assuming that MEM_UNIT is a power of two. */
    *size = (*size + (MEM_UNIT - 1)) & ~(MEM_UNIT - 1);

    *size += HEADER_SIZE + FOOTER_SIZE;
}

static uint8_t bucket_index_from_size(size_t size);
static header_t* get_block_from_bucket(header_t* bucket, size_t size);
header_t* get_block_from_buckets(size_t size)
{
    for (uint8_t i = bucket_index_from_size(size); i < NUM_BUCKETS ; i++)
    {
        header_t* block;
        if ((block = get_block_from_bucket(buckets[i], size))) return block;
    }
    return NULL;
}

uint8_t bucket_index_from_size(size_t size)
{
    static uint8_t index_1024 = 1024 / MEM_UNIT, log2_1024 = 10;
    if (size < 1024) return size / MEM_UNIT;
    uint8_t log2;
    for (log2 = log2_1024; 1 << (log2 + 1) <= size; log2++);
    return log2 + (index_1024 - log2_1024);
}

static header_t* adjusted_block(header_t* block, size_t size);
header_t* get_block_from_bucket(header_t* bucket, size_t size)
{
    header_t* block = bucket;
    /* This is the condition to signal an empty bucket. */
    if (block->next == block) return NULL;
    block = block->next;
    for (; block->size > 0; block = block->next) {
        if (block->size >= size) {
            return adjusted_block(block, size);
        }
    }
    return NULL;
}

static void split_after(header_t* block, size_t size);
static void update_size(header_t* block, size_t size);
header_t* adjusted_block(header_t* block, size_t size)
{
    if (block->size <= size + MIN_ALLOC) return block;
    split_after(block, size);
    update_size(block, size);
    return block;
}

static void insert_into_buckets(header_t* block);
void split_after(header_t* block, size_t size)
{
    size_t remaining_size = block->size - size;
    header_t* new_block = block + size;
    update_size(new_block, remaining_size);
    new_block->free = 1;
    insert_into_buckets(new_block);
}

inline void update_size(header_t* block, size_t size)
{
    block->size = ((footer_t*) block + size - FOOTER_SIZE)->size = size;
}

static void insert_into_bucket(header_t* block_to_insert, header_t* bucket);
inline void insert_into_buckets(header_t* block)
{
    insert_into_bucket(block, buckets[bucket_index_from_size(block->size)]);
}

/*
 * Buckets are kept sorted according to block size ascending order. Ties are broken with an oldest-first rule;
 * see http://gee.cs.oswego.edu/dl/html/malloc.html.
 */
void insert_into_bucket(header_t* block_to_insert, header_t* bucket)
{
    header_t* pre_insertion_block;
    for (pre_insertion_block = bucket; ; pre_insertion_block = pre_insertion_block->next) {
        uint64_t next_block_size = pre_insertion_block->next->size;
        if (next_block_size == 0 || next_block_size > block_to_insert->size) break;
    }
    block_to_insert->next = pre_insertion_block->next;
    pre_insertion_block->next = block_to_insert;
}
