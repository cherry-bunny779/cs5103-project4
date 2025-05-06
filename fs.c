#include "fs.h"
#include "disk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#define FS_MAGIC 0xf0f03410
#define INODES_PER_BLOCK 128
#define POINTERS_PER_INODE 5
#define POINTERS_PER_BLOCK 1024
#define MAX_FILE_SIZE 4214784

// Returns the number of dedicated inode blocks given the disk size in blocks
#define NUM_INODE_BLOCKS(disk_size_in_blocks) (1 + (disk_size_in_blocks / 10))

struct fs_superblock
{
    int magic;        // Magic bytes
    int nblocks;      // Size of the disk in number of blocks
    int ninodeblocks; // Number of blocks dedicated to inodes
    int ninodes;      // Number of dedicated inodes
};

struct fs_inode
{
    int isvalid;                    // 1 if valid (in use), 0 otherwise
    int size;                       // Size of file in bytes
    int direct[POINTERS_PER_INODE]; // Direct data block numbers (0 if invalid)
    int indirect;                   // Indirect data block number (0 if invalid)
};

union fs_block
{
    struct fs_superblock super;              // Superblock
    struct fs_inode inode[INODES_PER_BLOCK]; // Block of inodes
    int pointers[POINTERS_PER_BLOCK];        // Indirect block of direct data block numbers
    char data[DISK_BLOCK_SIZE];              // Data block
};

static union fs_block superblock;

/**
 * + Freemap should be allocated in fs_mount (via malloc) 
 *   based on the number of blocks (nblocks) in your super block
 * 
 * + freemap can then be indexed like an array for determining whether or
 *   not a block is free... e.g.
 *      if(!freemap[123])
 *          printf("Block 123 is NOT free\n");
*/
static int *freemap = 0;

void fs_debug()
{
    union fs_block block;

    disk_read(0, block.data);

    printf("superblock:\n");
    printf("    %d blocks\n", block.super.nblocks);
    printf("    %d inode blocks\n", block.super.ninodeblocks);
    printf("    %d inodes\n", block.super.ninodes);

    for (int i = 0; i < block.super.ninodeblocks; i++) {
        union fs_block inode_block;
        disk_read(i + 1, inode_block.data);
        for (int j = 0; j < INODES_PER_BLOCK; j++) {
            int inumber = i * INODES_PER_BLOCK + j;
            if (inumber >= block.super.ninodes) break;

            struct fs_inode *inode = &inode_block.inode[j];
            if (inode->isvalid) {
                printf("inode %d:\n", inumber);
                printf("    size: %d bytes\n", inode->size);
                printf("    direct blocks:");
                for (int k = 0; k < POINTERS_PER_INODE; k++) {
                    if (inode->direct[k])
                        printf(" %d", inode->direct[k]);
                }
                printf("\n");

                if (inode->indirect) {
                    printf("    indirect block: %d\n", inode->indirect);
                    union fs_block indirect_block;
                    disk_read(inode->indirect, indirect_block.data);
                    printf("    indirect data blocks:");
                    for (int k = 0; k < POINTERS_PER_BLOCK; k++) {
                        if (indirect_block.pointers[k])
                            printf(" %d", indirect_block.pointers[k]);
                    }
                    printf("\n");
                }
            }
        }
    }
}

int fs_format()
{
    int nblocks = disk_size();
    int ninodeblocks = NUM_INODE_BLOCKS(nblocks);
    int ninodes = ninodeblocks * INODES_PER_BLOCK;

    // Initialize superblock
    union fs_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.super.magic = FS_MAGIC;
    sb.super.nblocks = nblocks;
    sb.super.ninodeblocks = ninodeblocks;
    sb.super.ninodes = ninodes;
    disk_write(0, sb.data);

    // Clear inode blocks
    union fs_block empty_block;
    memset(&empty_block, 0, sizeof(empty_block));
    for (int i = 1; i <= ninodeblocks; i++) {
        disk_write(i, empty_block.data);
    }

    return 1;
}


