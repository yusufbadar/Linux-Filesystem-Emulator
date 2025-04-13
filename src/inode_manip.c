#include "filesys.h"
#include "utility.h"
#include "debug.h"
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define INDIRECT_DBLOCK_INDEX_COUNT (DATA_BLOCK_SIZE / sizeof(dblock_index_t) - 1
#define NEXT_INDIRECT_INDEX_OFFSET (DATA_BLOCK_SIZE - sizeof(dblock_index_t))

static fs_retcode_t get_or_allocate_indirect_data_block(filesystem_t *fs, inode_t *inode, 
    size_t indirect_index, dblock_index_t *result) {
    if (!fs || !inode || !result) return INVALID_INPUT;

    if (inode->internal.indirect_dblock == 0) {
        fs_retcode_t ret = claim_available_dblock(fs, &inode->internal.indirect_dblock);
        if (ret != SUCCESS) return ret;
        memset(fs->dblocks + inode->internal.indirect_dblock * DATA_BLOCK_SIZE, 0, DATA_BLOCK_SIZE);
    }

    dblock_index_t current = inode->internal.indirect_dblock;
    size_t levels = indirect_index / INDIRECT_DBLOCK_INDEX_COUNT;
    
    for (size_t i = 0; i < levels; i++) {
        dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
        
        if (index_arr[INDIRECT_DBLOCK_INDEX_COUNT] == 0) {
            fs_retcode_t ret = claim_available_dblock(fs, &index_arr[INDIRECT_DBLOCK_INDEX_COUNT]);
            if (ret != SUCCESS) return ret;
            memset(fs->dblocks + index_arr[INDIRECT_DBLOCK_INDEX_COUNT] * DATA_BLOCK_SIZE, 0, DATA_BLOCK_SIZE);
        }
        current = index_arr[INDIRECT_DBLOCK_INDEX_COUNT];
    }

    dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
    size_t rem = indirect_index % INDIRECT_DBLOCK_INDEX_COUNT;
    
    if (index_arr[rem] == 0) {
        fs_retcode_t ret = claim_available_dblock(fs, &index_arr[rem]);
        if (ret != SUCCESS) return ret;
    }
    
    *result = index_arr[rem];
    return SUCCESS;
}

static fs_retcode_t get_data_block(filesystem_t *fs, inode_t *inode, size_t block_index, dblock_index_t *result) {
    if (!fs || !inode || !result) return INVALID_INPUT;

    if (block_index < INODE_DIRECT_BLOCK_COUNT) {
        *result = inode->internal.direct_data[block_index];
        return SUCCESS;
    }

    size_t indirect_index = block_index - INODE_DIRECT_BLOCK_COUNT;
    if (inode->internal.indirect_dblock == 0) return INVALID_INPUT;

    dblock_index_t current = inode->internal.indirect_dblock;
    while (indirect_index >= INDIRECT_DBLOCK_INDEX_COUNT) {
        dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
        if (index_arr[INDIRECT_DBLOCK_INDEX_COUNT] == 0) return INVALID_INPUT;
        current = index_arr[INDIRECT_DBLOCK_INDEX_COUNT];
        indirect_index -= INDIRECT_DBLOCK_INDEX_COUNT;
    }

    dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
    *result = index_arr[indirect_index];
    return SUCCESS;
}

fs_retcode_t inode_write_data(filesystem_t *fs, inode_t *inode, void *data, size_t n) {
    if (!fs || !inode || !data) return INVALID_INPUT;
    if (n == 0) return SUCCESS; 

    size_t original_size = inode->internal.file_size;
    dblock_index_t original_direct[INODE_DIRECT_BLOCK_COUNT];
    memcpy(original_direct, inode->internal.direct_data, sizeof(original_direct));
    dblock_index_t original_indirect = inode->internal.indirect_dblock;

    size_t new_size = original_size + n;
    size_t required_blocks = (new_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;
    size_t current_blocks = (original_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;

    size_t needed_blocks = required_blocks - current_blocks;
    if (needed_blocks > 0 && available_dblocks(fs) < needed_blocks) {
        return INSUFFICIENT_DBLOCKS;
    }

    byte *data_ptr = (byte *)data;
    size_t bytes_remaining = n;
    size_t offset_in_block = original_size % DATA_BLOCK_SIZE;
    size_t block_index = current_blocks;

    if (offset_in_block != 0 && current_blocks > 0) {
        dblock_index_t dblock_id;
        fs_retcode_t ret = get_data_block(fs, inode, current_blocks - 1, &dblock_id);
        if (ret != SUCCESS) return ret;

        size_t space_in_block = DATA_BLOCK_SIZE - offset_in_block;
        size_t to_copy = (bytes_remaining < space_in_block) ? bytes_remaining : space_in_block;
        memcpy(fs->dblocks + dblock_id * DATA_BLOCK_SIZE + offset_in_block, data_ptr, to_copy);

        data_ptr += to_copy;
        bytes_remaining -= to_copy;
        inode->internal.file_size += to_copy;
    }

    while (bytes_remaining > 0) {
        dblock_index_t current_dblock;
        fs_retcode_t ret;

        if (block_index < INODE_DIRECT_BLOCK_COUNT) {
            ret = claim_available_dblock(fs, &inode->internal.direct_data[block_index]);
            if (ret != SUCCESS) goto rollback;
            current_dblock = inode->internal.direct_data[block_index];
        } else {
            ret = get_or_allocate_indirect_data_block(fs, inode, block_index - INODE_DIRECT_BLOCK_COUNT, &current_dblock);
            if (ret != SUCCESS) goto rollback;
        }

        size_t to_copy = (bytes_remaining < DATA_BLOCK_SIZE) ? bytes_remaining : DATA_BLOCK_SIZE;
        memcpy(fs->dblocks + current_dblock * DATA_BLOCK_SIZE, data_ptr, to_copy);

        data_ptr += to_copy;
        bytes_remaining -= to_copy;
        inode->internal.file_size += to_copy;
        block_index++;
    }

    return SUCCESS;

rollback:
    inode->internal.file_size = original_size;
    memcpy(inode->internal.direct_data, original_direct, sizeof(original_direct));
    inode->internal.indirect_dblock = original_indirect;
    return INSUFFICIENT_DBLOCKS;
}

fs_retcode_t inode_read_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n, size_t *bytes_read) {
    if (!fs || !inode || !buffer || !bytes_read) return INVALID_INPUT;
    
    *bytes_read = 0;
    size_t file_size = inode->internal.file_size;
    
    if (offset >= file_size) return SUCCESS;
    
    size_t to_read = (offset + n > file_size) ? (file_size - offset) : n;
    *bytes_read = to_read;
    
    size_t start_block = offset / DATA_BLOCK_SIZE;
    size_t block_offset = offset % DATA_BLOCK_SIZE;
    size_t remaining = to_read;
    byte *buf_ptr = (byte *)buffer;

    while (remaining > 0) {
        dblock_index_t dblock;
        fs_retcode_t ret = get_data_block(fs, inode, start_block, &dblock);
        if (ret != SUCCESS) return ret;

        size_t copy_size = DATA_BLOCK_SIZE - block_offset;
        if (copy_size > remaining) copy_size = remaining;

        memcpy(buf_ptr, fs->dblocks + dblock * DATA_BLOCK_SIZE + block_offset, copy_size);

        buf_ptr += copy_size;
        remaining -= copy_size;
        start_block++;
        block_offset = 0;
    }

    return SUCCESS;
}

fs_retcode_t inode_modify_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n) {
    if (!fs || !inode || !buffer) return INVALID_INPUT;
    if (n == 0) return SUCCESS;
    size_t file_size = inode->internal.file_size;
    if (offset > file_size) return INVALID_INPUT;

    size_t end_offset = offset + n;
    size_t overwrite = (end_offset <= file_size) ? n : (file_size - offset);
    size_t append = (end_offset > file_size) ? (end_offset - file_size) : 0;

    if (overwrite > 0) {
        size_t current = offset;
        size_t remaining = overwrite;
        byte *buf_ptr = (byte *)buffer;

        while (remaining > 0) {
            dblock_index_t dblock;
            fs_retcode_t ret = get_data_block(fs, inode, current / DATA_BLOCK_SIZE, &dblock);
            if (ret != SUCCESS) return ret;

            size_t block_offset = current % DATA_BLOCK_SIZE;
            size_t copy_size = DATA_BLOCK_SIZE - block_offset;
            if (copy_size > remaining) copy_size = remaining;

            memcpy(fs->dblocks + dblock * DATA_BLOCK_SIZE + block_offset, buf_ptr, copy_size);

            current += copy_size;
            buf_ptr += copy_size;
            remaining -= copy_size;
        }
    }

    if (append > 0) {
        return inode_write_data(fs, inode, (byte *)buffer + overwrite, append);
    }

    return SUCCESS;
}

fs_retcode_t inode_shrink_data(filesystem_t *fs, inode_t *inode, size_t new_size) {
    if (!fs || !inode) return INVALID_INPUT;
    if (new_size > inode->internal.file_size) return INVALID_INPUT;

    size_t current_size = inode->internal.file_size;
    if (new_size == current_size) return SUCCESS;

    size_t new_blocks = (new_size == 0) ? 0 : ((new_size - 1) / DATA_BLOCK_SIZE + 1);
    size_t current_blocks = (current_size == 0) ? 0 : ((current_size - 1) / DATA_BLOCK_SIZE + 1);

    for (size_t b = new_blocks; b < current_blocks; b++) {
        dblock_index_t dblock;
        fs_retcode_t ret = get_data_block(fs, inode, b, &dblock);
        if (ret != SUCCESS) return ret;

        release_dblock(fs, fs->dblocks + dblock * DATA_BLOCK_SIZE);

        if (b < INODE_DIRECT_BLOCK_COUNT) {
            inode->internal.direct_data[b] = 0;
        } else {
            size_t indirect_index = b - INODE_DIRECT_BLOCK_COUNT;
            dblock_index_t current = inode->internal.indirect_dblock;
            
            while (indirect_index >= INDIRECT_DBLOCK_INDEX_COUNT) {
                dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
                current = arr[INDIRECT_DBLOCK_INDEX_COUNT];
                indirect_index -= INDIRECT_DBLOCK_INDEX_COUNT;
            }
            
            dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
            arr[indirect_index] = 0;
        }
    }

    int has_entries;
    dblock_index_t prev = 0;
    dblock_index_t current = inode->internal.indirect_dblock;
    while (current != 0) {
        dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
        has_entries = 0;
        
        for (size_t i = 0; i < INDIRECT_DBLOCK_INDEX_COUNT; i++) {
            if (arr[i] != 0) {
                has_entries = 1;
                break;
            }
        }
        
        if (!has_entries) {
            dblock_index_t next = arr[INDIRECT_DBLOCK_INDEX_COUNT];
            release_dblock(fs, fs->dblocks + current * DATA_BLOCK_SIZE);
            if (prev == 0) {
                inode->internal.indirect_dblock = next;
            } else {
                dblock_index_t *prev_arr = cast_dblock_ptr(fs->dblocks + prev * DATA_BLOCK_SIZE);
                prev_arr[INDIRECT_DBLOCK_INDEX_COUNT] = next;
            }
            current = next;
        } else {
            prev = current;
            current = arr[INDIRECT_DBLOCK_INDEX_COUNT];
        }
    }
    if (new_size > 0 && new_size % DATA_BLOCK_SIZE != 0) {
        dblock_index_t last_block;
        fs_retcode_t ret = get_data_block(fs, inode, new_blocks - 1, &last_block);
        if (ret != SUCCESS) return ret;

        size_t tail_offset = new_size % DATA_BLOCK_SIZE;
        memset(fs->dblocks + last_block * DATA_BLOCK_SIZE + tail_offset, 0, DATA_BLOCK_SIZE - tail_offset);
    }

    inode->internal.file_size = new_size;
    return SUCCESS;
}

fs_retcode_t inode_release_data(filesystem_t *fs, inode_t *inode) {
    return inode_shrink_data(fs, inode, 0);
}