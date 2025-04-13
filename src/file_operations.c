#include "filesys.h"
#include "debug.h"
#include "utility.h"

#include <string.h>

#define DIRECTORY_ENTRY_SIZE (sizeof(inode_index_t) + MAX_FILE_NAME_LEN)
#define DIRECTORY_ENTRIES_PER_DATABLOCK (DATA_BLOCK_SIZE / DIRECTORY_ENTRY_SIZE)
//Helpers//
static int resolve_parent(terminal_context_t *context, const char *path,
                          inode_t **parent, char *base_name_out) {
    char *dup = strdup(path);
    if (!dup)
        return -1;
    char *last_slash = strrchr(dup, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (strlen(dup) == 0) {
            *parent = context->working_directory;
        } else {
            inode_t *curr = context->working_directory;
            char *token = strtok(dup, "/");
            while (token) {
                size_t entries = curr->internal.file_size / DIRECTORY_ENTRY_SIZE;
                int found = 0;
                for (size_t i = 0; i < entries; i++) {
                    size_t offset = i * DIRECTORY_ENTRY_SIZE;
                    byte buf[DIRECTORY_ENTRY_SIZE];
                    size_t br;
                    if (inode_read_data(context->fs, curr, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
                        continue;
                    inode_index_t idx;
                    memcpy(&idx, buf, sizeof(inode_index_t));
                    if (idx == 0)
                        continue;
                    char entry_name[MAX_FILE_NAME_LEN + 1] = {0};
                    memcpy(entry_name, buf + sizeof(inode_index_t), MAX_FILE_NAME_LEN);
                    if (strcmp(token, entry_name) == 0) {
                        inode_t *child = &context->fs->inodes[idx];
                        if (child->internal.file_type != DIRECTORY) {
                            free(dup);
                            return -1;
                        }
                        curr = child;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    free(dup);
                    return -1;
                }
                token = strtok(NULL, "/");
            }
            *parent = curr;
        }
        strncpy(base_name_out, last_slash + 1, MAX_FILE_NAME_LEN);
        base_name_out[MAX_FILE_NAME_LEN] = '\0';
    } else {
        *parent = context->working_directory;
        strncpy(base_name_out, dup, MAX_FILE_NAME_LEN);
        base_name_out[MAX_FILE_NAME_LEN] = '\0';
    }
    free(dup);
    return 0;
}

static int resolve_path(terminal_context_t *context, const char *path, inode_t **result) {
    char *dup = strdup(path);
    if (!dup)
        return -1;
    inode_t *curr = context->working_directory;
    char *token = strtok(dup, "/");
    while (token) {
        size_t entries = curr->internal.file_size / DIRECTORY_ENTRY_SIZE;
        int found = 0;
        for (size_t i = 0; i < entries; i++) {
            size_t offset = i * DIRECTORY_ENTRY_SIZE;
            byte buf[DIRECTORY_ENTRY_SIZE];
            size_t br;
            if (inode_read_data(context->fs, curr, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
                continue;
            inode_index_t idx;
            memcpy(&idx, buf, sizeof(inode_index_t));
            if (idx == 0)
                continue;
            char entry_name[MAX_FILE_NAME_LEN + 1];
            memcpy(entry_name, buf + sizeof(inode_index_t), MAX_FILE_NAME_LEN);
            entry_name[MAX_FILE_NAME_LEN] = '\0';
            if (strcmp(token, entry_name) == 0) {
                curr = &context->fs->inodes[idx];
                found = 1;
                break;
            }
        }
        if (!found) {
            free(dup);
            return -1;
        }
        token = strtok(NULL, "/");
    }
    free(dup);
    *result = curr;
    return 0;
}

static int find_directory_entry(filesystem_t *fs, inode_t *dir, const char *name, size_t *entry_offset, inode_index_t *child_idx) {
    size_t entries = dir->internal.file_size / DIRECTORY_ENTRY_SIZE;
    for (size_t i = 0; i < entries; i++) {
        size_t offset = i * DIRECTORY_ENTRY_SIZE;
        byte buf[DIRECTORY_ENTRY_SIZE];
        size_t br;
        if (inode_read_data(fs, dir, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
            continue;
        inode_index_t idx;
        memcpy(&idx, buf, sizeof(inode_index_t));
        if (idx == 0)
            continue;
        char entry_name[MAX_FILE_NAME_LEN + 1];
        memcpy(entry_name, buf + sizeof(inode_index_t), MAX_FILE_NAME_LEN);
        entry_name[MAX_FILE_NAME_LEN] = '\0';
        if (strcmp(entry_name, name) == 0) {
            if (entry_offset)
                *entry_offset = offset;
            if (child_idx)
                *child_idx = idx;
            return 0;
        }
    }
    return -1;
}

static int add_directory_entry(filesystem_t *fs, inode_t *parent, inode_index_t child_idx, const char *name) {
    byte new_entry[DIRECTORY_ENTRY_SIZE];
    memset(new_entry, 0, DIRECTORY_ENTRY_SIZE);
    memcpy(new_entry, &child_idx, sizeof(inode_index_t));
    strncpy((char*)(new_entry + sizeof(inode_index_t)), name, MAX_FILE_NAME_LEN);
    size_t entries = parent->internal.file_size / DIRECTORY_ENTRY_SIZE;
    for (size_t i = 0; i < entries; i++) {
        size_t offset = i * DIRECTORY_ENTRY_SIZE;
        byte buf[DIRECTORY_ENTRY_SIZE];
        size_t br;
        if (inode_read_data(fs, parent, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
            continue;
        inode_index_t idx;
        memcpy(&idx, buf, sizeof(inode_index_t));
        if (idx == 0) {
            if (inode_modify_data(fs, parent, offset, new_entry, DIRECTORY_ENTRY_SIZE) != SUCCESS)
                return -1;
            return 0;
        }
    }
    if (inode_write_data(fs, parent, new_entry, DIRECTORY_ENTRY_SIZE) != SUCCESS)
        return -1;
    return 0;
}

static int remove_directory_entry(filesystem_t *fs, inode_t *parent, const char *name) {
    size_t offset;
    inode_index_t child_idx;
    if (find_directory_entry(fs, parent, name, &offset, &child_idx) != 0)
        return -1;
    byte tomb[DIRECTORY_ENTRY_SIZE];
    memset(tomb, 0, DIRECTORY_ENTRY_SIZE);
    if (inode_modify_data(fs, parent, offset, tomb, DIRECTORY_ENTRY_SIZE) != SUCCESS)
        return -1;
    while (parent->internal.file_size >= DIRECTORY_ENTRY_SIZE) {
        size_t last_offset = parent->internal.file_size - DIRECTORY_ENTRY_SIZE;
        byte buf[DIRECTORY_ENTRY_SIZE];
        size_t br;
        if (inode_read_data(fs, parent, last_offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
            break;
        int is_tomb = 1;
        for (size_t i = 0; i < DIRECTORY_ENTRY_SIZE; i++) {
            if (buf[i] != 0) {
                is_tomb = 0;
                break;
            }
        }
        if (is_tomb)
            inode_shrink_data(fs, parent, last_offset);
        else
            break;
    }
    return 0;
}

static void tree_helper(filesystem_t *fs, inode_t *node, int depth) {
    for (int i = 0; i < depth; i++)
        printf("   ");
    printf("%s\n", node->internal.file_name);
    if (node->internal.file_type == DIRECTORY) {
        size_t entries = node->internal.file_size / DIRECTORY_ENTRY_SIZE;
        for (size_t i = 0; i < entries; i++) {
            size_t offset = i * DIRECTORY_ENTRY_SIZE;
            byte buf[DIRECTORY_ENTRY_SIZE];
            size_t br;
            if (inode_read_data(fs, node, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
                continue;
            inode_index_t idx;
            memcpy(&idx, buf, sizeof(inode_index_t));
            if (idx == 0)
                continue;
            char entry_name[MAX_FILE_NAME_LEN + 1];
            memcpy(entry_name, buf + sizeof(inode_index_t), MAX_FILE_NAME_LEN);
            entry_name[MAX_FILE_NAME_LEN] = '\0';
            if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
                continue;
            inode_t *child = &fs->inodes[idx];
            tree_helper(fs, child, depth + 1);
        }
    }
}
// ----------------------- CORE FUNCTION ----------------------- //
int new_file(terminal_context_t *context, char *path, permission_t perms) {
    if (!context || !path)
        return 0;
    filesystem_t *fs = context->fs;
    inode_t *parent;
    char base_name[MAX_FILE_NAME_LEN + 1];
    if (resolve_parent(context, path, &parent, base_name) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    size_t dummy;
    inode_index_t exist;
    if (find_directory_entry(fs, parent, base_name, &dummy, &exist) == 0) {
        REPORT_RETCODE(FILE_EXIST);
        return -1;
    }
    inode_index_t new_idx;
    if (claim_available_inode(fs, &new_idx) != SUCCESS) {
        REPORT_RETCODE(INODE_UNAVAILABLE);
        return -1;
    }
    inode_t *new_inode = &fs->inodes[new_idx];
    new_inode->internal.file_type = DATA_FILE;
    new_inode->internal.file_perms = perms;
    new_inode->internal.file_size = 0;
    strncpy(new_inode->internal.file_name, base_name, MAX_FILE_NAME_LEN);
    if (strlen(base_name) < MAX_FILE_NAME_LEN)
        new_inode->internal.file_name[strlen(base_name)] = '\0';
    if (add_directory_entry(fs, parent, new_idx, base_name) != 0) {
        release_inode(fs, new_inode);
        return -1;
    }
    return 0;
}

int new_directory(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    filesystem_t *fs = context->fs;
    inode_t *parent;
    char base_name[MAX_FILE_NAME_LEN + 1];
    if (resolve_parent(context, path, &parent, base_name) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    size_t dummy;
    inode_index_t exist;
    if (find_directory_entry(fs, parent, base_name, &dummy, &exist) == 0) {
        REPORT_RETCODE(DIRECTORY_EXIST);
        return -1;
    }
    inode_index_t new_idx;
    if (claim_available_inode(fs, &new_idx) != SUCCESS) {
        REPORT_RETCODE(INODE_UNAVAILABLE);
        return -1;
    }
    inode_t *new_inode = &fs->inodes[new_idx];
    new_inode->internal.file_type = DIRECTORY;
    new_inode->internal.file_perms = 0;
    strncpy(new_inode->internal.file_name, base_name, MAX_FILE_NAME_LEN);
    if (strlen(base_name) < MAX_FILE_NAME_LEN)
        new_inode->internal.file_name[strlen(base_name)] = '\0';
    dblock_index_t dblock;
    if (claim_available_dblock(fs, &dblock) != SUCCESS) {
        release_inode(fs, new_inode);
        REPORT_RETCODE(INSUFFICIENT_DBLOCKS);
        return -1;
    }
    new_inode->internal.direct_data[0] = dblock;
    byte entry[DIRECTORY_ENTRY_SIZE];
    memset(entry, 0, DIRECTORY_ENTRY_SIZE);
    memcpy(entry, &new_idx, sizeof(inode_index_t));
    strncpy((char*)(entry + sizeof(inode_index_t)), ".", MAX_FILE_NAME_LEN);
    if (inode_write_data(fs, new_inode, entry, DIRECTORY_ENTRY_SIZE) != SUCCESS) {
        release_dblock(fs, fs->dblocks + dblock * DATA_BLOCK_SIZE);
        release_inode(fs, new_inode);
        return -1;
    }
    memset(entry, 0, DIRECTORY_ENTRY_SIZE);
    inode_index_t parent_idx = parent - fs->inodes;
    memcpy(entry, &parent_idx, sizeof(inode_index_t));
    strncpy((char*)(entry + sizeof(inode_index_t)), "..", MAX_FILE_NAME_LEN);
    if (inode_write_data(fs, new_inode, entry, DIRECTORY_ENTRY_SIZE) != SUCCESS) {
        inode_shrink_data(fs, new_inode, 0);
        release_dblock(fs, fs->dblocks + dblock * DATA_BLOCK_SIZE);
        release_inode(fs, new_inode);
        return -1;
    }
    if (add_directory_entry(fs, parent, new_idx, base_name) != 0) {
        inode_release_data(fs, new_inode);
        release_inode(fs, new_inode);
        return -1;
    }
    return 0;
}

int remove_file(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    filesystem_t *fs = context->fs;
    inode_t *parent;
    char base_name[MAX_FILE_NAME_LEN + 1];
    if (resolve_parent(context, path, &parent, base_name) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    size_t offset;
    inode_index_t child_idx;
    if (find_directory_entry(fs, parent, base_name, &offset, &child_idx) != 0) {
        REPORT_RETCODE(FILE_NOT_FOUND);
        return -1;
    }
    inode_t *child = &fs->inodes[child_idx];
    if (child->internal.file_type != DATA_FILE) {
        REPORT_RETCODE(FILE_NOT_FOUND);
        return -1;
    }
    if (remove_directory_entry(fs, parent, base_name) != 0)
        return -1;
    if (inode_release_data(fs, child) != SUCCESS)
        return -1;
    if (release_inode(fs, child) != SUCCESS)
        return -1;
    return 0;
}

// we can only delete a directory if it is empty!!
int remove_directory(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    filesystem_t *fs = context->fs;
    inode_t *parent;
    char base_name[MAX_FILE_NAME_LEN + 1];
    if (resolve_parent(context, path, &parent, base_name) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    if (strcmp(base_name, ".") == 0 || strcmp(base_name, "..") == 0) {
        REPORT_RETCODE(INVALID_FILENAME);
        return -1;
    }
    size_t offset;
    inode_index_t child_idx;
    if (find_directory_entry(fs, parent, base_name, &offset, &child_idx) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    inode_t *child = &fs->inodes[child_idx];
    if (child->internal.file_type != DIRECTORY) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    if (child->internal.file_size > 2 * DIRECTORY_ENTRY_SIZE) {
        REPORT_RETCODE(DIR_NOT_EMPTY);
        return -1;
    }
    if (child == context->working_directory) {
        REPORT_RETCODE(ATTEMPT_DELETE_CWD);
        return -1;
    }
    if (remove_directory_entry(fs, parent, base_name) != 0)
        return -1;
    if (inode_release_data(fs, child) != SUCCESS)
        return -1;
    if (release_inode(fs, child) != SUCCESS)
        return -1;
    return 0;
}

int change_directory(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    inode_t *target;
    if (resolve_path(context, path, &target) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    if (target->internal.file_type != DIRECTORY) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    context->working_directory = target;
    return 0;
}

int list(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    filesystem_t *fs = context->fs;
    inode_t *target;
    if (resolve_path(context, path, &target) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    if (target->internal.file_type == DATA_FILE) {
        char perm[4] = {
            (target->internal.file_perms & FS_READ) ? 'r' : '-',
            (target->internal.file_perms & FS_WRITE) ? 'w' : '-',
            (target->internal.file_perms & FS_EXECUTE) ? 'x' : '-',
            '\0'
        };
        printf("f%s\t%lu\t%s\n", perm, (unsigned long) target->internal.file_size, target->internal.file_name);
    } else if (target->internal.file_type == DIRECTORY) {
        size_t entries = target->internal.file_size / DIRECTORY_ENTRY_SIZE;
        for (size_t i = 0; i < entries; i++) {
            size_t offset = i * DIRECTORY_ENTRY_SIZE;
            byte buf[DIRECTORY_ENTRY_SIZE];
            size_t br;
            if (inode_read_data(fs, target, offset, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
                continue;
            inode_index_t idx;
            memcpy(&idx, buf, sizeof(inode_index_t));
            if (idx == 0)
                continue;
            char entry_name[MAX_FILE_NAME_LEN + 1];
            memcpy(entry_name, buf + sizeof(inode_index_t), MAX_FILE_NAME_LEN);
            entry_name[MAX_FILE_NAME_LEN] = '\0';
            inode_t *child = &fs->inodes[idx];
            char type = (child->internal.file_type == DIRECTORY) ? 'd' :
                        (child->internal.file_type == DATA_FILE) ? 'f' : 'E';
            char perm[4] = {
                (child->internal.file_perms & FS_READ) ? 'r' : '-',
                (child->internal.file_perms & FS_WRITE) ? 'w' : '-',
                (child->internal.file_perms & FS_EXECUTE) ? 'x' : '-',
                '\0'
            };
            if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0) {
                printf("%c%s\t%lu\t%s -> %s\n", type, perm, (unsigned long) child->internal.file_size, entry_name, child->internal.file_name);
            } else {
                printf("%c%s\t%lu\t%s\n", type, perm, (unsigned long) child->internal.file_size, entry_name);
            }
        }
    } else {
        return -1;
    }
    return 0;
}

char *get_path_string(terminal_context_t *context) {
    if (!context)
        return strdup("");
    filesystem_t *fs = context->fs;
    char *names[256];
    int count = 0;
    inode_t *curr = context->working_directory;
    while (curr != &fs->inodes[0] && count < 256) {
        names[count++] = strdup(curr->internal.file_name);
        byte buf[DIRECTORY_ENTRY_SIZE];
        size_t br;
        if (inode_read_data(fs, curr, DIRECTORY_ENTRY_SIZE, buf, DIRECTORY_ENTRY_SIZE, &br) != SUCCESS)
            break;
        inode_index_t parent_idx;
        memcpy(&parent_idx, buf, sizeof(inode_index_t));
        curr = &fs->inodes[parent_idx];
    }
    names[count++] = strdup(fs->inodes[0].internal.file_name);
    size_t total_length = 0;
    for (int i = 0; i < count; i++) {
        total_length += strlen(names[i]) + 1;
    }
    char *path_str = malloc(total_length + 1);
    if (!path_str) {
        for (int i = 0; i < count; i++)
            free(names[i]);
        return strdup("");
    }
    path_str[0] = '\0';
    for (int i = count - 1; i >= 0; i--) {
        strcat(path_str, names[i]);
        if (i > 0)
            strcat(path_str, "/");
        free(names[i]);
    }
    return path_str;
}


int tree(terminal_context_t *context, char *path) {
    if (!context || !path)
        return 0;
    inode_t *target;
    if (resolve_path(context, path, &target) != 0) {
        REPORT_RETCODE(DIR_NOT_FOUND);
        return -1;
    }
    tree_helper(context->fs, target, 0);
    return 0;
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

int fs_seek(fs_file_t file, seek_mode_t mode, int offset) {
    if (file == NULL)
        return -1;

    int new_offset;
    switch (mode) {
        case FS_SEEK_START:
            new_offset = offset;
            break;
        case FS_SEEK_CURRENT:
            new_offset = (int)file->offset + offset;
            break;
        case FS_SEEK_END:
            new_offset = (int)file->inode->internal.file_size + offset;
            break;
        default:
            return -1;
    }

    if (new_offset < 0)
        return -1;

    if ((size_t)new_offset > file->inode->internal.file_size)
        new_offset = file->inode->internal.file_size;

    file->offset = (size_t)new_offset;
    return 0;
}
