//
//  src/string_buffer.c
//  tbd
//
//  Created by inoahdev on 11/10/19.
//  Copyright © 2019 - 2020 inoahdev. All rights reserved.
//

#include <stdlib.h>
#include <string.h>

#include "likely.h"
#include "string_buffer.h"

static inline uint64_t get_new_capacity(const uint64_t capacity) {
    return (capacity * 2);
}

static uint64_t
capacity_for_length(struct string_buffer *__notnull sb, const uint64_t needed) {
    const uint64_t capacity = sb->capacity;
    const uint64_t wanted = sb->length + needed;

    /*
     * Add one to wanted for the null-terminator.
     */

    if (unlikely(capacity == 0)) {
        return (wanted + 1);
    }

    uint64_t new_capacity = capacity;
    uint64_t wanted_bytes = wanted + 1;

    do {
        new_capacity = get_new_capacity(new_capacity);
    } while (new_capacity < wanted_bytes);

    /*
     * Add one to wanted for the null-terminator.
     */

    return new_capacity;
}

static char *
expand_to_capacity(struct string_buffer *__notnull const sb,
                   const uint64_t new_cap)
{
    char *const new_data = malloc(new_cap);
    if (new_data == NULL) {
        return NULL;
    }

    char *const data = sb->data;
    const uint64_t sb_length = sb->length;

    memcpy(new_data, data, sb_length);

    if (sb->was_alloced) {
        free(data);
    } else {
        sb->was_alloced = true;
    }

    sb->data = new_data;
    sb->capacity = new_cap;

    return new_data;
}

/*
 * NOTE: get_free_space() doesn't check if capacity is zero!
 */

static inline
uint64_t get_free_space(const uint64_t length, const uint64_t capacity) {
    return ((capacity - length) - 1);
}

enum string_buffer_result
sb_add_c_str(struct string_buffer *__notnull const sb,
             const char *__notnull const c_str,
             const uint64_t length)
{
    char *data = sb->data;

    const uint64_t sb_length = sb->length;
    const uint64_t sb_capacity = sb->capacity;

    if (sb_capacity != 0) {
        const uint64_t free_count = get_free_space(sb_length, sb_capacity);
        if (free_count < length) {
            const uint64_t new_cap = capacity_for_length(sb, length);
            if ((data = expand_to_capacity(sb, new_cap)) == NULL) {
                return E_STRING_BUFFER_ALLOC_FAIL;
            }
        }
    } else {
        const uint64_t new_cap = length + 1;
        if ((data = expand_to_capacity(sb, new_cap)) == NULL) {
            return E_STRING_BUFFER_ALLOC_FAIL;
        }
    }

    const uint64_t new_length = sb_length + length;

    memcpy(data + sb_length, c_str, length);
    data[new_length] = '\0';

    sb->length = new_length;
    return E_STRING_BUFFER_OK;
}

enum string_buffer_result
sb_reserve_space(struct string_buffer *__notnull const sb, const uint64_t space)
{
    const uint64_t sb_capacity = sb->capacity;
    uint64_t new_cap = space;

    if (sb_capacity != 0) {
        if (get_free_space(sb->length, sb_capacity) >= space) {
            return E_STRING_BUFFER_OK;
        }

        new_cap = capacity_for_length(sb, space);
    }

    if (expand_to_capacity(sb, new_cap) == NULL) {
        return E_STRING_BUFFER_ALLOC_FAIL;
    }

    return E_STRING_BUFFER_OK;
}

void sb_clear(struct string_buffer *__notnull const sb) {
    sb->length = 0;
}

void sb_destroy(struct string_buffer *__notnull const sb) {
    free(sb->data);

    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}
