/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌​​‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    // Fill to fulfill spec
    struct uio base; //Uio struct associated with each file
    uint64_t fileSize; //length of the file in bytes
    struct ktfs_dir_entry dentry; //contains inode number for file and file name
    uint64_t pos; //current position in bytes for the file
    struct ktfs_fs * fs; //pointer to file system of this file
    struct ktfs_file * next; //pointer to next file that is opened
    struct lock file_lock; //lock for this file 				
};

struct ktfs_fs{
	struct filesystem  fs; //pointer to filesystem functions
	struct cache * cache; //cache associated with file system
	struct ktfs_superblock * superblock; //pointer to
};

struct ktfs_listing {
	struct uio base;
	struct ktfs_fs * fs;
	uint32_t index; //next dentry index to return
};



// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);
void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

//INTERNAL GLOBAL VARIABLES
//
static const struct uio_intf uio_intf = {
    .close = &ktfs_close,
    .read = &ktfs_fetch,
    .write = &ktfs_store,
    .cntl = &ktfs_cntl
};

static const struct uio_intf listing_uio_intf = {
	.close = &ktfs_listing_close,
	.read = &ktfs_listing_read,
	.write = NULL,
	.cntl = NULL
};

static struct ktfs_file * g_open_head = NULL; //global open heads

static struct lock g_open_head_lock; //lock for global open files list




/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {

    //FIX ME
    if (!name || !cache) 
	    return -ENOTSUP;

    struct ktfs_fs *fs = kcalloc(1, sizeof(struct ktfs_fs));
    if (!fs) 
	    return -ENOTSUP;

    fs->cache        = cache;
    fs->fs.open    = ktfs_open;
    fs->fs.create  = ktfs_create;
    fs->fs.delete  = ktfs_delete;
    fs->fs.flush   = ktfs_flush;

    lock_init(&g_open_head_lock);

    int rc = attach_filesystem(name, &fs->fs);
    if (rc) {
        kfree(fs);
        return rc;
    }
    return 0;
}


//helper function to get an inode given the inode number
static int get_inode(struct cache * cache, struct ktfs_superblock * superblock, uint16_t inodeNum, struct ktfs_inode * inode_out){
    void * temp = NULL;
    int rc;
    uint64_t inode_block_start = (1 + superblock->inode_bitmap_block_count + 
                                   superblock->bitmap_block_count) * KTFS_BLKSZ;
    uint64_t inode_block_num = inodeNum / (KTFS_BLKSZ / KTFS_INOSZ);
    uint64_t pos = inode_block_start + (inode_block_num * KTFS_BLKSZ);
    
    rc = cache_get_block(cache, pos, &temp);
    if(rc < 0)
        return rc;
    
    struct ktfs_inode * inodeBlock = (struct ktfs_inode *)temp;
    *inode_out = inodeBlock[inodeNum % (KTFS_BLKSZ/KTFS_INOSZ)];
    cache_release_block(cache, temp, 0);
    return 0;
}
//sets inode bitmap bit as in use or not in use dependent on status bit
static void set_inode_status(struct ktfs_superblock * superblock, struct cache * cache, uint32_t inodeNum,uint32_t status){
    void * temp = NULL;
    int rc = 0;
    uint8_t * inodeBitMapBlock = NULL;

    uint32_t inodesPerBitmapBlock = KTFS_BLKSZ * 8;
    uint32_t inodeBitMapBlockNum = inodeNum / inodesPerBitmapBlock;
    

    rc = cache_get_block(cache, (1 + inodeBitMapBlockNum) * KTFS_BLKSZ, &temp);
    if(rc)
	    return;

    inodeBitMapBlock = (uint8_t *)temp;
    uint32_t bitInBlock = inodeNum % inodesPerBitmapBlock;
    uint32_t byteIdx = bitInBlock / 8;
    uint32_t bitIdx  = bitInBlock % 8;

    uint8_t select = (uint8_t)(1u << bitIdx);
    if (status)
        inodeBitMapBlock[byteIdx] |= select;
    else
        inodeBitMapBlock[byteIdx] &= ~select;

    __sync_synchronize();
    cache_release_block(cache, temp, 1);
    __sync_synchronize();
}


static int get_dentry_at_index(struct ktfs_superblock * superblock, struct cache * cache, const struct ktfs_inode * rootInode, uint32_t index, struct ktfs_dir_entry * dentry_out){
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;

    uint32_t per_block = (uint32_t)(KTFS_BLKSZ / KTFS_DENSZ);
    uint32_t ptrsPerBlock = (uint32_t)(KTFS_BLKSZ / sizeof(uint32_t));
    void * temp = NULL;
    int rc;

    // direct blocks
    uint32_t direct_cap = KTFS_NUM_DIRECT_DATA_BLOCKS * per_block;

    if (index < direct_cap) {
        uint32_t block_idx = index / per_block;
        uint32_t entry_idx = index % per_block;
        uint32_t dataIdx   = rootInode->block[block_idx];

        rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        struct ktfs_dir_entry * dentryBlock = (struct ktfs_dir_entry *)temp;
        *dentry_out = dentryBlock[entry_idx];
        cache_release_block(cache, temp, 0);
        return 0;
    }

    index -= direct_cap;

    // indirect block
    uint32_t indirect_cap = ptrsPerBlock * per_block;

    if (index < indirect_cap && rootInode->indirect != 0) {
        rc = cache_get_block(cache, (1 + K + B + N + rootInode->indirect) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        uint32_t * indirect = (uint32_t *)temp;

        uint32_t ptr_idx   = index / per_block;
        uint32_t entry_idx = index % per_block;
        uint32_t dataIdx   = indirect[ptr_idx];

        cache_release_block(cache, temp, 0);

        rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        struct ktfs_dir_entry * dentryBlock = (struct ktfs_dir_entry *)temp;
        *dentry_out = dentryBlock[entry_idx];
        cache_release_block(cache, temp, 0);
        return 0;
    }

    index -= indirect_cap;

    // DOUBLE INDIRECT BLOCKS 
    uint32_t per_indirect_block_dentries    = ptrsPerBlock * per_block;
    uint32_t per_dindirect_block_dentries   = ptrsPerBlock * per_indirect_block_dentries;

    for (uint32_t d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        if (index >= per_dindirect_block_dentries) {
            index -= per_dindirect_block_dentries;
            continue;
        }

        uint32_t dind_block_idx = rootInode->dindirect[d];
        if (dind_block_idx == 0)
            return -ENOTSUP;

        rc = cache_get_block(cache, (1 + K + B + N + dind_block_idx) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        uint32_t * dind = (uint32_t *)temp;

        uint32_t second_idx = index / per_indirect_block_dentries;
        uint32_t remain     = index % per_indirect_block_dentries;
        uint32_t second_block_idx = dind[second_idx];

        cache_release_block(cache, temp, 0);

        rc = cache_get_block(cache, (1 + K + B + N + second_block_idx) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        uint32_t * ind = (uint32_t *)temp;

        uint32_t third_idx = remain / per_block;
        uint32_t entry_idx = remain % per_block;
        uint32_t dataIdx   = ind[third_idx];

        cache_release_block(cache, temp, 0);

        rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        struct ktfs_dir_entry * dentryBlock = (struct ktfs_dir_entry *)temp;
        *dentry_out = dentryBlock[entry_idx];
        cache_release_block(cache, temp, 0);
        return 0;
    }

    return -ENOENT;
}





static void set_data_block_status(struct ktfs_superblock * superblock, uint32_t dataNum, struct cache * cache, uint32_t status){
    void * temp = NULL;
    int rc = 0;
    uint8_t * dataBitMapBlock = NULL;

    uint32_t datasPerBitmapBlock = KTFS_BLKSZ * 8;
    uint32_t dataBitMapBlockNum = dataNum / datasPerBitmapBlock;

    
    rc = cache_get_block(cache, (1 + dataBitMapBlockNum + superblock->inode_bitmap_block_count) * KTFS_BLKSZ, &temp);
    if(rc)
	    return;
    dataBitMapBlock = (uint8_t *)temp;
    uint32_t bitInBlock = dataNum % datasPerBitmapBlock;
    uint32_t byteIdx = bitInBlock / 8;
    uint32_t bitIdx  = bitInBlock % 8;

    uint8_t select = (uint8_t)(1u << bitIdx);
    if (status)
        dataBitMapBlock[byteIdx] |= select;
    else
        dataBitMapBlock[byteIdx] &= ~select;

    __sync_synchronize();
    cache_release_block(cache, temp, 1);
    __sync_synchronize();
}





static void free_data_blocks(struct ktfs_superblock * superblock, uint32_t size, struct cache * cache, struct ktfs_inode inode){
    uint32_t numDataBlocks = (size + KTFS_BLKSZ - 1)/KTFS_BLKSZ; //number of data blocks for the file, not including indirect and doubly indirect
    uint32_t x = 0;
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    int rc = 0;
    void * temp = NULL;
    
    for (int j = 0; j < KTFS_NUM_DIRECT_DATA_BLOCKS && x < numDataBlocks; j++) {
    

        uint32_t dataIdx = inode.block[j];
	set_data_block_status(superblock, dataIdx, cache, 0); 
   	x++;
    }
    if(x == numDataBlocks)
	    return;

    // INDIRECT BLOCK
        rc = cache_get_block(cache, (1 + N + B + K + (uint64_t)inode.indirect) * KTFS_BLKSZ, &temp);
	set_data_block_status(superblock, inode.indirect, cache, 0);
        if (rc < 0){
            return;
        }
	uint32_t * indirect = NULL;
        indirect = (uint32_t*)temp;

        for (int i = 0; i < (KTFS_BLKSZ / 4)  && x < numDataBlocks; i++) {

		set_data_block_status(superblock, indirect[i], cache, 0);
		x++;
        }
    	cache_release_block(cache, temp, 0);

	if(x == numDataBlocks)
		return;
   

    // DOUBLE-INDIRECT BLOCKS
    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS && x < numDataBlocks; i++) {


        rc = cache_get_block(cache, (1 + N + K + B + (uint64_t)inode.dindirect[i]) * KTFS_BLKSZ, &temp);
	set_data_block_status(superblock, inode.dindirect[i], cache, 0);
        if (rc < 0){
            return;
        }
	uint32_t * dindirect = NULL;
        dindirect = (uint32_t*)temp;

        for (int j = 0; j < (KTFS_BLKSZ / 4)  && x < numDataBlocks; j++) {

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + N + K + B + (uint64_t)dindirect[j]) * KTFS_BLKSZ, &temp2);
	    set_data_block_status(superblock, dindirect[j], cache, 0);
            if (rc < 0){
                cache_release_block(cache, temp, 0);
                return;
            }
	    uint32_t * indirect = NULL;
            indirect = (uint32_t*)temp2;

            for (int k = 0; k < (KTFS_BLKSZ / 4)  && x < numDataBlocks; k++) {
		set_data_block_status(superblock, indirect[k], cache, 0);
		x++;
                }
                cache_release_block(cache, temp2, 0);
            }
	cache_release_block(cache, temp, 0);
        }
    
    }	


