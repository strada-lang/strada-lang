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
 * cannoli_dispatch.c - C wrapper for dispatch to handle return type conversion
 *
 * This provides the cannoli_dispatch function with the correct char* return type.
 * It calls the Strada dispatch function and extracts the string.
 *
 * New interface passes full request data for the Cannoli object.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations from Strada runtime */
typedef struct StradaValue StradaValue;
extern const char* strada_to_str(StradaValue *sv);
extern StradaValue* strada_from_str(const char *s);

/* Forward declaration of the Strada dispatch function (new signature) */
extern StradaValue* strada_dispatch_impl(
    StradaValue* method,
    StradaValue* path,
    StradaValue* path_info,
    StradaValue* query_string,
    StradaValue* body,
    StradaValue* headers,
    StradaValue* remote_addr,
    StradaValue* content_type
);

/* The dispatch function that cannoli calls - returns char*
 * New signature with full request data
 */
char* cannoli_dispatch(
    StradaValue* method,
    StradaValue* path,
    StradaValue* path_info,
    StradaValue* query_string,
    StradaValue* body,
    StradaValue* headers,
    StradaValue* remote_addr,
    StradaValue* content_type
) {
    /* Call the Strada dispatch implementation */
    StradaValue* result_sv = strada_dispatch_impl(
        method,
        path,
        path_info,
        query_string,
        body,
        headers,
        remote_addr,
        content_type
    );

    if (!result_sv) {
        return strdup("");
    }

    /* Extract string from StradaValue */
    const char* result = strada_to_str(result_sv);
    if (!result) {
        return strdup("");
    }

    /* Return a copy (caller will free) */
    return strdup(result);
}
