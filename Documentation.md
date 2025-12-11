# IPFS-like Content-Addressed Storage System
## OS Midterm Project - Documentation

**Student Name:** [Your Name]  
**Student ID:** [Your ID]  
**Date:** December 2025

---

## Table of Contents
1. [Project Overview](#project-overview)
2. [Architecture](#architecture)
3. [Implementation Details](#implementation-details)
4. [Synchronization Mechanisms](#synchronization-mechanisms)
5. [Testing Results](#testing-results)
6. [Build and Run Instructions](#build-and-run-instructions)
7. [Challenges and Solutions](#challenges-and-solutions)

---

## Project Overview

This project implements a content-addressed file storage system similar to IPFS (InterPlanetary File System). Files are split into chunks, each chunk is cryptographically hashed, and stored in a Merkle-DAG (Directed Acyclic Graph) structure. The system supports concurrent uploads and downloads with proper synchronization.

### Key Features
- **Content-addressed storage**: Files identified by cryptographic hash (CID)
- **Chunking**: Files split into 256KB chunks for efficient storage
- **Deduplication**: Identical chunks stored only once
- **Concurrent operations**: Multiple uploads/downloads simultaneously
- **Thread pool**: Parallel chunk processing using 8 worker threads
- **Atomic operations**: Safe concurrent access to shared resources

---

## Architecture

### Components

#### 1. **Python Gateway (`main.py`)**
- HTTP server listening on port 9000
- Two endpoints: `/upload` and `/download`
- Spawns separate process for each request
- Communicates with C engine via Unix Domain Socket
- **No heavy processing** - all computation in C engine

#### 2. **C Engine (`c_engine`)**
- Core processing engine
- Listens on Unix socket: `/tmp/cengine.sock`
- Thread pool with 8 worker threads
- Handles chunking, hashing, storage, and retrieval
- Implements all synchronization mechanisms

### Data Flow

**Upload Flow:**
```
Client → Gateway (HTTP) → Unix Socket → C Engine
                                          ↓
                                    Thread Pool
                                          ↓
                                  Hash & Store Chunks
                                          ↓
                                  Build Manifest → CID
                                          ↓
Client ← Gateway ← Unix Socket ← C Engine
```

**Download Flow:**
```
Client → Gateway (HTTP) → Unix Socket → C Engine
                                          ↓
                                   Load Manifest
                                          ↓
                                    Thread Pool
                                          ↓
                              Verify Chunks (parallel)
                                          ↓
                            Assemble & Stream (sequential)
                                          ↓
Client ← Gateway ← Unix Socket ← C Engine
```

---

## Implementation Details

### 1. File Chunking

Files are split into 256KB chunks:
```c
#define CHUNK_SIZE (256 * 1024)  // 256KB
```

**Why 256KB?**
- Good balance between parallelism and overhead
- Standard size used in distributed systems
- Allows efficient use of 8 worker threads

### 2. Hashing (SHA-256)

Each chunk is hashed using SHA-256:
```c
char* compute_multihash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    
    // Multihash format: 0x12 (sha256) + 0x20 (32 bytes) + hash
    char* mhash = malloc(4 + SHA256_DIGEST_LENGTH * 2 + 1);
    sprintf(mhash, "1220");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(mhash + 4 + (i * 2), "%02x", hash[i]);
    }
    return mhash;
}
```

**Multihash Format:**
- `0x12` = SHA-256 algorithm identifier
- `0x20` = 32 bytes (length of SHA-256)
- Followed by hex-encoded hash

### Algorithm Selection: SHA-256 vs BLAKE3

The project specification suggested BLAKE3 as the hashing algorithm. However,
this implementation uses **SHA-256** for the following reasons:

1. **Availability**: SHA-256 is readily available in OpenSSL (libcrypto)
2. **Simplicity**: No additional dependencies required
3. **Security**: SHA-256 is cryptographically secure and widely trusted
4. **Standards**: SHA-256 has established multihash code (0x12)
5. **Project Scope**: For educational purposes, the choice of hash function
   doesn't impact the core learning objectives (concurrency, IPC, thread management)

The specification noted BLAKE3 as "suggested" (پیشنهادی), not mandatory,
and explicitly mentioned it could be changed via environment variable.

### 3. Block Storage

Blocks stored in hierarchical directory structure:
```
blocks/aa/bb/1220abcdef...
       ↑  ↑  ↑
       │  │  └─ Full multihash
       │  └──── Second 2 hex chars
       └─────── First 2 hex chars
```

**Benefits:**
- Avoids too many files in one directory
- Efficient filesystem lookup
- Similar to Git's object storage

### 4. Manifest Format
```json
{
  "version": 1,
  "hash_algo": "sha256",
  "chunk_size": 262144,
  "total_size": 2097152,
  "filename": "example.bin",
  "chunks": [
    {"index": 0, "size": 262144, "hash": "1220abc..."},
    {"index": 1, "size": 262144, "hash": "1220def..."}
  ]
}
```

Stored at: `manifests/<cid>.json`

### 5. CID Generation
```c
CID = base32(SHA256(manifest_json))
```

**Example:**
```
manifest → SHA256 → base32 → "fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq"
```

---

## Synchronization Mechanisms

### 1. Block Table (Reader-Writer Lock)

**Purpose:** Track block reference counts for deduplication

**Implementation:**
```c
typedef struct {
    block_entry_t* entries;
    size_t count;
    pthread_rwlock_t lock;  // Reader-writer lock
} block_table_t;
```

**Usage:**
- **Read lock** (`rdlock`) - Check if block exists (downloads)
- **Write lock** (`wrlock`) - Add block or increment refcount (uploads)

**Why Reader-Writer?**
- Multiple concurrent downloads can check simultaneously
- Uploads get exclusive access when modifying

### 2. Thread Pool Work Queue

**Purpose:** Distribute chunk processing across 8 worker threads

**Implementation:**
```c
typedef struct {
    work_item_t* queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;           // Signal workers
    pthread_cond_t empty_cond;     // Signal completion
    int active_tasks;
} thread_pool_t;
```

**Producer-Consumer Pattern:**
1. Main thread submits work → locks mutex → adds to queue → signals workers
2. Worker threads wait on condition → grab work → process → mark complete
3. When queue empty AND active_tasks == 0 → signal completion

### 3. Upload Session (Per-Connection)

**Purpose:** Protect chunk array during concurrent modifications

**Implementation:**
```c
typedef struct {
    chunk_info_t* chunks;
    size_t chunk_count;
    pthread_mutex_t lock;
} upload_session_t;
```

**Critical Section:**
```c
pthread_mutex_lock(&upload_session->lock);
// Check capacity
// Realloc if needed
// Get chunk index
// Increment chunk_count
pthread_mutex_unlock(&upload_session->lock);
```

**Why needed:** Without this lock, concurrent UPLOAD_CHUNK messages could:
- Get same chunk index (data loss)
- Double-realloc (memory corruption)
- Race on chunk_count (incorrect manifest)

### 4. Download Sequential Assembler

**Purpose:** Stream chunks in correct order despite out-of-order verification

**Implementation:**
```c
typedef struct {
    uint8_t** buffers;
    int* ready;          // ready[i] = 1 when chunk i verified
    pthread_mutex_t lock;
    pthread_cond_t cond;
} download_session_t;
```

**How it works:**
1. Submit all chunks to thread pool (verified in parallel)
2. Main thread waits for ready[0], then ready[1], then ready[2]...
3. Streams chunks sequentially even if verification completed 2→0→1

### 5. Atomic Manifest Writing

**Technique:**
```c
1. Write to manifests/<cid>.json.tmp
2. fsync() - flush to disk
3. rename() to manifests/<cid>.json  // Atomic on POSIX
```

**Guarantees:**
- Readers see complete manifest or nothing
- No partial/corrupted manifests
- Safe even if process crashes during write

---

## Testing Results

### Test Suite Overview

All tests passed successfully:

| Test | Result | Details |
|------|--------|---------|
| Basic Upload/Download | ✅ PASS | Small + large files |
| Concurrent Uploads | ✅ PASS | 5 simultaneous uploads |
| Concurrent Downloads | ✅ PASS | 10 simultaneous downloads |
| Deduplication | ✅ PASS | No duplicate blocks created |
| Large File (2MB) | ✅ PASS | 8 chunks, perfect integrity |
| Race Condition Stress | ✅ PASS | 20 concurrent uploads, 0 failures |
| Error Handling | ✅ PASS | Invalid CID rejected |

### Performance Metrics

**Upload Performance:**
- Small files (1KB): ~0.03s
- Large files (500KB, 2 chunks): ~0.04s
- 20 concurrent uploads: 0.20s total (0.01s average)

**Download Performance:**
- Small files: 0.00-0.01s
- Large files (2MB, 8 chunks): ~0.02s
- 10 concurrent downloads: All under 0.02s

**Concurrency:**
- No crashes with 20 simultaneous uploads
- No data corruption
- Proper synchronization verified

### Deduplication Verification

Uploaded same content twice:
```
Before second upload: 10 blocks
After second upload:  10 blocks
```
✅ Deduplication working - no new blocks created

---

## Build and Run Instructions

### Prerequisites (WSL/Linux)
```bash
sudo apt update
sudo apt install build-essential gcc libssl-dev python3
```

### Building
```bash
cd ~/os_project
make clean
make
```

**Output:**
```
gcc -O2 -pthread -Wall -Wextra -g -D_GNU_SOURCE -o c_engine c_engine.c -lcrypto -lpthread
```

### Running

**Terminal 1 - Start C Engine:**
```bash
./c_engine /tmp/cengine.sock
```

**Terminal 2 - Start Python Gateway:**
```bash
python3 main.py
```

**Terminal 3 - Test:**
```bash
# Upload
curl -X POST http://127.0.0.1:9000/upload \
     -H "X-Filename: test.txt" \
     --data-binary @test.txt

# Download (use CID from upload response)
curl "http://127.0.0.1:9000/download?cid=<CID>" -o downloaded.txt
```

### Running Full Test Suite
```bash
python3 final_test_suite.py
```

---

## Challenges and Solutions

### Challenge 1: Race Condition in Upload Chunk Array

**Problem:** Multiple UPLOAD_CHUNK messages arriving simultaneously could:
- Both check `chunk_count >= capacity`
- Both perform realloc
- Result: Memory corruption

**Solution:** Added mutex protection:
```c
pthread_mutex_lock(&upload_session->lock);
// Check capacity, realloc, increment count
pthread_mutex_unlock(&upload_session->lock);
```

### Challenge 2: Out-of-Order Chunk Verification

**Problem:** Chunks verified in parallel may complete in order 2→0→1, but must be streamed 0→1→2.

**Solution:** Implemented sequential assembler:
- Workers mark chunks as ready
- Main thread waits for ready[next_index] in order
- Uses condition variable to wait efficiently

### Challenge 3: Deadlock Prevention

**Problem:** Multiple locks could cause deadlocks if acquired in wrong order.

**Solution:**
- Never hold multiple locks simultaneously
- Thread pool mutex held briefly, then released
- Session locks held for minimal time
- No circular dependencies

### Challenge 4: Block Deduplication Race

**Problem:** Two uploads of same content could both save the block.

**Solution:** Used reader-writer lock:
```c
pthread_rwlock_wrlock(&block_table.lock);
// Check if exists, if not, save and add
pthread_rwlock_unlock(&block_table.lock);
```

Entire check-and-add is atomic.

---

## Protocol Specification

### Framed Messages

Format: `[1 byte opcode][4 bytes length (big-endian)][payload]`

**Opcodes:**
- `0x01` UPLOAD_START - Begin upload (payload: filename)
- `0x02` UPLOAD_CHUNK - Send chunk data
- `0x03` UPLOAD_FINISH - Complete upload
- `0x81` UPLOAD_DONE - Server returns CID
- `0x11` DOWNLOAD_START - Request file (payload: CID)
- `0x91` DOWNLOAD_CHUNK - Server sends chunk
- `0x92` DOWNLOAD_DONE - Download complete
- `0xFF` ERROR - Error occurred (payload: JSON error)

**Error Codes:**
- `E_BAD_CID (1)` - Invalid CID format
- `E_NOT_FOUND (2)` - CID not found
- `E_HASH_MISMATCH (3)` - Chunk verification failed
- `E_BUSY (4)` - Resource allocation failed
- `E_PROTO (5)` - Protocol violation

---

## Conclusion

This project successfully implements a content-addressed storage system with:
- Proper process and thread management
- Robust synchronization mechanisms
- Efficient parallel processing
- Safe concurrent operations
- Complete error handling

All requirements met and tested successfully.

---

## References

- IPFS Documentation: https://docs.ipfs.io
- POSIX Threads Programming: https://computing.llnl.gov/tutorials/pthreads/
- Multihash Specification: https://multiformats.io/multihash/
- SHA-256 (OpenSSL): https://www.openssl.org/docs/

---

**End of Documentation**