static int find_free_inode(struct ktfs_superblock * superblock, struct cache * cache){
	void * temp = NULL;
	uint8_t * inodeBlock = NULL;
	uint8_t byte = 0;
	uint8_t mask = 0;
	int rc;
	for(int i = 0; i < superblock->inode_bitmap_block_count; i++){
		rc = cache_get_block(cache, (1 + i) * KTFS_BLKSZ, &temp);
		if(rc)
			return -ENOTSUP;
		inodeBlock = (uint8_t *) temp;
		for(int j = 0; j < KTFS_BLKSZ; j++){
			byte = inodeBlock[j];
			for(int k = 0; k < 8; k++){
				mask = 1u << k;
				if((byte & mask) == 0){
					cache_release_block(cache, temp, 0);
					return i * (KTFS_BLKSZ * 8) + j * 8 + k;
				}
			}
		}
	}
	return -ENOTSUP; //no free inodes	
}

static int find_free_data_block(struct ktfs_superblock * superblock, struct cache * cache){
    void * temp = NULL;
    uint8_t * dataBlock = NULL;
    uint8_t byte = 0;
    uint8_t mask = 0;
    int rc;

    for (uint32_t i = 0; i < superblock->bitmap_block_count; i++) {
        rc = cache_get_block(cache,(1 + superblock->inode_bitmap_block_count + i) * KTFS_BLKSZ,&temp);
        if (rc)
            return -ENOTSUP;

        dataBlock = (uint8_t *)temp;

        for (uint32_t j = 0; j < KTFS_BLKSZ; j++) {
            byte = dataBlock[j];
	    for (uint32_t k = 0; k < 8; k++) {
                mask = (uint8_t)(1u << k);
                if ((byte & mask) == 0) {
                    cache_release_block(cache, temp, 0);
                    return (int)(i * (KTFS_BLKSZ * 8) + j * 8 + k);
                }
            }
        }

        cache_release_block(cache, temp, 0);
    }

    return -ENOTSUP; // no free data blocks
}




static void add_dentry(const char * name,struct ktfs_superblock * superblock, struct cache * cache, uint16_t inodeNum){

    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    int rc = 0;
    uint32_t ptrsPerBlock = KTFS_BLKSZ / sizeof(uint32_t);

    struct ktfs_inode * rootInode = NULL;
    void * root = NULL;
    uint16_t rootNum = superblock->root_directory_inode;
    uint64_t inode_block_start = (1 + superblock->inode_bitmap_block_count +
                                   superblock->bitmap_block_count) * KTFS_BLKSZ;
    uint64_t inode_block_num = rootNum / (KTFS_BLKSZ / KTFS_INOSZ);
    uint64_t pos = inode_block_start + (inode_block_num * KTFS_BLKSZ);

    rc = cache_get_block(cache, pos, &root);
    if(rc < 0)
        return;

    struct ktfs_inode * rootBlock = (struct ktfs_inode *)root;
    rootInode = &rootBlock[rootNum % (KTFS_BLKSZ/KTFS_INOSZ)];

    uint32_t size_before = rootInode->size;
    uint32_t size_after = size_before + KTFS_DENSZ;
    uint32_t size_direct = KTFS_BLKSZ * KTFS_NUM_DIRECT_DATA_BLOCKS;
    uint32_t size_indirect = size_direct + KTFS_BLKSZ * ptrsPerBlock;

    struct ktfs_dir_entry dentry;
    strncpy(dentry.name, name, sizeof(dentry.name));
    dentry.inode = inodeNum;

    void * temp = NULL;
    struct ktfs_dir_entry * finalDentryBlock = NULL;

    if(size_after <= size_direct){
        uint32_t directBlockNum = (size_after - 1) / KTFS_BLKSZ;
        uint32_t directBlockIdx = rootInode->block[directBlockNum];

        if(size_before % KTFS_BLKSZ == 0){
            if(directBlockIdx == 0){
                int newData = find_free_data_block(superblock, cache);
                if(newData < 0){
                    cache_release_block(cache, root, 0);
                    return;
                }
                set_data_block_status(superblock, newData, cache, 1);
                rootInode->block[directBlockNum] = (uint32_t)newData;
                directBlockIdx = (uint32_t)newData;
            }
        }

        rc = cache_get_block(cache, (1 + K + B + N + directBlockIdx) * KTFS_BLKSZ, &temp);
        if(rc){
            cache_release_block(cache, root, 0);
            return;
        }

        finalDentryBlock = (struct ktfs_dir_entry *) temp;
        finalDentryBlock[((size_after - 1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)] = dentry;
        rootInode->size = size_after;
        __sync_synchronize();
        cache_release_block(cache, temp, 1);
        __sync_synchronize();
        cache_release_block(cache, root, 1);
        __sync_synchronize();
        return;
    }

    else if(size_after > size_direct && size_after <= size_indirect){
        uint32_t offset = size_after - size_direct;
        uint32_t indirectBlockNum = (offset - 1) / KTFS_BLKSZ;

        if(rootInode->indirect == 0){
            int newIndirectBlk = find_free_data_block(superblock, cache);
            if(newIndirectBlk < 0){
                cache_release_block(cache, root, 0);
                return;
            }
            set_data_block_status(superblock, newIndirectBlk, cache, 1);
            rootInode->indirect = (uint32_t)newIndirectBlk;
        }

        void * temp1 = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + rootInode->indirect) * KTFS_BLKSZ, &temp1);
        if(rc){
            cache_release_block(cache, root, 0);
            return;
        }

        uint32_t * indirectBlock = (uint32_t *) temp1;

        if(size_before % KTFS_BLKSZ == 0){
            if(indirectBlock[indirectBlockNum] == 0){
                int newData = find_free_data_block(superblock, cache);
                if(newData < 0){
                    cache_release_block(cache, temp1, 0);
                    cache_release_block(cache, root, 0);
                    return;
                }
                set_data_block_status(superblock, newData, cache, 1);
                indirectBlock[indirectBlockNum] = (uint32_t)newData;
            }
        }

        uint32_t finalDentryBlockIdx = indirectBlock[indirectBlockNum];
        rc = cache_get_block(cache, (1 + K + B + N + finalDentryBlockIdx) * KTFS_BLKSZ, &temp);
        if(rc){
            cache_release_block(cache, temp1, 0);
            cache_release_block(cache, root, 0);
            return;
        }

        finalDentryBlock = (struct ktfs_dir_entry *) temp;
        finalDentryBlock[((size_after - 1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)] = dentry;
        rootInode->size = size_after;
        __sync_synchronize();
        cache_release_block(cache, temp, 1);
        __sync_synchronize();
        cache_release_block(cache, temp1, 1);
        __sync_synchronize();
        cache_release_block(cache, root, 1);
        __sync_synchronize();
        return;
    }

    else{
        uint32_t offset = size_after - size_indirect;
        uint32_t big_chunk = KTFS_BLKSZ * ptrsPerBlock * ptrsPerBlock;
        uint32_t mid_chunk = KTFS_BLKSZ * ptrsPerBlock;

        uint32_t dindirectBlockNum = (offset - 1) / big_chunk;
        uint32_t rem1 = offset - dindirectBlockNum * big_chunk;
        uint32_t indirectBlockNum = (rem1 - 1) / mid_chunk;
        uint32_t rem2 = rem1 - indirectBlockNum * mid_chunk;
        uint32_t directBlockNum = (rem2 - 1) / KTFS_BLKSZ;

        if(rootInode->dindirect[dindirectBlockNum] == 0){
            int newDindirectBlk = find_free_data_block(superblock, cache);
            if(newDindirectBlk < 0){
                cache_release_block(cache, root, 0);
                return;
            }
            set_data_block_status(superblock, newDindirectBlk, cache, 1);
            rootInode->dindirect[dindirectBlockNum] = (uint32_t)newDindirectBlk;
        }

        void * temp2 = NULL;
        rc = cache_get_block(cache, (1 + B + K + N + rootInode->dindirect[dindirectBlockNum]) * KTFS_BLKSZ, &temp2);
        if(rc){
            cache_release_block(cache, root, 0);
            return;
        }

        uint32_t * dindirectBlock = (uint32_t *) temp2;

        if(dindirectBlock[indirectBlockNum] == 0){
            int newIndirectBlk = find_free_data_block(superblock, cache);
            if(newIndirectBlk < 0){
                cache_release_block(cache, temp2, 0);
                cache_release_block(cache, root, 0);
                return;
            }
            set_data_block_status(superblock, newIndirectBlk, cache, 1);
            dindirectBlock[indirectBlockNum] = (uint32_t)newIndirectBlk;
        }

        void * temp1 = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + dindirectBlock[indirectBlockNum]) * KTFS_BLKSZ, &temp1);
        if(rc){
            cache_release_block(cache, temp2, 0);
            cache_release_block(cache, root, 0);
            return;
        }

        uint32_t * indirectBlock = (uint32_t *) temp1;

        if(size_before % KTFS_BLKSZ == 0){
            if(indirectBlock[directBlockNum] == 0){
                int newData = find_free_data_block(superblock, cache);
                if(newData < 0){
                    cache_release_block(cache, temp1, 0);
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, root, 0);
                    return;
                }
                set_data_block_status(superblock, newData, cache, 1);
                indirectBlock[directBlockNum] = (uint32_t)newData;
            }
        }

        uint32_t directBlockIdx = indirectBlock[directBlockNum];
        rc = cache_get_block(cache, (1 + K + B + N + directBlockIdx) * KTFS_BLKSZ, &temp);
        if(rc){
            cache_release_block(cache, temp1, 0);
            cache_release_block(cache, temp2, 0);
            cache_release_block(cache, root, 0);
            return;
        }

        finalDentryBlock = (struct ktfs_dir_entry *) temp;
        finalDentryBlock[((size_after - 1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)] = dentry;
        rootInode->size = size_after;
        __sync_synchronize();
        cache_release_block(cache, temp, 1);
        __sync_synchronize();
        cache_release_block(cache, temp1, 1);
        __sync_synchronize();
        cache_release_block(cache, temp2, 1);
        __sync_synchronize();
        cache_release_block(cache, root, 1);
        __sync_synchronize();
        return;
    }
}




