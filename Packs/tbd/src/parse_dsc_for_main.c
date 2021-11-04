//
//  src/parse_dsc_for_main.c
//  tbd
//
//  Created by inoahdev on 12/01/18.
//  Copyright © 2018 - 2020 inoahdev. All rights reserved.
//

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "handle_dsc_parse_result.h"
#include "magic_buffer.h"
#include "parse_dsc_for_main.h"

#include "notnull.h"
#include "our_io.h"
#include "path.h"

#include "recursive.h"
#include "tbd_for_main.h"
#include "tbd_write.h"
#include "unused.h"

struct dsc_iterate_images_info {
    struct dyld_shared_cache_info *dsc_info;

    /*
     * dsc_dir_path will point to the full path, and dsc_name will be NULL,
     * when not recursing.
     */

    const char *dsc_dir_path;
    const char *dsc_name;

    const char *image_path;
    uint64_t image_path_length;

    char *write_path;
    uint64_t write_path_length;

    struct tbd_for_main *tbd;
    struct tbd_for_main *orig;

    struct array images;
    FILE *combine_file;

    macho_file_parse_error_callback callback;
    struct handle_dsc_image_parse_error_cb_info *callback_info;

    bool print_paths : 1;
    bool parse_all_images : 1;
    bool did_print_messages_header : 1;

    struct retained_user_info *retained;
    struct string_buffer *export_trie_sb;
};

enum dyld_cache_image_info_pad {
    F_DYLD_CACHE_IMAGE_INFO_PAD_ALREADY_EXTRACTED = 1ull << 0
};

static void
print_messages_header(
    struct dsc_iterate_images_info *__notnull const iterate_info)
{
    if (iterate_info->did_print_messages_header) {
        return;
    }

    print_dsc_image_parse_error_message_header(iterate_info->print_paths,
                                               iterate_info->dsc_dir_path,
                                               iterate_info->dsc_name);

    iterate_info->did_print_messages_header = true;
}

static void
print_image_error(struct dsc_iterate_images_info *__notnull const iterate_info,
                  const char *__notnull const image_path,
                  const enum dsc_image_parse_result result)
{
    switch (result) {
        case E_DSC_IMAGE_PARSE_OK:
        case E_DSC_IMAGE_PARSE_ERROR_PASSED_TO_CALLBACK:
            return;

        case E_DSC_IMAGE_PARSE_NO_DATA: {
            const uint64_t should_ignore =
                (iterate_info->tbd->options.ignore_warnings ||
                 iterate_info->tbd->options.recurse_directories);

            if (should_ignore) {
                return;
            }

            break;
        }

        case E_DSC_IMAGE_PARSE_ALLOC_FAIL:
        case E_DSC_IMAGE_PARSE_ARRAY_FAIL:
        case E_DSC_IMAGE_PARSE_SEEK_FAIL:
        case E_DSC_IMAGE_PARSE_READ_FAIL:
        case E_DSC_IMAGE_PARSE_NO_MAPPING:
        case E_DSC_IMAGE_PARSE_SIZE_TOO_SMALL:
        case E_DSC_IMAGE_PARSE_INVALID_RANGE:
        case E_DSC_IMAGE_PARSE_NOT_A_MACHO:
        case E_DSC_IMAGE_PARSE_FAT_NOT_SUPPORTED:
        case E_DSC_IMAGE_PARSE_NO_LOAD_COMMANDS:
        case E_DSC_IMAGE_PARSE_TOO_MANY_LOAD_COMMANDS:
        case E_DSC_IMAGE_PARSE_LOAD_COMMANDS_AREA_TOO_SMALL:
        case E_DSC_IMAGE_PARSE_INVALID_LOAD_COMMAND:
        case E_DSC_IMAGE_PARSE_TOO_MANY_SECTIONS:
        case E_DSC_IMAGE_PARSE_INVALID_SECTION:
        case E_DSC_IMAGE_PARSE_INVALID_CLIENT:
        case E_DSC_IMAGE_PARSE_INVALID_REEXPORT:
        case E_DSC_IMAGE_PARSE_INVALID_SYMBOL_TABLE:
        case E_DSC_IMAGE_PARSE_INVALID_STRING_TABLE:
        case E_DSC_IMAGE_PARSE_NO_EXPORT_TRIE:
        case E_DSC_IMAGE_PARSE_NO_SYMBOL_TABLE:
        case E_DSC_IMAGE_PARSE_INVALID_EXPORTS_TRIE:
        case E_DSC_IMAGE_PARSE_CREATE_SYMBOLS_FAIL:
        case E_DSC_IMAGE_PARSE_CREATE_TARGET_LIST_FAIL:
        case E_DSC_IMAGE_PARSE_SIMULATOR_TYPE_MISMATCH:
            break;
    }

    print_messages_header(iterate_info);
    print_dsc_image_parse_error(image_path, result, true);
}

static void
print_write_file_result(
    struct dsc_iterate_images_info *__notnull const iterate_info,
    const struct tbd_for_main *__notnull const tbd,
    const enum tbd_for_main_open_write_file_result result)
{
    switch (result) {
        case E_TBD_FOR_MAIN_OPEN_WRITE_FILE_OK:
            break;

        case E_TBD_FOR_MAIN_OPEN_WRITE_FILE_FAILED:
            print_messages_header(iterate_info);
            fprintf(stderr,
                    "\tImage (with path %s) could not be parsed and written "
                    "out due to a write fail\r\n",
                    iterate_info->image_path);

            break;

        case E_TBD_FOR_MAIN_OPEN_WRITE_FILE_PATH_ALREADY_EXISTS:
            if (tbd->options.ignore_warnings) {
                break;
            }

            print_messages_header(iterate_info);
            fprintf(stderr,
                    "\tImage (with path %s) already has an existing file at "
                    "(one of) its write-paths that could not be overwritten.\t"
                    "Skipping\r\n",
                    iterate_info->image_path);

            break;
    }
}

