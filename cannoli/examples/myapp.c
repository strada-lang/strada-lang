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
 * myapp.c - Example Cannoli application as a shared library
 *
 * This demonstrates how to create a dynamic handler library for Cannoli.
 * Compile with:
 *   gcc -shared -fPIC -o myapp.so myapp.c
 *
 * Use with:
 *   cannoli --library ./myapp.so --dev -p 8080
 *
 * Dispatch function signature:
 *   cannoli_dispatch(method, path, path_info, body) -> response
 *
 * - method: HTTP method (GET, POST, etc.)
 * - path: The full request path
 * - path_info: For prefix matches, the remainder after the matched prefix
 *              e.g., if "/api" matches "/api/users/123", path_info is "/users/123"
 * - body: Request body for POST/PUT requests
 *
 * Return values:
 * - Response body string (auto-detects JSON if starts with { or [)
 * - Empty string "" for 404
 * - "STATUS:xxx:content" to specify status code (e.g., "STATUS:201:Created")
 * - "REDIRECT:url" for redirects
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <regex.h>

/* Forward declaration - defined in Strada's runtime */
typedef struct StradaValue StradaValue;
extern const char* strada_to_str(StradaValue* v);

/* Helper: check if path starts with prefix */
static int starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/* Helper: get path_info (remainder after prefix) */
static const char* get_path_info(const char* path, const char* prefix) {
    size_t prefix_len = strlen(prefix);
    if (strlen(path) <= prefix_len) return "";
    return path + prefix_len;
}

/* Helper: simple regex match */
static int regex_match(const char* pattern, const char* str, char* captures[], int max_captures) {
    regex_t regex;
    regmatch_t matches[10];
    int ret;

    ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret) return 0;

    ret = regexec(&regex, str, 10, matches, 0);
    regfree(&regex);

    if (ret == 0) {
        /* Extract captures */
        for (int i = 1; i < 10 && i <= max_captures && matches[i].rm_so >= 0; i++) {
            int len = matches[i].rm_eo - matches[i].rm_so;
            captures[i-1] = malloc(len + 1);
            strncpy(captures[i-1], str + matches[i].rm_so, len);
            captures[i-1][len] = '\0';
        }
        return 1;
    }
    return 0;
}

/* The dispatch function */
const char* cannoli_dispatch(StradaValue* method_sv, StradaValue* path_sv,
                             StradaValue* path_info_sv, StradaValue* body_sv) {
    static char response[8192];
    char* captures[10] = {0};

    const char* method = strada_to_str(method_sv);
    const char* path = strada_to_str(path_sv);
    const char* path_info = strada_to_str(path_info_sv);
    const char* body = strada_to_str(body_sv);

    /* ===== EXACT ROUTES ===== */

    /* GET / - Home page */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        snprintf(response, sizeof(response),
            "<!DOCTYPE html>\n"
            "<html><head><title>My App</title></head>\n"
            "<body>\n"
            "<h1>Welcome to My App!</h1>\n"
            "<p>Dynamic library with advanced routing.</p>\n"
            "<h2>Routes:</h2>\n"
            "<ul>\n"
            "  <li><a href=\"/hello\">/hello</a> - Simple text</li>\n"
            "  <li><a href=\"/api/status\">/api/status</a> - JSON status</li>\n"
            "  <li><a href=\"/api/users/123\">/api/users/123</a> - Prefix match with path_info</li>\n"
            "  <li><a href=\"/user/42\">/user/{id}</a> - Regex capture</li>\n"
            "  <li><a href=\"/files/docs/readme.txt\">/files/...</a> - Path remainder</li>\n"
            "</ul>\n"
            "</body></html>\n"
        );
        return response;
    }

    /* GET /hello */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/hello") == 0) {
        return "Hello from the dynamic library!\n";
    }

    /* GET /api/status */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        return "{\"status\":\"ok\",\"source\":\"dynamic library\",\"features\":[\"regex\",\"prefix\",\"path_info\"]}";
    }

    /* ===== PREFIX/MATCH ROUTES ===== */

    /* /api/users/* - RESTful API with path_info */
    if (strcmp(method, "GET") == 0 && starts_with(path, "/api/users")) {
        const char* remainder = get_path_info(path, "/api/users");
        snprintf(response, sizeof(response),
            "{\"route\":\"/api/users\",\"path_info\":\"%s\",\"method\":\"%s\"}",
            remainder, method
        );
        return response;
    }

    /* /files/* - Serve path info as file path */
    if (starts_with(path, "/files/")) {
        const char* file_path = get_path_info(path, "/files");
        snprintf(response, sizeof(response),
            "File requested: %s\n(In a real app, this would serve the file)\n",
            file_path
        );
        return response;
    }

    /* ===== REGEX ROUTES ===== */

    /* /user/{id} - Capture numeric ID */
    if (strcmp(method, "GET") == 0 && regex_match("^/user/([0-9]+)$", path, captures, 10)) {
        snprintf(response, sizeof(response),
            "{\"user_id\":\"%s\",\"name\":\"User %s\"}",
            captures[0] ? captures[0] : "?",
            captures[0] ? captures[0] : "?"
        );
        /* Free captures */
        for (int i = 0; i < 10 && captures[i]; i++) free(captures[i]);
        return response;
    }

    /* /product/{category}/{id} - Multiple captures */
    if (regex_match("^/product/([a-z]+)/([0-9]+)$", path, captures, 10)) {
        snprintf(response, sizeof(response),
            "{\"category\":\"%s\",\"product_id\":\"%s\"}",
            captures[0] ? captures[0] : "?",
            captures[1] ? captures[1] : "?"
        );
        for (int i = 0; i < 10 && captures[i]; i++) free(captures[i]);
        return response;
    }

    /* POST /api/echo - Echo back the body */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/echo") == 0) {
        snprintf(response, sizeof(response),
            "{\"received\":\"%s\",\"length\":%zu}",
            body ? body : "", body ? strlen(body) : 0
        );
        return response;
    }

    /* No route matched - return empty for 404 */
    return "";
}