//swap the dentry to be deleted with the final dentry to keep everything contiguous and decrement rootInode size
static void delete_dentry(struct ktfs_superblock * superblock, struct cache * cache, struct ktfs_dir_entry * dentry){
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    int rc = 0;
    uint32_t ptrsPerBlock = KTFS_BLKSZ / sizeof(uint32_t);
    
    //get rootInode
    struct ktfs_inode * rootInode = NULL;
    void * root = NULL;
    uint16_t rootNum = superblock->root_directory_inode;
    uint64_t inode_block_start = (1 + superblock->inode_bitmap_block_count +
                                   superblock->bitmap_block_count) * KTFS_BLKSZ;
    uint64_t inode_block_num = rootNum / (KTFS_BLKSZ / KTFS_INOSZ);
    uint64_t pos = inode_block_start + (inode_block_num * KTFS_BLKSZ);

    rc = cache_get_block(cache, pos, &root);
    if(rc < 0)
        return;

    struct ktfs_inode * rootBlock = (struct ktfs_inode *)root;
    rootInode = &rootBlock[rootNum % (KTFS_BLKSZ/KTFS_INOSZ)];
    

    
    
    
    
    
    //get the final dentry
    uint32_t size = rootInode->size;
    uint32_t size_direct = KTFS_BLKSZ * KTFS_NUM_DIRECT_DATA_BLOCKS;
    uint32_t size_indirect = size_direct + KTFS_BLKSZ * ptrsPerBlock;

    void * temp = NULL;
    struct ktfs_dir_entry * finalDentryBlock = NULL;
    if(size <= size_direct){ //this mean our final dentry is in a direct block
    	uint32_t directBlockNum = (size - 1)/KTFS_BLKSZ;
	uint32_t directBlockIdx = rootInode->block[directBlockNum];
	rc = cache_get_block(cache, (1 + K + B + N + directBlockIdx) * KTFS_BLKSZ, &temp);
	if(rc)
		return;
        finalDentryBlock = (struct ktfs_dir_entry *) temp;
        
	*dentry = (finalDentryBlock[((size-1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)]);
        rootInode-> size -= KTFS_DENSZ;
     	cache_release_block(cache, temp, 0);
	__sync_synchronize();
	cache_release_block(cache, root, 1);
	__sync_synchronize();
        return;

    }

    else if(size > size_direct && size <= size_indirect){//this means our final dentry is in an indirect block
	size = size - KTFS_BLKSZ * KTFS_NUM_DIRECT_DATA_BLOCKS;
	uint32_t * indirectBlock = NULL;
	void * temp1 = NULL;
	uint32_t indirectIdx = rootInode->indirect;
	rc = cache_get_block(cache, (1 + K + B + N + indirectIdx) * KTFS_BLKSZ, &temp1);
	if(rc)
		return;
	indirectBlock = (uint32_t *) temp1;
	uint32_t indirectBlockNum = (size-1)/KTFS_BLKSZ;
	uint32_t finalDentryBlockIdx = indirectBlock[indirectBlockNum];
	rc = cache_get_block(cache, (1 + K + B + N + finalDentryBlockIdx) * KTFS_BLKSZ, &temp);
	if(rc)
		return;
	finalDentryBlock = (struct ktfs_dir_entry *) temp;
	*dentry = (finalDentryBlock[((size-1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)]);
        rootInode-> size -= KTFS_DENSZ;
	cache_release_block(cache, temp1, 0);
        cache_release_block(cache, temp, 0);
	__sync_synchronize();
	cache_release_block(cache, root, 1);
	__sync_synchronize();
        return;


    }

    else{ // this means our final dentry is in a doubly indirect block
    	size = size - KTFS_BLKSZ * KTFS_NUM_DIRECT_DATA_BLOCKS - KTFS_BLKSZ * ptrsPerBlock;
	uint32_t dindirectBlockNum = (size-1)/(KTFS_BLKSZ* ptrsPerBlock * ptrsPerBlock);
	uint32_t dindirectBlockIdx = rootInode->dindirect[dindirectBlockNum];
	void * temp2 = NULL;
	rc = cache_get_block(cache, (1 + B + K + N + dindirectBlockIdx) * KTFS_BLKSZ, &temp2);
	if(rc)
		return;
	uint32_t * dindirectBlock = NULL;
	dindirectBlock = (uint32_t *) temp2;
	size = size - (dindirectBlockNum * KTFS_BLKSZ * ptrsPerBlock * ptrsPerBlock);
	uint32_t indirectBlockNum = (size-1)/(KTFS_BLKSZ * ptrsPerBlock);
	uint32_t indirectBlockIdx = dindirectBlock[indirectBlockNum];
	void * temp1 = NULL;
	rc = cache_get_block(cache, (1 + K + B + N + indirectBlockIdx) * KTFS_BLKSZ, &temp1);
	if(rc)
		return;
	uint32_t * indirectBlock = (uint32_t *) temp1;
	size = size - indirectBlockNum * KTFS_BLKSZ * ptrsPerBlock;
	uint32_t directBlockNum = (size-1)/(KTFS_BLKSZ);
	uint32_t directBlockIdx = indirectBlock[directBlockNum];
	rc = cache_get_block(cache, (1 + K + B + N + directBlockIdx) * KTFS_BLKSZ, &temp);
	if(rc)
		return;
	finalDentryBlock = (struct ktfs_dir_entry *) temp;
	*dentry = (finalDentryBlock[((size-1)/KTFS_DENSZ) % (KTFS_BLKSZ/KTFS_DENSZ)]);
        rootInode-> size -= KTFS_DENSZ;
	cache_release_block(cache, temp2, 0);
        cache_release_block(cache, temp1, 0);
        cache_release_block(cache, temp, 0);
	__sync_synchronize();
	cache_release_block(cache, root, 1);
	__sync_synchronize();
        return;
        
    }


}	


static int get_or_alloc_data_block(struct ktfs_superblock * superblock, struct cache * cache, struct ktfs_inode * inode, uint32_t blkno, uint32_t * dataIdx_out){
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    uint32_t ptrsPerBlock = KTFS_BLKSZ / sizeof(uint32_t);
    int rc;

    if (blkno < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        if (inode->block[blkno] == 0) {
            int newidx = find_free_data_block(superblock, cache);
            if (newidx < 0)
                return newidx;
            set_data_block_status(superblock, newidx, cache, 1);
            inode->block[blkno] = newidx;
        }
        *dataIdx_out = inode->block[blkno];
        return 0;
    }

    blkno -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    if (blkno < ptrsPerBlock) {
        if (inode->indirect == 0) {
            int ib = find_free_data_block(superblock, cache);
            if (ib < 0)
                return ib;
            set_data_block_status(superblock, ib, cache, 1);

            void * temp = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + ib) * KTFS_BLKSZ, &temp);
            if (rc < 0)
                return rc;
            memset(temp, 0, KTFS_BLKSZ);
            __sync_synchronize();
            cache_release_block(cache, temp, 1);
            __sync_synchronize();

            inode->indirect = ib;
        }

        void * temp = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + inode->indirect) * KTFS_BLKSZ, &temp);
        if (rc < 0)
            return rc;

        uint32_t * indirect = (uint32_t *)temp;
        int dirty = 0;

        if (indirect[blkno] == 0) {
            int newidx = find_free_data_block(superblock, cache);
            if (newidx < 0) {
                cache_release_block(cache, temp, 0);
                return newidx;
            }
            set_data_block_status(superblock, newidx, cache, 1);
            indirect[blkno] = newidx;
            dirty = 1;
        }

        *dataIdx_out = indirect[blkno];
        __sync_synchronize();
        cache_release_block(cache, temp, dirty);
        __sync_synchronize();
        return 0;
    }

    blkno -= ptrsPerBlock;

    {
        uint32_t perDind = ptrsPerBlock * ptrsPerBlock;
        uint32_t outer = blkno / perDind;
        uint32_t inner = blkno % perDind;

        if (outer >= KTFS_NUM_DINDIRECT_BLOCKS)
            return -ENOTSUP;

        if (inode->dindirect[outer] == 0) {
            int dib = find_free_data_block(superblock, cache);
            if (dib < 0)
                return dib;
            set_data_block_status(superblock, dib, cache, 1);

            void * temp = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + dib) * KTFS_BLKSZ, &temp);
            if (rc < 0)
                return rc;
            memset(temp, 0, KTFS_BLKSZ);
            __sync_synchronize();
            cache_release_block(cache, temp, 1);
            __sync_synchronize();

            inode->dindirect[outer] = dib;
        }

        void * temp_d = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + inode->dindirect[outer]) * KTFS_BLKSZ, &temp_d);
        if (rc < 0)
            return rc;

        uint32_t * dind = (uint32_t *)temp_d;
        uint32_t second = inner / ptrsPerBlock;
        uint32_t idxInSecond = inner % ptrsPerBlock;
        int dirty_d = 0;

        if (second >= ptrsPerBlock) {
            cache_release_block(cache, temp_d, 0);
            return -ENOTSUP;
        }

        if (dind[second] == 0) {
            int ib = find_free_data_block(superblock, cache);
            if (ib < 0) {
                cache_release_block(cache, temp_d, 0);
                return ib;
            }
            set_data_block_status(superblock, ib, cache, 1);

            void * temp_i = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + ib) * KTFS_BLKSZ, &temp_i);
            if (rc < 0) {
                cache_release_block(cache, temp_d, 0);
                return rc;
            }
            memset(temp_i, 0, KTFS_BLKSZ);
            __sync_synchronize();
            cache_release_block(cache, temp_i, 1);
            __sync_synchronize();

            dind[second] = ib;
            dirty_d = 1;
        }

        uint32_t secondIdx = dind[second];
        __sync_synchronize();
        cache_release_block(cache, temp_d, dirty_d);
        __sync_synchronize();

        void * temp_i2 = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + secondIdx) * KTFS_BLKSZ, &temp_i2);
        if (rc < 0)
            return rc;

        uint32_t * ind = (uint32_t *)temp_i2;
        int dirty_i = 0;

        if (ind[idxInSecond] == 0) {
            int newidx = find_free_data_block(superblock, cache);
            if (newidx < 0) {
                cache_release_block(cache, temp_i2, 0);
                return newidx;
            }
            set_data_block_status(superblock, newidx, cache, 1);
            ind[idxInSecond] = newidx;
            dirty_i = 1;
        }

        *dataIdx_out = ind[idxInSecond];
        __sync_synchronize();
        cache_release_block(cache, temp_i2, dirty_i);
        __sync_synchronize();
        return 0;
    }
}



