static FILE *
open_file_for_path(struct dsc_iterate_images_info *__notnull const info,
                   const struct tbd_for_main *__notnull const tbd,
                   char *__notnull const path,
                   const uint64_t path_length,
                   const bool should_combine,
                   char **__notnull const terminator_out)
{
    FILE *file = info->combine_file;
    if (file != NULL) {
        return file;
    }

    const enum tbd_for_main_open_write_file_result open_file_result =
        tbd_for_main_open_write_file_for_path(tbd,
                                              path,
                                              path_length,
                                              &file,
                                              terminator_out);

    if (open_file_result != E_TBD_FOR_MAIN_OPEN_WRITE_FILE_OK) {
        print_write_file_result(info, tbd, open_file_result);
        return NULL;
    }

    if (should_combine) {
        info->combine_file = file;
    }

    return file;
}

static void
write_to_path(struct dsc_iterate_images_info *__notnull const iterate_info,
              const struct tbd_for_main *__notnull const tbd,
              char *__notnull const write_path,
              const uint64_t write_path_length)
{
    char *terminator = NULL;
    const bool should_combine = tbd->options.combine_tbds;

    FILE *const file =
        open_file_for_path(iterate_info,
                           tbd,
                           write_path,
                           write_path_length,
                           should_combine,
                           &terminator);

    if (file == NULL) {
        return;
    }

    tbd_for_main_write_to_file(tbd,
                               write_path,
                               write_path_length,
                               terminator,
                               file,
                               iterate_info->print_paths);

    if (!should_combine) {
        fclose(file);
    }
}

static void
write_out_tbd_info_for_filter_dir(
    struct dsc_iterate_images_info *__notnull const iterate_info,
    struct tbd_for_main *__notnull const tbd,
    const char *__notnull const filter_dir,
    const char *__notnull const image_path,
    const uint64_t image_path_length)
{
    const uint64_t delta = (const uint64_t)(filter_dir - image_path);
    const uint64_t path_length = image_path_length - delta;

    uint64_t length = 0;
    char *const write_path =
        tbd_for_main_create_dsc_image_write_path(tbd,
                                                 tbd->write_path,
                                                 tbd->write_path_length,
                                                 filter_dir,
                                                 path_length,
                                                 "tbd",
                                                 3,
                                                 &length);

    write_to_path(iterate_info, tbd, write_path, length);
    free(write_path);
}

void
write_out_tbd_info_for_filter_filename(
    struct dsc_iterate_images_info *const iterate_info,
    struct tbd_for_main *__notnull const tbd,
    const char *__notnull const filter_filename,
    const uint64_t filter_length)
{
    uint64_t length = 0;
    char *const write_path =
        tbd_for_main_create_dsc_image_write_path(tbd,
                                                 tbd->write_path,
                                                 tbd->write_path_length,
                                                 filter_filename,
                                                 filter_length,
                                                 "tbd",
                                                 3,
                                                 &length);

    write_to_path(iterate_info, tbd, write_path, length);
    free(write_path);
}

static void
write_out_tbd_info_for_image_path(
    struct dsc_iterate_images_info *const iterate_info,
    const struct tbd_for_main *__notnull const tbd,
    const char *__notnull const image_path,
    const uint64_t image_path_length)
{
    const bool alloc_path = !tbd->flags.dsc_write_path_is_file;

    uint64_t length = iterate_info->write_path_length;
    char *write_path = iterate_info->write_path;

    if (alloc_path) {
        write_path =
            tbd_for_main_create_dsc_image_write_path(tbd,
                                                     write_path,
                                                     length,
                                                     image_path,
                                                     image_path_length,
                                                     "tbd",
                                                     3,
                                                     &length);
    }

    write_to_path(iterate_info, tbd, write_path, length);

    if (alloc_path) {
        free(write_path);
    }
}

static void
write_out_tbd_info_for_filter(
    struct dsc_iterate_images_info *__notnull const info,
    const struct tbd_for_main_dsc_image_filter *__notnull const filter,
    struct tbd_for_main *__notnull const tbd,
    const char *__notnull const image_path,
    const uint64_t image_path_length)
{
    switch (filter->type) {
        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_PATH:
            write_out_tbd_info_for_image_path(info,
                                              tbd,
                                              image_path,
                                              image_path_length);

            break;

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_DIRECTORY:
            write_out_tbd_info_for_filter_dir(info,
                                              tbd,
                                              filter->tmp_ptr,
                                              image_path,
                                              image_path_length);

            break;

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_FILE:
            write_out_tbd_info_for_filter_filename(info,
                                                   tbd,
                                                   filter->tmp_ptr,
                                                   filter->length);

            break;
    }
}

