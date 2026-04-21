// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Determine type string
    const char *type_str;
    if      (type == OBJ_BLOB)   type_str = "blob";
    else if (type == OBJ_TREE)   type_str = "tree";
    else                         type_str = "commit";

    // 2. Build header: "blob 16\0"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // 3. Build full object = header + data
    size_t total = (size_t)header_len + len;
    uint8_t *obj = malloc(total);
    if (!obj) return -1;
    memcpy(obj, header, header_len);
    memcpy(obj + header_len, data, len);

    // 4. Hash the full object
    compute_hash(obj, total, id_out);

    // 5. Deduplication: already stored, nothing to do
    if (object_exists(id_out)) {
        free(obj);
        return 0;
    }

    // 6. Build paths
    char path[512], dir[512], tmp[520];
    object_path(id_out, path, sizeof(path));

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (!last_slash) { free(obj); return -1; }
    *last_slash = '\0';

    // 7. Create shard directory
    mkdir(dir, 0755);

    // 8. Write to temp file with correct permissions
    mode_t old_mask = umask(0);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    umask(old_mask);

    if (fd < 0) { free(obj); return -1; }
    if (write(fd, obj, total) != (ssize_t)total) {
        close(fd); free(obj); return -1;
    }
    fsync(fd);
    close(fd);

    // 9. Atomic rename + fix permissions
    rename(tmp, path);
    chmod(path, 0644);

    // 10. fsync the shard directory
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    free(obj);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Get the file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read entire file into memory
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size <= 0) { fclose(f); return -1; }
    rewind(f);

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    size_t nread = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    if (nread != (size_t)file_size) {
        free(buf); return -1;
    }

    // 3. Integrity check: rehash and compare
    ObjectID check;
    compute_hash(buf, (size_t)file_size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // 4. Find the '\0' separating header from data
    uint8_t *null_pos = memchr(buf, '\0', (size_t)file_size);
    if (!null_pos) { free(buf); return -1; }

    // 5. Parse object type
    if      (strncmp((char*)buf, "blob",   4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree",   4) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // 6. Copy out the data portion (after the '\0')
    uint8_t *data_start = null_pos + 1;
    *len_out = (size_t)(file_size - (data_start - buf));
    *data_out = malloc(*len_out);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, *len_out);

    free(buf);
    return 0;
}