/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    // FIXME
    if(fs == NULL || uioptr == NULL)
	    return -ENOTSUP;



    
    //setup
    struct ktfs_fs * filesys = (void*)fs - offsetof(struct ktfs_fs, fs);
    struct cache * cache = filesys->cache;

    //
    if(name == NULL || name[0] == '\0'){
    	struct ktfs_listing * listing = kcalloc(1, sizeof(struct ktfs_listing));
	if(!listing)
		return -ENOTSUP;
	listing->fs = filesys;
	listing->index = 0;
	uio_init1(&listing->base, &listing_uio_intf);
	*uioptr = &listing -> base;
	return 0;
    }





    struct ktfs_file * file = kcalloc(1, sizeof(struct ktfs_file));
    if(!file)
        return -ENOMEM;
    
    file->fs = filesys;
    uio_init1(&file->base, &uio_intf);
    file->pos = 0;
    lock_init(&file->file_lock);
    
    //testing
    
        
    
    //get superblock
    void * temp = NULL;
    int rc = cache_get_block(cache, 0, &temp); //the super block is always the 0th block in memory
    if(rc < 0){
        kfree(file);
        return rc;
    }
    
    
    //get root inode and numDentries
    struct ktfs_superblock * superblock =(struct ktfs_superblock *) temp;
    //testing
     
    //
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    struct ktfs_inode rootInode;
    rc = get_inode(cache, superblock, superblock->root_directory_inode, &rootInode);
    if(rc < 0){
        cache_release_block(cache, temp, 0);
        kfree(file);
        return rc;
    }
    
    uint32_t numDentries = rootInode.size/KTFS_DENSZ; //there are 16bytes per dentry
    cache_release_block(cache, temp, 0);
    temp = NULL;
    
    int i = 0;
    struct ktfs_dir_entry * dentryBlock; //each dir entry is a half word
    uint32_t dentryBlkIdx; 
    
    //go through direct blocks
    for(int j = 0; j < KTFS_NUM_DIRECT_DATA_BLOCKS && i < numDentries; j++){   
    	dentryBlkIdx = rootInode.block[j];
        
	    rc = cache_get_block(cache, (1 + B + K + N + dentryBlkIdx) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            kfree(file);
            return rc;
        }
        dentryBlock = (struct ktfs_dir_entry *)temp;
	for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
		if(strcmp(dentryBlock[k].name, name) == 0){
			file->dentry = dentryBlock[k];
			struct ktfs_inode fileInode;
			void * temp_sb = NULL;
			rc = cache_get_block(cache, 0, &temp_sb);
			if(rc < 0){
                            cache_release_block(cache, temp, 0);
                            kfree(file);
                            return rc;
                        }
			struct ktfs_superblock * sb = (struct ktfs_superblock *)temp_sb;
			rc = get_inode(cache, sb, dentryBlock[k].inode, &fileInode);
			cache_release_block(cache, temp_sb, 0);
			if(rc < 0){
                            cache_release_block(cache, temp, 0);
                            kfree(file);
                            return rc;
                        }
			file->fileSize = fileInode.size;
			lock_acquire(&g_open_head_lock);
			file->next = g_open_head;
			g_open_head = file;
			lock_release(&g_open_head_lock);
			*uioptr = &file->base;
			cache_release_block(cache, temp, 0);
			return 0;
		}
		i++;
	}
        cache_release_block(cache, temp, 0);
    }

    if(i == numDentries){//this means the file was not found
	    kfree(file);
	    return -ENOENT;
    }

    //moving on to the indirect block
    uint32_t * indirect;
    if(rootInode.indirect != 0){
        rc = cache_get_block(cache,(1 + K + B + N + rootInode.indirect) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            kfree(file);
            return rc;
        }
        indirect = (uint32_t *) temp;
        
        for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){//4 bytes per dentry block index
            
            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                kfree(file);
                return rc;
            }
            
            dentryBlock = (struct ktfs_dir_entry *)temp2;
            for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                if(strcmp(dentryBlock[k].name, name) == 0){
                    file->dentry = dentryBlock[k];
                    struct ktfs_inode fileInode;
                    void * temp_sb = NULL;
                    rc = cache_get_block(cache, 0, &temp_sb);
                    if(rc < 0){
                        cache_release_block(cache, temp2, 0);
                        cache_release_block(cache, temp, 0);
                        kfree(file);
                        return rc;
                    }
                    struct ktfs_superblock * sb = (struct ktfs_superblock *)temp_sb;
                    rc = get_inode(cache, sb, dentryBlock[k].inode, &fileInode);
                    cache_release_block(cache, temp_sb, 0);
                    if(rc < 0){
                        cache_release_block(cache, temp2, 0);
                        cache_release_block(cache, temp, 0);
                        kfree(file);
                        return rc;
                    }
                    file->fileSize = fileInode.size;
                    lock_acquire(&g_open_head_lock);
                    file->next = g_open_head;
                    g_open_head = file;
                    lock_release(&g_open_head_lock);
                    *uioptr = &file->base;
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    return 0;
                }
                i++;
            }
            cache_release_block(cache, temp2, 0);
        }
        cache_release_block(cache, temp, 0);
    }


    if(i == numDentries){
    	kfree(file);//if we reach here it means the file was not found
        return -ENOENT;
    }
    
    //moving on to the doubly indirect blocks
    uint32_t * dindirect;
    for(int l = 0; l < KTFS_NUM_DINDIRECT_BLOCKS && i < numDentries; l++){
        
        rc = cache_get_block(cache, (1 + K + B + N + rootInode.dindirect[l]) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            kfree(file);
            return rc;
        }
        dindirect = (uint32_t *) temp;
        
	for(int g = 0; g < KTFS_BLKSZ/4 && i < numDentries; g++){
            
            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + dindirect[g]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                kfree(file);
                return rc;
            }
            indirect = (uint32_t *)temp2;
            
            for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){
                
                void * temp3 = NULL;
                rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp3);
                if(rc < 0){
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    kfree(file);
                    return rc;
                }
                
                dentryBlock = (struct ktfs_dir_entry *)temp3;
                for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                    if(strcmp(dentryBlock[k].name, name) == 0){
                        file->dentry = dentryBlock[k];
                        struct ktfs_inode fileInode;
                        void * temp_sb = NULL;
                        rc = cache_get_block(cache, 0, &temp_sb);
                        if(rc < 0){
                            cache_release_block(cache, temp3, 0);
                            cache_release_block(cache, temp2, 0);
                            cache_release_block(cache, temp, 0);
                            kfree(file);
                            return rc;
                        }
                        struct ktfs_superblock * sb = (struct ktfs_superblock *)temp_sb;
                        rc = get_inode(cache, sb, dentryBlock[k].inode, &fileInode);
                        cache_release_block(cache, temp_sb, 0);
                        if(rc < 0){
                            cache_release_block(cache, temp3, 0);
                            cache_release_block(cache, temp2, 0);
                            cache_release_block(cache, temp, 0);
                            kfree(file);
                            return rc;
                        }
                        file->fileSize = fileInode.size;
                        lock_acquire(&g_open_head_lock);
                        file->next = g_open_head;
                        g_open_head = file;
                        lock_release(&g_open_head_lock);
                        *uioptr = &file->base;
                        cache_release_block(cache, temp3, 0);
                        cache_release_block(cache, temp2, 0);
                        cache_release_block(cache, temp, 0);
                        return 0;
                    }
                    i++;
                }
                cache_release_block(cache, temp3, 0);
            }
            cache_release_block(cache, temp2, 0);
    	}
        cache_release_block(cache, temp, 0);
    }
    kfree(file);//if we reach here it means the file was not found
    return -ENOENT;
    
}
/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    // FIXME
    if(uio == NULL)
            return;

    lock_acquire(&g_open_head_lock);

    struct ktfs_file * prev = NULL;
    struct ktfs_file * curr = g_open_head;

    while(curr != NULL){
            if(&curr->base == uio){
                if(prev != NULL)
                        prev->next = curr->next;
                else
                        g_open_head = curr->next;

                struct cache * cache = curr->fs->cache;

                lock_release(&g_open_head_lock);
                cache_flush(cache);
                kfree(curr);
                return;
            }
            prev = curr;
            curr = curr->next;
    }

    lock_release(&g_open_head_lock);
    return;
}