static bool
write_out_tbd_info_for_filter_list(
    struct dsc_iterate_images_info *__notnull const info,
    struct tbd_for_main *__notnull const tbd,
    const char *__notnull const image_path,
    const uint64_t length)
{
    bool result = false;
    const struct array *const filters = &tbd->dsc_image_filters;

    struct tbd_for_main_dsc_image_filter *filter = filters->data;
    const struct tbd_for_main_dsc_image_filter *const end = filters->data_end;

    for (; filter != end; filter++) {
        if (filter->status != TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING) {
            continue;
        }

        write_out_tbd_info_for_filter(info,
                                      filter,
                                      tbd,
                                      image_path,
                                      length);

        filter->status = TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_OK;
        result = true;
    }

    return result;
}

static void
mark_happening_list_found(struct tbd_for_main *__notnull const tbd) {
    struct array *const list = &tbd->dsc_image_filters;

    struct tbd_for_main_dsc_image_filter *filter = list->data;
    const struct tbd_for_main_dsc_image_filter *const end = list->data_end;

    for (; filter != end; filter++) {
        if (filter->status == TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING) {
            filter->status = TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_FOUND;
        }
    }
}

static void
write_out_tbd_info(struct dsc_iterate_images_info *__notnull const info,
                   struct tbd_for_main *__notnull const tbd,
                   const char *__notnull const path,
                   const uint64_t path_length)
{
    if (info->parse_all_images) {
        write_out_tbd_info_for_image_path(info, tbd, path, path_length);
        return;
    }

    if (tbd->write_path == NULL) {
        /*
         * Since write_path won't be NULL while recursing, we can be sure
         * dsc_dir_path points to a full-path.
         */

        const char *const dsc_path = info->dsc_dir_path;

        tbd_for_main_write_to_stdout_for_dsc_image(tbd, dsc_path, path, true);
        mark_happening_list_found(tbd);

        return;
    }

    if (!write_out_tbd_info_for_filter_list(info, tbd, path, path_length)) {
        if (tbd->flags.dsc_write_path_is_file) {
            write_to_path(info, tbd, tbd->write_path, tbd->write_path_length);
            return;
        }
    }
}

static int
actually_parse_image(
    struct dsc_iterate_images_info *__notnull const iterate_info,
    struct dyld_cache_image_info *__notnull const image,
    const char *const image_path)
{
    struct tbd_for_main *const tbd = iterate_info->tbd;
    struct tbd_for_main *const orig = iterate_info->orig;

    struct tbd_create_info *const info = &tbd->info;
    struct handle_dsc_image_parse_error_cb_info *const cb_info =
        iterate_info->callback_info;

    cb_info->image_path = image_path;
    cb_info->did_print_messages_header =
        iterate_info->did_print_messages_header;

    struct dsc_image_parse_options options = {};
    const enum dsc_image_parse_result parse_image_result =
        dsc_image_parse(info,
                        iterate_info->dsc_info,
                        image,
                        iterate_info->callback,
                        cb_info,
                        iterate_info->export_trie_sb,
                        tbd->macho_options,
                        tbd->parse_options,
                        options);

    iterate_info->did_print_messages_header =
        cb_info->did_print_messages_header;

    if (parse_image_result != E_DSC_IMAGE_PARSE_OK) {
        tbd_create_info_clear_fields_and_create_from(info, &orig->info);
        print_image_error(iterate_info, image_path, parse_image_result);

        return 1;
    }

    tbd_for_main_handle_post_parse(tbd);

    uint64_t image_path_length = iterate_info->image_path_length;
    if (image_path_length == 0) {
        image_path_length = strlen(image_path);
        iterate_info->image_path_length = image_path_length;
    }

    write_out_tbd_info(iterate_info, tbd, image_path, image_path_length);
    tbd_create_info_clear_fields_and_create_from(info, &orig->info);

    return 0;
}

static bool
image_path_passes_through_filter(
    struct dsc_iterate_images_info *__notnull const info,
    const char *__notnull const path,
    struct tbd_for_main_dsc_image_filter *__notnull const filter)
{
    const char *const string = filter->string;
    const uint64_t length = filter->length;

    const char **const ptr = &filter->tmp_ptr;
    uint64_t path_len = info->image_path_length;

    if (path_len == 0) {
        path_len = strlen(path);
        info->image_path_length = path_len;
    }

    switch (filter->type) {
        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_PATH:
            if (length != path_len) {
                return false;
            }

            return (memcmp(path, string, length) == 0);

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_FILE:
            return path_has_filename(path, path_len, string, length, ptr);

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_DIRECTORY:
            return path_has_dir_component(path, path_len, string, length, ptr);
    }

    return false;
}

static inline bool
filter_was_parsed(
    const struct tbd_for_main_dsc_image_filter *__notnull const filter)
{
    return (filter->status > TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING);
}

static bool
should_parse_image(struct dsc_iterate_images_info *__notnull const info,
                   const struct array *__notnull const list,
                   const char *__notnull const path)
{
    bool should_parse = false;

    struct tbd_for_main_dsc_image_filter *filter = list->data;
    const struct tbd_for_main_dsc_image_filter *const end = list->data_end;

    for (; filter != end; filter++) {
        /*
         * If we've already determined that the image should be parsed, and the
         * filter doesn't need to be marked as completed, we can avoid an
         * unnecessary image_path_passes_through_filter() call.
         */

        if (filter_was_parsed(filter)) {
            if (should_parse) {
                continue;
            }
        }

        if (image_path_passes_through_filter(info, path, filter)) {
            filter->status = TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING;
            should_parse = true;
        }
    }

    return should_parse;
}

