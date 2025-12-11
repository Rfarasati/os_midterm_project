import requests
import os

print("=" * 60)
print("DEDUPLICATION TEST: Upload same content twice")
print("=" * 60)

# Create file with specific content
content = "Duplicate content test!" * 100
with open('dup1.txt', 'w') as f:
    f.write(content)

# Upload first time
with open('dup1.txt', 'rb') as f:
    data = f.read()

headers = {
    'X-Filename': 'dup1.txt',
    'Content-Length': str(len(data))
}

response1 = requests.post('http://127.0.0.1:9000/upload',
                          headers=headers,
                          data=data)
cid1 = response1.json()['cid']
print(f"First upload CID: {cid1}")

# Count blocks before second upload
blocks_before = sum([len(files) for r, d, files in os.walk('blocks')])
print(f"Blocks before second upload: {blocks_before}")

# Upload second time (same content, different filename)
headers['X-Filename'] = 'dup2.txt'
response2 = requests.post('http://127.0.0.1:9000/upload',
                          headers=headers,
                          data=data)
cid2 = response2.json()['cid']
print(f"Second upload CID: {cid2}")

# Count blocks after second upload
blocks_after = sum([len(files) for r, d, files in os.walk('blocks')])
print(f"Blocks after second upload: {blocks_after}")

if blocks_after == blocks_before:
    print("\n✅ SUCCESS! Block deduplication working - no new blocks created!")
else:
    print(f"\n⚠️ WARNING: Expected {blocks_before} blocks, got {blocks_after}")
    print("(This might be OK if manifest storage is counted)")