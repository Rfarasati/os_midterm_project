import requests

print("=" * 60)
print("ERROR HANDLING TESTS")
print("=" * 60)

# Test 1: Download non-existent CID
print("\nTest 1: Download non-existent CID")
response = requests.get('http://127.0.0.1:9000/download?cid=nonexistent123')
print(f"Status: {response.status_code}")
if response.status_code == 502:
    print("✅ Correctly returns error for non-existent CID")
else:
    print(f"Response: {response.text}")

# Test 2: Upload without filename header
print("\nTest 2: Upload without X-Filename header")
try:
    response = requests.post('http://127.0.0.1:9000/upload',
                             data=b'test',
                             headers={'Content-Length': '4'})
    print(f"Status: {response.status_code}")
    if response.status_code == 400:
        print("✅ Correctly rejects upload without filename")
except Exception as e:
    print(f"Error (expected): {e}")

# Test 3: Download with empty CID
print("\nTest 3: Download with empty CID")
response = requests.get('http://127.0.0.1:9000/download?cid=')
print(f"Status: {response.status_code}")
if response.status_code == 400:
    print("✅ Correctly rejects empty CID")

print("\n" + "=" * 60)
print("ERROR TESTS COMPLETE")
print("=" * 60)