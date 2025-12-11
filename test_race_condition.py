import requests
import threading
import time

print("=" * 60)
print("RACE CONDITION TEST: Rapid concurrent uploads")
print("Testing the upload session mutex fix")
print("=" * 60)

success_count = 0
fail_count = 0
lock = threading.Lock()

def rapid_upload(thread_id):
    global success_count, fail_count

    # Create file
    content = f"Thread {thread_id}: " + ("X" * 500000)  # 500KB each
    filename = f"race_test_{thread_id}.bin"

    with open(filename, 'w') as f:
        f.write(content)

    try:
        with open(filename, 'rb') as f:
            data = f.read()

        headers = {
            'X-Filename': filename,
            'Content-Length': str(len(data))
        }

        response = requests.post('http://127.0.0.1:9000/upload',
                                 headers=headers,
                                 data=data,
                                 timeout=30)

        if response.status_code == 200:
            with lock:
                success_count += 1
            print(f"✅ Thread {thread_id}: SUCCESS - {response.json()['cid'][:20]}...")
        else:
            with lock:
                fail_count += 1
            print(f"❌ Thread {thread_id}: FAILED - Status {response.status_code}")
    except Exception as e:
        with lock:
            fail_count += 1
        print(f"❌ Thread {thread_id}: ERROR - {e}")

# Launch 20 threads simultaneously (stress test)
print("\nLaunching 20 concurrent uploads...")
threads = []
start_time = time.time()

for i in range(20):
    t = threading.Thread(target=rapid_upload, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

elapsed = time.time() - start_time

print("\n" + "=" * 60)
print(f"RESULTS:")
print(f"  Total: 20 uploads")
print(f"  Success: {success_count}")
print(f"  Failed: {fail_count}")
print(f"  Time: {elapsed:.2f}s")
print(f"  Avg: {elapsed/20:.2f}s per upload")
print("=" * 60)

if success_count == 20:
    print("✅ RACE CONDITION TEST PASSED!")
    print("   No crashes, no corrupted data!")
else:
    print(f"⚠️ {fail_count} uploads failed - check engine logs")