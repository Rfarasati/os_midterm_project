import requests
import threading
import time

def upload_file(file_num):
    # Create unique file
    content = f"File {file_num}: " + "X" * 1000
    filename = f"file_{file_num}.txt"

    with open(filename, 'w') as f:
        f.write(content)

    # Upload
    with open(filename, 'rb') as f:
        data = f.read()

    headers = {
        'X-Filename': filename,
        'Content-Length': str(len(data))
    }

    start = time.time()
    response = requests.post('http://127.0.0.1:9000/upload',
                             headers=headers,
                             data=data)
    elapsed = time.time() - start

    print(f"Thread {file_num}: Status {response.status_code}, CID: {response.json()['cid'][:20]}..., Time: {elapsed:.2f}s")

print("=" * 60)
print("CONCURRENT UPLOAD TEST: 5 files simultaneously")
print("=" * 60)

threads = []
for i in range(5):
    t = threading.Thread(target=upload_file, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

print("\n✅ All concurrent uploads completed!")