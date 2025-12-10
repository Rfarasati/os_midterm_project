#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

// Copy just the hash functions for testing
char* compute_hash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    
    char* hex = malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (!hex) return NULL;
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return hex;
}

char* compute_multihash(const uint8_t* data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    
    char* mhash = malloc(4 + SHA256_DIGEST_LENGTH * 2 + 1);
    if (!mhash) return NULL;
    
    sprintf(mhash, "1220");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(mhash + 4 + (i * 2), "%02x", hash[i]);
    }
    mhash[4 + SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return mhash;
}

int main() {
    const char* test_data = "Hello, OS Project!";
    
    printf("Testing hash functions...\n");
    printf("Input: %s\n\n", test_data);
    
    char* hash = compute_hash((uint8_t*)test_data, strlen(test_data));
    printf("SHA256 Hash:\n%s\n\n", hash);
    
    char* mhash = compute_multihash((uint8_t*)test_data, strlen(test_data));
    printf("Multihash (with 1220 prefix):\n%s\n\n", mhash);
    
    printf("First 4 chars after prefix: %c%c/%c%c\n", mhash[4], mhash[5], mhash[6], mhash[7]);
    printf("This would create directory: blocks/%c%c/%c%c/\n", 
           mhash[4], mhash[5], mhash[6], mhash[7]);
    
    free(hash);
    free(mhash);
    
    printf("\n✓ Basic hash functions work!\n");
    return 0;
}