static void
unmark_happening_filters(const struct array *__notnull const list) {
    struct tbd_for_main_dsc_image_filter *filter = list->data;
    const struct tbd_for_main_dsc_image_filter *const end = list->data_end;

    for (; filter != end; filter++) {
        if (filter->status == TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING) {
            filter->status = TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_NOT_FOUND;
        }
    }
}

static bool
found_entire_filter_list(const struct array *__notnull const filters) {
    const struct tbd_for_main_dsc_image_filter *filter = filters->data;
    const struct tbd_for_main_dsc_image_filter *const end = filters->data_end;

    for (; filter != end; filter++) {
        if (!filter_was_parsed(filter)) {
            return false;
        }
    }

    return true;
}

static void
print_missing_filter(
    const struct tbd_for_main_dsc_image_filter *__notnull const filter)
{
    switch (filter->status) {
        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_NOT_FOUND:
            switch (filter->type) {
                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_FILE:
                    fprintf(stderr,
                            "\tNo images were found that passed the provided "
                            "filter (a file named: %s)\r\n",
                            filter->string);

                    break;

                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_DIRECTORY:
                    fprintf(stderr,
                            "\tNo images were found that passed the provided "
                            "filter (a directory named: %s)\r\n",
                            filter->string);

                    break;

                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_PATH:
                    fprintf(stderr,
                            "\tNo images were found with the provided "
                            "path (%s)\r\n",
                            filter->string);

                    break;
            }

            break;

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_FOUND:
            switch (filter->type) {
                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_FILE:
                    fprintf(stderr,
                            "\tAt least one image that passed the provided "
                            "filter (a file named: %s) was not successfully "
                            "parsed\r\n",
                            filter->string);

                    break;

                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_DIRECTORY:
                    fprintf(stderr,
                            "\tAt least one image that passed the provided "
                            "filter (a directory named: %s) was not "
                            "successfully parsed\r\n",
                            filter->string);

                    break;

                /*
                 * Since only one image corresponds to a path, the user already
                 * knows that this filter failed.
                 */

                case TBD_FOR_MAIN_DSC_IMAGE_FILTER_TYPE_PATH:
                    break;
            }

            break;

        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_OK:
        case TBD_FOR_MAIN_DSC_IMAGE_FILTER_PARSE_HAPPENING:
            break;
    }
}

/*
 * Iterate over every filter to print out errors if at least one image wasn't
 * found for every filter.
 *
 * We verify this here, rather that in
 * dyld_shared_cache_iterate_images_with_callback as we don't want to loop over
 * the filters once for the error-code, then again here to print out.
 */

static void
print_missing_filter_list(const struct array *__notnull const filters) {
    const struct tbd_for_main_dsc_image_filter *filter = filters->data;
    const struct tbd_for_main_dsc_image_filter *const end = filters->data_end;

    for (; filter != end; filter++) {
        if (!filter_was_parsed(filter)) {
            print_missing_filter(filter);
        }
    }
}

/*
 * Print out any errors we may have received after parsing all images from the
 * dyld shared-cache file.
 */

static void
print_dsc_warnings(struct dsc_iterate_images_info *__notnull const iterate_info,
                   const struct array *__notnull const filters)
{
    if (found_entire_filter_list(filters)) {
        return;
    }

    print_messages_header(iterate_info);
    print_missing_filter_list(filters);
}

static void
dsc_iterate_images(
    const struct dyld_shared_cache_info *__notnull const dsc_info,
    struct dsc_iterate_images_info *__notnull const info)
{
    const struct tbd_for_main *const tbd = info->tbd;
    const struct array *const filters = &tbd->dsc_image_filters;
    const uint64_t images_count = dsc_info->images_count;

    struct dyld_cache_image_info *image = dsc_info->images;
    const struct dyld_cache_image_info *const end = image + images_count;

    for (uint32_t i = 0; image != end; i++, image++) {
        if (image->pad & F_DYLD_CACHE_IMAGE_INFO_PAD_ALREADY_EXTRACTED) {
            continue;
        }

        const char *const image_path =
            (const char *)(dsc_info->map + image->pathFileOffset);

        /*
         * We never expect to encounter an empty image-path string, but we
         * check regardless as a general precaution.
         */

        if (unlikely(image_path[0] == '\0')) {
            continue;
        }

        info->image_path = image_path;
        info->image_path_length = 0;

        /*
         * If we're not parsing all images, we need to verify that our image
         * passes through either a name-filter or a path-filter.
         */

        if (!info->parse_all_images) {
            if (!should_parse_image(info, filters, image_path)) {
                continue;
            }
        }

        if (actually_parse_image(info, image, image_path)) {
            /*
             * actually_parse_image() would usually unmark the happening status
             * flag through write_out_tbd_info(), but the function was never
             * called, and so we have to manually unmark the status ourselves.
             */

            unmark_happening_filters(filters);
            continue;
        }

        image->pad |= F_DYLD_CACHE_IMAGE_INFO_PAD_ALREADY_EXTRACTED;
    }

    print_dsc_warnings(info, filters);
}

enum read_magic_result {
    E_READ_MAGIC_OK,
    E_READ_MAGIC_READ_FAILED,
    E_READ_MAGIC_NOT_LARGE_ENOUGH
};

