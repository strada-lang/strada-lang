/*
 This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 Copyright (c) 2026 Michael J. Flickinger

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 2.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * strada_crypt.c - Password hashing wrapper for Strada extern "C"
 *
 * This library wraps the C crypt.h functions for secure password hashing.
 * Functions take raw C types (char*, int) for use with extern "C".
 *
 * Compile as object file:
 *   gcc -c -o strada_crypt.o strada_crypt.c -lcrypt
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <crypt.h>

/* Last error message */
static char last_error[256] = "";

/* Base64-like alphabet for salt generation */
static const char salt_chars[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Generate random bytes from /dev/urandom */
static int get_random_bytes(unsigned char *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        snprintf(last_error, sizeof(last_error), "Failed to open /dev/urandom");
        return -1;
    }

    ssize_t n = read(fd, buf, len);
    close(fd);

    if (n != (ssize_t)len) {
        snprintf(last_error, sizeof(last_error), "Failed to read random bytes");
        return -1;
    }

    return 0;
}

/* Generate random salt characters */
static void generate_salt_chars(char *buf, size_t len) {
    unsigned char random[64];
    if (get_random_bytes(random, len) < 0) {
        /* Fallback to less secure random */
        srand(time(NULL) ^ getpid());
        for (size_t i = 0; i < len; i++) {
            random[i] = rand() & 0xFF;
        }
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = salt_chars[random[i] % 64];
    }
    buf[len] = '\0';
}

/* Internal: generate salt for algorithm (returns static buffer) */
static const char* gen_salt_internal(const char *algorithm, int rounds) {
    static char salt[128];
    char random_chars[32];

    last_error[0] = '\0';

    if (!algorithm) {
        algorithm = "sha512";
    }

    if (strcmp(algorithm, "des") == 0) {
        generate_salt_chars(random_chars, 2);
        snprintf(salt, sizeof(salt), "%s", random_chars);
    }
    else if (strcmp(algorithm, "md5") == 0) {
        generate_salt_chars(random_chars, 8);
        snprintf(salt, sizeof(salt), "$1$%s$", random_chars);
    }
    else if (strcmp(algorithm, "sha256") == 0) {
        generate_salt_chars(random_chars, 16);
        if (rounds > 0) {
            if (rounds < 1000) rounds = 1000;
            if (rounds > 999999999) rounds = 999999999;
            snprintf(salt, sizeof(salt), "$5$rounds=%d$%s$", rounds, random_chars);
        } else {
            snprintf(salt, sizeof(salt), "$5$%s$", random_chars);
        }
    }
    else if (strcmp(algorithm, "sha512") == 0) {
        generate_salt_chars(random_chars, 16);
        if (rounds > 0) {
            if (rounds < 1000) rounds = 1000;
            if (rounds > 999999999) rounds = 999999999;
            snprintf(salt, sizeof(salt), "$6$rounds=%d$%s$", rounds, random_chars);
        } else {
            snprintf(salt, sizeof(salt), "$6$%s$", random_chars);
        }
    }
    else if (strcmp(algorithm, "bcrypt") == 0) {
        generate_salt_chars(random_chars, 22);
        int cost = (rounds > 0) ? rounds : 12;
        if (cost < 4) cost = 4;
        if (cost > 31) cost = 31;
        snprintf(salt, sizeof(salt), "$2b$%02d$%s", cost, random_chars);
    }
    else {
        snprintf(last_error, sizeof(last_error), "Unknown algorithm: %s", algorithm);
        salt[0] = '\0';
    }

    return salt;
}

/* Hash a password with the given salt
 * Returns pointer to static buffer (do not free) */
const char* strada_crypt_hash(const char *password, const char *salt) {
    static char result_buf[256];
    last_error[0] = '\0';

    if (!password || !salt) {
        snprintf(last_error, sizeof(last_error), "Invalid password or salt");
        return "";
    }

    struct crypt_data data;
    memset(&data, 0, sizeof(data));

    char *result = crypt_r(password, salt, &data);

    if (!result) {
        snprintf(last_error, sizeof(last_error), "crypt() failed");
        return "";
    }

    if (result[0] == '*') {
        snprintf(last_error, sizeof(last_error), "Invalid salt or algorithm not supported");
        return "";
    }

    strncpy(result_buf, result, sizeof(result_buf) - 1);
    result_buf[sizeof(result_buf) - 1] = '\0';
    return result_buf;
}

/* Generate a salt for the specified algorithm */
const char* strada_crypt_gen_salt(const char *algorithm) {
    return gen_salt_internal(algorithm, 0);
}

/* Generate a salt with custom rounds */
const char* strada_crypt_gen_salt_rounds(const char *algorithm, int rounds) {
    return gen_salt_internal(algorithm, rounds);
}

/* Verify a password against a hash
 * Returns 1 if match, 0 otherwise */
int strada_crypt_verify(const char *password, const char *hash) {
    last_error[0] = '\0';

    if (!password || !hash || strlen(hash) == 0) {
        return 0;
    }

    struct crypt_data data;
    memset(&data, 0, sizeof(data));

    char *result = crypt_r(password, hash, &data);

    if (!result) {
        return 0;
    }

    /* Constant-time comparison to prevent timing attacks */
    size_t hash_len = strlen(hash);
    size_t result_len = strlen(result);

    if (hash_len != result_len) {
        return 0;
    }

    int diff = 0;
    for (size_t i = 0; i < hash_len; i++) {
        diff |= hash[i] ^ result[i];
    }

    return (diff == 0) ? 1 : 0;
}

/* Get the algorithm used in a hash string */
const char* strada_crypt_get_algorithm(const char *hash) {
    if (!hash || strlen(hash) < 3) {
        return "unknown";
    }

    if (hash[0] != '$') {
        return "des";
    }

    if (strncmp(hash, "$1$", 3) == 0) {
        return "md5";
    }
    else if (strncmp(hash, "$5$", 3) == 0) {
        return "sha256";
    }
    else if (strncmp(hash, "$6$", 3) == 0) {
        return "sha512";
    }
    else if (strncmp(hash, "$2a$", 4) == 0 ||
             strncmp(hash, "$2b$", 4) == 0 ||
             strncmp(hash, "$2y$", 4) == 0) {
        return "bcrypt";
    }
    else if (strncmp(hash, "$y$", 3) == 0) {
        return "yescrypt";
    }

    return "unknown";
}

/* Check if an algorithm is supported
 * Returns 1 if supported, 0 otherwise */
int strada_crypt_is_supported(const char *algorithm) {
    if (!algorithm) {
        return 0;
    }

    const char *salt = gen_salt_internal(algorithm, 0);

    if (strlen(salt) == 0) {
        return 0;
    }

    struct crypt_data data;
    memset(&data, 0, sizeof(data));

    char *result = crypt_r("test", salt, &data);

    if (!result || result[0] == '*') {
        return 0;
    }

    return 1;
}

/* Get last error message */
const char* strada_crypt_error(void) {
    return last_error;
}
