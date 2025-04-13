#include "filesys.h"
#include "debug.h"
#include "utility.h"

#include <string.h>

#define DIRECTORY_ENTRY_SIZE (sizeof(inode_index_t) + MAX_FILE_NAME_LEN)
#define DIRECTORY_ENTRIES_PER_DATABLOCK (DATA_BLOCK_SIZE / DIRECTORY_ENTRY_SIZE)

// ----------------------- CORE FUNCTION ----------------------- //
int new_file(terminal_context_t *context, char *path, permission_t perms)
{
    (void) context;
    (void) path;
    (void) perms;
    return -2;
}

int new_directory(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

int remove_file(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

// we can only delete a directory if it is empty!!
int remove_directory(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

int change_directory(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

int list(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

char *get_path_string(terminal_context_t *context)
{
    (void) context;

    return NULL;
}

int tree(terminal_context_t *context, char *path)
{
    (void) context;
    (void) path;
    return -2;
}

//Part 2
void new_terminal(filesystem_t *fs, terminal_context_t *term)
{
    if (!fs || !term) return;
    term->fs = fs;
    term->working_directory = &fs->inodes[0];
}

fs_file_t fs_open(terminal_context_t *context, char *path)
{
    if (!context || !path) return NULL;
    filesystem_t *fs = context->fs;
    inode_t *cwd = context->working_directory;

    char *pcopy = strdup(path);
    if (!pcopy) return NULL;

    char *token = strtok(pcopy, "/");
    inode_t *dir = cwd;
    char *next;
    while (token) {
        next = strtok(NULL, "/");
        if (!next) break;
        size_t dir_size = dir->internal.file_size;
        size_t entries = dir_size / DIRECTORY_ENTRY_SIZE;
        int found = 0;
        for (size_t i = 0; i < entries; i++) {
            size_t offset = i * DIRECTORY_ENTRY_SIZE;
            byte buf[DIRECTORY_ENTRY_SIZE];
            size_t br;
            inode_read_data(fs, dir, offset, buf, DIRECTORY_ENTRY_SIZE, &br);
            inode_index_t idx;
            memcpy(&idx, buf, sizeof(idx));
            char name[MAX_FILE_NAME_LEN+1];
            memcpy(name, buf + sizeof(idx), MAX_FILE_NAME_LEN);
            name[MAX_FILE_NAME_LEN] = '\0';
            if (strcmp(name, token)==0) {
                if (fs->inodes[idx].internal.file_type != DIRECTORY) {
                    REPORT_RETCODE(DIR_NOT_FOUND);
                    free(pcopy);
                    return NULL;
                }
                dir = &fs->inodes[idx];
                found = 1;
                break;
            }
        }
        if (!found) {
            REPORT_RETCODE(DIR_NOT_FOUND);
            free(pcopy);
            return NULL;
        }
        token = next;
    }

    char *basename = token;
    size_t dir_size = dir->internal.file_size;
    size_t entries = dir_size / DIRECTORY_ENTRY_SIZE;
    inode_t *file_inode = NULL;
    for (size_t i = 0; i < entries; i++) {
        size_t offset = i * DIRECTORY_ENTRY_SIZE;
        byte buf[DIRECTORY_ENTRY_SIZE];
        size_t br;
        inode_read_data(fs, dir, offset, buf, DIRECTORY_ENTRY_SIZE, &br);
        inode_index_t idx;
        memcpy(&idx, buf, sizeof(idx));
        char name[MAX_FILE_NAME_LEN+1];
        memcpy(name, buf + sizeof(idx), MAX_FILE_NAME_LEN);
        name[MAX_FILE_NAME_LEN] = '\0';
        if (strcmp(name, basename)==0) {
            file_inode = &fs->inodes[idx];
            break;
        }
    }
    if (!file_inode) {
        REPORT_RETCODE(FILE_NOT_FOUND);
        free(pcopy);
        return NULL;
    }
    if (file_inode->internal.file_type != DATA_FILE) {
        REPORT_RETCODE(INVALID_FILE_TYPE);
        free(pcopy);
        return NULL;
    }

    fs_file_t f = malloc(sizeof(*f));
    if (!f) {
        free(pcopy);
        return NULL;
    }
    f->fs = fs;
    f->inode = file_inode;
    f->offset = 0;
    free(pcopy);
    return f;
}

void fs_close(fs_file_t file)
{
    if (file) free(file);
}

size_t fs_read(fs_file_t file, void *buffer, size_t n)
{
    if (!file || !buffer) return 0;
    size_t bytes_read = 0;
    fs_retcode_t ret = inode_read_data(file->fs, file->inode, file->offset, buffer, n, &bytes_read);
    if (ret != SUCCESS) REPORT_RETCODE(ret);
    file->offset += bytes_read;
    return bytes_read;
}

size_t fs_write(fs_file_t file, void *buffer, size_t n)
{
    if (!file || !buffer) return 0;
    size_t written = 0;
    size_t sz = file->inode->internal.file_size;
    size_t off = file->offset;

    if (off < sz) {
        size_t to_over = (off + n <= sz) ? n : (sz - off);
        fs_retcode_t ret = inode_modify_data(file->fs, file->inode, off, buffer, to_over);
        if (ret != SUCCESS) {
            REPORT_RETCODE(ret);
            return 0;
        }
        written += to_over;
    }

    if (off + n > sz) {
        size_t to_app = off + n - sz;
        fs_retcode_t ret = inode_write_data(file->fs, file->inode, (byte*)buffer + written, to_app);
        if (ret != SUCCESS) {
            REPORT_RETCODE(ret);
            return written;
        }
        written += to_app;
    }

    file->offset += n;
    return n;
}

int fs_seek(fs_file_t file, seek_mode_t seek_mode, int offset)
{
    if (!file) return -1;
    size_t sz = file->inode->internal.file_size;
    long newoff = 0;
    if (seek_mode == FS_SEEK_START) {
        newoff = offset;
    } else if (seek_mode == FS_SEEK_CURRENT) {
        newoff = (long)file->offset + offset;
    } else if (seek_mode == FS_SEEK_END) {
        newoff = (long)sz + offset;
    } else {
        return -1;
    }
    if (newoff < 0) return -1;
    if ((size_t)newoff > sz) newoff = sz;
    file->offset = (size_t)newoff;
    return 0;
}