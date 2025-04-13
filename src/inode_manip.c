#include "filesys.h"
#include "utility.h"
#include "debug.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define INDIRECT_DBLOCK_INDEX_COUNT (DATA_BLOCK_SIZE / sizeof(dblock_index_t) - 1)
#define NEXT_INDIRECT_INDEX_OFFSET (DATA_BLOCK_SIZE - sizeof(dblock_index_t))

static fs_retcode_t get_data_block_index(filesystem_t *fs, inode_t *inode, size_t block_index, dblock_index_t *result) {
    if (!fs || !inode || !result) return INVALID_INPUT;

    if (block_index < INODE_DIRECT_BLOCK_COUNT) {
        *result = inode->internal.direct_data[block_index];
        return (*result != 0) ? SUCCESS : DBLOCK_UNAVAILABLE;
    } else {
        size_t indirect_index = block_index - INODE_DIRECT_BLOCK_COUNT;
        if (inode->internal.indirect_dblock == 0) return DBLOCK_UNAVAILABLE;

        dblock_index_t current_index_block = inode->internal.indirect_dblock;
        size_t rem = indirect_index;

        while (rem >= INDIRECT_DBLOCK_INDEX_COUNT) {
            if (current_index_block == 0 || current_index_block >= fs->dblock_count) {
                 return DBLOCK_UNAVAILABLE;
            }
            dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current_index_block * DATA_BLOCK_SIZE);
            dblock_index_t next_index_block = index_arr[INDIRECT_DBLOCK_INDEX_COUNT];
            if (next_index_block == 0) return DBLOCK_UNAVAILABLE;
            current_index_block = next_index_block;
            rem -= INDIRECT_DBLOCK_INDEX_COUNT;
        }

         if (current_index_block == 0 || current_index_block >= fs->dblock_count) {
             return DBLOCK_UNAVAILABLE;
         }

        dblock_index_t *final_index_arr = cast_dblock_ptr(fs->dblocks + current_index_block * DATA_BLOCK_SIZE);
        *result = final_index_arr[rem];
        return (*result != 0) ? SUCCESS : DBLOCK_UNAVAILABLE;
    }
}


// Helper to allocate blocks atomically for write/modify
static fs_retcode_t allocate_needed_blocks(filesystem_t *fs, size_t num_data_blocks, size_t num_index_blocks, dblock_index_t **allocated_data_indices, dblock_index_t **allocated_index_indices) {
    if ((num_data_blocks + num_index_blocks) > available_dblocks(fs)) {
        return INSUFFICIENT_DBLOCKS;
    }

    *allocated_data_indices = NULL;
    *allocated_index_indices = NULL;
    dblock_index_t *data_indices = NULL;
    dblock_index_t *index_indices = NULL;
    size_t data_allocated = 0;
    size_t index_allocated = 0;
    fs_retcode_t ret = SUCCESS;

    if (num_data_blocks > 0) {
        data_indices = malloc(num_data_blocks * sizeof(dblock_index_t));
        if (!data_indices) return SYSTEM_ERROR;
        for (data_allocated = 0; data_allocated < num_data_blocks; ++data_allocated) {
            ret = claim_available_dblock(fs, &data_indices[data_allocated]);
            if (ret != SUCCESS) goto cleanup;
        }
    }

    if (num_index_blocks > 0) {
        index_indices = malloc(num_index_blocks * sizeof(dblock_index_t));
        if (!index_indices) { ret = SYSTEM_ERROR; goto cleanup; }
        for (index_allocated = 0; index_allocated < num_index_blocks; ++index_allocated) {
            ret = claim_available_dblock(fs, &index_indices[index_allocated]);
            if (ret != SUCCESS) goto cleanup;
            memset(fs->dblocks + index_indices[index_allocated] * DATA_BLOCK_SIZE, 0, DATA_BLOCK_SIZE);
        }
    }

    *allocated_data_indices = data_indices;
    *allocated_index_indices = index_indices;
    return SUCCESS;

cleanup:
    for (size_t i = 0; i < data_allocated; ++i) {
        release_dblock(fs, fs->dblocks + data_indices[i] * DATA_BLOCK_SIZE);
    }
    for (size_t i = 0; i < index_allocated; ++i) {
        release_dblock(fs, fs->dblocks + index_indices[i] * DATA_BLOCK_SIZE);
    }
    free(data_indices);
    free(index_indices);
    *allocated_data_indices = NULL;
    *allocated_index_indices = NULL;

    return (ret == SYSTEM_ERROR) ? SYSTEM_ERROR : INSUFFICIENT_DBLOCKS;
}

