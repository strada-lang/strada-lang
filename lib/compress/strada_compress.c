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
 * strada_compress.c - Compression library for Strada using zlib
 *
 * Provides gzip and deflate compression via zlib.
 * Build: gcc -c -o strada_compress.o strada_compress.c -lz
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Store last output length for binary data */
static size_t last_output_len = 0;

/* Get the length of the last compression/decompression output */
size_t strada_compress_last_len(void) {
    return last_output_len;
}

/* Compress data using gzip format
 * Returns pointer to allocated buffer (caller must free via c::free)
 * Output length available via strada_compress_last_len() */
void* strada_gzip_compress(const char* input, size_t input_len) {
    last_output_len = 0;

    if (!input || input_len == 0) {
        return NULL;
    }

    /* Allocate output buffer (worst case: input + gzip overhead) */
    size_t output_size = compressBound(input_len) + 18;
    char* output = malloc(output_size);
    if (!output) {
        return NULL;
    }

    /* Initialize zlib stream for gzip (windowBits = 15 + 16 for gzip) */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        free(output);
        return NULL;
    }

    strm.next_in = (Bytef*)input;
    strm.avail_in = input_len;
    strm.next_out = (Bytef*)output;
    strm.avail_out = output_size;

    ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        free(output);
        return NULL;
    }

    last_output_len = strm.total_out;
    return output;
}

/* Compress data using deflate format (no header) */
void* strada_deflate_compress(const char* input, size_t input_len) {
    last_output_len = 0;

    if (!input || input_len == 0) {
        return NULL;
    }

    size_t output_size = compressBound(input_len);
    char* output = malloc(output_size);
    if (!output) {
        return NULL;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        free(output);
        return NULL;
    }

    strm.next_in = (Bytef*)input;
    strm.avail_in = input_len;
    strm.next_out = (Bytef*)output;
    strm.avail_out = output_size;

    ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        free(output);
        return NULL;
    }

    last_output_len = strm.total_out;
    return output;
}

/* Decompress gzip data */
void* strada_gzip_decompress(const char* input, size_t input_len) {
    last_output_len = 0;

    if (!input || input_len == 0) {
        return NULL;
    }

    /* Start with 4x input size, grow if needed */
    size_t output_size = input_len * 4;
    if (output_size < 1024) output_size = 1024;
    char* output = malloc(output_size);
    if (!output) {
        return NULL;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    int ret = inflateInit2(&strm, 15 + 16);
    if (ret != Z_OK) {
        free(output);
        return NULL;
    }

    strm.next_in = (Bytef*)input;
    strm.avail_in = input_len;
    strm.next_out = (Bytef*)output;
    strm.avail_out = output_size;

    while (1) {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&strm);
            free(output);
            return NULL;
        }
        if (strm.avail_out == 0) {
            size_t new_size = output_size * 2;
            char* new_output = realloc(output, new_size);
            if (!new_output) {
                inflateEnd(&strm);
                free(output);
                return NULL;
            }
            output = new_output;
            strm.next_out = (Bytef*)(output + output_size);
            strm.avail_out = new_size - output_size;
            output_size = new_size;
        }
    }

    inflateEnd(&strm);
    last_output_len = strm.total_out;
    return output;
}

/* Check if gzip compression is worthwhile (returns 1 if yes) */
int strada_should_compress(const char* content_type, size_t data_len) {
    if (!content_type) {
        return 0;
    }

    /* Don't compress if too small (< 1KB) */
    if (data_len < 1024) {
        return 0;
    }

    /* Compress text-based content types */
    if (strstr(content_type, "text/") ||
        strstr(content_type, "application/json") ||
        strstr(content_type, "application/javascript") ||
        strstr(content_type, "application/xml") ||
        strstr(content_type, "application/xhtml") ||
        strstr(content_type, "+xml") ||
        strstr(content_type, "+json")) {
        return 1;
    }

    /* Don't compress already-compressed formats */
    if (strstr(content_type, "image/") ||
        strstr(content_type, "video/") ||
        strstr(content_type, "audio/") ||
        strstr(content_type, "application/zip") ||
        strstr(content_type, "application/gzip") ||
        strstr(content_type, "application/x-gzip")) {
        return 0;
    }

    return 0;
}
