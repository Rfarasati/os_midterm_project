import requests
import hashlib

print("=" * 60)
print("LARGE FILE TEST: 2MB file (8 chunks)")
print("=" * 60)

# Create 2MB file
size_mb = 2
size_bytes = size_mb * 1024 * 1024

print(f"Creating {size_mb}MB file...")
with open('large_test.bin', 'wb') as f:
    f.write(b'A' * size_bytes)

# Calculate hash of original
with open('large_test.bin', 'rb') as f:
    original_data = f.read()
    original_hash = hashlib.sha256(original_data).hexdigest()

print(f"Original file hash: {original_hash[:16]}...")

# Upload
print("Uploading...")
with open('large_test.bin', 'rb') as f:
    data = f.read()

headers = {
    'X-Filename': 'large_test.bin',
    'Content-Length': str(len(data))
}

response = requests.post('http://127.0.0.1:9000/upload',
                         headers=headers,
                         data=data)

if response.status_code == 200:
    cid = response.json()['cid']
    print(f"✅ Upload successful! CID: {cid[:40]}...")

    # Download
    print("Downloading...")
    response = requests.get(f'http://127.0.0.1:9000/download?cid={cid}')

    if response.status_code == 200:
        downloaded_hash = hashlib.sha256(response.content).hexdigest()
        print(f"Downloaded file hash: {downloaded_hash[:16]}...")

        if original_hash == downloaded_hash:
            print("\n✅ SUCCESS! Large file integrity verified!")
            print(f"   Size: {len(response.content):,} bytes")
            print(f"   Expected chunks: ~{size_bytes // (256*1024) + (1 if size_bytes % (256*1024) else 0)}")
        else:
            print("\n❌ FAIL! Hash mismatch!")
    else:
        print(f"\n❌ Download failed: {response.status_code}")
else:
    print(f"\n❌ Upload failed: {response.status_code}")