fs_retcode_t inode_write_data(filesystem_t *fs, inode_t *inode, void *data, size_t n) {
    if (!fs || !inode || (!data && n > 0)) return INVALID_INPUT;
    if (n == 0) return SUCCESS;

    size_t current_size = inode->internal.file_size;
    size_t new_size = current_size + n;

    size_t current_data_blocks = (current_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;
    size_t required_data_blocks = (new_size + DATA_BLOCK_SIZE - 1) / DATA_BLOCK_SIZE;
    size_t additional_data_blocks = (required_data_blocks > current_data_blocks) ? (required_data_blocks - current_data_blocks) : 0;

    size_t current_index_blocks = 0;
    if (current_data_blocks > INODE_DIRECT_BLOCK_COUNT) {
        current_index_blocks = (current_data_blocks - INODE_DIRECT_BLOCK_COUNT + INDIRECT_DBLOCK_INDEX_COUNT - 1) / INDIRECT_DBLOCK_INDEX_COUNT;
    }
    size_t required_index_blocks = 0;
    if (required_data_blocks > INODE_DIRECT_BLOCK_COUNT) {
        required_index_blocks = (required_data_blocks - INODE_DIRECT_BLOCK_COUNT + INDIRECT_DBLOCK_INDEX_COUNT - 1) / INDIRECT_DBLOCK_INDEX_COUNT;
    }
    size_t additional_index_blocks = (required_index_blocks > current_index_blocks) ? (required_index_blocks - current_index_blocks) : 0;

    dblock_index_t *new_data_indices = NULL;
    dblock_index_t *new_index_indices = NULL;
    fs_retcode_t alloc_ret = allocate_needed_blocks(fs, additional_data_blocks, additional_index_blocks, &new_data_indices, &new_index_indices);
    if (alloc_ret != SUCCESS) {
        return alloc_ret;
    }

    byte *data_ptr = (byte *)data;
    size_t bytes_to_write = n;
    size_t current_offset = current_size;
    size_t new_data_idx_ctr = 0;
    size_t new_index_idx_ctr = 0;

    if (current_size > 0) {
        size_t offset_in_last_block = current_size % DATA_BLOCK_SIZE;
        if (offset_in_last_block != 0) {
            size_t space_in_block = DATA_BLOCK_SIZE - offset_in_last_block;
            size_t copy_amount = (bytes_to_write < space_in_block) ? bytes_to_write : space_in_block;
            dblock_index_t last_block_idx;

            get_data_block_index(fs, inode, current_data_blocks - 1, &last_block_idx);
            memcpy(fs->dblocks + last_block_idx * DATA_BLOCK_SIZE + offset_in_last_block, data_ptr, copy_amount);
            data_ptr += copy_amount;
            bytes_to_write -= copy_amount;
            current_offset += copy_amount;
        }
    }

    size_t block_write_index = current_data_blocks;
    while (bytes_to_write > 0) {
         dblock_index_t target_data_block = new_data_indices[new_data_idx_ctr++];

        if (block_write_index < INODE_DIRECT_BLOCK_COUNT) {
            inode->internal.direct_data[block_write_index] = target_data_block;
        } else {
            size_t indirect_data_index = block_write_index - INODE_DIRECT_BLOCK_COUNT;
            size_t index_block_level = indirect_data_index / INDIRECT_DBLOCK_INDEX_COUNT;
            size_t slot_in_index_block = indirect_data_index % INDIRECT_DBLOCK_INDEX_COUNT;

            dblock_index_t current_index_block = inode->internal.indirect_dblock;

            if (current_index_block == 0) {
                 current_index_block = new_index_indices[new_index_idx_ctr++];
                 inode->internal.indirect_dblock = current_index_block;
            }

            for (size_t level = 0; level < index_block_level; ++level) {
                dblock_index_t *index_arr = cast_dblock_ptr(fs->dblocks + current_index_block * DATA_BLOCK_SIZE);
                if (index_arr[INDIRECT_DBLOCK_INDEX_COUNT] == 0) {

                    index_arr[INDIRECT_DBLOCK_INDEX_COUNT] = new_index_indices[new_index_idx_ctr++];
                }
                 current_index_block = index_arr[INDIRECT_DBLOCK_INDEX_COUNT];
            }

            dblock_index_t *final_index_arr = cast_dblock_ptr(fs->dblocks + current_index_block * DATA_BLOCK_SIZE);
            final_index_arr[slot_in_index_block] = target_data_block;
        }

        size_t copy_amount = (bytes_to_write < DATA_BLOCK_SIZE) ? bytes_to_write : DATA_BLOCK_SIZE;
        memcpy(fs->dblocks + target_data_block * DATA_BLOCK_SIZE, data_ptr, copy_amount);
        data_ptr += copy_amount;
        bytes_to_write -= copy_amount;
        current_offset += copy_amount;
        block_write_index++;
    }

    inode->internal.file_size = new_size;
    free(new_data_indices);
    free(new_index_indices);

    return SUCCESS;
}


fs_retcode_t inode_read_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n, size_t *bytes_read) {
    if (!fs || !inode || !buffer || !bytes_read) return INVALID_INPUT;

    *bytes_read = 0;
    size_t file_size = inode->internal.file_size;

    if (offset >= file_size || n == 0) {
        return SUCCESS;
    }

    size_t read_limit = file_size - offset;
    size_t total_to_read = (n < read_limit) ? n : read_limit;
    size_t bytes_remaining = total_to_read;
    size_t current_read_offset = offset;
    byte *buffer_ptr = (byte *)buffer;

    while (bytes_remaining > 0) {
        size_t current_block_index = current_read_offset / DATA_BLOCK_SIZE;
        size_t offset_in_block = current_read_offset % DATA_BLOCK_SIZE;
        size_t read_from_this_block = DATA_BLOCK_SIZE - offset_in_block;
        if (read_from_this_block > bytes_remaining) {
            read_from_this_block = bytes_remaining;
        }

        dblock_index_t data_block_idx;
        fs_retcode_t get_ret = get_data_block_index(fs, inode, current_block_index, &data_block_idx);

        if (get_ret != SUCCESS) {

            return SYSTEM_ERROR;
        }

        memcpy(buffer_ptr, fs->dblocks + data_block_idx * DATA_BLOCK_SIZE + offset_in_block, read_from_this_block);

        buffer_ptr += read_from_this_block;
        bytes_remaining -= read_from_this_block;
        current_read_offset += read_from_this_block;
        *bytes_read += read_from_this_block;
    }

    return SUCCESS;
}