static void verify_write_path(struct tbd_for_main *__notnull const tbd) {
    const char *const write_path = tbd->write_path;
    if (write_path == NULL) {
        /*
         * If we have exactly zero filters and zero numbers, and exactly one
         * path, we can write to stdout (which is what NULL write_path
         * represents).
         *
         * Or if we have exactly zero filters and zero paths, and exactly one
         * number, we can write to stdout.
         *
         * The reason why no filters, no numbers, and no paths is not allowed to
         * write to stdout is because no filters, no numbers, and no paths means
         * all images are parsed.
         */

        const struct array *const filters = &tbd->dsc_image_filters;
        const struct array *const numbers = &tbd->dsc_image_numbers;
        const uint64_t paths_count = tbd->dsc_filter_paths_count;

        if (paths_count == 1) {
            if (filters->item_count == paths_count) {
                if (numbers->item_count == 0) {
                    return;
                }
            }
        } else if (numbers->item_count == 1) {
            if (paths_count == 0) {
                if (filters->item_count == paths_count) {
                    return;
                }
            }
        }

        fprintf(stderr,
                "Please provide a directory to write .tbd files created from "
                "images of the dyld_shared_cache file at the provided "
                "path: %s\n",
                tbd->parse_path);

        exit(1);
    }

    struct stat sbuf = {};
    if (stat(write_path, &sbuf) < 0) {
        /*
         * Ignore any errors if the object doesn't even exist.
         *
         * Note:
         * ENOTDIR is returned when a directory in the hierarchy of the path is
         * not a directory at all, which means that an object doesn't exist at
         * the provided path.
         */

        if (errno != ENOENT && errno != ENOTDIR) {
            fprintf(stderr,
                    "Failed to get information on object at the provided "
                    "write-path (%s), error: %s\n",
                    write_path,
                    strerror(errno));

            exit(1);
        }

        if (tbd->options.combine_tbds) {
            tbd->flags.dsc_write_path_is_file = true;
            tbd->write_options.ignore_footer = true;
        }

        /*
         * If we have exactly zero filters and zero numbers, and exactly one
         * path, we can write to stdout (which is what NULL write_path
         * represents).
         *
         * Or if we have exactly zero filters and zero paths, and exactly one
         * number, we can write to stdout.
         *
         * The reason why no filters, no numbers, and no paths is not allowed to
         * write to stdout is because no filters, no numbers, and no paths means
         * all images are parsed.
         */

        const struct array *const filters = &tbd->dsc_image_filters;
        const struct array *const numbers = &tbd->dsc_image_numbers;
        const uint64_t paths_count = tbd->dsc_filter_paths_count;

        if (paths_count == 1) {
            if (filters->item_count == paths_count) {
                if (numbers->item_count == 0) {
                    tbd->flags.dsc_write_path_is_file = true;
                }
            }
        } else if (numbers->item_count == 1) {
            if (paths_count == 0) {
                if (filters->item_count == paths_count) {
                    tbd->flags.dsc_write_path_is_file = true;
                }
            }
        }

        return;
    }

    if (S_ISREG(sbuf.st_mode)) {
        /*
         * We allow writing to regular files only with the following conditions:
         *     (1) We are combining all created tbds into one .tbd file.
         *
         *     (2) No filters have been provided. This is because we can't tell
         *         before iterating how many images will pass a provided filter.
         *
         *     (3) Either only one image-number, or only one image-path has been
         *         provided.
         */

        if (tbd->options.combine_tbds) {
            tbd->flags.dsc_write_path_is_file = true;
            tbd->write_options.ignore_footer = true;

            return;
        }

        const struct array *const filters = &tbd->dsc_image_filters;
        const struct array *const numbers = &tbd->dsc_image_numbers;

        const uint64_t numbers_count = numbers->item_count;
        const uint64_t paths_count = tbd->dsc_filter_paths_count;

        if (numbers_count == 1) {
            if (paths_count == filters->item_count) {
                tbd->flags.dsc_write_path_is_file = true;
                return;
            }
        } else if (numbers_count == 0) {
            if (paths_count == filters->item_count) {
                tbd->flags.dsc_write_path_is_file = true;
                return;
            }
        }

        fputs("Writing to a regular file while parsing multiple images from a "
              "dyld_shared_cache file is not supported.\nPlease provide a "
              "directory to write all .tbd files to\n",
              stderr);

        exit(1);
    }

    return;
}

