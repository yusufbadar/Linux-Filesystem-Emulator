#include "filesys.h"
#include "utility.h"
#include "debug.h"
#include <string.h>
#include <assert.h>

#define INDIRECT_DBLOCK_INDEX_COUNT (DATA_BLOCK_SIZE / sizeof(dblock_index_t) - 1)
#define NEXT_INDIRECT_INDEX_OFFSET (DATA_BLOCK_SIZE - sizeof(dblock_index_t))

static fs_retcode_t get_or_allocate_indirect_data_block(filesystem_t *fs, inode_t *inode, size_t indirect_index, dblock_index_t *result) {
    fs_retcode_t ret;
    if (inode->internal.indirect_dblock == 0) {
        dblock_index_t new_index;
        ret = claim_available_dblock(fs, &new_index);
        if (ret != SUCCESS) return ret;
        inode->internal.indirect_dblock = new_index;
        memset(fs->dblocks + new_index * DATA_BLOCK_SIZE, 0, DATA_BLOCK_SIZE);
    }
    size_t rem = indirect_index;
    dblock_index_t current = inode->internal.indirect_dblock;
    while (rem >= INDIRECT_DBLOCK_INDEX_COUNT) {
        dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
        if (index_arr[INDIRECT_DBLOCK_INDEX_COUNT] == 0) {
            dblock_index_t new_index;
            ret = claim_available_dblock(fs, &new_index);
            if (ret != SUCCESS) return ret;
            index_arr[INDIRECT_DBLOCK_INDEX_COUNT] = new_index;
            memset(fs->dblocks + new_index * DATA_BLOCK_SIZE, 0, DATA_BLOCK_SIZE);
        }
        current = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE)[INDIRECT_DBLOCK_INDEX_COUNT];
        rem -= INDIRECT_DBLOCK_INDEX_COUNT;
    }
    dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current * DATA_BLOCK_SIZE);
    if (index_arr[rem] == 0) {
        dblock_index_t new_data;
        ret = claim_available_dblock(fs, &new_data);
        if (ret != SUCCESS) return ret;
        index_arr[rem] = new_data;
    }
    *result = index_arr[rem];
    return SUCCESS;
}

static fs_retcode_t get_data_block(filesystem_t *fs, inode_t *inode, size_t block_index, dblock_index_t *result) {
    if (block_index < INODE_DIRECT_BLOCK_COUNT) {
        *result = inode->internal.direct_data[block_index];
        return SUCCESS;
    } else {
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
}

fs_retcode_t inode_write_data(filesystem_t *fs, inode_t *inode, void *data, size_t n) {
    if (!fs || !inode || !data) return INVALID_INPUT;
    size_t current_size = inode->internal.file_size;
    size_t new_size = current_size + n;
    size_t blocks_required = (new_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;
    size_t current_blocks = (current_size == 0) ? 0 : ((current_size - 1) / DATA_BLOCK_SIZE + 1);
    size_t additional_blocks = (blocks_required > current_blocks) ? (blocks_required - current_blocks) : 0;
    size_t required_index_blocks = 0;
    if (blocks_required > INODE_DIRECT_BLOCK_COUNT) {
        size_t req = blocks_required - INODE_DIRECT_BLOCK_COUNT;
        required_index_blocks = (req + INDIRECT_DBLOCK_INDEX_COUNT - 1) / INDIRECT_DBLOCK_INDEX_COUNT;
    }
    size_t current_index_blocks = 0;
    if (inode->internal.indirect_dblock != 0) {
        dblock_index_t curr = inode->internal.indirect_dblock;
        while (curr != 0) {
            current_index_blocks++;
            dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + curr * DATA_BLOCK_SIZE);
            if (arr[INDIRECT_DBLOCK_INDEX_COUNT] == 0) break;
            curr = arr[INDIRECT_DBLOCK_INDEX_COUNT];
        }
    }
    size_t additional_index_blocks = (required_index_blocks > current_index_blocks) ? (required_index_blocks - current_index_blocks) : 0;
    size_t total_additional = additional_blocks + additional_index_blocks;
    if (total_additional > available_dblocks(fs)) return INSUFFICIENT_DBLOCKS;
    size_t original_size = current_size;
    byte *data_ptr = (byte *)data;
    size_t bytes_remaining = n;
    size_t offset_in_block = current_size % DATA_BLOCK_SIZE;
    size_t block_index = current_blocks;
    if (offset_in_block != 0 && current_blocks > 0) {
        dblock_index_t dblock_id;
        if (current_blocks <= INODE_DIRECT_BLOCK_COUNT)
            dblock_id = inode->internal.direct_data[current_blocks - 1];
        else {
            if (get_data_block(fs, inode, current_blocks - 1, &dblock_id) != SUCCESS)
                return INVALID_INPUT;
        }
        size_t space_in_block = DATA_BLOCK_SIZE - offset_in_block;
        size_t to_copy = (bytes_remaining < space_in_block) ? bytes_remaining : space_in_block;
        memcpy(fs->dblocks + dblock_id * DATA_BLOCK_SIZE + offset_in_block, data_ptr, to_copy);
        data_ptr += to_copy;
        bytes_remaining -= to_copy;
        current_size += to_copy;
    }
    while (bytes_remaining > 0) {
        if (block_index < INODE_DIRECT_BLOCK_COUNT) {
            dblock_index_t new_dblock;
            if (claim_available_dblock(fs, &new_dblock) != SUCCESS) {
                inode->internal.file_size = original_size;
                return INSUFFICIENT_DBLOCKS;
            }
            inode->internal.direct_data[block_index] = new_dblock;
        } else {
            dblock_index_t new_dblock;
            if (get_or_allocate_indirect_data_block(fs, inode, block_index - INODE_DIRECT_BLOCK_COUNT, &new_dblock) != SUCCESS) {
                inode->internal.file_size = original_size;
                return INSUFFICIENT_DBLOCKS;
            }
        }
        size_t to_copy = (bytes_remaining < DATA_BLOCK_SIZE) ? bytes_remaining : DATA_BLOCK_SIZE;
        dblock_index_t current_dblock;
        if (block_index < INODE_DIRECT_BLOCK_COUNT)
            current_dblock = inode->internal.direct_data[block_index];
        else {
            if (get_data_block(fs, inode, block_index, &current_dblock) != SUCCESS) {
                inode->internal.file_size = original_size;
                return INVALID_INPUT;
            }
        }
        memcpy(fs->dblocks + current_dblock * DATA_BLOCK_SIZE, data_ptr, to_copy);
        data_ptr += to_copy;
        bytes_remaining -= to_copy;
        current_size += to_copy;
        block_index++;
    }
    inode->internal.file_size = new_size;
    return SUCCESS;
}

fs_retcode_t inode_read_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n, size_t *bytes_read) {
    if (!fs || !inode || !buffer || !bytes_read) return INVALID_INPUT;
    size_t file_size = inode->internal.file_size;
    if (offset > file_size) {
        *bytes_read = 0;
        return SUCCESS;
    }
    size_t to_read = (offset + n > file_size) ? (file_size - offset) : n;
    *bytes_read = to_read;
    size_t start_block = offset / DATA_BLOCK_SIZE;
    size_t block_offset = offset % DATA_BLOCK_SIZE;
    size_t remaining = to_read, copied = 0;
    while (remaining > 0) {
        dblock_index_t dblock;
        if (get_data_block(fs, inode, start_block, &dblock) != SUCCESS)
            return INVALID_INPUT;
        size_t copy_size = DATA_BLOCK_SIZE - block_offset;
        if (copy_size > remaining) copy_size = remaining;
        memcpy((byte *)buffer + copied, fs->dblocks + dblock * DATA_BLOCK_SIZE + block_offset, copy_size);
        remaining -= copy_size;
        copied += copy_size;
        start_block++;
        block_offset = 0;
    }
    return SUCCESS;
}

