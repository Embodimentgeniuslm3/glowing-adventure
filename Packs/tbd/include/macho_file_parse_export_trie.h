//
//  include/macho_file_parse_export_trie.h
//  tbd
//
//  Created by inoahdev on 11/3/19.
//  Copyright © 2019 - 2020 inoahdev. All rights reserved.
//

#ifndef MACHO_FILE_PARSE_EXPORT_TRIE_H
#define MACHO_FILE_PARSE_EXPORT_TRIE_H

#include "macho_file.h"
#include "range.h"

struct macho_file_parse_export_trie_args {
    struct tbd_create_info *info_in;
    struct range available_range;

    uint64_t arch_index;

    bool is_64 : 1;
    bool is_big_endian : 1;

    uint32_t export_off;
    uint32_t export_size;

    struct string_buffer *sb_buffer;
    struct tbd_parse_options tbd_options;
};

enum macho_file_parse_result
macho_file_parse_export_trie_from_file(
    struct macho_file_parse_export_trie_args args,
    int fd,
    uint64_t base_offset);

enum macho_file_parse_result
macho_file_parse_export_trie_from_map(
    struct macho_file_parse_export_trie_args args,
    const uint8_t *__notnull map);

#endif /* MACHO_FILE_PARSE_EXPORT_TRIE_H */
