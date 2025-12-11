// Build: gcc -O2 -pthread -o c_engine c_engine.c -lcrypto
// Run:   ./c_engine /tmp/cengine.sock

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>

// OpenSSL for SHA256
#include <openssl/sha.h>
#include <openssl/evp.h>

// Operation codes
#define OP_UPLOAD_START  0x01
#define OP_UPLOAD_CHUNK  0x02
#define OP_UPLOAD_FINISH 0x03
#define OP_UPLOAD_DONE   0x81

#define OP_DOWNLOAD_START 0x11
#define OP_DOWNLOAD_CHUNK 0x91
#define OP_DOWNLOAD_DONE  0x92

#define OP_ERROR         0xFF

// Error codes
#define E_BAD_CID       1    // Invalid CID format
#define E_NOT_FOUND     2    // CID/resource not found
#define E_HASH_MISMATCH 3    // Chunk hash verification failed
#define E_BUSY          4    // Server busy/resource allocation failed
#define E_PROTO         5    // Protocol violation (wrong opcode, wrong sequence)

// Constants
#define CHUNK_SIZE (256 * 1024)  // 256KB
#define MAX_FILENAME 256
#define MAX_HASH_LEN 128
#define THREAD_POOL_SIZE 8

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// Chunk metadata
typedef struct {
    uint32_t index;
    uint32_t size;
    char hash[MAX_HASH_LEN];
} chunk_info_t;

// Upload session (per connection)
typedef struct {
    char filename[MAX_FILENAME];
    uint64_t total_size;
    uint32_t chunk_size;
    chunk_info_t* chunks;
    size_t chunk_count;
    size_t chunk_capacity;
    pthread_mutex_t lock;
} upload_session_t;

// Download session
typedef struct {
    char cid[256];
    char filename[MAX_FILENAME];
    size_t total_chunks;
    chunk_info_t* chunks;

    // For sequential streaming
    uint8_t** buffers;      // Array of chunk buffers
    size_t* sizes;          // Size of each chunk
    int* ready;             // Which chunks are ready
    size_t next_index;      // Next chunk to send

    pthread_mutex_t lock;
    pthread_cond_t cond;
} download_session_t;

// Block reference entry
typedef struct {
    char hash[MAX_HASH_LEN];
    int refcount;
} block_entry_t;

// Global block table
typedef struct {
    block_entry_t* entries;
    size_t count;
    size_t capacity;
    pthread_rwlock_t lock;
} block_table_t;

// Work item types
typedef enum {
    WORK_HASH_AND_SAVE,
    WORK_LOAD_AND_VERIFY
} work_type_t;

// Work item for thread pool
typedef struct {
    work_type_t type;
    void* session;           // upload_session_t* or download_session_t*
    uint32_t index;
    uint8_t* data;
    size_t size;
    char expected_hash[MAX_HASH_LEN];
} work_item_t;

// Thread pool
typedef struct {
    pthread_t* threads;
    size_t num_threads;

    work_item_t* queue;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_size;
    size_t queue_capacity;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_cond_t empty_cond;

    int shutdown;
    int active_tasks;
} thread_pool_t;

// Global instances
static thread_pool_t* g_thread_pool = NULL;
static block_table_t g_block_table = {0};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

// Block table functions
void block_table_init();
int block_table_find(const char* hash);
void block_table_add_or_inc(const char* hash);
int block_table_exists(const char* hash);

// Block storage functions
char* get_block_path(const char* multihash);
int save_block(const char* multihash, const uint8_t* data, size_t len);
uint8_t* load_block(const char* multihash, size_t* out_len);

// Thread pool functions
thread_pool_t* thread_pool_create(size_t num_threads);
void thread_pool_submit(thread_pool_t* pool, work_item_t work);
void thread_pool_wait(thread_pool_t* pool);
void thread_pool_destroy(thread_pool_t* pool);
void* worker_thread(void* arg);

// Upload session functions
upload_session_t* upload_session_create(const char* filename);
void upload_session_destroy(upload_session_t* session);
char* build_manifest(upload_session_t* session);
int save_manifest(const char* cid, const char* json);

// Download session functions
download_session_t* download_session_create(const char* cid);
void download_session_destroy(download_session_t* session);
char* load_manifest(const char* cid);

