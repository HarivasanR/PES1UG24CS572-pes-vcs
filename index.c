// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
// ─── HELPER FUNCTIONS (Add these to fix linker errors) ──────────────────────

static void* read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(*size);
    if (data) {
        if (fread(data, 1, *size, f) != *size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

static uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0100644;
    return st.st_mode;
}

// Declaration for the function in object.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; 

    index->count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count++];
        char hex[HASH_HEX_SIZE + 1];
        
        // Use %u for uint32_t fields (mode and )
        if (sscanf(line, "%o %64s %ld %u %511s", 
                   &e->mode, hex, &e->mtime_sec, &e->size, e->path) >= 3) {
            hex_to_hash(hex, &e->hash);
        }
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    char temp_path[512];
    sprintf(temp_path, "%s.tmp", INDEX_FILE);
    
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&index->entries[i].hash, hex);
        // Use %u for uint32_t fields (mode and size)
        fprintf(f, "%o %s %ld %u %s\n", 
                index->entries[i].mode, hex, 
                index->entries[i].mtime_sec, index->entries[i].size, 
                index->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(temp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    size_t size;
    void *data = read_file(path, &size); 
    if (!data) return -1;

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    struct stat st;
    stat(path, &st);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    
    memcpy(&e->hash, &id, sizeof(ObjectID));
    e->mode = st.st_mode;
    e->size = st.st_size;
    e->mtime_sec = st.st_mtime;
    
    return index_save(index);
}