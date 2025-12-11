# Complete Code Explanation - Line by Line Understanding
## For Students Who Want to REALLY Understand Every Detail

---

## Table of Contents
1. [Big Picture Overview](#big-picture-overview)
2. [Part 1: Data Structures](#part-1-data-structures)
3. [Part 2: Hashing and CID](#part-2-hashing-and-cid)
4. [Part 3: Thread Pool](#part-3-thread-pool)
5. [Part 4: Upload Flow](#part-4-upload-flow)
6. [Part 5: Download Flow](#part-5-download-flow)
7. [Part 6: Synchronization Deep Dive](#part-6-synchronization-deep-dive)
8. [Part 7: Common Questions](#part-7-common-questions)

---

## Big Picture Overview

### What Does This System Do?

Imagine you have a file called `photo.jpg` (1MB). Here's what happens:

**When you UPLOAD:**
```
1. File arrives at Python gateway (main.py)
2. Python sends it to C engine via Unix socket
3. C engine splits it into chunks:
   - Chunk 0: bytes 0-262143 (256KB)
   - Chunk 1: bytes 262144-524287 (256KB)
   - Chunk 2: bytes 524288-786431 (256KB)
   - Chunk 3: bytes 786432-1048575 (remaining)
4. Each chunk goes to thread pool
5. 8 worker threads process chunks in parallel:
   - Hash the chunk (SHA-256)
   - Save to disk: blocks/ab/cd/1220abcd...
6. Build a manifest (JSON) listing all chunks
7. Hash the manifest → This is the CID
8. Save manifest: manifests/<cid>.json
9. Return CID to client
```

**When you DOWNLOAD:**
```
1. Client sends CID to gateway
2. Gateway forwards to C engine
3. Engine reads manifest: manifests/<cid>.json
4. Submits all chunks to thread pool (parallel verification):
   - Thread 1: Load chunk 0, verify hash
   - Thread 2: Load chunk 1, verify hash
   - Thread 3: Load chunk 2, verify hash
   - Thread 4: Load chunk 3, verify hash
5. Assembler waits for chunks IN ORDER (0, 1, 2, 3)
6. Streams chunks to client sequentially
7. Client reconstructs original file
```

**Why this is cool:**
- Content-addressed: Same file = same CID (like Git)
- Deduplication: Upload same file twice → stored once
- Parallel processing: 8 threads work simultaneously
- Safe concurrency: Multiple users can upload/download at once

---

## Part 1: Data Structures

Let's understand each data structure and WHY it exists.

### 1.1 Chunk Info
```c
typedef struct {
    uint32_t index;      // Which chunk is this? 0, 1, 2, 3...
    uint32_t size;       // How many bytes? Usually 262144, last chunk might be smaller
    char hash[MAX_HASH_LEN];  // The multihash of this chunk
} chunk_info_t;
```

**Example:**
```c
chunk_info_t chunk;
chunk.index = 0;
chunk.size = 262144;
chunk.hash = "1220abc123..."; // Multihash string
```

**Why we need this:**
- To remember metadata about each chunk
- Used in manifest to list all chunks
- Needed for reconstruction during download

---

### 1.2 Upload Session
```c
typedef struct {
    char filename[MAX_FILENAME];     // Original filename
    uint64_t total_size;              // Total bytes uploaded
    uint32_t chunk_size;              // 256KB (262144 bytes)
    chunk_info_t* chunks;             // Dynamic array of chunk metadata
    size_t chunk_count;               // How many chunks so far
    size_t chunk_capacity;            // Array capacity (grows if needed)
    pthread_mutex_t lock;             // Protects this structure
} upload_session_t;
```

**Real-world analogy:**
Think of this as a **notebook** where you write down information as chunks arrive:
- Page 1: Chunk 0 info
- Page 2: Chunk 1 info
- Page 3: Chunk 2 info
- ...

**Why the mutex?**
Imagine two people trying to write in the same notebook at the same time → chaos!
The mutex ensures only ONE person writes at a time.

**Example usage:**
```c
// Create session when upload starts
upload_session_t* session = upload_session_create("photo.jpg");

// As chunks arrive:
session->chunks[0] = chunk_0_info;
session->chunks[1] = chunk_1_info;
session->chunk_count = 2;
session->total_size = 524288; // 2 * 256KB

// When done, build manifest from session
```

---

### 1.3 Download Session
```c
typedef struct {
    char cid[256];                    // The CID we're downloading
    char filename[MAX_FILENAME];      // Original filename
    size_t total_chunks;              // How many chunks total
    chunk_info_t* chunks;             // Array of chunk metadata
    
    // For sequential streaming:
    uint8_t** buffers;                // buffers[0] = chunk 0 data
    size_t* sizes;                    // sizes[0] = chunk 0 size
    int* ready;                       // ready[0] = 1 means chunk 0 verified
    size_t next_index;                // Next chunk to send
    
    pthread_mutex_t lock;
    pthread_cond_t cond;              // Signal when chunk becomes ready
} download_session_t;
```

**Real-world analogy:**
Think of this as an **assembly line** for a product with 4 parts:
```
Station 1 (Thread 1): Verify part 2 ✓ → ready[2] = 1
Station 2 (Thread 2): Verify part 0 ✓ → ready[0] = 1
Station 3 (Thread 3): Verify part 1 ✓ → ready[1] = 1
Station 4 (Thread 4): Verify part 3 ✓ → ready[3] = 1

But shipping must be in order!
Shipping waits: ready[0]? YES → Ship part 0
                ready[1]? YES → Ship part 1
                ready[2]? YES → Ship part 2
                ready[3]? YES → Ship part 3
```

**Why condition variable?**
Instead of constantly checking "Is part 0 ready? Is it ready now? How about now?"
We WAIT and get NOTIFIED when it's ready. Much more efficient!

---

### 1.4 Block Table
```c
typedef struct {
    char hash[MAX_HASH_LEN];
    int refcount;              // How many files use this block
} block_entry_t;

typedef struct {
    block_entry_t* entries;    // Array of all blocks
    size_t count;              // How many blocks
    size_t capacity;           // Array capacity
    pthread_rwlock_t lock;     // Reader-writer lock
} block_table_t;
```

**Real-world analogy:**
Think of this as a **library catalog**:
```
Book (Block)              Copies Available (Refcount)
-----------------         -----------------------
"1220abc123..."          3 (used by 3 different files)
"1220def456..."          1 (used by 1 file)
"1220ghi789..."          5 (popular block!)
```

**Why reader-writer lock?**
- Many people can READ the catalog at once (check if book exists)
- Only ONE person can WRITE to catalog at a time (add new book)

**Example:**
```c
// User 1 uploads file A with chunk "1220abc..."
block_table_add_or_inc("1220abc...");  // refcount = 1

// User 2 uploads file B with SAME chunk "1220abc..."
block_table_add_or_inc("1220abc...");  // refcount = 2
// Block NOT saved again! Just increment counter
```

This is **deduplication** → saves disk space!

---

### 1.5 Work Item
```c
typedef struct {
    work_type_t type;              // WORK_HASH_AND_SAVE or WORK_LOAD_AND_VERIFY
    void* session;                 // Pointer to upload/download session
    uint32_t index;                // Which chunk number
    uint8_t* data;                 // The chunk data (for upload)
    size_t size;                   // Data size
    char expected_hash[MAX_HASH_LEN];  // Expected hash (for download)
} work_item_t;
```

**Real-world analogy:**
Think of this as a **work order** given to a factory worker:

**Upload Work Order:**
```
WORK ORDER #42
--------------
Type: Hash and Save
Data: [256KB of chunk data]
Size: 262144 bytes
Task: 1. Hash this data
      2. Save to blocks/aa/bb/1220...
      3. Update session with hash
```

**Download Work Order:**
```
WORK ORDER #17
--------------
Type: Load and Verify
Expected Hash: "1220abc123..."
Task: 1. Load block from blocks/12/20/1220abc123...
      2. Hash it
      3. Compare with expected hash
      4. If match: store in session buffer
      5. If mismatch: mark as error
```

---

### 1.6 Thread Pool
```c
typedef struct {
    pthread_t* threads;           // Array of 8 worker threads
    size_t num_threads;           // 8
    
    work_item_t* queue;           // Circular queue of work items
    size_t queue_head;            // Where to take work from
    size_t queue_tail;            // Where to add work
    size_t queue_size;            // Current number of items
    size_t queue_capacity;        // Max capacity (1024)
    
    pthread_mutex_t mutex;        // Protects the queue
    pthread_cond_t cond;          // Signal workers: "Work available!"
    pthread_cond_t empty_cond;    // Signal main: "All work done!"
    
    int shutdown;                 // Time to quit?
    int active_tasks;             // How many workers busy right now
} thread_pool_t;
```

**Real-world analogy:**
Think of this as a **restaurant kitchen**:
```
Kitchen = Thread Pool
-----------------
Chefs (threads) = 8
Order queue = work_item_t queue
Head chef (main thread) = submits orders
Orders in queue = queue_size
Active orders being cooked = active_tasks

When order arrives:
1. Head chef puts order in queue
2. Rings bell (signals condition variable)
3. Available chef picks up order
4. Chef marks "I'm busy" (active_tasks++)
5. Chef cooks (processes work)
6. Chef marks "I'm done" (active_tasks--)
7. If queue empty AND no active chefs: ring bell "All done!"
```

**Why condition variables?**
Instead of chefs constantly asking "Any orders? Any orders now?"
They WAIT and get WOKEN UP when order arrives. Saves CPU!

---

## Part 2: Hashing and CID

### 2.1 What is a Hash?

A hash is like a **fingerprint** for data. Same data = same fingerprint.
```c
char* compute_hash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];  // 32 bytes
    SHA256(data, len, hash);  // OpenSSL function
    
    // Convert to hex string: "a1b2c3d4..."
    char* hex = malloc(SHA256_DIGEST_LENGTH * 2 + 1);  // 64 chars + null
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    return hex;
}
```

**Example:**
```
Input: "Hello"
SHA256: 185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969
        ↑ 32 bytes → 64 hex characters
```

**Properties of SHA-256:**
- Same input ALWAYS gives same output
- Different input gives COMPLETELY different output
- One-way: Can't reverse (hash → data)
- Collision-resistant: Nearly impossible to find two inputs with same hash

---

### 2.2 What is Multihash?

Multihash adds a **prefix** to say "This is SHA-256":
```c
char* compute_multihash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    
    char* mhash = malloc(4 + SHA256_DIGEST_LENGTH * 2 + 1);
    sprintf(mhash, "1220");  // Prefix!
    //          ↑↑
    //          ||
    //          |└─ 0x20 = 32 bytes (length)
    //          └── 0x12 = SHA-256 (algorithm)
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(mhash + 4 + (i * 2), "%02x", hash[i]);
    }
    return mhash;
}
```

**Example:**
```
Regular hash:  185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969
Multihash:   1220185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969
             ↑↑
             └─ Prefix
```

**Why?**
So in the future we can support other hash algorithms:
- `1220...` = SHA-256
- `1314...` = SHA-512 (hypothetical)
- `1118...` = SHA-1 (hypothetical)

---

### 2.3 Base32 Encoding

Base32 converts binary data to **human-friendly** characters:
```c
static const char base32_chars[] = "abcdefghijklmnopqrstuvwxyz234567";
```

**Why only these characters?**
- Case-insensitive (a = A)
- No confusing characters (no 0, O, 1, l)
- Safe for URLs and filenames

**How it works:**
```
Binary (5 bits) → Base32 character
00000 → 'a'
00001 → 'b'
00010 → 'c'
...
11111 → '7'
```

**Example:**
```
Hash (binary): 10110110...
↓ Group into 5-bit chunks
10110 | 11001 | 01011 | ...
  ↓       ↓       ↓
  w       z       l
  
Result: "wzl..."
```

---

### 2.4 CID Generation
```c
char* compute_cid(const char* manifest_json) {
    // Step 1: Hash the manifest
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)manifest_json, strlen(manifest_json), hash);
    
    // Step 2: Encode in base32
    return base32_encode(hash, SHA256_DIGEST_LENGTH);
}
```

**Full process:**
```
1. Build manifest:
{
  "version": 1,
  "chunks": [{"index": 0, "hash": "1220abc..."}, ...]
}

2. Serialize to string (exact formatting matters!)

3. Hash the string:
   SHA256(manifest) → 32 bytes

4. Encode to base32:
   32 bytes → ~52 characters
   
5. Result: "fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq"
```

**Why hash the manifest?**
- Manifest lists all chunk hashes
- CID is hash of manifest
- This creates a **Merkle Tree** structure
- Change ANY chunk → manifest changes → CID changes
- Guarantees integrity of entire file!

---

## Part 3: Thread Pool

### 3.1 Why Do We Need a Thread Pool?

**Without thread pool:**
```
File arrives with 8 chunks
→ Create thread for chunk 0
→ Create thread for chunk 1
→ ...
→ Create thread for chunk 7
→ 8 chunks done
→ Destroy 8 threads

Next file arrives
→ Create 8 threads AGAIN
→ Process
→ Destroy 8 threads AGAIN
```

**Creating/destroying threads is EXPENSIVE!**

**With thread pool:**
```
At startup: Create 8 threads ONCE
They wait for work...

File arrives with 8 chunks
→ Submit work item 0 to pool
→ Submit work item 1 to pool
→ ...
→ Threads pick up work and process
→ Done!

Next file arrives
→ Submit work items to SAME threads
→ No creation/destruction overhead!
```

---

### 3.2 Thread Pool Creation
```c
thread_pool_t* thread_pool_create(size_t num_threads) {
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));
    
    pool->num_threads = 8;
    pool->threads = malloc(sizeof(pthread_t) * 8);
    pool->queue_capacity = 1024;
    pool->queue = malloc(sizeof(work_item_t) * 1024);
    
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_size = 0;
    pool->shutdown = 0;
    pool->active_tasks = 0;
    
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->empty_cond, NULL);
    
    // Create 8 worker threads
    for (size_t i = 0; i < 8; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }
    
    return pool;
}
```

**What happens:**
1. Allocate memory for pool structure
2. Allocate queue (circular buffer, 1024 items max)
3. Initialize synchronization primitives (mutex, cond vars)
4. Create 8 threads, all running `worker_thread` function
5. Threads immediately start waiting for work

**Visual:**
```
[Thread 1] → waiting...
[Thread 2] → waiting...
[Thread 3] → waiting...
[Thread 4] → waiting...
[Thread 5] → waiting...
[Thread 6] → waiting...
[Thread 7] → waiting...
[Thread 8] → waiting...

Queue: [empty]
```

---

### 3.3 Submitting Work
```c
void thread_pool_submit(thread_pool_t* pool, work_item_t work) {
    pthread_mutex_lock(&pool->mutex);
    
    // Wait if queue is full
    while (pool->queue_size >= pool->queue_capacity) {
        pthread_cond_wait(&pool->cond, &pool->mutex);
    }
    
    // Add work to queue (at tail)
    pool->queue[pool->queue_tail] = work;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_size++;
    
    // Wake up ONE worker thread
    pthread_cond_signal(&pool->cond);
    
    pthread_mutex_unlock(&pool->mutex);
}
```

**Step by step:**
```
Before:
Queue: [empty] (size=0, head=0, tail=0)

Submit work A:
1. Lock mutex (exclusive access)
2. Check if full (size < capacity) ✓
3. queue[0] = work A
4. tail = 1
5. size = 1
6. Signal: "Hey workers, job available!"
7. Unlock mutex

Queue: [A] (size=1, head=0, tail=1)

Submit work B:
Queue: [A, B] (size=2, head=0, tail=2)

Submit work C:
Queue: [A, B, C] (size=3, head=0, tail=3)
```

**Circular buffer:**
```
Capacity = 4
[0][1][2][3]
 ↑       ↑
head    tail

Add D: tail wraps around!
[D][1][2][3]
     ↑   ↑
    head tail
```

---

### 3.4 Worker Thread
```c
void* worker_thread(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg;
    
    while (1) {
        pthread_mutex_lock(&pool->mutex);
        
        // Wait for work
        while (pool->queue_size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }
        
        // Check if time to quit
        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;  // Exit thread
        }
        
        // Get work from queue (from head)
        work_item_t work = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_size--;
        pool->active_tasks++;  // I'm working now!
        
        pthread_mutex_unlock(&pool->mutex);
        
        // ═══════════════════════════════════════
        // PROCESS WORK (outside lock!)
        // ═══════════════════════════════════════
        
        if (work.type == WORK_HASH_AND_SAVE) {
            // Hash chunk
            char* mhash = compute_multihash(work.data, work.size);
            
            // Save if new
            if (!block_table_exists(mhash)) {
                save_block(mhash, work.data, work.size);
            }
            
            // Update refcount
            block_table_add_or_inc(mhash);
            
            // Update session
            upload_session_t* session = (upload_session_t*)work.session;
            pthread_mutex_lock(&session->lock);
            session->chunks[work.index].hash = mhash;
            session->chunks[work.index].size = work.size;
            pthread_mutex_unlock(&session->lock);
            
            free(mhash);
            free(work.data);
        }
        
        // Mark task complete
        pthread_mutex_lock(&pool->mutex);
        pool->active_tasks--;  // I'm done!
        
        // If all work done, signal main thread
        if (pool->queue_size == 0 && pool->active_tasks == 0) {
            pthread_cond_broadcast(&pool->empty_cond);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
    
    return NULL;
}
```

**Visualization:**
```
Thread 1 flow:
1. Lock mutex
2. Check queue: empty? → WAIT
   [Thread sleeps, releases mutex]
   
[Work arrives, signal sent]
   
3. Wake up, reacquire mutex
4. Check queue: has work? YES
5. Get work item from head
6. active_tasks++ (now I'm busy)
7. Unlock mutex
8. ⚡ PROCESS WORK (hash, save, etc.) ⚡
9. Lock mutex
10. active_tasks-- (done working)
11. Check: queue empty AND no active tasks?
    → YES: Broadcast "all done!"
    → NO: Nothing
12. Unlock mutex
13. Loop back to step 1
```

**Key insight:**
- Worker holds mutex for SHORT time (just to get/put work)
- Actual processing happens OUTSIDE lock
- This allows other workers to grab work simultaneously

---

### 3.5 Waiting for Completion
```c
void thread_pool_wait(thread_pool_t* pool) {
    pthread_mutex_lock(&pool->mutex);
    
    // Wait until queue empty AND no active tasks
    while (pool->queue_size > 0 || pool->active_tasks > 0) {
        pthread_cond_wait(&pool->empty_cond, &pool->mutex);
    }
    
    pthread_mutex_unlock(&pool->mutex);
}
```

**When is this used?**

In upload, after all chunks submitted:
```c
// Submit all chunks
for (each chunk) {
    thread_pool_submit(pool, work);
}

// Wait for ALL to complete
thread_pool_wait(pool);

// NOW we can build manifest (all chunks processed!)
```

**Why wait?**
Can't build manifest until ALL chunk hashes known!

**Visual:**
```
Main thread:              Worker threads:
-----------               ---------------
Submit chunk 0 →          Thread 1: Process chunk 0
Submit chunk 1 →          Thread 2: Process chunk 1
Submit chunk 2 →          Thread 3: Process chunk 2
Submit chunk 3 →          Thread 4: Process chunk 3
Call wait() ⏸            
  ↓                       Thread 1: Done! ✓
  ↓                       Thread 2: Done! ✓
  ↓                       Thread 3: Done! ✓
  ↓ (sleeping...)         Thread 4: Done! ✓
  ↓                       
  ↓ (broadcast signal)    [All done!]
Wake up! ⏩              
Build manifest...
```

---

## Part 4: Upload Flow

### 4.1 Upload Start

**Client sends:**
```
HTTP POST /upload
X-Filename: photo.jpg
Content-Length: 1048576

[file data...]
```

**Gateway receives, creates process, connects to engine via Unix socket:**

**Engine receives:**
```
Frame:
[0x01][00 00 00 09][photo.jpg]
  ↑        ↑            ↑
  |        |            └─ Payload: filename
  |        └───────────── Length: 9 bytes
  └────────────────────── Opcode: UPLOAD_START
```

**Handler code:**
```c
if (op == OP_UPLOAD_START) {
    // Extract filename
    char filename[MAX_FILENAME];
    size_t fn_len = (len < MAX_FILENAME - 1) ? len : MAX_FILENAME - 1;
    memcpy(filename, payload, fn_len);
    filename[fn_len] = '\0';
    
    printf("[ENGINE] UPLOAD_START: name=\"%s\"\n", filename);
    
    // Create session for this upload
    upload_session = upload_session_create(filename);
}
```

**What happens:**
1. Parse filename from payload
2. Print log message
3. Create empty session to collect chunks
4. Session starts with capacity for 256 chunks

**Session after creation:**
```c
upload_session->filename = "photo.jpg"
upload_session->chunk_count = 0
upload_session->chunk_capacity = 256
upload_session->chunks = [empty array]
upload_session->total_size = 0
```

---

### 4.2 Upload Chunk (The Important Part!)

**Gateway sends chunks:**
```
Frame:
[0x02][00 04 00 00][...256KB of data...]
  ↑        ↑             ↑
  |        |             └─ Payload: chunk data
  |        └───────────── Length: 262144 bytes (256KB)
  └────────────────────── Opcode: UPLOAD_CHUNK
```

**Handler code (with detailed comments):**
```c
else if (op == OP_UPLOAD_CHUNK) {
    // ═══════════════════════════════════════════════════════════
    // CRITICAL SECTION: Must be thread-safe!
    // Multiple UPLOAD_CHUNK messages might arrive simultaneously
    // ═══════════════════════════════════════════════════════════
    
    if (!upload_session) {
        send_error(cfd, E_PROTO, "UPLOAD_CHUNK without UPLOAD_START");
        free(payload);
        break;
    }
    
    // ╔═══════════════════════════════════════════════════════╗
    // ║  LOCK BEFORE MODIFYING SESSION                        ║
    // ║  Without this, two chunks could get same index!       ║
    // ╚═══════════════════════════════════════════════════════╝
    
    pthread_mutex_lock(&upload_session->lock);
    
    // Check if we need more space in array
    if (upload_session->chunk_count >= upload_session->chunk_capacity) {
        // Double the capacity
        upload_session->chunk_capacity *= 2;
        
        // Reallocate array
        chunk_info_t* new_chunks = realloc(
            upload_session->chunks,
            upload_session->chunk_capacity * sizeof(chunk_info_t)
        );
        
        if (!new_chunks) {
            pthread_mutex_unlock(&upload_session->lock);
            send_error(cfd, E_BUSY, "Memory allocation failed");
            free(payload);
            break;
        }
        
        upload_session->chunks = new_chunks;
    }
    
    // Get this chunk's index
    uint32_t chunk_idx = upload_session->chunk_count;
    
    // IMPORTANT: Increment BEFORE releasing lock!
    upload_session->chunk_count++;
    
    // Update total size
    upload_session->total_size += len;
    
    pthread_mutex_unlock(&upload_session->lock);
    
    // ╔═══════════════════════════════════════════════════════╗
    // ║  NOW SAFE TO RELEASE LOCK                             ║
    // ║  This chunk has unique index, won't be overwritten    ║
    // ╚═══════════════════════════════════════════════════════╝
    
    // Create work item for thread pool
    work_item_t work;
    work.type = WORK_HASH_AND_SAVE;
    work.session = upload_session;
    work.index = chunk_idx;
    work.data = payload;        // Worker will free this!
    work.size = len;
    
    // Submit to thread pool
    thread_pool_submit(g_thread_pool, work);
    
    // DON'T free payload - worker thread will do it
    payload = NULL;
}
```

**What happens to the work item:**
```
Main thread creates work item:
{
  type: WORK_HASH_AND_SAVE,
  index: 0,
  data: [256KB chunk data],
  size: 262144,
  session: pointer to upload_session
}

↓ Submit to thread pool

Worker thread picks it up:
1. Hash the 256KB data → "1220abc123..."
2. Check block table: does "1220abc123..." exist?
   - NO: Save to blocks/12/20/1220abc123...
   - YES: Don't save (already have it)
3. Update block table refcount
4. Lock session
5. Write hash to session->chunks[0].hash
6. Unlock session
7. Free the 256KB data
```

**Parallel processing:**
```
Time →
Main:     [Submit ch0] [Submit ch1] [Submit ch2] [Submit ch3] [Wait...]
Thread1:              [Process ch0.................]
Thread2:                     [Process ch1.................]
Thread3:                            [Process ch2.................]
Thread4:                                   [Process ch3.................]
Main:                                                              [Continue]
```

All 4 chunks processed in PARALLEL → much faster!

---

### 4.3 Upload Finish

**Gateway sends:**
```
Frame:
[0x03][00 00 00 00]
  ↑        ↑
  |        └─ Length: 0 (no payload)
  └────────── Opcode: UPLOAD_FINISH
```

**Handler:**
```c
else if (op == OP_UPLOAD_FINISH) {
    printf("[ENGINE] UPLOAD_FINISH: waiting for %zu chunks...\n",
           upload_session->chunk_count);
    
    // ═══════════════════════════════════════════════════
    // WAIT FOR ALL WORKERS TO FINISH
    // ═══════════════════════════════════════════════════
    thread_pool_wait(g_thread_pool);
    
    printf("[ENGINE] All chunks processed, building manifest...\n");
    
    // ═══════════════════════════════════════════════════
    // BUILD MANIFEST
    // ═══════════════════════════════════════════════════
    char* manifest_json = build_manifest(upload_session);
    // Result:
    // {
    //   "version": 1,
    //   "chunks": [
    //     {"index": 0, "size": 262144, "hash": "1220abc..."},
    //     {"index": 1, "size": 262144, "hash": "1220def..."},
    //     ...
    //   ]
    // }
    
    // ═══════════════════════════════════════════════════
    // COMPUTE CID
    // ═══════════════════════════════════════════════════
    char* cid = compute_cid(manifest_json);
    // CID = base32(SHA256(manifest_json))
    
    printf("[ENGINE] CID computed: %s\n", cid);
    
    // ═══════════════════════════════════════════════════
    // SAVE MANIFEST (ATOMIC!)
    // ═══════════════════════════════════════════════════
    save_manifest(cid, manifest_json);
    // 1. Write to manifests/<cid>.json.tmp
    // 2. fsync()
    // 3. rename() to manifests/<cid>.json  ← Atomic!
    
    // ═══════════════════════════════════════════════════
    // SEND CID BACK TO CLIENT
    // ═══════════════════════════════════════════════════
    send_frame(cfd, OP_UPLOAD_DONE, cid, strlen(cid));
    
    // Cleanup
    free(cid);
    free(manifest_json);
    upload_session_destroy(upload_session);
}
```

**Key steps:**
1. Wait for workers (can't build manifest without all hashes!)
2. Build JSON manifest
3. Hash manifest → CID
4. Save manifest atomically
5. Return CID to client

**Client receives:**
```
Frame:
[0x81][00 00 00 34][fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq]
  ↑        ↑                               ↑
  |        |                               └─ The CID!
  |        └─────────────────────────────── Length: 52 bytes
  └──────────────────────────────────────── Opcode: UPLOAD_DONE
```

---

## Part 5: Download Flow

### 5.1 Download Start

**Client sends:**
```
HTTP GET /download?cid=fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq
```

**Engine receives:**
```
Frame:
[0x11][00 00 00 34][fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq]
  ↑        ↑                               ↑
  |        |                               └─ CID
  |        └─────────────────────────────── Length: 52
  └──────────────────────────────────────── Opcode: DOWNLOAD_START
```

**Handler:**
```c
else if (op == OP_DOWNLOAD_START) {
    // Extract CID
    char cid[256];
    memcpy(cid, payload, len);
    cid[len] = '\0';
    
    printf("[ENGINE] DOWNLOAD_START: cid=\"%s\"\n", cid);
    
    // ═══════════════════════════════════════════════════
    // VALIDATE CID
    // ═══════════════════════════════════════════════════
    if (!is_valid_cid(cid)) {
        send_error(cfd, E_BAD_CID, "Invalid CID format");
        continue;
    }
    
    // ═══════════════════════════════════════════════════
    // LOAD MANIFEST
    // ═══════════════════════════════════════════════════
    download_session_t* download_session = download_session_create(cid);
    // This function:
    // 1. Reads manifests/<cid>.json
    // 2. Parses JSON
    // 3. Extracts chunk list
    // 4. Allocates buffers for chunks
    
    if (!download_session) {
        send_error(cfd, E_NOT_FOUND, "CID not found");
        continue;
    }
    
    printf("[ENGINE] Found %zu chunks, submitting for verification...\n",
           download_session->total_chunks);
```

**After loading manifest:**
```c
download_session structure:
{
  cid: "fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq",
  total_chunks: 4,
  chunks: [
    {index: 0, size: 262144, hash: "1220abc..."},
    {index: 1, size: 262144, hash: "1220def..."},
    {index: 2, size: 262144, hash: "1220ghi..."},
    {index: 3, size: 100000, hash: "1220jkl..."}
  ],
  buffers: [NULL, NULL, NULL, NULL],  // Will be filled
  ready: [0, 0, 0, 0],                 // Will mark 1 when done
  next_index: 0
}
```

---

### 5.2 Submit Chunks for Verification
```c
    // ═══════════════════════════════════════════════════
    // SUBMIT ALL CHUNKS TO THREAD POOL (PARALLEL!)
    // ═══════════════════════════════════════════════════
    for (size_t i = 0; i < download_session->total_chunks; i++) {
        work_item_t work;
        work.type = WORK_LOAD_AND_VERIFY;
        work.session = download_session;
        work.index = i;
        work.data = NULL;  // No data, we'll load it
        work.expected_hash = download_session->chunks[i].hash;
        
        thread_pool_submit(g_thread_pool, work);
    }
```

**What happens in worker:**
```c
// Worker thread receives WORK_LOAD_AND_VERIFY
if (work.type == WORK_LOAD_AND_VERIFY) {
    // ═══════════════════════════════════════════
    // LOAD BLOCK FROM DISK
    // ═══════════════════════════════════════════
    size_t block_size;
    uint8_t* block_data = load_block(work.expected_hash, &block_size);
    // Loads from: blocks/12/20/1220abc...
    
    if (!block_data) {
        // Block not found!
        download_session->ready[work.index] = -1;  // Error
        pthread_cond_broadcast(&download_session->cond);
        return;
    }
    
    // ═══════════════════════════════════════════
    // VERIFY HASH
    // ═══════════════════════════════════════════
    char* computed_hash = compute_multihash(block_data, block_size);
    int hash_match = (strcmp(computed_hash, work.expected_hash) == 0);
    free(computed_hash);
    
    // ═══════════════════════════════════════════
    // STORE RESULT
    // ═══════════════════════════════════════════
    pthread_mutex_lock(&download_session->lock);
    
    if (hash_match) {
        // SUCCESS! Store verified chunk
        download_session->buffers[work.index] = block_data;
        download_session->sizes[work.index] = block_size;
        download_session->ready[work.index] = 1;  // ✓ Ready!
    } else {
        // HASH MISMATCH! Data corrupted!
        free(block_data);
        download_session->ready[work.index] = -1;  // ✗ Error!
    }
    
    // Wake up main thread: "A chunk is ready!"
    pthread_cond_broadcast(&download_session->cond);
    
    pthread_mutex_unlock(&download_session->lock);
}
```

**Parallel verification:**
```
Time →
Main:     [Submit all] [Wait for ready[0]]
Thread1:              [Load+Verify chunk 2] → ready[2]=1
Thread2:                     [Load+Verify chunk 0] → ready[0]=1
Thread3:                            [Load+Verify chunk 3] → ready[3]=1
Thread4:                                   [Load+Verify chunk 1] → ready[1]=1
Main:                                              [Continue]
```

Chunks verified OUT OF ORDER, but marked in ready[] array!

---

### 5.3 Sequential Streaming
```c
    // ═══════════════════════════════════════════════════
    // STREAM CHUNKS IN ORDER (0, 1, 2, 3, ...)
    // ═══════════════════════════════════════════════════
    for (size_t i = 0; i < download_session->total_chunks; i++) {
        pthread_mutex_lock(&download_session->lock);
        
        // ═══════════════════════════════════════════
        // WAIT FOR THIS SPECIFIC CHUNK
        // ═══════════════════════════════════════════
        while (download_session->ready[i] == 0) {
            // Not ready yet, wait...
            pthread_cond_wait(&download_session->cond, 
                             &download_session->lock);
        }
        
        // ═══════════════════════════════════════════
        // CHECK FOR ERRORS
        // ═══════════════════════════════════════════
        if (download_session->ready[i] < 0) {
            // Verification failed!
            pthread_mutex_unlock(&download_session->lock);
            send_error(cfd, E_HASH_MISMATCH, "Chunk verification failed");
            goto cleanup;
        }
        
        // ═══════════════════════════════════════════
        // SEND CHUNK TO CLIENT
        // ═══════════════════════════════════════════
        send_frame(cfd, OP_DOWNLOAD_CHUNK,
                   download_session->buffers[i],
                   download_session->sizes[i]);
        
        pthread_mutex_unlock(&download_session->lock);
    }
    
    // ═══════════════════════════════════════════════════
    // ALL CHUNKS SENT! SEND DONE
    // ═══════════════════════════════════════════════════
    send_frame(cfd, OP_DOWNLOAD_DONE, NULL, 0);
```

**Visual explanation:**
```
ready array: [0, 0, 0, 0]  (all waiting)

Worker 2 finishes chunk 2:
ready array: [0, 0, 1, 0]

Worker 1 finishes chunk 0:
ready array: [1, 0, 1, 0]

Main thread: Wait for ready[0]...
             ready[0] == 1? YES! Send chunk 0 ✓
             
Main thread: Wait for ready[1]...
             ready[1] == 0? Wait...
             
Worker 3 finishes chunk 1:
ready array: [1, 1, 1, 0]

Main thread: (woken up)
             ready[1] == 1? YES! Send chunk 1 ✓
             
Main thread: Wait for ready[2]...
             ready[2] == 1? YES! Send chunk 2 ✓
             
Main thread: Wait for ready[3]...
             ready[3] == 0? Wait...
             
Worker 4 finishes chunk 3:
ready array: [1, 1, 1, 1]

Main thread: (woken up)
             ready[3] == 1? YES! Send chunk 3 ✓
             
Main thread: All done! Send DOWNLOAD_DONE
```

**Key insight:**
Even though chunks verified out-of-order (2, 0, 1, 3), they're SENT in correct order (0, 1, 2, 3)!

---

## Part 6: Synchronization Deep Dive

### 6.1 Why Do We Need Synchronization?

**Scenario without synchronization:**
```
Two users upload at same time:

User A's thread:                User B's thread:
--------------                  --------------
Read chunk_count = 5            Read chunk_count = 5
Calculate index = 5             Calculate index = 5
Write to chunks[5]              Write to chunks[5]  ← COLLISION!
chunk_count = 6                 chunk_count = 6

Result: Both chunks written to same slot!
        One chunk is LOST forever!
```

**With mutex:**
```
User A's thread:                User B's thread:
--------------                  --------------
Lock mutex ✓                    Try lock mutex... BLOCKED
Read chunk_count = 5
Calculate index = 5
chunk_count = 6
Unlock mutex

                                Now gets lock ✓
                                Read chunk_count = 6
                                Calculate index = 6
                                chunk_count = 7
                                Unlock mutex

Result: A gets index 5, B gets index 6
        No collision! ✓
```

---

### 6.2 Mutex (Mutual Exclusion)

**What is a mutex?**
Think of it as a **single bathroom key**:
- Only ONE person can have the key at a time
- Others must wait in line
- When done, return key to next person

**Code:**
```c
pthread_mutex_t lock;
pthread_mutex_init(&lock, NULL);

// Person A
pthread_mutex_lock(&lock);     // Grab key
// Use bathroom (critical section)
pthread_mutex_unlock(&lock);   // Return key

// Person B
pthread_mutex_lock(&lock);     // Wait for key...
// Use bathroom
pthread_mutex_unlock(&lock);
```

**When to use:**
- Protecting shared data structures
- Ensuring only ONE thread modifies at a time

---

### 6.3 Reader-Writer Lock

**Problem with regular mutex:**
```
10 people want to READ phone book (no changes)
Mutex: All 10 must wait in line! 😞

But reading doesn't conflict!
Multiple people CAN read simultaneously!
```

**Reader-Writer lock:**
```
pthread_rwlock_t lock;

// Multiple readers OK!
pthread_rwlock_rdlock(&lock);  // Reader 1
pthread_rwlock_rdlock(&lock);  // Reader 2 (allowed!)
pthread_rwlock_rdlock(&lock);  // Reader 3 (allowed!)
// All reading simultaneously ✓

// Writer needs exclusive access
pthread_rwlock_wrlock(&lock);  // Writer (waits for readers...)
// Once all readers done, writer gets exclusive access
```

**Rules:**
- Multiple readers: ✓ Allowed
- Multiple writers: ✗ Only ONE
- Reader + writer: ✗ Exclusive

**Our use case (block table):**
```
Download (reader): Check if block exists
Download (reader): Check if block exists  } All simultaneous!
Download (reader): Check if block exists

Upload (writer): Add new block ← Waits for readers, then exclusive
```

---

### 6.4 Condition Variable

**Problem:**
```c
// Bad way (busy waiting):
while (chunk_not_ready) {
    // Keep checking... wastes CPU!
}
```

**Solution with condition variable:**
```c
pthread_cond_t cond;
pthread_mutex_t mutex;

// Thread A (producer):
pthread_mutex_lock(&mutex);
ready = 1;  // Make chunk ready
pthread_cond_signal(&cond);  // Wake up waiter!
pthread_mutex_unlock(&mutex);

// Thread B (consumer):
pthread_mutex_lock(&mutex);
while (ready == 0) {
    pthread_cond_wait(&cond, &mutex);  // Sleep until signaled
}
// Now ready == 1, proceed!
pthread_mutex_unlock(&mutex);
```

**How `pthread_cond_wait` works:**
```
1. Releases mutex
2. Goes to sleep
3. Waits for signal
4. Wakes up
5. Reacquires mutex
6. Returns
```

**Why release mutex?**
So other threads can make progress while we sleep!

---

### 6.5 The Download Assembler Pattern

This is the MOST COMPLEX synchronization in the project!

**Goal:** Stream chunks 0, 1, 2, 3... even if they complete 2, 0, 3, 1.

**Data structures:**
```c
int* ready;              // ready[i] = 0 (waiting), 1 (done), -1 (error)
pthread_mutex_t lock;    // Protects ready array
pthread_cond_t cond;     // Signal when chunk ready
```

**Worker thread (any order):**
```c
// Worker finishes chunk 2
pthread_mutex_lock(&lock);
ready[2] = 1;  // Mark chunk 2 ready
pthread_cond_broadcast(&cond);  // Wake up EVERYONE
pthread_mutex_unlock(&lock);
```

**Main thread (must be sequential):**
```c
// Want to send chunk 0
pthread_mutex_lock(&lock);
while (ready[0] == 0) {
    pthread_cond_wait(&cond, &lock);  // Sleep until signaled
}
// Now ready[0] == 1!
send_chunk(0);
pthread_mutex_unlock(&lock);

// Want to send chunk 1
pthread_mutex_lock(&lock);
while (ready[1] == 0) {
    pthread_cond_wait(&cond, &lock);
}
send_chunk(1);
pthread_mutex_unlock(&lock);

// And so on...
```

**Timeline:**
```
t=0: Main waits for ready[0]
t=1: Worker finishes chunk 2 → ready[2]=1, broadcast
     Main still waiting (ready[0] still 0)
t=2: Worker finishes chunk 0 → ready[0]=1, broadcast
     Main wakes up! Sends chunk 0 ✓
     Main now waits for ready[1]
t=3: Worker finishes chunk 3 → ready[3]=1, broadcast
     Main still waiting (ready[1] still 0)
t=4: Worker finishes chunk 1 → ready[1]=1, broadcast
     Main wakes up! Sends chunk 1 ✓
     Main now waits for ready[2]
     ready[2] already 1! Sends immediately ✓
     Main now waits for ready[3]
     ready[3] already 1! Sends immediately ✓
     Done!
```

**Why `broadcast` instead of `signal`?**
- `signal`: Wakes up ONE waiter
- `broadcast`: Wakes up ALL waiters

We use broadcast because we don't know which chunk will be ready next!

---

## Part 7: Common Questions

### Q1: Why split files into chunks?

**Without chunks:**
```
Upload 1GB file:
→ Read entire file into memory (1GB RAM!)
→ Hash entire file (slow!)
→ Save entire file
→ Download: load entire file, verify entire file
```

**With chunks:**
```
Upload 1GB file:
→ Split into 4096 chunks of 256KB each
→ Hash each chunk in parallel (8 threads → 8x faster!)
→ Can start downloading before upload finishes!
→ Only need 256KB in memory at a time
→ If one chunk corrupted, only redownload that chunk
```

---

### Q2: What is content-addressing?

**Traditional (location-based):**
```
File is at: /home/user/documents/photo.jpg
Problem: If file moves, link breaks!
```

**Content-addressed:**
```
File is identified by its hash (CID)
CID = fa2nv5xm2cr6ojkxnwpwtqyqhamwm4doinxfkzoo3akyjbmzgruq

Advantages:
- Same content = same CID (deduplication!)
- Can verify integrity (hash the file, check against CID)
- Location independent (doesn't matter where stored)
- Immutable (change content = different CID)
```

---

### Q3: Why use a thread pool instead of creating threads?

**Creating threads is EXPENSIVE:**
```
Time to create thread: ~1ms
Time to process chunk: ~10ms

Without pool:
  Create thread: 1ms
  Process: 10ms
  Destroy thread: 1ms
  Total: 12ms per chunk
  
With pool (threads already exist):
  Process: 10ms
  Total: 10ms per chunk
  
For 1000 chunks: Save 2000ms (2 seconds)!
```

---

### Q4: What is atomic operation?

**Non-atomic write:**
```
Process A:                  Process B (reading):
Write "Hello"               
                            Read "Hel" ← Incomplete!
Write " World"
```

**Atomic write (our manifest):**
```
Process A:
Write to temp file "manifest.json.tmp"
fsync() ← Flush to disk
rename("manifest.json.tmp", "manifest.json") ← Atomic!

Process B:
Open "manifest.json"
Either: Gets old version (before rename)
Or: Gets new version (after rename)
Never: Gets half-written file!
```

`rename()` is atomic on POSIX systems → all-or-nothing!

---

### Q5: How does deduplication work?

**Block table tracks refcounts:**
```
User A uploads file1.txt:
  Chunk: "Hello World"
  Hash: 1220abc...
  Block table: {"1220abc...": refcount=1}
  Save to: blocks/12/20/1220abc...

User B uploads file2.txt with same content:
  Chunk: "Hello World"
  Hash: 1220abc... (same!)
  Block table: {"1220abc...": refcount=2}
  DON'T save again! Block already exists!

Result: One block on disk, used by two files!
```

---

### Q6: Why use Unix Domain Socket instead of TCP?

**Unix Domain Socket:**
- Local only (same machine)
- Lower overhead (no network stack)
- Faster (~2x than TCP localhost)
- Safer (filesystem permissions)

**TCP:**
- Can connect over network
- More overhead
- Slower for local communication

For this project: Gateway and engine on same machine → Unix socket is better!

---

### Q7: What happens if two people upload at exactly the same time?

**Scenario:**
```
User A uploads file → Creates process → Connects to engine
User B uploads file → Creates process → Connects to engine

Engine accepts both connections:
- Connection 1 (User A) handled by thread 1
- Connection 2 (User B) handled by thread 2

Both threads use same thread pool:
- User A's chunks go to worker pool
- User B's chunks go to worker pool
- Workers process both users' chunks

Each user gets their own upload_session:
- User A: session_1
- User B: session_2
- No interference!

Block table protected by rwlock:
- If same chunk: Refcount incremented (dedup!)
- If different: Both saved

Result: Both uploads succeed! ✓
```

---

### Q8: How do you test if synchronization is correct?

**Race condition test:**
```python
# Launch 20 uploads simultaneously
for i in range(20):
    threading.Thread(target=upload_file, args=(i,)).start()

# If synchronization broken:
# - Some uploads fail
# - Some chunks lost
# - Segmentation faults
# - Data corruption

# If synchronization correct:
# - All 20 succeed
# - No crashes
# - All files verify correctly
```

We ran this test → All 20 passed! ✓

---

### Q9: What is a Merkle tree?

**Structure:**
```
                      Root Hash (CID)
                           |
                    Hash(Manifest)
                           |
           ┌───────────────┴───────────────┐
           |                               |
      Chunk 0 Hash                    Chunk 1 Hash
           |                               |
      [Data 0]                         [Data 1]
```

**Properties:**
- Change ANY data → chunk hash changes → manifest changes → CID changes
- Can verify integrity by checking hashes up the tree
- Don't need to download entire file to verify a chunk

---

### Q10: How does parallel verification but sequential streaming work?

**Key insight:** Two separate phases!

**Phase 1: Verification (parallel)**
```
Thread 1: Verify chunk 0 → Store in buffer[0]
Thread 2: Verify chunk 1 → Store in buffer[1]
Thread 3: Verify chunk 2 → Store in buffer[2]
Thread 4: Verify chunk 3 → Store in buffer[3]

All happening simultaneously!
Order doesn't matter for verification.
```

**Phase 2: Streaming (sequential)**
```
Main thread:
  Send buffer[0] ✓
  Send buffer[1] ✓
  Send buffer[2] ✓
  Send buffer[3] ✓

Always in order!
Client reconstructs file correctly.
```

**Benefit:**
- Fast verification (parallel)
- Correct assembly (sequential)
- Best of both worlds!

---

## Final Mental Model

Think of the system as a **factory**:

**Upload factory:**
```
Raw materials (file) arrive
  ↓
Cutting station: Split into chunks
  ↓
Assembly line: 8 workers hash chunks in parallel
  ↓
Quality control: Check if parts already in warehouse
  ↓
Warehouse: Store unique parts
  ↓
Documentation: Create manifest (parts list)
  ↓
Label: Compute CID
  ↓
Catalog: Save manifest
  ↓
Ship label (CID) to customer
```

**Download factory:**
```
Order arrives (CID)
  ↓
Lookup catalog: Find manifest
  ↓
Parts list: Read chunk hashes
  ↓
Warehouse: 8 workers fetch & verify parts in parallel
  ↓
Quality control: Check each part's hash
  ↓
Assembly line: Put parts together IN ORDER
  ↓
Ship completed product to customer
```

**Safety equipment (synchronization):**
- Mutex: Safety locks (one person at a time)
- RWLock: Multiple inspectors OK, one modifier
- Condition variables: Bell to wake workers
- Atomic operations: Tamper-proof packaging

---

## Congratulations!

You now understand:
- ✓ What every data structure does
- ✓ How hashing and CID work
- ✓ How thread pool processes work in parallel
- ✓ How upload flow works step-by-step
- ✓ How download maintains correct order
- ✓ Why synchronization is necessary
- ✓ How each synchronization primitive works
- ✓ The big picture of the entire system

**You're ready for your presentation/defense!** 🎓

---

**End of Complete Explanation**