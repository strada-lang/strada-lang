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
 * lib_admin.c - Admin API library for multi-library testing
 * Routes: /admin, /admin/*
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct StradaValue StradaValue;
extern const char* strada_to_str(StradaValue* v);

static int starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

const char* cannoli_dispatch(StradaValue* method_sv, StradaValue* path_sv,
                             StradaValue* path_info_sv, StradaValue* body_sv) {
    static char response[4096];

    const char* method = strada_to_str(method_sv);
    const char* path = strada_to_str(path_sv);
    const char* path_info = strada_to_str(path_info_sv);

    /* Only handle /admin routes */
    if (!starts_with(path, "/admin")) {
        return "";  /* Pass to next library */
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/admin") == 0) {
        return "{\"service\":\"admin\",\"status\":\"ok\",\"routes\":[\"/admin\",\"/admin/users\",\"/admin/stats\"]}";
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/admin/users") == 0) {
        return "{\"users\":[{\"id\":1,\"name\":\"admin\"},{\"id\":2,\"name\":\"guest\"}]}";
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/admin/stats") == 0) {
        return "{\"requests\":100,\"errors\":0,\"uptime\":3600}";
    }

    /* /admin/* fallback */
    if (starts_with(path, "/admin/")) {
        snprintf(response, sizeof(response),
            "{\"error\":\"admin route not found\",\"path\":\"%s\",\"path_info\":\"%s\"}",
            path, path_info);
        return response;
    }

    return "";
}