// Hashing functions
char* compute_hash(const uint8_t* data, size_t len);
char* compute_multihash(const uint8_t* data, size_t len);
char* compute_cid(const char* manifest_json);
char* base32_encode(const uint8_t* data, size_t len);

// Validation functions
int is_valid_cid(const char* cid);

static const char* g_sock_path = NULL;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Create directory recursively
int mkdirs(const char* path) {
    char tmp[512];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

// ============================================================================
// HASHING AND CID FUNCTIONS
// ============================================================================

// Compute SHA256 hash and return as hex string
char* compute_hash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);

    char* hex = malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (!hex) return NULL;

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';

    return hex;
}

// Compute multihash: prefix (0x1220 for sha256) + hash
char* compute_multihash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);

    // Multihash format: 0x12 (sha256) + 0x20 (32 bytes) + hash
    char* mhash = malloc(4 + SHA256_DIGEST_LENGTH * 2 + 1);
    if (!mhash) return NULL;

    sprintf(mhash, "1220");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(mhash + 4 + (i * 2), "%02x", hash[i]);
    }
    mhash[4 + SHA256_DIGEST_LENGTH * 2] = '\0';

    return mhash;
}

// Simple base32 encoding (lowercase)
static const char base32_chars[] = "abcdefghijklmnopqrstuvwxyz234567";

char* base32_encode(const uint8_t* data, size_t len) {
    size_t output_len = ((len * 8 + 4) / 5);
    char* output = malloc(output_len + 1);
    if (!output) return NULL;

    size_t bit_buffer = 0;
    int bits_in_buffer = 0;
    size_t output_pos = 0;

    for (size_t i = 0; i < len; i++) {
        bit_buffer = (bit_buffer << 8) | data[i];
        bits_in_buffer += 8;

        while (bits_in_buffer >= 5) {
            output[output_pos++] = base32_chars[(bit_buffer >> (bits_in_buffer - 5)) & 0x1F];
            bits_in_buffer -= 5;
        }
    }

    if (bits_in_buffer > 0) {
        output[output_pos++] = base32_chars[(bit_buffer << (5 - bits_in_buffer)) & 0x1F];
    }

    output[output_pos] = '\0';
    return output;
}

// Compute CID from manifest JSON
char* compute_cid(const char* manifest_json) {
    // Hash the manifest
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)manifest_json, strlen(manifest_json), hash);

    // Encode in base32
    return base32_encode(hash, SHA256_DIGEST_LENGTH);
}

// ============================================================================
// THREAD POOL
// ============================================================================

void* worker_thread(void* arg);

thread_pool_t* thread_pool_create(size_t num_threads) {
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    pool->queue_capacity = 1024;
    pool->queue = malloc(sizeof(work_item_t) * pool->queue_capacity);
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_size = 0;
    pool->shutdown = 0;
    pool->active_tasks = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->empty_cond, NULL);

    // Create worker threads
    for (size_t i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }

    return pool;
}