/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len)
{
    //FIX ME
    if (uio == NULL || buf == NULL)
        return -ENOTSUP;

    lock_acquire(&g_open_head_lock);
    struct ktfs_file* file = g_open_head;
    while (file != NULL) {
        if (&file->base == uio)
            break;
        file = file->next;
    }
    if (file == NULL) {
        lock_release(&g_open_head_lock);
        return -ENOTSUP;
    }

    lock_acquire(&file->file_lock);
    lock_release(&g_open_head_lock);

    struct cache* cache = file->fs->cache;
    uint64_t pos = file->pos;
    uint64_t size = file->fileSize;

    if (pos >= size){
        lock_release(&file->file_lock);
        return 0;
    }

    unsigned long toRead = len;
    if (pos + toRead > size)
        toRead = (unsigned long)(size - pos);

    void * temp_sb = NULL;
    int rc = cache_get_block(cache, 0, &temp_sb);
    if(rc < 0){
        lock_release(&file->file_lock);
        return rc;
    }
    struct ktfs_superblock * superblock = (struct ktfs_superblock *)temp_sb;

    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;

    struct ktfs_inode inode;
    rc = get_inode(cache, superblock, file->dentry.inode, &inode);
    cache_release_block(cache, temp_sb, 0);
    if(rc < 0){
        lock_release(&file->file_lock);
        return rc;
    }

    uint64_t start = (pos / KTFS_BLKSZ) * KTFS_BLKSZ;
    uint64_t walked = 0;

    unsigned long copied = 0;
    void* temp = NULL;
    uint32_t* indirect = NULL;
    uint32_t* dindirect = NULL;

    // DIRECT BLOCKS
    for (int j = 0; j < KTFS_NUM_DIRECT_DATA_BLOCKS; j++) {
        if (copied >= toRead)
            break;

        if (walked < start) {
            walked += KTFS_BLKSZ;
            continue;
        }

        uint32_t dataIdx = inode.block[j];
        

        rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp);
        if (rc < 0){
            lock_release(&file->file_lock);
            return rc;
        }

        size_t intraOffset;
        if (copied == 0)
            intraOffset = (size_t)(pos % KTFS_BLKSZ);
        else
            intraOffset = 0;

        size_t remaining = (size_t)(toRead - copied);
        size_t chunk = KTFS_BLKSZ - intraOffset;
        if (chunk > remaining)
            chunk = remaining;

        memcpy((uint8_t*)buf + copied, (uint8_t*)temp + intraOffset, chunk);
        cache_release_block(cache, temp, 0);

        copied += (unsigned long)chunk;
        pos    += (uint64_t)chunk;
        walked += KTFS_BLKSZ;

        if (copied == toRead) {
            file->pos = pos;
            lock_release(&file->file_lock);
            return (long)copied;
        }
    }

    // INDIRECT BLOCK
    if(inode.indirect != 0){
        rc = cache_get_block(cache, (1 + N + B + K + inode.indirect) * KTFS_BLKSZ, &temp);
        if (rc < 0){
            lock_release(&file->file_lock);
            return rc;
        }

        indirect = (uint32_t*)temp;

        for (int i = 0; i < (KTFS_BLKSZ / 4); i++) {
            if (copied >= toRead){
                cache_release_block(cache, temp, 0);
                file->pos = pos;
                lock_release(&file->file_lock);
                return (long)copied;
            }

            if (walked < start) {
                walked += KTFS_BLKSZ;
                continue;
           } 


            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + N + B + K  + indirect[i]) * KTFS_BLKSZ, &temp2);
            if (rc < 0){
                lock_release(&file->file_lock);
                return rc;
            }

            size_t intraOffset;
            if (copied == 0)
                intraOffset = (size_t)(pos % KTFS_BLKSZ);
            else
                intraOffset = 0;

            size_t remaining = (size_t)(toRead - copied);
            size_t chunk = KTFS_BLKSZ - intraOffset;
            if (chunk > remaining)
                chunk = remaining;

            memcpy((uint8_t*)buf + copied, (uint8_t*)temp2 + intraOffset, chunk);
            cache_release_block(cache, temp2, 0);

            copied += (unsigned long)chunk;
            pos    += (uint64_t)chunk;
            walked += KTFS_BLKSZ;

            if (copied == toRead) {
                cache_release_block(cache, temp, 0);
                file->pos = pos;
                lock_release(&file->file_lock);
                return (long)copied;
            }
        }
        cache_release_block(cache, temp, 0);
    }

    // DOUBLE-INDIRECT BLOCKS
    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        if (copied >= toRead)
            break;


        rc = cache_get_block(cache, (1 + N + K + B +  inode.dindirect[i]) * KTFS_BLKSZ, &temp);
        if (rc < 0){
            lock_release(&file->file_lock);
            return rc;
        }

        dindirect = (uint32_t*)temp;

        for (int j = 0; j < (KTFS_BLKSZ / 4); j++) {
            if (copied >= toRead){
                cache_release_block(cache, temp, 0);
                file->pos = pos;
                lock_release(&file->file_lock);
                return (long)copied;
            }

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + N + K + B +  dindirect[j]) * KTFS_BLKSZ, &temp2);
            if (rc < 0){
                lock_release(&file->file_lock);
                return rc;
            }

            indirect = (uint32_t*)temp2;

            for (int k = 0; k < (KTFS_BLKSZ / 4); k++) {
                if (copied >= toRead){
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    file->pos = pos;
                    lock_release(&file->file_lock);
                    return (long)copied;
                }

                if (walked < start) {
                    walked += KTFS_BLKSZ;
                    continue;
                }


                void * temp3 = NULL;
                rc = cache_get_block(cache, (1 + N + K + B +  indirect[k]) * KTFS_BLKSZ, &temp3);
                if (rc < 0){
                    lock_release(&file->file_lock);
                    return rc;
                }

                size_t intraOffset;
                if (copied == 0)
                    intraOffset = (size_t)(pos % KTFS_BLKSZ);
                else
                    intraOffset = 0;

                size_t remaining = (size_t)(toRead - copied);
                size_t chunk = KTFS_BLKSZ - intraOffset;
                if (chunk > remaining)
                    chunk = remaining;

                memcpy((uint8_t*)buf + copied, (uint8_t*)temp3 + intraOffset, chunk);
                cache_release_block(cache, temp3, 0);

                copied += (unsigned long)chunk;
                pos    += (uint64_t)chunk;
                walked += KTFS_BLKSZ;

                if (copied == toRead) {
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    file->pos = pos;
                    lock_release(&file->file_lock);
                    return (long)copied;
                }
            }
            cache_release_block(cache, temp2, 0);
        }
        cache_release_block(cache, temp, 0);
    }

    file->pos = pos;
    lock_release(&file->file_lock);
    return (long)copied;
}





