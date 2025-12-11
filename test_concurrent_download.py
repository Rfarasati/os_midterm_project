import requests
import threading
import time

# First, upload a file
print("Uploading test file...")
with open('shared.txt', 'w') as f:
    f.write('Shared content for concurrent downloads!')

with open('shared.txt', 'rb') as f:
    data = f.read()

headers = {
    'X-Filename': 'shared.txt',
    'Content-Length': str(len(data))
}

response = requests.post('http://127.0.0.1:9000/upload',
                         headers=headers,
                         data=data)
cid = response.json()['cid']
print(f"Uploaded, CID: {cid}")

print("\n" + "=" * 60)
print("CONCURRENT DOWNLOAD TEST: 10 simultaneous downloads")
print("=" * 60)

def download_file(thread_num):
    start = time.time()
    response = requests.get(f'http://127.0.0.1:9000/download?cid={cid}')
    elapsed = time.time() - start

    success = response.content == data
    status = "✅" if success else "❌"
    print(f"Thread {thread_num}: {status} Status {response.status_code}, Size: {len(response.content)}, Time: {elapsed:.2f}s")

threads = []
for i in range(10):
    t = threading.Thread(target=download_file, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

print("\n✅ All concurrent downloads completed!")