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
 * Shim for Strada-based Cannoli libraries
 * This wraps the Strada cannoli_dispatch function to match the C interface
 */

#include "strada_runtime.h"

/* Global variables required by Strada runtime */
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

/* Forward declaration of the Strada dispatch function */
extern StradaValue* strada_dispatch(StradaValue* method, StradaValue* path,
                                    StradaValue* path_info, StradaValue* body);

/* Static buffer for response (thread-local would be better for production) */
static char response_buffer[65536];

/* Initialize Strada runtime (called once on library load) */
__attribute__((constructor))
static void init_strada_runtime(void) {
    if (ARGV == NULL) {
        ARGV = strada_new_array();
        ARGC = strada_new_int(0);
    }
}

/* C-compatible dispatch function that Cannoli calls
 * This wrapper converts Strada's StradaValue* return to a C string */
const char* cannoli_dispatch(StradaValue* method_sv, StradaValue* path_sv,
                             StradaValue* path_info_sv, StradaValue* body_sv) {
    /* Call the Strada dispatch function */
    StradaValue* result = strada_dispatch(method_sv, path_sv, path_info_sv, body_sv);

    /* Convert result to C string */
    const char* str = strada_to_str(result);
    if (str == NULL) {
        return "";
    }

    /* Copy to buffer (ensures lifetime) */
    strncpy(response_buffer, str, sizeof(response_buffer) - 1);
    response_buffer[sizeof(response_buffer) - 1] = '\0';

    return response_buffer;
}
