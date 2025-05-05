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
    return -1;
}

int fs_read(int inumber, char *data, int length, int offset)
{
    // check valid inumber
    if (inumber < 0 || inumber >= superblock.super.ninodes)
        return 0;

    int block_num = 1 + inumber / INODES_PER_BLOCK;
    int inode_index = inumber % INODES_PER_BLOCK;

    union fs_block block;
    disk_read(block_num, block.data);
    struct fs_inode *inode = &block.inode[inode_index];

    // check if inode is valid
    if (!inode->isvalid)
        return 0;

    //  offset too big
    if (offset >= inode->size)
        return 0;

    // set len
    if (offset + length > inode->size)
        length = inode->size - offset;

    int bytes_read = 0;
    int start_block = offset / DISK_BLOCK_SIZE;
    int start_offset = offset % DISK_BLOCK_SIZE;

    // read from direct blocks first
    for (int i = start_block; i < POINTERS_PER_INODE && bytes_read < length; i++) {
        if (inode->direct[i] == 0)
            break;

        union fs_block data_block;
        disk_read(inode->direct[i], data_block.data);

        int copy_start = (i == start_block) ? start_offset : 0;
        int copy_len = DISK_BLOCK_SIZE - copy_start;
        if (copy_len > length - bytes_read)
            copy_len = length - bytes_read;

        memcpy(data + bytes_read, data_block.data + copy_start, copy_len);
        bytes_read += copy_len;
    }

    // If needed, read from indirect block
    if (bytes_read < length && inode->indirect) {
        union fs_block indirect_block;
        disk_read(inode->indirect, indirect_block.data);

        for (int i = 0; i < POINTERS_PER_BLOCK && bytes_read < length; i++) {
            int logical_block = POINTERS_PER_INODE + i;
            if ((offset / DISK_BLOCK_SIZE) > logical_block)
                continue;

            if (indirect_block.pointers[i] == 0)
                break;

            union fs_block data_block;
            disk_read(indirect_block.pointers[i], data_block.data);

            int copy_start = (logical_block == offset / DISK_BLOCK_SIZE) ? start_offset : 0;
            int copy_len = DISK_BLOCK_SIZE - copy_start;
            if (copy_len > length - bytes_read)
                copy_len = length - bytes_read;

            memcpy(data + bytes_read, data_block.data + copy_start, copy_len);
            bytes_read += copy_len;
        }
    }

    return bytes_read;
}

int fs_write(int inumber, const char *data, int length, int offset)
{
    return 0;
}
