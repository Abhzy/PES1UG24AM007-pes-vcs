// tree.c — Tree object serialization and construction

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Forward declaration for object_write
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Recursive helper: build a tree for entries that share the given prefix.
// prefix="" means root level. Returns 0 on success.
static int write_tree_recursive(IndexEntry *entries, int count,
                                 const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;

        // Strip the prefix from the path
        const char *rel = path;
        if (prefix[0] != '\0') {
            size_t plen = strlen(prefix);
            if (strncmp(path, prefix, plen) != 0 || path[plen] != '/') {
                i++;
                continue;
            }
            rel = path + plen + 1;
        }

        // Check if this entry is directly at this level or in a subdirectory
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // Direct file at this level
            TreeEntry *te = &tree.entries[tree.count];
            te->mode = entries[i].mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = entries[i].hash;
            tree.count++;
            i++;
        } else {
            // It's in a subdirectory — collect all entries with this subdir prefix
            char subdir_name[512];
            size_t dir_len = slash - rel;
            strncpy(subdir_name, rel, dir_len);
            subdir_name[dir_len] = '\0';

            // Build the full prefix for the subdir
            char full_prefix[512];
            if (prefix[0] != '\0') {
                snprintf(full_prefix, sizeof(full_prefix), "%s/%s", prefix, subdir_name);
            } else {
                strncpy(full_prefix, subdir_name, sizeof(full_prefix) - 1);
                full_prefix[sizeof(full_prefix) - 1] = '\0';
            }

            // Recursively build the subtree
            ObjectID sub_id;
            if (write_tree_recursive(entries, count, full_prefix, &sub_id) != 0)
                return -1;

            // Add as a tree entry
            TreeEntry *te = &tree.entries[tree.count];
            te->mode = MODE_DIR;
            strncpy(te->name, subdir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = sub_id;
            tree.count++;

            // Skip all entries that belong to this subdir
            while (i < count) {
                const char *p = entries[i].path;
                const char *r = p;
                if (prefix[0] != '\0') {
                    size_t plen = strlen(prefix);
                    if (strncmp(p, prefix, plen) == 0 && p[plen] == '/')
                        r = p + plen + 1;
                    else { i++; continue; }
                }
                if (strncmp(r, subdir_name, dir_len) == 0 && r[dir_len] == '/')
                    i++;
                else
                    break;
            }
        }
    }

    // Serialize and store this tree
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) return -1;
    if (idx.count == 0) return -1;
    return write_tree_recursive(idx.entries, idx.count, "", id_out);
}
