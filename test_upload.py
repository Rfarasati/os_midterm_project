import requests

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

print("Status:", response.status_code)
print("Response:", response.text)