fs_retcode_t inode_modify_data(filesystem_t *fs, inode_t *inode, size_t offset, void *buffer, size_t n) {
    if (!fs || !inode || (!buffer && n > 0)) return INVALID_INPUT;
    if (n == 0) return SUCCESS;

    size_t file_size = inode->internal.file_size;

    if (offset > file_size) return INVALID_INPUT;

    size_t overwrite_end = offset + n;
    size_t overwrite_bytes = 0;
    size_t append_bytes = 0;

    if (overwrite_end <= file_size) {
        overwrite_bytes = n;
    } else {
        overwrite_bytes = file_size - offset;
        append_bytes = overwrite_end - file_size;
    }

    byte *buffer_ptr = (byte *)buffer;

    size_t current_write_offset = offset;
    size_t bytes_to_overwrite_remaining = overwrite_bytes;
    while (bytes_to_overwrite_remaining > 0) {
        size_t current_block_index = current_write_offset / DATA_BLOCK_SIZE;
        size_t offset_in_block = current_write_offset % DATA_BLOCK_SIZE;
        size_t write_in_this_block = DATA_BLOCK_SIZE - offset_in_block;
        if (write_in_this_block > bytes_to_overwrite_remaining) {
            write_in_this_block = bytes_to_overwrite_remaining;
        }

        dblock_index_t data_block_idx;

        fs_retcode_t get_ret = get_data_block_index(fs, inode, current_block_index, &data_block_idx);
        if (get_ret != SUCCESS) return SYSTEM_ERROR;

        memcpy(fs->dblocks + data_block_idx * DATA_BLOCK_SIZE + offset_in_block, buffer_ptr, write_in_this_block);

        buffer_ptr += write_in_this_block;
        bytes_to_overwrite_remaining -= write_in_this_block;
        current_write_offset += write_in_this_block;
    }

    if (append_bytes > 0) {

        fs_retcode_t write_ret = inode_write_data(fs, inode, buffer_ptr, append_bytes);

        if (write_ret != SUCCESS) {

            return write_ret;
        }
    }


    return SUCCESS;
}