/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    if (uio == NULL || buf == NULL)
        return -ENOTSUP;

    lock_acquire(&g_open_head_lock);
    struct ktfs_file* file = g_open_head;
    while (file != NULL) {
        if (&file->base == uio)
            break;
        file = file->next;
    }
    if (file == NULL) {
        lock_release(&g_open_head_lock);
        return -ENOTSUP;
    }

    lock_acquire(&file->file_lock);
    lock_release(&g_open_head_lock);

    if (len == 0) {
        lock_release(&file->file_lock);
        return 0;
    }

    struct cache* cache = file->fs->cache;
    uint64_t pos  = file->pos;
    

    void * temp_sb = NULL;
    int rc = cache_get_block(cache, 0, &temp_sb);
    if (rc < 0) {
        lock_release(&file->file_lock);
        return rc;
    }
    struct ktfs_superblock * superblock = (struct ktfs_superblock *)temp_sb;

    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;

    uint16_t inodeNum = file->dentry.inode;
    uint64_t inode_block_start = (1 + superblock->inode_bitmap_block_count + superblock->bitmap_block_count) * KTFS_BLKSZ;
    uint64_t inode_block_num = inodeNum / (KTFS_BLKSZ / KTFS_INOSZ);
    uint64_t ipos = inode_block_start + inode_block_num * KTFS_BLKSZ;

    void * inode_block = NULL;
    rc = cache_get_block(cache, ipos, &inode_block);
    if (rc < 0) {
        return rc;
    }

    struct ktfs_inode * inode_arr = (struct ktfs_inode *)inode_block;
    struct ktfs_inode * inode = &inode_arr[inodeNum % (KTFS_BLKSZ / KTFS_INOSZ)];

    uint64_t old_size = inode->size;

    // If writing past EOF, expand and zero fill the gap
    if (pos > old_size) {
        uint32_t first_blk = (old_size / KTFS_BLKSZ);
        uint32_t last_blk_before_pos = ((pos - 1) / KTFS_BLKSZ);
        size_t off_in_first = (old_size % KTFS_BLKSZ);

        if (off_in_first != 0) {
            uint32_t dataIdx = 0;
            rc = get_or_alloc_data_block(superblock, cache, inode, first_blk, &dataIdx);
            if (rc < 0) {
                return rc;
            }

            void * temp_gap = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp_gap);
            if (rc < 0) {
                return rc;
            }

            size_t start_off = off_in_first;
            size_t end_off;

            if (first_blk == last_blk_before_pos) {
                size_t pos_off = (size_t)(pos % KTFS_BLKSZ);
                if (pos_off == 0)
                    pos_off = KTFS_BLKSZ;
                end_off = pos_off;
            } else {
                end_off = KTFS_BLKSZ;
            }

            if (end_off > start_off)
                memset((uint8_t *)temp_gap + start_off, 0, end_off - start_off);

            __sync_synchronize();
            cache_release_block(cache, temp_gap, 1);
            __sync_synchronize();
            first_blk++;
        }

        for (uint32_t blk = first_blk; blk <= last_blk_before_pos; blk++) {
            uint32_t dataIdx = 0;
            rc = get_or_alloc_data_block(superblock, cache, inode, blk, &dataIdx);
            if (rc < 0) {
                __sync_synchronize();
                cache_release_block(cache, inode_block, 1);
                __sync_synchronize();
                cache_release_block(cache, temp_sb, 0);
                lock_release(&file->file_lock);
                return rc;
            }
            // Newly allocated blocks are zeroed inside get_or_alloc_data_block
        }

        inode->size = pos;
        
    }

    unsigned long toWrite = len;
    unsigned long copied  = 0;

    while (copied < toWrite) {
        uint32_t blkno = (uint32_t)(pos / KTFS_BLKSZ);
        size_t block_off = (size_t)(pos % KTFS_BLKSZ);

        uint32_t dataIdx = 0;
        rc = get_or_alloc_data_block(superblock, cache, inode, blkno, &dataIdx);
        if (rc < 0) {
            return rc;
        }

        void * temp = NULL;
        rc = cache_get_block(cache, (1 + K + B + N + dataIdx) * KTFS_BLKSZ, &temp);
        if (rc < 0) {
            return rc;
        }

        size_t remaining = (size_t)(toWrite - copied);
        size_t chunk = KTFS_BLKSZ - block_off;
        if (chunk > remaining)
            chunk = remaining;

        memcpy((uint8_t *)temp + block_off, (const uint8_t *)buf + copied, chunk);

        __sync_synchronize();
        cache_release_block(cache, temp, 1);
        __sync_synchronize();

        pos    += (uint64_t)chunk;
        copied += (unsigned long)chunk;
    }

    if (pos > inode->size)
        inode->size = pos;

    file->fileSize = inode->size;
    file->pos      = pos;

    __sync_synchronize();
    cache_release_block(cache, inode_block, 1);
    __sync_synchronize();
    cache_release_block(cache, temp_sb, 0);
    lock_release(&file->file_lock);

    return (long)copied;
}


