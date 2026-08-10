/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌​​‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"

#ifndef CACHE_BLKS
#define CACHE_BLKS 64
#endif

// INTERNAL TYPE DEFINITIONS

struct block{
	char dirty; //if this block is dirty
	unsigned long long pos;//position in the backing storage device. Must be aligned to a multiple of the block size of the backing interface.
	uint8_t buf[CACHE_BLKSZ];
	struct block * next;
	int pin_count; //number of active references to this block
};


struct cache{
	struct storage* storage;
	uint8_t numBlocks;	//current number of valid blocks
	struct block * head;	//head of blocks linked list
	struct block * tail;	//tail of blocks linked list

    struct lock cache_lock;
};
/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    if(disk == NULL || cptr == NULL)
    	return -ENOTSUP;
    *cptr = kcalloc(1, sizeof(struct cache));
    if(*cptr == NULL)
        return -ENOMEM;
    (*cptr)->storage = disk;
    (*cptr)->numBlocks = 0;
    (*cptr)->head = NULL;
    (*cptr)->tail = NULL;

    lock_init(&(*cptr)->cache_lock);
    return 0;
}



/* small helper for fetch/init into an existing block */
static int cache_fetch_into(struct cache* cache, struct block* blk, unsigned long long pos) {
    int rc;
    blk->pos = pos;
    blk->dirty = 0;
    blk->pin_count = 0;
    rc = storage_fetch(cache->storage, pos, blk->buf, CACHE_BLKSZ);
    if (rc < 0)
        return rc;
    return 0;
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME
    int rc;
    struct block* temp;    

    if (cache == NULL || pptr == NULL)
	return -ENOTSUP;
    
    if ((pos % CACHE_BLKSZ) != 0)
        return -ENOTSUP;

    lock_acquire(&cache->cache_lock);

    if (cache->head == NULL) {
        cache->head = kcalloc(1, sizeof(struct block)); // if cache is empty
        if (cache->head == NULL){
            lock_release(&cache->cache_lock);
            return -ENOMEM;
        }

        cache->tail = cache->head;
        cache->head->next = NULL;
        rc = cache_fetch_into(cache, cache->head, pos);

        if (rc < 0) {
            kfree(cache->head);
            cache->head = NULL;
            cache->tail = NULL;
            lock_release(&cache->cache_lock);
            return rc;
        }
        cache->numBlocks += 1;
        cache->head->pin_count = 1;
        *pptr = cache->head->buf;
        lock_release(&cache->cache_lock);
        return 0;
    }

    // if head is the desired block
    if (cache->head->pos == pos) {
        cache->head->pin_count++;
        *pptr = cache->head->buf;
        if (cache->head == cache->tail) {
            lock_release(&cache->cache_lock);
            return 0; // single node, do nothing
        } else {
            // move head to tail to maintain LRU (head=LRU, tail=MRU)
            temp = cache->head;
            cache->head = temp->next;
            temp->next = NULL;
            cache->tail->next = temp;
            cache->tail = temp;
            lock_release(&cache->cache_lock);
            return 0;
        }
    }

    // search the list for a hit (middle or tail)
    {
        struct block* currBlock = cache->head;
        while (currBlock->next != NULL) {
            if (currBlock->next->pos == pos) {
                temp = currBlock->next;
                temp->pin_count++;
                *pptr = temp->buf;

                if (temp == cache->tail) {
                    lock_release(&cache->cache_lock);
                    return 0; // already MRU
                } else {
                    // unlink and append to tail
                    currBlock->next = temp->next;
                    temp->next = NULL;
                    cache->tail->next = temp;
                    cache->tail = temp;
                    lock_release(&cache->cache_lock);
                    return 0;
                }
            }
            currBlock = currBlock->next;
        }
    }

    // miss
    if (cache->numBlocks < CACHE_BLKS) {
        // cache not full: allocate, fetch, THEN link at tail
        struct block* nb = kcalloc(1, sizeof(struct block));
        if (nb == NULL){
            lock_release(&cache->cache_lock);
            return -ENOMEM;
        }

        nb->next = NULL; // prepare node
        rc = cache_fetch_into(cache, nb, pos);
        if (rc < 0) {
            kfree(nb);
            lock_release(&cache->cache_lock);
            return rc;
        }

        cache->tail->next = nb;
        cache->tail = nb;
        cache->numBlocks += 1;
        nb->pin_count = 1;
        *pptr = nb->buf;
        lock_release(&cache->cache_lock);
        return 0;
    }

    // cache full: find unpinned LRU block to evict
    struct block* victim = NULL;
    struct block* curr = cache->head;
    
    while (curr != NULL) {
        if (curr->pin_count == 0) {
            victim = curr;
            break;
        }
        curr = curr->next;
    }
    
    if (victim == NULL) {
        lock_release(&cache->cache_lock);
        return -ENOTSUP;
    }

    if (victim->dirty) {
        rc = storage_store(cache->storage, victim->pos, victim->buf, CACHE_BLKSZ);
        if (rc < 0){
            lock_release(&cache->cache_lock);
            return rc;
        }
        victim->dirty = 0;
    }

    rc = cache_fetch_into(cache, victim, pos);
    if (rc < 0){
        lock_release(&cache->cache_lock);
        return rc;
    }

    if (victim == cache->head) {
        if (cache->head == cache->tail) {
            victim->pin_count = 1;
            *pptr = victim->buf;
            lock_release(&cache->cache_lock);
            return 0;
        } else {
            temp = cache->head;
            cache->head = temp->next;
            temp->next = NULL;
            cache->tail->next = temp;
            cache->tail = temp;
            cache->tail->pin_count = 1;
            *pptr = cache->tail->buf;
            lock_release(&cache->cache_lock);
            return 0;
        }
    } else {
        struct block* prev = cache->head;
        while (prev->next != victim) {
            prev = prev->next;
        }
        prev->next = victim->next;
        

        if(cache->tail == victim) cache->tail = prev;


        victim->next = NULL;
        cache->tail->next = victim;
        cache->tail = victim;
        cache->tail->pin_count = 1;
        *pptr = cache->tail->buf;
        lock_release(&cache->cache_lock);
        return 0;
    }
}


/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME
    if (cache == NULL || pblk == NULL)
        return;

    lock_acquire(&cache->cache_lock);

    struct block* currBlock = cache->head;
    while (currBlock != NULL) {
        if (currBlock->buf == pblk) {
            if (currBlock->pin_count > 0) {
                currBlock->pin_count--;
            }
            if (dirty) {
                currBlock->dirty = 1;
            }
            break;
        }
        currBlock = currBlock->next;
    }
    
    lock_release(&cache->cache_lock);
    return;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    int rc;
    struct block* currBlock;

    if (cache == NULL)
        return -ENOTSUP;

    lock_acquire(&cache->cache_lock);

    currBlock = cache->head;
    while (currBlock != NULL) {
        if (currBlock->dirty) {
            rc = storage_store(cache->storage, currBlock->pos, currBlock->buf, CACHE_BLKSZ);
            if (rc < 0){
                lock_release(&cache->cache_lock);
                return rc;
            }
            currBlock->dirty = 0;
        }
        currBlock = currBlock->next;
    }
    lock_release(&cache->cache_lock);
    return 0;
}