fs_retcode_t inode_modify_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n) {
    if (!fs || !inode || !buffer) return INVALID_INPUT;
    size_t file_size = inode->internal.file_size;
    if (offset > file_size) return INVALID_INPUT;
    size_t end_offset = offset + n;
    size_t overwrite = (end_offset <= file_size) ? n : (file_size - offset);
    size_t appended = (end_offset > file_size) ? (end_offset - file_size) : 0;
    size_t current = offset, remaining = overwrite, copied = 0;
    while (remaining > 0) {
        dblock_index_t dblock;
        if (get_data_block(fs, inode, current / DATA_BLOCK_SIZE, &dblock) != SUCCESS)
            return INVALID_INPUT;
        size_t block_offset = current % DATA_BLOCK_SIZE;
        size_t copy_size = DATA_BLOCK_SIZE - block_offset;
        if (copy_size > remaining) copy_size = remaining;
        memcpy(fs->dblocks + dblock * DATA_BLOCK_SIZE + block_offset, (byte *)buffer + copied, copy_size);
        current += copy_size;
        copied += copy_size;
        remaining -= copy_size;
    }
    if (appended > 0) {
        if (inode_write_data(fs, inode, (byte *)buffer + copied, appended) != SUCCESS)
            return INSUFFICIENT_DBLOCKS;
    }
    return SUCCESS;
}

fs_retcode_t inode_shrink_data(filesystem_t *fs, inode_t *inode, size_t new_size) {
    if (!fs || !inode) return INVALID_INPUT;
    if (new_size > inode->internal.file_size) return INVALID_INPUT;

    size_t old_size = inode->internal.file_size;
    size_t old_blocks = (old_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;
    size_t new_blocks = (new_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;

    for (size_t b = new_blocks; b < old_blocks; b++) {
        dblock_index_t db;
        if (get_data_block(fs, inode, b, &db) != SUCCESS) return INVALID_INPUT;
        release_dblock(fs, fs->dblocks + db * DATA_BLOCK_SIZE);
    }

    inode->internal.file_size = new_size;
    return SUCCESS;
}

fs_retcode_t inode_release_data(filesystem_t *fs, inode_t *inode) {
    if (!fs || !inode) return INVALID_INPUT;

    size_t file_size = inode->internal.file_size;
    size_t blocks = (file_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;

    for (size_t b = 0; b < blocks; b++) {
        dblock_index_t db;
        if (get_data_block(fs, inode, b, &db) != SUCCESS) return INVALID_INPUT;
        release_dblock(fs, fs->dblocks + db * DATA_BLOCK_SIZE);
    }

    dblock_index_t chain = inode->internal.indirect_dblock;
    while (chain != 0) {
        dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + chain * DATA_BLOCK_SIZE);
        dblock_index_t next = arr[INDIRECT_DBLOCK_INDEX_COUNT];
        release_dblock(fs, fs->dblocks + chain * DATA_BLOCK_SIZE);
        chain = next;
    }
    inode->internal.indirect_dblock = 0;

    inode->internal.file_size = 0;
    return SUCCESS;
}