/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME
        if(fs == NULL || name == NULL)
            return -ENOTSUP;
    
	
    //if name too long return
    struct ktfs_dir_entry tmp;
    size_t max_name = sizeof(tmp.name) - 1;
    if (strlen(name) > max_name)
        return -ENOTSUP;
    //setup
    struct ktfs_fs * filesys = (void*)fs - offsetof(struct ktfs_fs, fs);
    struct cache * cache = filesys->cache;
    void * temp = NULL;
    void * sb = NULL;
    int rc = 0;

    //get superblock
    rc = cache_get_block(cache, 0, &sb);
    if(rc)
            return -ENOTSUP;


    //find inode of the file we want to delete
    struct ktfs_superblock * superblock =(struct ktfs_superblock *) sb;
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    struct ktfs_inode rootInode;
    rc = get_inode(cache, superblock, superblock->root_directory_inode, &rootInode);
    if(rc < 0){
        cache_release_block(cache, sb, 0);
        return rc;
    }

    uint32_t numDentries = rootInode.size/KTFS_DENSZ; //there are 16bytes per dentry
    
  

    int i = 0;
    struct ktfs_dir_entry * dentryBlock; //each dir entry is a half word
    uint32_t dentryBlkIdx;

    //go through direct blocks
    for(int j = 0; j < KTFS_NUM_DIRECT_DATA_BLOCKS && i < numDentries; j++){
        dentryBlkIdx = rootInode.block[j];

            rc = cache_get_block(cache, (1 + B + K + N + dentryBlkIdx) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        dentryBlock = (struct ktfs_dir_entry *)temp;
        for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                if(strcmp(dentryBlock[k].name, name) == 0){ //file to delete found
                cache_release_block(cache, temp, 0);
                cache_release_block(cache, sb, 0);
		return -ENOTSUP;
                }
                i++;
        }
        cache_release_block(cache, temp, 0);
    }

    //moving on to the indirect block
    uint32_t * indirect;
        rc = cache_get_block(cache,(1 + K + B + N  + rootInode.indirect) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        indirect = (uint32_t *) temp;

        for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){//4 bytes per dentry block index

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                return rc;
            }

            dentryBlock = (struct ktfs_dir_entry *)temp2;
            for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                if(strcmp(dentryBlock[k].name, name) == 0){
			cache_release_block(cache, temp, 0);
			cache_release_block(cache, temp2, 0);
                	cache_release_block(cache, sb, 0);
                	return -ENOTSUP;
                }
                i++;
            }
            cache_release_block(cache, temp2, 0);
        }
        cache_release_block(cache, temp, 0);
    


    //moving on to the doubly indirect blocks
    uint32_t * dindirect;
    for(int l = 0; l < KTFS_NUM_DINDIRECT_BLOCKS && i < numDentries; l++){

        rc = cache_get_block(cache, (1 + K + B + N + rootInode.dindirect[l]) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        dindirect = (uint32_t *) temp;

        for(int g = 0; g < KTFS_BLKSZ/4 && i < numDentries; g++){

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + dindirect[g]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                return rc;
            }
            indirect = (uint32_t *)temp2;
		for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){
		void * temp3 = NULL;
                rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp3);
                if(rc < 0){
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    return rc;
                }
		dentryBlock = (struct ktfs_dir_entry *)temp3;
                for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                    if(strcmp(dentryBlock[k].name, name) == 0){
			cache_release_block(cache, temp, 0);
                        cache_release_block(cache, temp2, 0);
			cache_release_block(cache, temp3, 0);
                        cache_release_block(cache, sb, 0);
                        return -ENOTSUP;
                    }
                    i++;
                }
                cache_release_block(cache, temp3, 0);
            }
            cache_release_block(cache, temp2, 0);
        }
        cache_release_block(cache, temp, 0);
    }
    //if we reach here it means the file was not found, so we can create the file
    int inodeNum = find_free_inode(superblock, cache);
    if(inodeNum < 0){
	    cache_release_block(cache, sb, 0);
	    return inodeNum;
    }
    set_inode_status(superblock, cache, inodeNum, 1);
    add_dentry(name, superblock, cache, inodeNum );


    
    //initialize the inode with a size of 0
    uint64_t inode_block_start = (1 + superblock->inode_bitmap_block_count + superblock->bitmap_block_count) * KTFS_BLKSZ;
    uint64_t inode_block_num = inodeNum / (KTFS_BLKSZ / KTFS_INOSZ);
    uint64_t pos = inode_block_start + inode_block_num * KTFS_BLKSZ;

    void *inode_block = NULL;
    rc = cache_get_block(cache, pos, &inode_block);
    if (rc < 0) {
	cache_release_block(cache, sb, 0);
    	return rc;
}
    struct ktfs_inode *inode_arr = (struct ktfs_inode *)inode_block;
    struct ktfs_inode *newino = &inode_arr[inodeNum % (KTFS_BLKSZ / KTFS_INOSZ)];
    memset(newino, 0, sizeof(*newino));
    newino->size = 0;

    __sync_synchronize();
    cache_release_block(cache, inode_block, 1);
    __sync_synchronize();
    cache_release_block(cache, sb, 0);
    
    cache_flush(cache);
    
    return 0;
}