void thread_pool_submit(thread_pool_t* pool, work_item_t work) {
    pthread_mutex_lock(&pool->mutex);

    // Wait if queue is full
    while (pool->queue_size >= pool->queue_capacity) {
        pthread_cond_wait(&pool->cond, &pool->mutex);
    }

    pool->queue[pool->queue_tail] = work;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_size++;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_wait(thread_pool_t* pool) {
    pthread_mutex_lock(&pool->mutex);

    while (pool->queue_size > 0 || pool->active_tasks > 0) {
        pthread_cond_wait(&pool->empty_cond, &pool->mutex);
    }

    pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_destroy(thread_pool_t* pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    free(pool->queue);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pthread_cond_destroy(&pool->empty_cond);
    free(pool);
}

void* worker_thread(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        // Wait for work
        while (pool->queue_size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        // Get work item
        work_item_t work = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_size--;
        pool->active_tasks++;

        pthread_mutex_unlock(&pool->mutex);

        // Process work
        if (work.type == WORK_HASH_AND_SAVE) {
            // Hash the chunk
            char* mhash = compute_multihash(work.data, work.size);

            // Check if block already exists
            if (!block_table_exists(mhash)) {
                // Save new block
                save_block(mhash, work.data, work.size);
            }

            // Add to block table (or increment refcount)
            block_table_add_or_inc(mhash);

            // Update session
            upload_session_t* session = (upload_session_t*)work.session;
            pthread_mutex_lock(&session->lock);

            if (work.index < session->chunk_capacity) {
                session->chunks[work.index].index = work.index;
                session->chunks[work.index].size = work.size;
                strncpy(session->chunks[work.index].hash, mhash, MAX_HASH_LEN - 1);
                // DON'T update chunk_count here - it's already set in the main thread
            }

            pthread_mutex_unlock(&session->lock);

            free(mhash);
            free(work.data);
        } else if (work.type == WORK_LOAD_AND_VERIFY) {
            // Load block
            size_t block_size;
            uint8_t* block_data = load_block(work.expected_hash, &block_size);

            if (!block_data) {
                // Block not found - mark as error
                download_session_t* session = (download_session_t*)work.session;
                pthread_mutex_lock(&session->lock);
                session->ready[work.index] = -1; // Error marker
                pthread_cond_broadcast(&session->cond);
                pthread_mutex_unlock(&session->lock);
            } else {
                // Verify hash
                char* computed_hash = compute_multihash(block_data, block_size);
                int hash_match = (strcmp(computed_hash, work.expected_hash) == 0);
                free(computed_hash);

                download_session_t* session = (download_session_t*)work.session;
                pthread_mutex_lock(&session->lock);

                if (hash_match) {
                    // Store verified chunk
                    session->buffers[work.index] = block_data;
                    session->sizes[work.index] = block_size;
                    session->ready[work.index] = 1;
                } else {
                    // Hash mismatch
                    free(block_data);
                    session->ready[work.index] = -1;
                }

                pthread_cond_broadcast(&session->cond);
                pthread_mutex_unlock(&session->lock);
            }
        }

        // Mark task complete
        pthread_mutex_lock(&pool->mutex);
        pool->active_tasks--;
        if (pool->queue_size == 0 && pool->active_tasks == 0) {
            pthread_cond_broadcast(&pool->empty_cond);
        }
        pthread_mutex_unlock(&pool->mutex);
    }

    return NULL;
}

// ============================================================================
// UPLOAD SESSION MANAGEMENT
// ============================================================================

upload_session_t* upload_session_create(const char* filename) {
    upload_session_t* session = malloc(sizeof(upload_session_t));
    if (!session) return NULL;

    strncpy(session->filename, filename, MAX_FILENAME - 1);
    session->filename[MAX_FILENAME - 1] = '\0';
    session->total_size = 0;
    session->chunk_size = CHUNK_SIZE;
    session->chunk_capacity = 256;  // Initial capacity
    session->chunk_count = 0;
    session->chunks = calloc(session->chunk_capacity, sizeof(chunk_info_t));

    pthread_mutex_init(&session->lock, NULL);

    return session;
}

void upload_session_destroy(upload_session_t* session) {
    if (!session) return;
    free(session->chunks);
    pthread_mutex_destroy(&session->lock);
    free(session);
}

// Build manifest JSON from session
char* build_manifest(upload_session_t* session) {
    // Allocate large buffer for JSON
    size_t buf_size = 4096 + session->chunk_count * 256;
    char* json = malloc(buf_size);
    if (!json) return NULL;

    size_t offset = 0;

    // Start JSON
    offset += snprintf(json + offset, buf_size - offset,
                      "{\n"
                      "  \"version\": 1,\n"
                      "  \"hash_algo\": \"sha256\",\n"
                      "  \"chunk_size\": %u,\n"
                      "  \"total_size\": %lu,\n"
                      "  \"filename\": \"%s\",\n"
                      "  \"chunks\": [\n",
                      session->chunk_size,
                      session->total_size,
                      session->filename);

    // Add chunks
    for (size_t i = 0; i < session->chunk_count; i++) {
        offset += snprintf(json + offset, buf_size - offset,
                          "    {\"index\": %u, \"size\": %u, \"hash\": \"%s\"}%s\n",
                          session->chunks[i].index,
                          session->chunks[i].size,
                          session->chunks[i].hash,
                          (i < session->chunk_count - 1) ? "," : "");
    }

    // End JSON
    offset += snprintf(json + offset, buf_size - offset, "  ]\n}\n");

    return json;
}

// Save manifest atomically
// NOTE: This is safe for concurrent calls with DIFFERENT CIDs.
// For the SAME CID, the last writer wins (which is acceptable).
int save_manifest(const char* cid, const char* json) {
    char tmp_path[512];
    char final_path[512];

    snprintf(tmp_path, sizeof(tmp_path), "manifests/%s.json.tmp", cid);
    snprintf(final_path, sizeof(final_path), "manifests/%s.json", cid);

    // Write to temp file
    FILE* f = fopen(tmp_path, "w");
    if (!f) return -1;

    fwrite(json, 1, strlen(json), f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // Atomic rename
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// ============================================================================
// DOWNLOAD SESSION MANAGEMENT
// ============================================================================

// Load manifest from disk
char* load_manifest(const char* cid) {
    char path[512];
    snprintf(path, sizeof(path), "manifests/%s.json", cid);

    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    // Get file size
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = malloc(len + 1);
    if (!json) {
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(json, 1, len, f);
    fclose(f);

    if (read_bytes != len) {
        free(json);
        return NULL;
    }

    json[len] = '\0';
    return json;
}

// Simple JSON parser for manifest
download_session_t* download_session_create(const char* cid) {
    // Load manifest
    char* manifest_json = load_manifest(cid);
    if (!manifest_json) return NULL;

    download_session_t* session = malloc(sizeof(download_session_t));
    if (!session) {
        free(manifest_json);
        return NULL;
    }

    strncpy(session->cid, cid, sizeof(session->cid) - 1);
    session->cid[sizeof(session->cid) - 1] = '\0';

    // Parse JSON manually (simple approach for this project)
    // Extract filename
    char* fn_start = strstr(manifest_json, "\"filename\": \"");
    if (fn_start) {
        fn_start += 13; // skip past "filename": "
        char* fn_end = strchr(fn_start, '"');
        if (fn_end) {
            size_t fn_len = fn_end - fn_start;
            if (fn_len >= MAX_FILENAME) fn_len = MAX_FILENAME - 1;
            memcpy(session->filename, fn_start, fn_len);
            session->filename[fn_len] = '\0';
        }
    }

    // Count chunks
    size_t chunk_count = 0;
    char* p = manifest_json;
    while ((p = strstr(p, "\"index\":")) != NULL) {
        chunk_count++;
        p++;
    }

    session->total_chunks = chunk_count;
    session->chunks = calloc(chunk_count, sizeof(chunk_info_t));
    session->buffers = calloc(chunk_count, sizeof(uint8_t*));
    session->sizes = calloc(chunk_count, sizeof(size_t));
    session->ready = calloc(chunk_count, sizeof(int));
    session->next_index = 0;

    // Parse chunks
    p = manifest_json;
    for (size_t i = 0; i < chunk_count; i++) {
        p = strstr(p, "\"index\":");
        if (!p) break;
        sscanf(p, "\"index\": %u", &session->chunks[i].index);

        p = strstr(p, "\"size\":");
        if (!p) break;
        sscanf(p, "\"size\": %u", &session->chunks[i].size);

        p = strstr(p, "\"hash\": \"");
        if (!p) break;
        p += 9; // skip past "hash": "
        char* hash_end = strchr(p, '"');
        if (hash_end) {
            size_t hash_len = hash_end - p;
            if (hash_len >= MAX_HASH_LEN) hash_len = MAX_HASH_LEN - 1;
            memcpy(session->chunks[i].hash, p, hash_len);
            session->chunks[i].hash[hash_len] = '\0';
        }
    }

    pthread_mutex_init(&session->lock, NULL);
    pthread_cond_init(&session->cond, NULL);

    free(manifest_json);
    return session;
}

void download_session_destroy(download_session_t* session) {
    if (!session) return;

    // Free any remaining buffers
    if (session->buffers) {
        for (size_t i = 0; i < session->total_chunks; i++) {
            free(session->buffers[i]);
        }
        free(session->buffers);
    }

    free(session->chunks);
    free(session->sizes);
    free(session->ready);
    pthread_mutex_destroy(&session->lock);
    pthread_cond_destroy(&session->cond);
    free(session);
}

// ============================================================================
// BLOCK STORAGE FUNCTIONS
// ============================================================================

// Get block file path from multihash
// Example: "1220abcd..." -> "blocks/ab/cd/1220abcd..."
char* get_block_path(const char* multihash) {
    if (strlen(multihash) < 8) return NULL;

    char* path = malloc(512);
    if (!path) return NULL;

    // Extract first 4 hex chars (after "1220" prefix)
    char dir1[3] = {multihash[4], multihash[5], '\0'};
    char dir2[3] = {multihash[6], multihash[7], '\0'};

    snprintf(path, 512, "blocks/%s/%s/%s", dir1, dir2, multihash);
    return path;
}

// Save block to disk
int save_block(const char* multihash, const uint8_t* data, size_t len) {
    char* path = get_block_path(multihash);
    if (!path) return -1;

    // Create parent directories
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char* last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (mkdirs(dir) != 0) {
            free(path);
            return -1;
        }
    }

    // Write file
    FILE* f = fopen(path, "wb");
    if (!f) {
        free(path);
        return -1;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    free(path);

    return (written == len) ? 0 : -1;
}

// Load block from disk
uint8_t* load_block(const char* multihash, size_t* out_len) {
    char* path = get_block_path(multihash);
    if (!path) return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) {
        free(path);
        return NULL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* data = malloc(len);
    if (!data) {
        fclose(f);
        free(path);
        return NULL;
    }

    size_t read_bytes = fread(data, 1, len, f);
    fclose(f);
    free(path);

    if (read_bytes != len) {
        free(data);
        return NULL;
    }

    *out_len = len;
    return data;
}

// ============================================================================
// BLOCK TABLE MANAGEMENT
// ============================================================================

void block_table_init() {
    g_block_table.capacity = 1024;
    g_block_table.count = 0;
    g_block_table.entries = calloc(g_block_table.capacity, sizeof(block_entry_t));
    pthread_rwlock_init(&g_block_table.lock, NULL);
}

// Find block by hash (returns index or -1)
int block_table_find(const char* hash) {
    for (size_t i = 0; i < g_block_table.count; i++) {
        if (strcmp(g_block_table.entries[i].hash, hash) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Add or increment refcount
void block_table_add_or_inc(const char* hash) {
    pthread_rwlock_wrlock(&g_block_table.lock);

    int idx = block_table_find(hash);
    if (idx >= 0) {
        // Block exists, increment refcount
        g_block_table.entries[idx].refcount++;
    } else {
        // New block
        if (g_block_table.count >= g_block_table.capacity) {
            g_block_table.capacity *= 2;
            g_block_table.entries = realloc(g_block_table.entries,
                                           g_block_table.capacity * sizeof(block_entry_t));
        }
        strncpy(g_block_table.entries[g_block_table.count].hash, hash, MAX_HASH_LEN - 1);
        g_block_table.entries[g_block_table.count].refcount = 1;
        g_block_table.count++;
    }

    pthread_rwlock_unlock(&g_block_table.lock);
}

// Check if block exists
int block_table_exists(const char* hash) {
    pthread_rwlock_rdlock(&g_block_table.lock);
    int idx = block_table_find(hash);
    pthread_rwlock_unlock(&g_block_table.lock);
    return idx >= 0;
}

// ============================================================================
// NETWORKING HELPER FUNCTIONS
// ============================================================================

ssize_t read_n(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; perror("read"); return -1; }
        got += r;
    }
    return (ssize_t)got;
}

int write_all(int fd, const void* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char*)buf + sent, n - sent);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); return -1; }
        sent += (size_t)w;
    }
    return 0;
}

int send_frame(int fd, uint8_t op, const void* payload, uint32_t len) {
    uint8_t header[5];
    header[0] = op;
    uint32_t be_len = htonl(len);
    memcpy(header + 1, &be_len, 4);
    if (write_all(fd, header, 5) < 0) return -1;
    if (len && write_all(fd, payload, len) < 0) return -1;
    return 0;
}

void send_error(int fd, int code, const char* message) {
    char payload[512];
    snprintf(payload, sizeof(payload), "{\"code\": %d, \"message\": \"%s\"}", code, message);
    send_frame(fd, OP_ERROR, payload, strlen(payload));
}

// ============================================================================
// VALIDATION FUNCTIONS
// ============================================================================

// Validate CID format (basic check)
int is_valid_cid(const char* cid) {
    if (!cid || strlen(cid) == 0) return 0;
    if (strlen(cid) > 100) return 0;  // Reasonable max length

    // Check for valid base32 characters (lowercase a-z, 2-7)
    for (const char* p = cid; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '2' && *p <= '7'))) {
            return 0;
        }
    }
    return 1;
}

