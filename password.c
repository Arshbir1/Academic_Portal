#include "common.h"
#include <crypt.h>

/*
 * Password hashing with the system's libcrypt (libxcrypt on Ubuntu), the same
 * library that hashes the passwords in /etc/shadow. No cryptography is
 * implemented here.
 *
 * crypt_gensalt_rn(NULL, 0, NULL, 0, ...) chooses libcrypt's preferred method
 * (yescrypt, "$y$") at its default cost and fills a random salt from the OS.
 * crypt_rn() then produces a self-describing string:
 *
 *     $y$j9T$<salt>$<hash>      method, cost parameters, salt, hash
 *
 * which is everything needed to verify a password later.
 *
 * These calls are deliberately slow (~10-20 ms). Callers must not hold data_mutex.
 * struct crypt_data is ~32 KB of scratch space; it lives on the calling thread's
 * stack, so concurrent calls never share state.
 */

#if !CRYPT_GENSALT_IMPLEMENTS_DEFAULT_PREFIX || !CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY
#error "libcrypt must choose the default method and generate salts itself (libxcrypt >= 4.1)"
#endif

// Hash password with a fresh random salt into hash (hash_size bytes, NUL-padded).
// Returns 0 on success, -1 on error. Never logs the password or the hash.
int hash_password(const char *password, char *hash, size_t hash_size) {
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    if (crypt_gensalt_rn(NULL, 0, NULL, 0, setting, sizeof(setting)) == NULL) {
        log_error("crypt_gensalt_rn failed");
        return -1;
    }

    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    const char *result = crypt_rn(password, setting, &data, sizeof(data));
    int rc = -1;
    if (result == NULL) {
        log_error("crypt_rn failed");
    } else if (strlen(result) >= hash_size) {
        log_message("Password hash is longer than the storage field");
    } else {
        memset(hash, 0, hash_size);
        memcpy(hash, result, strlen(result));
        rc = 0;
    }
    explicit_bzero(&data, sizeof(data)); // scrub intermediate values derived from the password
    return rc;
}

// Compare every byte instead of stopping at the first difference, so response
// time does not reveal how much of a computed hash matched.
static int equal_constant_time(const char *a, const char *b) {
    size_t len = strlen(b);
    if (strlen(a) != len) {
        return 0; // hash length is fixed by the method; it is not secret
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Returns 1 if password matches stored_hash. Hashing the password with the stored
// string as the "setting" reuses its method, cost and salt; the result must then
// equal the stored string exactly.
int verify_password(const char *password, const char *stored_hash) {
    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    const char *result = crypt_rn(password, stored_hash, &data, sizeof(data));
    int match = result != NULL && equal_constant_time(result, stored_hash);
    explicit_bzero(&data, sizeof(data));
    return match;
}
