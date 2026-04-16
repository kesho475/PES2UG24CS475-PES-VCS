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
int index_load(Index *index) {
// Start with an empty count
    index->count = 0;
    
    // Try to open the file. If it doesn't exist, just return success (empty index).
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; 

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    // Read the text file line by line: <mode> <hash> <mtime> <size> <path>
    while (fscanf(f, "%o %64s %llu %u %511[^\n]", &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        
        IndexEntry *entry = &index->entries[index->count];
        entry->mode = mode;
        hex_to_hash(hex, &entry->hash); // Convert the text hex string back to binary
        entry->mtime_sec = (uint64_t)mtime;
        entry->size = (uint32_t)size;
        strcpy(entry->path, path);
        
        index->count++;
    }
    
    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
// Helper for qsort to sort index entries by path
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // 1. Create a mutable copy of the index so we can sort it
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);

    // 2. Set up the temporary file path
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);

    // 3. Open the temp file for writing
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;

    // 4. Write each entry to the file: <mode> <hash> <mtime> <size> <path>
    for (int i = 0; i < sorted_index.count; i++) {
        const IndexEntry *entry = &sorted_index.entries[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&entry->hash, hex);

        fprintf(f, "%o %s %llu %u %s\n",
                entry->mode,
                hex,
                (unsigned long long)entry->mtime_sec,
                entry->size,
                entry->path);
    }

    // 5. Ensure everything is flushed from RAM and safely on the hard drive
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // 6. Atomically replace the old index with the new one
    if (rename(temp_path, INDEX_FILE) != 0) return -1;

    return 0;
}
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
// Stage a file: read its contents, write as a blob, update/add index entry.
int index_add(Index *index, const char *path) {
    // 1. Get file metadata (size, permissions)
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    
    // Only allow regular files to be staged
    if (!S_ISREG(st.st_mode)) return -1;

    // 2. Read the file contents into memory
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t *data = malloc(st.st_size);
    if (!data) { fclose(f); return -1; }

    if (st.st_size > 0 && fread(data, 1, st.st_size, f) != (size_t)st.st_size) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    // 3. Save the contents to the object store as a BLOB
    ObjectID hash;
    if (object_write(OBJ_BLOB, data, st.st_size, &hash) != 0) {
        free(data); return -1;
    }
    free(data);

    // --- PAUSE HERE FOR COMMIT 4 ---
    return -1;
}