/*
 * @file arena.c
 * @brief Arena allocator implementation
 */

#include "arena.h"

typedef struct block{

    unsigned char* mem;  /**< Pointer to the actual memory buffer. */
    size_t offset;       /**< Current used offset in bytes. */
    size_t capacity;     /**< Total capacity of this block in bytes. */

    struct block* next;  /**< Pointer to the next block; NULL at list tail. */

}block;

/** @brief Arena instance managing a linked list of blocks. */
struct arena{

    block* head;        /**< List head, used for traversal during destroy. */
    block* current;     /**< Currently active block for new allocations. */

};


/* ========== Internal Utilities ========== */

/**
 * @brief Rounds n up to the nearest multiple of ALIGNMENT.
 * @param n The original size.
 * @return The aligned size.
 *
 * ALIGNMENT must be a power of two.
 * Algorithm: (n + ALIGNMENT - 1) & ~(ALIGNMENT - 1)
 */
static inline size_t align_up(size_t n){

    return ( (n + ALIGNMENT - 1) & (~(ALIGNMENT - 1)) );

}

/**
 * @brief Appends a new memory block to the end of the arena's block list.
 * @param arena_pool Arena instance.
 * @param n Requested allocation size (ensures the new block can hold at least n bytes).
 * @return true on success, false on failure.
 *
 * Allocates a block descriptor and an actual memory buffer. The buffer capacity
 * starts from ARENA_BLOCK_DEFAULT_CAPACITY and doubles until it can accommodate
 * at least n bytes. On success, the new block is linked at the tail and becomes
 * the current block.
 *
 * @note arena_pool must not be NULL. Caller is responsible for validation.
 */
static bool arena_block_insert_tail(arena* const arena_pool, size_t n){

    if(arena_pool == NULL){
        return false;
    }

    /* Determine capacity: start from default, double until >= n */
    size_t new_block_capacity = ARENA_BLOCK_DEFAULT_CAPACITY;

    while (new_block_capacity < n){

        if(new_block_capacity > SIZE_MAX / 2){
            return false;
        }

        new_block_capacity <<= 1;

    }

    /* Allocate block descriptor */
    block* new_block = (block*)malloc(sizeof(block));

    if(new_block == NULL){
        return false;
    }
    
    /* Allocate the actual memory buffer */
    new_block->mem = (unsigned char*)malloc(new_block_capacity * sizeof(unsigned char));

    if(new_block->mem == NULL){
        free(new_block);
        return false;
    }

    new_block->offset = 0;
    new_block->capacity = new_block_capacity;
    new_block->next = NULL;

    /* Attach to list tail */
    if(arena_pool->head == NULL && arena_pool->current == NULL){
        arena_pool->head = arena_pool->current = new_block;
    }
    else{
        arena_pool->current->next = new_block;
        arena_pool->current = new_block;
    }

    return true;

}

/**
 * @brief Locates a block with sufficient remaining capacity for an allocation.
 * @param arena_pool Arena instance.
 * @param n Requested allocation size in bytes.
 * @return true if a suitable block is found or created, false on failure.
 *
 * Starting from the current block, traverses the block list sequentially and
 * checks whether align_padded remaining space can hold n bytes. If no existing
 * block has enough room, a new block is appended automatically via
 * arena_block_insert_tail(). The current pointer is advanced during traversal
 * and left at the block that will serve the allocation.
 *
 * @pre arena_pool != NULL
 */
static bool find_sufficient_block(arena* const arena_pool, size_t n){

    if(arena_pool == NULL){
        return false;
    }

    if(arena_pool->head == NULL && arena_pool->current == NULL){
        return arena_block_insert_tail(arena_pool, n);
    }

    while(arena_pool->current){

        size_t padded_offset = align_up(arena_pool->current->offset);

        if(n <= arena_pool->current->capacity - padded_offset){
            return true;
        }

        if(arena_pool->current->next == NULL){
            break;
        }

        arena_pool->current = arena_pool->current->next;

    }

    return arena_block_insert_tail(arena_pool, n);

}

/**
 * @brief Frees a single block and its associated memory buffer.
 * @param bck Block pointer; safe to pass NULL.
 *
 * Releases the memory buffer first, then the block descriptor itself.
 * Does not unlink from the list — the caller is responsible for managing
 * the linked list pointers.
 */
static void block_destroy(block* bck){

    if(bck == NULL){
        return;
    }

    free(bck->mem);
    free(bck);

}


/* ========== API Implementation ========== */

arena* arena_create(){

    arena* arena_pool = (arena*)calloc(1, sizeof(arena));

    if(arena_pool == NULL){
        return NULL;
    }
    /* head and current are zero-initialized to NULL by calloc.
     * The first block is lazily allocated on first use. */

    return arena_pool;

}

void* arena_calloc(arena* const arena_pool, size_t count, size_t size){

    /* Validate inputs */
    if(arena_pool == NULL){
        return NULL;
    }
    
    if(count == 0 || size == 0){
        return NULL;
    }

    if (size > SIZE_MAX / count){
        return NULL;
    }/* Multiplication overflow */

    size_t n_uchar = count * size;

    /* Ensure current block has room; allocate a new one if needed */
    if(find_sufficient_block(arena_pool, n_uchar) == false){
        return NULL;
    }

    /* Align the start address */
    size_t padded_offset = align_up(arena_pool->current->offset);

    if (n_uchar > arena_pool->current->capacity - padded_offset) {
        return NULL;
    }

    void* ptr = arena_pool->current->mem + padded_offset;

    /* Zero-initialize the allocated region */
    memset(ptr, 0, n_uchar);

    /* Advance offset */
    arena_pool->current->offset = padded_offset + n_uchar;

    return ptr;

}

void* arena_malloc(arena* const arena_pool, size_t size){

     /* Validate inputs */
    if(arena_pool == NULL){
        return NULL;
    }

    if (size == 0){
        return NULL;
    }

    /* Ensure current block has room */
    if(find_sufficient_block(arena_pool, size) == false){
        return NULL;
    }

    /* Align the start address */
    size_t padded_offset = align_up(arena_pool->current->offset);

    void* ptr = arena_pool->current->mem + padded_offset;

    /* Advance offset (no zero-initialization) */
    arena_pool->current->offset = padded_offset + size;

    return ptr;

}

void arena_reset(arena* const arena_pool){

    if(arena_pool == NULL){
        return;
    }

    block* cur_block = arena_pool->head;

    while(cur_block){

        cur_block->offset = 0;
        cur_block = cur_block->next;

    }

    arena_pool->current = arena_pool->head;

}

void arena_destroy(arena* const arena_pool){

    if(arena_pool == NULL){
        return;
    }

    /* Traverse and free all blocks */
    while(arena_pool->head != NULL){

        block* cur_block = arena_pool->head;
        arena_pool->head = cur_block->next;

        block_destroy(cur_block);

    }

    /* Free the arena struct itself */
    free(arena_pool);

}