enum parse_dsc_for_main_result
parse_dsc_for_main(const struct parse_dsc_for_main_args args) {
    const enum magic_buffer_result get_magic_result =
        magic_buffer_read_n(args.magic_buffer, args.fd, 16);

    if (get_magic_result != E_MAGIC_BUFFER_OK) {
        handle_dsc_file_parse_result(args.dsc_dir_path,
                                     args.dsc_name,
                                     E_DYLD_SHARED_CACHE_PARSE_READ_FAIL,
                                     args.print_paths,
                                     false);

        return E_PARSE_DSC_FOR_MAIN_OTHER_ERROR;
    }

    struct dyld_shared_cache_parse_options dsc_options = args.tbd->dsc_options;
    dsc_options.zero_image_pads = true;

    struct dyld_shared_cache_info dsc_info = {};
    const enum dyld_shared_cache_parse_result parse_dsc_file_result =
        dyld_shared_cache_parse_from_file(&dsc_info,
                                          args.fd,
                                          (const char *)args.magic_buffer->buff,
                                          dsc_options);

    if (parse_dsc_file_result == E_DYLD_SHARED_CACHE_PARSE_NOT_A_CACHE) {
        if (args.dont_handle_non_dsc_error) {
            return E_PARSE_DSC_FOR_MAIN_NOT_A_SHARED_CACHE;
        }

        handle_dsc_file_parse_result(args.dsc_dir_path,
                                     args.dsc_name,
                                     parse_dsc_file_result,
                                     args.print_paths,
                                     false);

        return E_PARSE_DSC_FOR_MAIN_NOT_A_SHARED_CACHE;
    }

    if (parse_dsc_file_result != E_DYLD_SHARED_CACHE_PARSE_OK) {
        handle_dsc_file_parse_result(args.dsc_dir_path,
                                     args.dsc_name,
                                     parse_dsc_file_result,
                                     args.print_paths,
                                     false);

        return E_PARSE_DSC_FOR_MAIN_OTHER_ERROR;
    }

    if (args.tbd->options.combine_tbds) {
        args.tbd->flags.dsc_write_path_is_file = true;
        args.tbd->write_options.ignore_footer = true;
    } else if (args.options.verify_write_path) {
        verify_write_path(args.tbd);
    }

    struct handle_dsc_image_parse_error_cb_info cb_info = {
        .orig = args.orig,
        .tbd = args.tbd,

        .dsc_dir_path = args.dsc_dir_path,
        .dsc_name = args.dsc_name,
    };

    struct dsc_iterate_images_info iterate_info = {
        .dsc_info = &dsc_info,

        .dsc_dir_path = args.dsc_dir_path,
        .dsc_name = args.dsc_name,

        .write_path = args.tbd->write_path,
        .write_path_length = args.tbd->write_path_length,

        .tbd = args.tbd,
        .orig = args.orig,

        .combine_file = args.combine_file,
        .retained = args.retained,

        .callback = handle_dsc_image_parse_error_callback,
        .callback_info = &cb_info,

        .print_paths = args.print_paths,
        .parse_all_images = true,

        .export_trie_sb = args.export_trie_sb
    };

    const struct array *const filters = &args.tbd->dsc_image_filters;
    const struct array *const numbers = &args.tbd->dsc_image_numbers;

    /*
     * If numbers have been provided, directly call actually_parse_image()
     * instead of waiting around for the numbers to match up.
     */

    if (numbers->item_count != 0) {
        /*
         * Since there numbers were provided, we do not parse all images
         * as we would otherwise do.
         */

        iterate_info.parse_all_images = false;

        const uint32_t *iter = numbers->data;
        const uint32_t *const end = numbers->data_end;

        for (; iter != end; iter++) {
            const uint32_t number = *iter;
            if (number > dsc_info.images_count) {
                if (args.print_paths) {
                    fprintf(stderr,
                            "dyld_shared_cache (at path %s/%s) does not have "
                            "an image with number %" PRIu32 ".Skipping\n",
                            args.dsc_dir_path,
                            args.dsc_name,
                            number);
                } else {
                    fprintf(stderr,
                            "dyld_shared_cache at the provided path does not "
                            "have an image with number %" PRIu32 ". Skipping\n",
                            number);
                }

                /*
                 * Continue looping over the numbers so we can print out the
                 * errors at the very end.
                 */

                continue;
            }

            const uint32_t index = number - 1;
            struct dyld_cache_image_info *const image = dsc_info.images + index;

            const uint32_t path_offset = image->pathFileOffset;
            const char *const image_path =
                (const char *)(dsc_info.map + path_offset);

            if (actually_parse_image(&iterate_info, image, image_path) == 0) {
                image->pad |= F_DYLD_CACHE_IMAGE_INFO_PAD_ALREADY_EXTRACTED;
            }
        }

        /*
         * If there are no filters, we should simply return after handling the
         * numbers.
         */

        if (filters->item_count == 0) {
            print_dsc_warnings(&iterate_info, filters);
            dyld_shared_cache_info_destroy(&dsc_info);

            return E_PARSE_DSC_FOR_MAIN_OK;
        }
    } else {
        /*
         * By default, if no filters, numbers, or paths are provided, we parse
         * all images.
         *
         * Otherwise, all images have to be explicitly allowed to be parsed.
         */

        if (filters->item_count != 0) {
            iterate_info.parse_all_images = false;
        }
    }

    /*
     * Only create the write-path directory at the last-moment to avoid
     * unnecessary mkdir() calls.
     */

    dsc_iterate_images(&dsc_info, &iterate_info);
    dyld_shared_cache_info_destroy(&dsc_info);

    /*
     * After iterating over all our images, we need to cleanup after
     * combine_file.
     *
     * Specifically, we need to do two things:
     *     (1) First, we need to write the tbd-footer, which is written last
     *         after writing out all the tbds.
     *
     *     (2) Second, finally close the combine-file.
     */

    FILE *const combine_file = iterate_info.combine_file;
    if (combine_file != NULL) {
        if (tbd_write_footer(combine_file)) {
            if (args.print_paths) {
                fprintf(stderr,
                        "Failed to write footer for combined .tbd file for "
                        "files from directory (at path %s)\n",
                        args.dsc_dir_path);
            } else {
                fputs("Failed to write footer for combined .tbd file for files "
                      "from directory at the provided path\n",
                      stderr);
            }

            return E_PARSE_DSC_FOR_MAIN_CLOSE_COMBINE_FILE_FAIL;
        }

        fclose(combine_file);
    }

    return E_PARSE_DSC_FOR_MAIN_OK;
}

