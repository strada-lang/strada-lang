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
 * lib_pathinfo.c - Library to test path_info functionality
 */

#include <stdio.h>
#include <string.h>

typedef struct StradaValue StradaValue;
extern const char* strada_to_str(StradaValue* v);

const char* cannoli_dispatch(StradaValue* method_sv, StradaValue* path_sv,
                             StradaValue* path_info_sv, StradaValue* body_sv) {
    static char response[4096];

    const char* method = strada_to_str(method_sv);
    const char* path = strada_to_str(path_sv);
    const char* path_info = strada_to_str(path_info_sv);

    /* Always respond to show what we received */
    snprintf(response, sizeof(response),
        "{\"method\":\"%s\",\"path\":\"%s\",\"path_info\":\"%s\",\"library\":\"pathinfo_test\"}",
        method, path, path_info);
    return response;
}