// ============================================================================
// CONNECTION HANDLER - COMPLETE UPLOAD/DOWNLOAD
// ============================================================================

void* handle_connection(void* arg) {
    int cfd = (int)(intptr_t)arg;
    upload_session_t* upload_session = NULL;

    for (;;) {
        uint8_t header[5];
        ssize_t r = read_n(cfd, header, 5);
        if (r == 0) break;
        if (r < 0) break;

        uint8_t op = header[0];
        uint32_t len;
        memcpy(&len, header + 1, 4);
        len = ntohl(len);

        uint8_t* payload = NULL;
        if (len) {
            payload = (uint8_t*)malloc(len);
            if (!payload) {
                perror("malloc");
                break;
            }
            if (read_n(cfd, payload, len) <= 0) {
                free(payload);
                break;
            }
        }

        // ====================================================================
        // UPLOAD HANDLERS
        // ====================================================================

        if (op == OP_UPLOAD_START) {
            // Extract filename from payload
            char filename[MAX_FILENAME];
            size_t fn_len = (len < MAX_FILENAME - 1) ? len : MAX_FILENAME - 1;
            memcpy(filename, payload, fn_len);
            filename[fn_len] = '\0';

            printf("[ENGINE] UPLOAD_START: name=\"%s\"\n", filename);
            fflush(stdout);

            // Create upload session
            upload_session = upload_session_create(filename);
            if (!upload_session) {
                send_error(cfd, E_BUSY, "Cannot create upload session");
                free(payload);
                break;
            }

        } else if (op == OP_UPLOAD_CHUNK) {
            if (!upload_session) {
                send_error(cfd, E_PROTO, "UPLOAD_CHUNK without UPLOAD_START");
                free(payload);
                break;
            }

            // LOCK before accessing/modifying session
            pthread_mutex_lock(&upload_session->lock);

            // Expand chunk array if needed
            if (upload_session->chunk_count >= upload_session->chunk_capacity) {
                upload_session->chunk_capacity *= 2;
                chunk_info_t* new_chunks = realloc(upload_session->chunks,
                    upload_session->chunk_capacity * sizeof(chunk_info_t));
                if (!new_chunks) {
                    pthread_mutex_unlock(&upload_session->lock);
                    send_error(cfd, E_BUSY, "Memory allocation failed");
                    free(payload);
                    break;
                }
                upload_session->chunks = new_chunks;
            }

            // Get chunk index (current count)
            uint32_t chunk_idx = upload_session->chunk_count;

            // PRE-INCREMENT chunk_count (IMPORTANT!)
            upload_session->chunk_count++;

            // Update total size
            upload_session->total_size += len;

            pthread_mutex_unlock(&upload_session->lock);

            // Create work item (outside lock)
            work_item_t work;
            work.type = WORK_HASH_AND_SAVE;
            work.session = upload_session;
            work.index = chunk_idx;
            work.data = payload;  // Thread will free this
            work.size = len;
            work.expected_hash[0] = '\0';

            // Submit to thread pool
            thread_pool_submit(g_thread_pool, work);

            // Don't free payload here - worker thread will free it
            payload = NULL;

        } else if (op == OP_UPLOAD_FINISH) {
            if (!upload_session) {
                send_error(cfd, E_PROTO, "UPLOAD_FINISH without UPLOAD_START");
                free(payload);
                break;
            }

            printf("[ENGINE] UPLOAD_FINISH: waiting for %zu chunks...\n",
                   upload_session->chunk_count);
            fflush(stdout);

            // Wait for all chunks to be processed
            thread_pool_wait(g_thread_pool);

            printf("[ENGINE] All chunks processed, building manifest...\n");
            fflush(stdout);

            // Build manifest
            char* manifest_json = build_manifest(upload_session);
            if (!manifest_json) {
                send_error(cfd, E_BUSY, "Failed to build manifest");
                upload_session_destroy(upload_session);
                free(payload);
                break;
            }

            // Compute CID
            char* cid = compute_cid(manifest_json);
            if (!cid) {
                send_error(cfd, E_BUSY, "Failed to compute CID");
                free(manifest_json);
                upload_session_destroy(upload_session);
                free(payload);
                break;
            }

            printf("[ENGINE] CID computed: %s\n", cid);
            fflush(stdout);

            // Save manifest
            if (save_manifest(cid, manifest_json) != 0) {
                send_error(cfd, E_BUSY, "Failed to save manifest");
                free(cid);
                free(manifest_json);
                upload_session_destroy(upload_session);
                free(payload);
                break;
            }

            printf("[ENGINE] UPLOAD_FINISH -> returning CID %s\n", cid);
            fflush(stdout);

            // Send CID back
            send_frame(cfd, OP_UPLOAD_DONE, cid, (uint32_t)strlen(cid));

            // Cleanup
            free(cid);
            free(manifest_json);
            upload_session_destroy(upload_session);
            upload_session = NULL;

        // ====================================================================
        // DOWNLOAD HANDLERS (Placeholder for now)
        // ====================================================================

        } else if (op == OP_DOWNLOAD_START) {
            char cid[256];
            size_t cid_len = (len < 255) ? len : 255;
            memcpy(cid, payload, cid_len);
            cid[cid_len] = '\0';

            printf("[ENGINE] DOWNLOAD_START: cid=\"%s\"\n", cid);
            fflush(stdout);

            // Validate CID format
            if (!is_valid_cid(cid)) {
                printf("[ENGINE] ERROR: Invalid CID format\n");
                fflush(stdout);
                send_error(cfd, E_BAD_CID, "Invalid CID format");
                free(payload);
                continue;
            }

            // Create download session
            download_session_t* download_session = download_session_create(cid);
            if (!download_session) {
                send_error(cfd, E_NOT_FOUND, "CID not found or invalid manifest");
                free(payload);
                continue;
            }

            printf("[ENGINE] Found %zu chunks, submitting for verification...\n",
                   download_session->total_chunks);
            fflush(stdout);

            // Submit all chunks for parallel verification
            for (size_t i = 0; i < download_session->total_chunks; i++) {
                work_item_t work;
                work.type = WORK_LOAD_AND_VERIFY;
                work.session = download_session;
                work.index = i;
                work.data = NULL;
                work.size = 0;
                strncpy(work.expected_hash, download_session->chunks[i].hash, MAX_HASH_LEN - 1);
                work.expected_hash[MAX_HASH_LEN - 1] = '\0';

                thread_pool_submit(g_thread_pool, work);
            }

            // Stream chunks in order
            for (size_t i = 0; i < download_session->total_chunks; i++) {
                pthread_mutex_lock(&download_session->lock);

                // Wait for chunk to be ready
                while (download_session->ready[i] == 0) {
                    pthread_cond_wait(&download_session->cond, &download_session->lock);
                }

                if (download_session->ready[i] < 0) {
                    // Error occurred
                    pthread_mutex_unlock(&download_session->lock);
                    send_error(cfd, E_HASH_MISMATCH, "Chunk verification failed");
                    download_session_destroy(download_session);
                    free(payload);
                    goto connection_end;
                }

                // Send chunk
                send_frame(cfd, OP_DOWNLOAD_CHUNK,
                           download_session->buffers[i],
                           download_session->sizes[i]);

                pthread_mutex_unlock(&download_session->lock);
            }

            // Send DONE
            send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);

            printf("[ENGINE] DOWNLOAD complete\n");
            fflush(stdout);

            download_session_destroy(download_session);
            } else {
            // Unknown opcode
            printf("[ENGINE] ERROR: Unknown opcode 0x%02x\n", op);
            fflush(stdout);
            send_error(cfd, E_PROTO, "Unknown operation code");
            free(payload);
            break;
        }

        free(payload);
    }

    connection_end:
        // Cleanup if connection closes with active session
        if (upload_session) {
            upload_session_destroy(upload_session);
        }

    close(cfd);
    return NULL;
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /tmp/cengine.sock\n", argv[0]);
        return 2;
    }
    g_sock_path = argv[1];

    // Create directories
    mkdirs("blocks");
    mkdirs("manifests");

    // Initialize global structures
    block_table_init();
    g_thread_pool = thread_pool_create(THREAD_POOL_SIZE);

    printf("[ENGINE] Thread pool initialized with %d threads\n", THREAD_POOL_SIZE);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
    unlink(g_sock_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 2; }
    if (listen(fd, 64) < 0) { perror("listen"); return 2; }

    printf("[ENGINE] listening on %s\n", g_sock_path);
    fflush(stdout);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        pthread_t th;
        pthread_create(&th, NULL, handle_connection, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(fd);
    unlink(g_sock_path);

    // Cleanup
    thread_pool_destroy(g_thread_pool);
    pthread_rwlock_destroy(&g_block_table.lock);
    free(g_block_table.entries);

    return 0;
}