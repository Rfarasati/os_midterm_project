# IPFS-lite — Content-Addressed File Storage

> **Course project** — Operating Systems, Ferdowsi University of Mashhad, Fall 2025 (Midterm)

A small clone of the core idea behind [IPFS](https://ipfs.tech/): files are stored and
retrieved by the **hash of their content** (a CID) rather than by a file path, with chunking
and multi-threaded processing at the core. Built to explore multithreading, synchronization,
and inter-process communication in C.

## Why content-addressing + chunking

Storing a file by content means two files with identical bytes get the exact same ID and are
only ever stored once (deduplication), and the ID stays valid regardless of where the file
physically lives. Naively, that means hashing the whole file before you can start saving or
serving it — slow for large files, and no streaming.

Instead, every file is split into 256 KB **chunks**. Chunks are hashed and stored in parallel
across a thread pool, so an 8-thread pool hashes a file roughly 8x faster than doing it
serially, and — for the same reason — a download can start streaming as soon as the first
chunk is ready rather than waiting for the whole file.

## Architecture

```
Client (Postman/curl) --HTTP--> main.py (gateway, :9000) --Unix socket--> c_engine (storage core)
```

- **`c_engine.c`** — the actual storage engine: chunk hashing, content-addressed block
  storage, manifest/CID computation, a thread pool, and session management for
  uploads/downloads. Speaks a small binary framing protocol over a Unix domain socket
  (`/tmp/cengine.sock`).
- **`main.py`** — an HTTP gateway (`ThreadingHTTPServer` on port 9000) exposing `/upload`
  and `/download`; translates HTTP requests into the engine's binary protocol and back.
- **`storage_gui/gui.py`** — a [Streamlit](https://streamlit.io/) UI for uploading/downloading
  files without needing Postman.

### How a chunk gets its address

- **Hashing** (`compute_multihash`): each chunk is SHA-256-hashed (the project spec suggested
  BLAKE3, but SHA-256 was used instead — simpler and more widely available, with no real
  impact on the project's actual learning goals: concurrency, thread/process management, and
  IPC) and prefixed with `1220` (a multihash-style tag identifying the hash algorithm).
- **Base32 encoding** (`base32_encode`): turns hash bytes into a readable string, skipping
  the characters `0`, `1`, `o`, `I` since they're easy to confuse with each other.
- **Block storage** (`save_block` / `get_block_path`): a chunk is saved at
  `blocks/<first-2-hex-chars>/<next-2-hex-chars>/<hash>` — the two extra directory levels
  keep any single directory from accumulating too many files. If a block with that hash
  already exists, it isn't written again — only a reference count is bumped, so uploading
  the same content twice costs no extra disk I/O.
- **CID** (`compute_cid`): once every chunk is hashed and saved, a manifest (chunk list +
  metadata) is serialized to JSON, hashed, and base32-encoded — that's the file's CID.
  Manifests are saved atomically (written to a `.tmp` file, then renamed) under `manifests/`,
  and are what a download reconstructs the file from.

### Concurrency

- A **global block table** (hash -> refcount) tracks which chunks already exist, guarded by
  a read-write lock so concurrent uploads can't double-save or corrupt the table.
- A fixed **8-thread pool** processes chunk work (hash + save on upload, load + verify on
  download) instead of spawning a thread per chunk. Workers block on a condition variable
  until work is queued.
- Each upload/download gets its own **session** (`upload_session_t` / `download_session_t`,
  each with its own mutex) tracking chunk state, so concurrent transfers don't interfere with
  each other. On download, chunks are verified against their recorded hash in parallel, then
  streamed back to the client **in order** — each chunk waits on a condition variable until
  the slot ahead of it is marked ready.

## Tested scenarios

`final_test_suite.py` runs (and passed) test scripts covering: concurrent uploads of
different files, concurrent downloads of the same file, deduplication (two identical files
under different names produce the same CID and don't double-store blocks), a 2 MB file
round-tripped with matching hashes, 20 simultaneous multi-chunk uploads under load (no
deadlocks, crashes, or corruption), and error handling (invalid/missing CID, missing
filename, etc).

## Tech stack

C (pthreads, OpenSSL's SHA-256), Python (`http.server` gateway, Streamlit GUI), Unix domain
sockets for engine<->gateway IPC.

## Running it

```bash
# Terminal 1 — build and start the storage engine
make
./c_engine /tmp/cengine.sock

# Terminal 2 — start the HTTP gateway
python3 main.py
# -> listening on http://127.0.0.1:9000

# Terminal 3 (optional) — GUI
cd storage_gui && streamlit run gui.py
```

`POST /upload` (raw file body, `X-Filename` header) returns `{"cid": "..."}`;
`GET /download?cid=<cid>` returns the reconstructed file content.

Full implementation write-up (in Persian): [project-report.pdf](project-report.pdf).

## Team

Built as a 2-person team project:

- Reza Farasati — [@Rfarasati](https://github.com/Rfarasati)
- Negar Bahrampoor — [@negarbahram](https://github.com/negarbahram)