enum parse_dsc_for_main_result
parse_dsc_for_main_while_recursing(
    struct parse_dsc_for_main_args *__notnull const args)
{
    struct magic_buffer *const magic_buffer = args->magic_buffer;
    const enum magic_buffer_result get_magic_result =
        magic_buffer_read_n(magic_buffer, args->fd, 16);

    if (get_magic_result != E_MAGIC_BUFFER_OK) {
        handle_dsc_file_parse_result(args->dsc_dir_path,
                                     args->dsc_name,
                                     E_DYLD_SHARED_CACHE_PARSE_READ_FAIL,
                                     args->print_paths,
                                     true);

        return E_PARSE_DSC_FOR_MAIN_OTHER_ERROR;
    }

    const char *const magic = (const char *)magic_buffer->buff;

    struct tbd_for_main *const tbd = args->tbd;
    struct dyld_shared_cache_parse_options dsc_options = tbd->dsc_options;

    dsc_options.zero_image_pads = true;

    struct dyld_shared_cache_info dsc_info = {};
    const enum dyld_shared_cache_parse_result parse_dsc_file_result =
        dyld_shared_cache_parse_from_file(&dsc_info,
                                          args->fd,
                                          magic,
                                          dsc_options);

    if (parse_dsc_file_result == E_DYLD_SHARED_CACHE_PARSE_NOT_A_CACHE) {
        if (args->dont_handle_non_dsc_error) {
            return E_PARSE_DSC_FOR_MAIN_NOT_A_SHARED_CACHE;
        }

        handle_dsc_file_parse_result(args->dsc_dir_path,
                                     args->dsc_name,
                                     parse_dsc_file_result,
                                     args->print_paths,
                                     true);

        return E_PARSE_DSC_FOR_MAIN_NOT_A_SHARED_CACHE;
    }

    if (parse_dsc_file_result != E_DYLD_SHARED_CACHE_PARSE_OK) {
        handle_dsc_file_parse_result(args->dsc_dir_path,
                                     args->dsc_name,
                                     parse_dsc_file_result,
                                     args->print_paths,
                                     true);

        return E_PARSE_DSC_FOR_MAIN_OTHER_ERROR;
    }

    if (tbd->options.combine_tbds) {
        tbd->flags.dsc_write_path_is_file = true;
        tbd->write_options.ignore_footer = true;
    } else if (args->options.verify_write_path) {
        verify_write_path(tbd);
    }

    /*
     * dyld_shared_cache tbds are always stored in a separate directory when
     * recursing.
     *
     * When recursing, the name of the directory is made by combining file-name
     * of the dyld_shared_cache, followed by the extension '.tbds'.
     */

    const char *const dsc_dir_path = args->dsc_dir_path;
    const char *const dsc_name = args->dsc_name;

    uint64_t write_path_length = 0;
    char *const write_path =
        tbd_for_main_create_dsc_folder_path(tbd,
                                            dsc_dir_path,
                                            args->dsc_dir_path_length,
                                            dsc_name,
                                            args->dsc_name_length,
                                            "tbds",
                                            4,
                                            &write_path_length);

    struct tbd_for_main *const orig = args->orig;
    const bool print_paths = args->print_paths;

    struct handle_dsc_image_parse_error_cb_info cb_info = {
        .tbd = tbd,
        .orig = orig,

        .dsc_dir_path = dsc_dir_path,
        .dsc_name = dsc_name,

        .print_paths = print_paths
    };

    struct dsc_iterate_images_info iterate_info = {
        .dsc_info = &dsc_info,
        .dsc_dir_path = dsc_dir_path,
        .dsc_name = dsc_name,

        .write_path = write_path,
        .write_path_length = write_path_length,

        .tbd = tbd,
        .orig = orig,

        .combine_file = args->combine_file,
        .retained = args->retained,

        .callback = handle_dsc_image_parse_error_callback,
        .callback_info = &cb_info,

        .print_paths = print_paths,
        .parse_all_images = true,

        .export_trie_sb = args->export_trie_sb
    };

    const struct array *const filters = &tbd->dsc_image_filters;
    const struct array *const numbers = &tbd->dsc_image_numbers;

    /*
     * If numbers have been provided, directly call actually_parse_image()
     * instead of waiting around for the numbers to match up.
     */

    if (numbers->item_count != 0) {
        const uint32_t *iter = numbers->data;
        const uint32_t *const end = numbers->data_end;

        for (; iter != end; iter++) {
            const uint32_t number = *iter;
            if (number > dsc_info.images_count) {
                if (print_paths) {
                    fprintf(stderr,
                            "An image-number of %" PRIu32 " goes beyond the "
                            "images-count of %" PRIu32 " the dyld_shared_cache "
                            "(at path %s/%s) has\n",
                            number,
                            dsc_info.images_count,
                            dsc_dir_path,
                            dsc_name);
                } else {
                    fprintf(stderr,
                            "An image-number of %" PRIu32 " goes beyond the "
                            "images-count of %" PRIu32 " the dyld_shared_cache "
                            "at the provided path has\n",
                            number,
                            dsc_info.images_count);
                }

                /*
                 * Continue looping over the numbers so we can print out the
                 * errors at the very end.
                 */

                continue;
            }

            const uint32_t index = number - 1;
            struct dyld_cache_image_info *const image = dsc_info.images + index;

            const uint32_t path_offset = image->pathFileOffset;
            const char *const image_path =
                (const char *)(dsc_info.map + path_offset);

            if (actually_parse_image(&iterate_info, image, image_path) == 0) {
                image->pad |= F_DYLD_CACHE_IMAGE_INFO_PAD_ALREADY_EXTRACTED;
            }
        }

        /*
         * If there are no filters and no paths, we should simply return after
         * handling the numbers.
         *
         * Note: Since there are numbers, we do not parse all images as we do by
         * default.
         */

        const uint64_t filters_count = filters->item_count;
        if (filters_count == 0) {
            free(write_path);

            print_dsc_warnings(&iterate_info, filters);
            dyld_shared_cache_info_destroy(&dsc_info);

            return E_PARSE_DSC_FOR_MAIN_OK;
        }

        iterate_info.parse_all_images = false;
    } else {
        /*
         * By default, if no filters or numbers are provided, we parse all
         * images.
         *
         * Otherwise, all images have to be explicitly allowed to be parsed.
         */

        if (filters->item_count != 0) {
            iterate_info.parse_all_images = false;
        }
    }

