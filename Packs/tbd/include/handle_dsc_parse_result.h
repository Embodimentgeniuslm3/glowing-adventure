//
//  include/handle_dsc_parse_result.h
//  tbd
//
//  Created by inoahdev on 12/31/18.
//  Copyright © 2018 - 2020 inoahdev. All rights reserved.
//

#ifndef HANDLE_DSC_PARSE_RESULT_H
#define HANDLE_DSC_PARSE_RESULT_H

#include "dsc_image.h"
#include "dyld_shared_cache.h"

#include "macho_file.h"
#include "notnull.h"
#include "tbd_for_main.h"

void
handle_dsc_file_parse_result(const char *dir_path,
                             const char *name,
                             enum dyld_shared_cache_parse_result parse_result,
                             bool print_paths,
                             bool is_recursing);

struct handle_dsc_image_parse_error_cb_info {
    struct tbd_for_main *orig;
    struct tbd_for_main *tbd;

    const char *dsc_dir_path;
    const char *dsc_name;
    const char *image_path;

    bool print_paths;
    bool did_print_messages_header;
};

void
print_dsc_image_parse_error_message_header(bool print_paths,
                                           const char *__notnull dsc_dir_path,
                                           const char *dsc_name);

bool
handle_dsc_image_parse_error_callback(struct tbd_create_info *__notnull info_in,
                                      enum macho_file_parse_callback_type type,
                                      void *callback_info);

void
print_dsc_image_parse_error(const char *__notnull image_path,
                            enum dsc_image_parse_result parse_error,
                            bool indent);

#endif /* HANDLE_DSC_PARSE_RESULT_H */