/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    // FIXME
    if(fs == NULL || name == NULL)
	    return -ENOTSUP;
    //setup
    struct ktfs_fs * filesys = (void*)fs - offsetof(struct ktfs_fs, fs);
    struct cache * cache = filesys->cache;
    void * temp = NULL;
    void * sb = NULL;
    int rc = 0;

    //get superblock
    rc = cache_get_block(cache, 0, &sb);
    if(rc)
	    return -ENOTSUP;
    
    
    //find inode of the file we want to delete
    struct ktfs_superblock * superblock =(struct ktfs_superblock *) sb;
    uint32_t K = superblock->inode_bitmap_block_count;
    uint32_t B = superblock->bitmap_block_count;
    uint32_t N = superblock->inode_block_count;
    struct ktfs_inode rootInode;
    rc = get_inode(cache, superblock, superblock->root_directory_inode, &rootInode);
    if(rc < 0){
        cache_release_block(cache, sb, 0);
        return rc;
    }

    uint32_t numDentries = rootInode.size/KTFS_DENSZ; //there are 16bytes per dentry
    

    int i = 0;
    struct ktfs_dir_entry * dentryBlock; //each dir entry is a half word
    uint32_t dentryBlkIdx;

    //go through direct blocks
    for(int j = 0; j < KTFS_NUM_DIRECT_DATA_BLOCKS && i < numDentries; j++){
    	dentryBlkIdx = rootInode.block[j];
	    rc = cache_get_block(cache, (1 + B + K + N + dentryBlkIdx) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        dentryBlock = (struct ktfs_dir_entry *)temp;
	for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
		if(strcmp(dentryBlock[k].name, name) == 0){ //file to delete found
			uint16_t inodeNum = dentryBlock[k].inode;
			delete_dentry(superblock, cache, &dentryBlock[k]);
			set_inode_status(superblock, cache, inodeNum, 0);
			struct ktfs_inode inode;
			rc = get_inode(cache, superblock, inodeNum, &inode);
			if(rc)
				return -ENOTSUP;
			
			free_data_blocks(superblock, inode.size, cache, inode); 
			__sync_synchronize();
			cache_release_block(cache, temp, 1);
			__sync_synchronize();
			cache_release_block(cache, sb, 0);
			cache_flush(cache);
			return 0;
		}
		i++;
	}
        cache_release_block(cache, temp, 0);
    }

    if(i == numDentries){//this means the file was not found
	    cache_release_block(cache, sb, 0);
	    return -ENOENT;
    }

    //moving on to the indirect block
    uint32_t * indirect;
    if(rootInode.indirect != 0){
        rc = cache_get_block(cache,(1 + K + B + N  + rootInode.indirect) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        indirect = (uint32_t *) temp;

        for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){//4 bytes per dentry block index

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                return rc;
            }

            dentryBlock = (struct ktfs_dir_entry *)temp2;
            for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                if(strcmp(dentryBlock[k].name, name) == 0){
    			uint16_t inodeNum = dentryBlock[k].inode;
                        delete_dentry(superblock, cache, &dentryBlock[k]);
                        set_inode_status(superblock, cache, inodeNum, 0);
                        struct ktfs_inode inode;
                        rc = get_inode(cache, superblock, inodeNum, &inode);
                        if(rc)
                                return -ENOTSUP;
                        free_data_blocks(superblock, inode.size, cache, inode);
                        __sync_synchronize();
                        cache_release_block(cache, temp2, 1);
                        __sync_synchronize();
			cache_release_block(cache, temp, 0);
                        cache_release_block(cache, sb, 0);
                        cache_flush(cache);
                        return 0;
                }
                i++;
            }
            cache_release_block(cache, temp2, 0);
        }
        cache_release_block(cache, temp, 0);
    }


    if(i == numDentries){//if we reach here it means the file was not found
        cache_release_block(cache, sb, 0);
	return -ENOENT;
    }

    //moving on to the doubly indirect blocks
    uint32_t * dindirect;
    for(int l = 0; l < KTFS_NUM_DINDIRECT_BLOCKS && i < numDentries; l++){

        rc = cache_get_block(cache, (1 + K + B + N + rootInode.dindirect[l]) * KTFS_BLKSZ, &temp);
        if(rc < 0){
            return rc;
        }
        dindirect = (uint32_t *) temp;

	for(int g = 0; g < KTFS_BLKSZ/4 && i < numDentries; g++){

            void * temp2 = NULL;
            rc = cache_get_block(cache, (1 + K + B + N + dindirect[g]) * KTFS_BLKSZ, &temp2);
            if(rc < 0){
                cache_release_block(cache, temp, 0);
                return rc;
            }
            indirect = (uint32_t *)temp2;

            for(int j = 0; j < KTFS_BLKSZ/4 && i < numDentries; j++){

                void * temp3 = NULL;
                rc = cache_get_block(cache, (1 + K + B + N + indirect[j]) * KTFS_BLKSZ, &temp3);
                if(rc < 0){
                    cache_release_block(cache, temp2, 0);
                    cache_release_block(cache, temp, 0);
                    return rc;
                }

                dentryBlock = (struct ktfs_dir_entry *)temp3;
                for(int k = 0; k < (KTFS_BLKSZ/KTFS_DENSZ) && i < numDentries; k++){
                    if(strcmp(dentryBlock[k].name, name) == 0){
			uint16_t inodeNum = dentryBlock[k].inode;
                        delete_dentry(superblock, cache, &dentryBlock[k]);
                        set_inode_status(superblock, cache, inodeNum, 0);
                        struct ktfs_inode inode;
                        rc = get_inode(cache, superblock, inodeNum, &inode);
                        if(rc)
                                return -ENOTSUP;
                        free_data_blocks(superblock, inode.size, cache, inode);
			__sync_synchronize();
			cache_release_block(cache, temp3, 1);
			__sync_synchronize();
                        cache_release_block(cache, temp2, 0);
                        cache_release_block(cache, temp, 0);
                        cache_release_block(cache, sb, 0);
                        cache_flush(cache);
                        return 0;
                    }
                    i++;
                }
                cache_release_block(cache, temp3, 0);
            }
            cache_release_block(cache, temp2, 0);
    	}
        cache_release_block(cache, temp, 0);
    }
    //if we reach here it means the file was not found
    cache_release_block(cache, sb, 0);
    return -ENOENT;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    // FIXME
    if(uio == NULL || arg == NULL)
            return -ENOTSUP;

    lock_acquire(&g_open_head_lock);
    struct ktfs_file * curr = g_open_head;
    struct ktfs_file * file = NULL;
    while(curr != NULL){
        if(&curr->base == uio){
                file = curr;
                break;
        }
        curr = curr->next;
    }
    if(file == NULL){
        lock_release(&g_open_head_lock);
        return -ENOTSUP;
    }

    lock_acquire(&file->file_lock);
    lock_release(&g_open_head_lock);

    if(cmd == FCNTL_GETEND){
        *(uint64_t *)arg = file->fileSize;
        lock_release(&file->file_lock);
        return 0;
    }
    if(cmd == FCNTL_SETPOS && *(uint64_t *)arg <= file->fileSize){
        file->pos = *(uint64_t *)arg;
        lock_release(&file->file_lock);
        return 0;
    }
    if(cmd == FCNTL_GETPOS){
        *(uint64_t *)arg = file->pos;
        lock_release(&file->file_lock);
        return 0;
    }

    if (cmd == FCNTL_SETEND) {
	//setup
        uint64_t new_size = *(uint64_t *)arg;
        
        struct cache *cache;
        
        struct ktfs_inode inode;
        int rc;
        
        
        
        

        if (new_size > KTFS_MAX_FILE_SIZE) {
            lock_release(&file->file_lock);
            return -ENOTSUP;
        }

        cache = file->fs->cache;

	//get superblock
	void *temp_sb = NULL;
        struct ktfs_superblock *superblock;
        rc = cache_get_block(cache, 0, &temp_sb);
        if (rc < 0) {
            return rc;
        }
        superblock = (struct ktfs_superblock *)temp_sb;

        rc = get_inode(cache, superblock, file->dentry.inode, &inode);
        if (rc < 0) {
            return rc;
        }

        uint64_t old_size = inode.size;

        uint32_t old_blocks = 0;
        if (old_size != 0) {
            old_blocks = (uint32_t)((old_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ);
        }

        uint32_t new_blocks = 0;
        if (new_size != 0) {
            new_blocks = (uint32_t)((new_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ);
        }
	//allocate new blocks as necessary
        if (new_size > old_size) {
            uint32_t b = old_blocks;
	    uint32_t dummy;
            while (b < new_blocks) {
                rc = get_or_alloc_data_block(superblock, cache, &inode, b, &dummy);
                if (rc < 0) {
                    return rc;
                }
                b++;
            }
        }
        // If new_size < old_size, just shrink size

        inode.size = new_size;

        // Write inode back to disk 
        {
            uint64_t inode_block_start;
            uint64_t inode_block_num;
            uint64_t pos;
            void *inode_block = NULL;
            struct ktfs_inode *inode_arr;

            inode_block_start = (1 + superblock->inode_bitmap_block_count + superblock->bitmap_block_count) * KTFS_BLKSZ;
            inode_block_num = file->dentry.inode / (KTFS_BLKSZ / KTFS_INOSZ);
            pos = inode_block_start + inode_block_num * KTFS_BLKSZ;

            rc = cache_get_block(cache, pos, &inode_block);
            if (rc < 0) {
                return rc;
            }

            inode_arr = (struct ktfs_inode *)inode_block;
            inode_arr[file->dentry.inode % (KTFS_BLKSZ / KTFS_INOSZ)] = inode;
            __sync_synchronize();
            cache_release_block(cache, inode_block, 1);
            __sync_synchronize();
        }

        cache_release_block(cache, temp_sb, 0);

        file->fileSize = new_size;
        if (file->pos > new_size) {
            file->pos = new_size;
        }

        lock_release(&file->file_lock);
        return 0;
    }

    
   

    lock_release(&file->file_lock);
    return -ENOTSUP;
}


/**
 * @brief Flushes the cache to the backing device
 * 
 *
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME
    if(fs == NULL)
	    return;
    struct ktfs_fs * filesys = (void*)fs - offsetof(struct ktfs_fs, fs);
    if(filesys->cache == NULL)
	    return;
    cache_flush(filesys->cache);
    return;
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    if(uio == NULL)
	    return;
    struct ktfs_listing * listing = (struct ktfs_listing *)((void *)uio-offsetof(struct ktfs_listing, base));
    kfree(listing);
    return;
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    if (uio == NULL || buf == NULL)
         return -ENOTSUP;

    if (bufsz == 0)
        return 0;

    struct ktfs_listing * listing = (struct ktfs_listing *)((void *)uio - offsetof(struct ktfs_listing, base));

    struct cache * cache = listing->fs->cache;

    void * temp_sb = NULL;
    int rc = cache_get_block(cache, 0, &temp_sb);
    if (rc < 0)
        return rc;

    struct ktfs_superblock * superblock = (struct ktfs_superblock *)temp_sb;

    struct ktfs_inode rootInode;
    rc = get_inode(cache, superblock, superblock->root_directory_inode, &rootInode);
    if (rc < 0) {
        return rc;
    }

    uint32_t numDentries = rootInode.size / KTFS_DENSZ;

    if (listing->index >= numDentries) {
        cache_release_block(cache, temp_sb, 0);
        return 0;
    }

    struct ktfs_dir_entry dentry;
    rc = get_dentry_at_index(superblock, cache, &rootInode, listing->index, &dentry);
    cache_release_block(cache, temp_sb, 0);
    if (rc < 0)
        return rc;

    size_t name_len = strlen(dentry.name);

    if (name_len + 1 > bufsz)
        return -ENOTSUP;

    strncpy((char *)buf, dentry.name, bufsz);

    listing->index += 1;

    return (long)(name_len + 1);
}