    /*
     * Only create the write-path directory at the last-moment to avoid
     * unnecessary mkdir() calls for a shared-cache that may turn up empty.
     */

    dsc_iterate_images(&dsc_info, &iterate_info);
    dyld_shared_cache_info_destroy(&dsc_info);

    /*
     * We may have opened combine_file, which we should turn over to the caller.
     */

    if (iterate_info.combine_file != NULL) {
        args->combine_file = iterate_info.combine_file;
    }

    free(write_path);
    return E_PARSE_DSC_FOR_MAIN_OK;
}

void print_list_of_dsc_images(const int fd) {
    char magic[16] = {};
    if (our_read(fd, &magic, sizeof(magic)) < 0) {
        handle_dsc_file_parse_result(NULL,
                                     NULL,
                                     E_DYLD_SHARED_CACHE_PARSE_READ_FAIL,
                                     false,
                                     false);

        exit(1);
    }

    struct dyld_shared_cache_info dsc_info = {};
    struct dyld_shared_cache_parse_options options = {};

    const enum dyld_shared_cache_parse_result parse_dsc_file_result =
        dyld_shared_cache_parse_from_file(&dsc_info, fd, magic, options);

    if (parse_dsc_file_result != E_DYLD_SHARED_CACHE_PARSE_OK) {
        handle_dsc_file_parse_result(NULL,
                                     NULL,
                                     parse_dsc_file_result,
                                     false,
                                     false);

        exit(1);
    }

    fprintf(stdout,
            "The provided dyld_shared_cache file has %" PRIu32 " images\n",
            dsc_info.images_count);

    const uint64_t images_count = dsc_info.images_count;

    const struct dyld_cache_image_info *image = dsc_info.images;
    const struct dyld_cache_image_info *const end = image + images_count;

    for (uint32_t index = 0; image != end; image++, index++) {
        const char *const image_path =
            (const char *)(dsc_info.map + image->pathFileOffset);

        fprintf(stdout, "\t%" PRIu32 ". %s\r\n", index + 1, image_path);
    }
}

static int
image_paths_comparator(const void *__notnull const left,
                       const void *__notnull const right)
{
    const char *const left_path = *(const char **)left;
    const char *const right_path = *(const char **)right;

    return strcmp(left_path, right_path);
}

void print_list_of_dsc_images_ordered(const int fd) {
    char magic[16] = {};
    if (our_read(fd, &magic, sizeof(magic)) < 0) {
        handle_dsc_file_parse_result(NULL,
                                     NULL,
                                     E_DYLD_SHARED_CACHE_PARSE_READ_FAIL,
                                     false,
                                     false);

        exit(1);
    }

    struct dyld_shared_cache_info dsc_info = {};
    struct dyld_shared_cache_parse_options options = {};

    const enum dyld_shared_cache_parse_result parse_dsc_file_result =
        dyld_shared_cache_parse_from_file(&dsc_info, fd, magic, options);

    if (parse_dsc_file_result != E_DYLD_SHARED_CACHE_PARSE_OK) {
        handle_dsc_file_parse_result(NULL,
                                     NULL,
                                     parse_dsc_file_result,
                                     false,
                                     false);

        exit(1);
    }

    struct array image_paths = { .item_count = dsc_info.images_count };
    const enum array_result ensure_capacity_result =
        array_ensure_item_capacity(&image_paths,
                                   sizeof(const char *),
                                   dsc_info.images_count);

    if (ensure_capacity_result != E_ARRAY_OK) {
        fputs("Experienced an array failure while trying to order dsc "
              "image-paths\n",
              stderr);

        exit(1);
    }

    const char **image_paths_ptr = image_paths.data;
    const uint64_t images_count = dsc_info.images_count;

    const struct dyld_cache_image_info *image = dsc_info.images;
    const struct dyld_cache_image_info *const end = image + images_count;

    for (; image != end; image++, image_paths_ptr++) {
        *image_paths_ptr = (const char *)(dsc_info.map + image->pathFileOffset);
    }

    array_sort_with_comparator(&image_paths,
                               sizeof(const char *),
                               image_paths_comparator);

    fprintf(stdout,
            "The provided dyld_shared_cache file has %" PRIu32 " images\n",
            dsc_info.images_count);

    image = dsc_info.images;
    image_paths_ptr = image_paths.data;

    for (uint32_t i = 0; image != end; i++, image++, image_paths_ptr++) {
        fprintf(stdout, "\t%" PRIu32 ". %s\r\n", i + 1, *image_paths_ptr);
    }

    array_destroy(&image_paths);
}
