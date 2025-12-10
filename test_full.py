import requests

print("=" * 60)
print("TEST 1: Upload small file")
print("=" * 60)

# Create test file
with open('test.txt', 'w') as f:
    f.write('Hello, Operating Systems Project!')

# Upload
with open('test.txt', 'rb') as f:
    data = f.read()

headers = {
    'X-Filename': 'test.txt',
    'Content-Length': str(len(data))
}

response = requests.post('http://127.0.0.1:9000/upload',
                         headers=headers,
                         data=data)

print(f"Upload Status: {response.status_code}")
print(f"Response: {response.text}")

cid = response.json()['cid']
print(f"CID: {cid}")

print("\n" + "=" * 60)
print("TEST 2: Download the file")
print("=" * 60)

# Download
response = requests.get(f'http://127.0.0.1:9000/download?cid={cid}')
print(f"Download Status: {response.status_code}")
print(f"Downloaded {len(response.content)} bytes")
print(f"Content: {response.content.decode()}")

# Verify
original = open('test.txt', 'rb').read()
downloaded = response.content

if original == downloaded:
    print("\n✅ SUCCESS! Downloaded file matches original!")
else:
    print("\n❌ FAIL! Downloaded file doesn't match!")

print("\n" + "=" * 60)
print("TEST 3: Upload larger file (multiple chunks)")
print("=" * 60)

# Create 300KB file (will be 2 chunks)
with open('large.bin', 'wb') as f:
    f.write(b'X' * (300 * 1024))

with open('large.bin', 'rb') as f:
    data = f.read()

headers = {
    'X-Filename': 'large.bin',
    'Content-Length': str(len(data))
}

response = requests.post('http://127.0.0.1:9000/upload',
                         headers=headers,
                         data=data)

print(f"Upload Status: {response.status_code}")
cid_large = response.json()['cid']
print(f"CID: {cid_large}")

# Download
response = requests.get(f'http://127.0.0.1:9000/download?cid={cid_large}')
print(f"Download Status: {response.status_code}")
print(f"Downloaded {len(response.content)} bytes")

# Verify
original_large = open('large.bin', 'rb').read()
if original_large == response.content:
    print("✅ SUCCESS! Large file matches!")
else:
    print("❌ FAIL! Large file doesn't match!")

print("\n" + "=" * 60)
print("ALL TESTS COMPLETE!")
print("=" * 60)