int fs_mount()
{
    union fs_block sb;
    disk_read(0, sb.data);

    if (sb.super.magic != FS_MAGIC) return 0;

    memcpy(&superblock, &sb, sizeof(sb));
    
    // Allocate and initialize free map
    freemap = malloc(sizeof(int) * superblock.super.nblocks);
    if (freemap == NULL) return 0;
    memset(freemap, 0, sizeof(int) * superblock.super.nblocks);

    // Mark reserved blocks (superblock + inode blocks)
    for (int i = 0; i <= superblock.super.ninodeblocks; i++) {
        freemap[i] = 1;
    }

    // Mark used data blocks
    for (int i = 0; i < superblock.super.ninodeblocks; i++) {
        union fs_block block;
        disk_read(i + 1, block.data);

        for (int j = 0; j < INODES_PER_BLOCK; j++) {
            struct fs_inode *inode = &block.inode[j];
            if (inode->isvalid) {
                for (int k = 0; k < POINTERS_PER_INODE; k++) {
                    if (inode->direct[k])
                        freemap[inode->direct[k]] = 1;
                }
                if (inode->indirect) {
                    freemap[inode->indirect] = 1;
                    union fs_block indirect_block;
                    disk_read(inode->indirect, indirect_block.data);
                    for (int k = 0; k < POINTERS_PER_BLOCK; k++) {
                        if (indirect_block.pointers[k])
                            freemap[indirect_block.pointers[k]] = 1;
                    }
                }
            }
        }
    }

    return 1;
}

int fs_unmount()
{
    if (freemap) {
        free(freemap);
        freemap = NULL;
    }
    return 1;
}

int fs_create()
{
    union fs_block block;

    disk_read(0, block.data);

    for (int i = 0; i < block.super.ninodeblocks; i++) {
        union fs_block inode_block;
        disk_read(i + 1, inode_block.data);
        for (int j = 0; j < INODES_PER_BLOCK; j++) {
            int inumber = i * INODES_PER_BLOCK + j;
            if (inumber >= block.super.ninodes) break;

            struct fs_inode *inode = &inode_block.inode[j];
            if (inode->isvalid != 1) {
                inode->isvalid = 1;
                disk_write(i+1,inode_block.data);
                return inumber;
            }
        }
    }
    return -1;
}

int fs_delete(int inumber)
{
    return 0;
}

int fs_getsize(int inumber)
{
    int inode_block_index = inumber/INODES_PER_BLOCK;
    int offset = inumber%INODES_PER_BLOCK;
    union fs_block inode_block;

    disk_read(inode_block_index+1, inode_block.data);
    struct fs_inode *inode = &inode_block.inode[inumber];
    if(inode->isvalid){
        return inode->size;
    }else{
        printf("fs_getsize: inode %i not valid.\n",inumber);
        return -1;
    }

}

int fs_read(int inumber, char *data, int length, int offset)
{
    return 0;
}

/* 
   Helper function to find empty space on disk
   populate from lower indices, return first empty
   index. if no empty space, return -1
*/
int find_empty_block(){
    
    for(int i = 0; i < superblock.super.nblocks; i++){
        if(freemap[i] == 0){
            return i;
        }
    }
    return -1;
}

/* Write data to a valid inode. Copy "length" bytes from the pointer "data" into
the inode starting at "offset" bytes. Allocate any necessary direct and indirect blocks in
the process. Return the number of bytes actually written. The number of bytes actually
written could be smaller than the number of bytes request, perhaps if the disk becomes
full. If the given inumber is invalid, or any other error is encountered, return 0.
*/