fs_retcode_t inode_shrink_data(filesystem_t *fs, inode_t *inode, size_t new_size) {
    if (!fs || !inode) return INVALID_INPUT;
    if (new_size > inode->internal.file_size) return INVALID_INPUT;
    size_t old_size = inode->internal.file_size;
    if (new_size == old_size) return SUCCESS;
    size_t old_blocks = (old_size == 0 ? 0 : ((old_size - 1) / DATA_BLOCK_SIZE + 1));
    size_t new_blocks = (new_size == 0 ? 0 : ((new_size - 1) / DATA_BLOCK_SIZE + 1));
    size_t direct = INODE_DIRECT_BLOCK_COUNT;
    size_t i;
    for (i = new_blocks; i < old_blocks; i++) {
        if (i < direct) {
            dblock_index_t d = inode->internal.direct_data[i];
            if (d != 0) {
                release_dblock(fs, fs->dblocks + d * DATA_BLOCK_SIZE);
                inode->internal.direct_data[i] = 0;
            }
        } else {
            size_t idx = i - direct;
            dblock_index_t cur = inode->internal.indirect_dblock;
            size_t rem = idx;
            while (rem >= INDIRECT_DBLOCK_INDEX_COUNT) {
                dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + cur * DATA_BLOCK_SIZE);
                cur = arr[INDIRECT_DBLOCK_INDEX_COUNT];
                rem -= INDIRECT_DBLOCK_INDEX_COUNT;
            }
            dblock_index_t *slot = cast_dblock_ptr(fs->dblocks + cur * DATA_BLOCK_SIZE) + rem;
            if (*slot != 0) {
                release_dblock(fs, fs->dblocks + (*slot) * DATA_BLOCK_SIZE);
                *slot = 0;
            }
        }
    }
    if (new_blocks <= direct) {
        if (inode->internal.indirect_dblock != 0) {
            dblock_index_t cur = inode->internal.indirect_dblock;
            while (cur != 0) {
                dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + cur * DATA_BLOCK_SIZE);
                dblock_index_t nxt = arr[INDIRECT_DBLOCK_INDEX_COUNT];
                release_dblock(fs, fs->dblocks + cur * DATA_BLOCK_SIZE);
                cur = nxt;
            }
            inode->internal.indirect_dblock = 0;
        }
    } else {
        size_t needed_slots = (new_blocks > direct) ? (new_blocks - direct) : 0;
        dblock_index_t prev = 0, cur = inode->internal.indirect_dblock;
        size_t count = 0;
        while (cur != 0) {
            count += INDIRECT_DBLOCK_INDEX_COUNT;
            if (count >= needed_slots) break;
            prev = cur;
            dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + cur * DATA_BLOCK_SIZE);
            cur = arr[INDIRECT_DBLOCK_INDEX_COUNT];
        }
        if (cur != 0) {
            if (prev == 0)
                inode->internal.indirect_dblock = 0;
            else {
                dblock_index_t *prev_arr = cast_dblock_ptr(fs->dblocks + prev * DATA_BLOCK_SIZE);
                prev_arr[INDIRECT_DBLOCK_INDEX_COUNT] = 0;
            }
            while (cur != 0) {
                dblock_index_t *arr = cast_dblock_ptr(fs->dblocks + cur * DATA_BLOCK_SIZE);
                dblock_index_t nxt = arr[INDIRECT_DBLOCK_INDEX_COUNT];
                release_dblock(fs, fs->dblocks + cur * DATA_BLOCK_SIZE);
                cur = nxt;
            }
        }
    }
    inode->internal.file_size = new_size;
    return SUCCESS;
}

fs_retcode_t inode_release_data(filesystem_t *fs, inode_t *inode) {
    if (!fs || !inode) return INVALID_INPUT;
    return inode_shrink_data(fs, inode, 0);
}