/* [To-do] 
    1. update free-map 1.1 overwriting already used 1.2 writing to unused blocks (need allocation)
        but no allocation above?
    2. update size
*/
int fs_write(int inumber, const char *data, int length, int offset)
{
    // read superblock
    union fs_block block;

    disk_read(0, block.data);

    // check inumber in bounds
    if(inumber > (block.super.ninodeblocks*INODES_PER_BLOCK) || inumber < 0){
        printf("fs_write: inumber out of bounds for i=%i\n",inumber);
        return 0;
    }

    // read inode entry
    union fs_block inode_block;
    disk_read(inumber/INODES_PER_BLOCK + 1, inode_block.data);
    struct fs_inode *inode = &inode_block.inode[inumber];

    // [To-do] Write updated inode info back to disk
    // check inode valid
    if(!(inode->isvalid)){
        printf("fs_write: target inode not valid, create inode first\n");
        return 0;
    }
    // check write size
    if(inode->size + length > MAX_FILE_SIZE){
        printf("fs_write: write too large. write aborted\n");
        return 0;
    }
    // check write bounds
    if(offset + length > MAX_FILE_SIZE){
        printf("fs_write: write out of bounds. write aborted\n");
        return 0;
    }

    // write data range = (offset + length)
    // calculate pointer association
    int ptr_index = offset/DISK_BLOCK_SIZE; // block to start write. if > 4, need to use indirect
    int ptr_block_index = offset%DISK_BLOCK_SIZE; // data index within start block
    int intr_index = ptr_index-5;

    int write_length_blks = length/DISK_BLOCK_SIZE;
    int write_last_blk_index = write_length_blks+intr_index+1;
    int write_length_in_blk = length%DISK_BLOCK_SIZE;

    int index_in_wdata = 0;

    // Three cases of write; 1. all in direct pointer blocks 2. start in dp blocks and extend to idp 3. all in idp

    // write to first block and overflow to following blocks
    // fix: all the disk_write are wrong
    if(ptr_index > 4){
        union fs_block indirect_block;
        union fs_block indirdata_block;
        union fs_block data_to_write;
        
        // copy and write first block
        disk_read(indirect_block.pointers[intr_index], indirdata_block.data);
        memcpy(data_to_write.data,indirdata_block.data,DISK_BLOCK_SIZE);
        for(int j = ptr_block_index; j < DISK_BLOCK_SIZE; j++){
            data_to_write.data[j] = data[j-ptr_block_index];// data starts from index 0
            index_in_wdata++;
        }
        disk_write(data_to_write.data,indirdata_block.data);

        // copy and write blocks 2 ~ n-1 
        for(int i = intr_index+1; i < write_length_blks; i++){
        disk_read(indirect_block.pointers[i], indirdata_block.data);
        memcpy(data_to_write.data,indirdata_block.data,DISK_BLOCK_SIZE);
            for(int j = 0; j < DISK_BLOCK_SIZE; j++){
                data_to_write.data[j] = data[j-ptr_block_index];// data starts from index 0
                index_in_wdata++;
            }
        disk_write(data_to_write.data,indirdata_block.data);
        }

        // copy and write last block
        int index_in_data = ptr_block_index + DISK_BLOCK_SIZE*(write_length_blks-1);
        disk_read(indirect_block.pointers[write_last_blk_index], indirdata_block.data);
        memcpy(data_to_write.data,indirdata_block.data,DISK_BLOCK_SIZE);
        for(int i = 0; i < write_length_in_blk; i++){
            data_to_write.data[i] = data[index_in_data+i];
        }
        disk_write(data_to_write.data,indirdata_block.data);

    } else if ((ptr_index + write_length_blks) > 4){
        union fs_block indirect_ptr_block;
        union fs_block indirdata_block;
        union fs_block data_to_write;
        
        // start in direct pointer space
        union fs_block direct_data_block;
        // [To-do] increase inode size with every new disk block allocation
        // touch first block
        if(freemap[ptr_index] == 1){
            disk_read(inode->direct[ptr_index], data_to_write.data);
            for(int j = ptr_block_index; j < DISK_BLOCK_SIZE; j++){
                data_to_write.data[j] = data[j-ptr_block_index]; // data starts from index 0
                index_in_wdata++;
            }
            disk_write(inode->direct[ptr_index],data_to_write.data);
        } else {
            freemap[ptr_index] = 1;
            int freeblock = find_empty_block();
            if(freeblock == -1){
                printf("fs_write: disk full. aborting operation 1\n");
                return 0;
            }
            inode->direct[ptr_index] = freeblock;
            for(int j = ptr_block_index; j < DISK_BLOCK_SIZE; j++){
                data_to_write.data[j] = data[j-ptr_block_index];
                index_in_wdata++;
            }
            // [To-do] block not in use, need to zero index[0] to index[ptr_index-1] ?
            disk_write(inode->direct[0],data_to_write.data);
        }

        // touch direct blocks ptr_index+1 ~ 4
        for(int i = ptr_index+1; i < 5; i++){
            if(freemap[i] == 1){
                disk_read(inode->direct[i], data_to_write.data);
                for(int j = 0; j < DISK_BLOCK_SIZE; j++){
                    data_to_write.data[j] = data[index_in_wdata];
                    index_in_wdata++;
                }
                disk_write(inode->direct[i],data_to_write.data);
            } else {
                freemap[ptr_index] = 1;
                int freeblock = find_empty_block();
                if(freeblock == -1){
                    printf("fs_write: disk full. aborting operation 2\n");
                    return 0;
                }
                inode->direct[i] = freeblock;
                for(int j = 0; j < DISK_BLOCK_SIZE; j++){
                    data_to_write.data[j] = data[index_in_wdata];
                    index_in_wdata++;
                }
                disk_write(inode->direct[i],data_to_write.data);
            }
        }

        // touch indirect blocks
        int indirect_block_range = ptr_index + ((length-(offset%DISK_BLOCK_SIZE))%DISK_BLOCK_SIZE) - 5;
        if(inode->indirect != 0){
            disk_read(inode->indirect,indirect_ptr_block.pointers);
            for(int i = 0; i < indirect_block_range; i++){
                if(freemap[indirect_ptr_block.pointers[i]] == 1){
                    disk_read(indirect_ptr_block.pointers[i], indirdata_block.data);
                    for(int j = 0; (j < DISK_BLOCK_SIZE) && (index_in_wdata < length); j++){
                        indirdata_block.data[j] = data[index_in_wdata];
                        index_in_wdata++;
                    }
                } else { // if file does not already have content in indirect, allocate new blocks
                    int freeblock = find_empty_block();
                    if(freeblock == -1){
                        printf("fs_write: disk full (indirect op). aborting operation 3\n");
                        return 0;
                    }
                    indirect_ptr_block.pointers[i] = freeblock;
                    for(int j = 0; (j < DISK_BLOCK_SIZE) && (index_in_wdata < length); j++){
                        indirdata_block.data[j] = data[index_in_wdata];
                        index_in_wdata++;
                    }
                    // [To-do]
                }
                disk_write(indirect_ptr_block.pointers[i],indirdata_block.data);
            }
        } else { // construct indirect blocks
            int freeblock = find_empty_block();
        }
    } else {
        union fs_block direct_data_block;
        union fs_block data_to_write;
        
        // copy and write first block
        disk_read(inode->direct[ptr_index], direct_data_block.data);
        memcpy(data_to_write.data,direct_data_block.data,DISK_BLOCK_SIZE);
        for(int j = ptr_block_index; j < DISK_BLOCK_SIZE; j++){
            data_to_write.data[j] = data[j-ptr_block_index];// data starts from index 0
        }
        disk_write(data_to_write.data,direct_data_block.data);

        // copy and write blocks 2 ~ n-1 
        for(int i = ptr_index+1; i < write_length_blks; i++){
        disk_read(inode->direct[i], direct_data_block.data);
        memcpy(data_to_write.data,direct_data_block.data,DISK_BLOCK_SIZE);
            for(int j = 0; j < DISK_BLOCK_SIZE; j++){
                data_to_write.data[j] = data[j-ptr_block_index];// data starts from index 0
            }
        disk_write(data_to_write.data,direct_data_block.data);
        }

        // copy and write last block
        int index_in_data = ptr_block_index + DISK_BLOCK_SIZE*(write_length_blks-1);
        disk_read(inode->direct[ptr_index+write_length_blks+1], direct_data_block.data);
        memcpy(data_to_write.data,direct_data_block.data,DISK_BLOCK_SIZE);
        for(int i = 0; i < write_length_in_blk; i++){
            data_to_write.data[i] = data[index_in_data+i];
        }
        disk_write(data_to_write.data,direct_data_block.data);
    }

    // [To-do] Write back the updated inode

    return 0;
}
