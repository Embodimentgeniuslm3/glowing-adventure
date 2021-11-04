//
//  src/macho_file_parse_single_lc.c
//  tbd
//
//  Created by inoahdev on 12/10/19.
//  Copyright © 2019 - 2020 inoahdev. All rights reserved.
//

#include <stdint.h>
#include <string.h>
#include "mach-o/loader.h"

#include "copy.h"
#include "macho_file_parse_single_lc.h"
#include "macho_file.h"

#include "swap.h"
#include "tbd.h"
#include "yaml.h"

static inline bool
call_callback(const macho_file_parse_error_callback callback,
              struct tbd_create_info *__notnull const info_in,
              const enum macho_file_parse_callback_type type,
              void *const cb_info)
{
    if (callback != NULL) {
        if (callback(info_in, type, cb_info)) {
            return true;
        }
    }

    return false;
}

enum verify_string_result {
    E_VERIFY_STRING_OK,
    E_VERIFY_STRING_EMPTY,
    E_VERIFY_STRING_INVALID
};

static enum verify_string_result
verify_string_offset(const uint8_t *const load_cmd,
                     const uint32_t offset,
                     const uint32_t load_cmd_min_size,
                     const uint32_t load_cmdsize,
                     uint32_t *__notnull const length_out)
{
    /*
     * Ensure the string is fully within the structure, and doesn't overlap the
     * basic structure of the load-command.
     */

    if (offset < load_cmd_min_size || offset > load_cmdsize) {
        return E_VERIFY_STRING_INVALID;
    }

    if (offset == load_cmdsize) {
        return E_VERIFY_STRING_EMPTY;
    }

    const char *const string = (const char *)load_cmd + offset;

    /*
     * The string extends from its offset to the end of the dylib-command
     * load-commmand.
     */

    const uint32_t max_length = load_cmdsize - offset;
    const uint32_t length = (uint32_t)strnlen(string, max_length);

    if (length == 0) {
        return E_VERIFY_STRING_EMPTY;
    }

    *length_out = length;
    return E_VERIFY_STRING_OK;
}

enum add_export_result {
    E_ADD_EXPORT_OK,
    E_ADD_EXPORT_INVALID,
    E_ADD_EXPORT_ADD_FAIL
};

static enum add_export_result
add_export_to_info(struct tbd_create_info *__notnull const info_in,
                   const uint64_t arch_index,
                   const enum tbd_symbol_type type,
                   const uint8_t *const load_cmd,
                   uint32_t offset,
                   const uint32_t load_cmd_min_size,
                   const uint32_t load_cmdsize,
                   const bool is_big_endian,
                   const struct tbd_parse_options options)
{
    if (is_big_endian) {
        offset = swap_uint32(offset);
    }

    uint32_t length = 0;
    const enum verify_string_result verify_string_result =
        verify_string_offset(load_cmd,
                             offset,
                             load_cmd_min_size,
                             load_cmdsize,
                             &length);

    if (verify_string_result != E_VERIFY_STRING_OK) {
        return E_ADD_EXPORT_INVALID;
    }

    const char *const string = (const char *)load_cmd + offset;
    const enum tbd_ci_add_data_result add_export_result =
        tbd_ci_add_symbol_with_type(info_in,
                                    string,
                                    length,
                                    arch_index,
                                    type,
                                    TBD_SYMBOL_META_TYPE_EXPORT,
                                    options);

    if (add_export_result != E_TBD_CI_ADD_DATA_OK) {
        return E_ADD_EXPORT_ADD_FAIL;
    }

    return E_ADD_EXPORT_OK;
}

static inline bool is_mac_or_iosmac_platform(const enum tbd_platform platform) {
    switch (platform) {
        case TBD_PLATFORM_NONE:
        case TBD_PLATFORM_IOS:
        case TBD_PLATFORM_TVOS:
        case TBD_PLATFORM_WATCHOS:
        case TBD_PLATFORM_BRIDGEOS:
        case TBD_PLATFORM_IOS_SIMULATOR:
        case TBD_PLATFORM_TVOS_SIMULATOR:
        case TBD_PLATFORM_WATCHOS_SIMULATOR:
        case TBD_PLATFORM_DRIVERKIT:
            return false;

        case TBD_PLATFORM_MACOS:
        case TBD_PLATFORM_IOSMAC:
            return true;
    }
}

static enum macho_file_parse_result
parse_bv_platform(
    struct macho_file_parse_single_lc_info *__notnull const parse_info,
    struct tbd_create_info *__notnull const info_in,
    const struct load_command load_cmd,
    const uint8_t *__notnull const lc_iter,
    const macho_file_parse_error_callback callback,
    void *const cb_info,
    const struct tbd_parse_options options)
{
    if (options.ignore_platform) {
        return E_MACHO_FILE_PARSE_OK;
    }

    /*
     * Every build-version load-command should be large enough to store the
     * build-version load-command's basic information.
     */

    if (load_cmd.cmdsize < sizeof(struct build_version_command)) {
        return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
    }

    const struct build_version_command *const build_version =
        (const struct build_version_command *)lc_iter;

    const struct macho_file_parse_slc_flags *const flags = parse_info->flags_in;
    uint32_t platform = build_version->platform;

    if (flags->is_big_endian) {
        platform = swap_uint32(platform);
    }

    switch (platform) {
        case PLATFORM_MACOS:
        case PLATFORM_IOS:
        case PLATFORM_TVOS:
        case PLATFORM_WATCHOS:
        case PLATFORM_BRIDGEOS:
        case PLATFORM_DRIVERKIT:
        case PLATFORM_IOSMAC:
        case PLATFORM_IOSSIMULATOR:
        case PLATFORM_TVOSSIMULATOR:
        case PLATFORM_WATCHOSSIMULATOR:
            break;

        default: {
            const bool should_continue =
                call_callback(callback,
                              info_in,
                              ERR_MACHO_FILE_PARSE_INVALID_PLATFORM,
                              cb_info);

            if (!should_continue) {
                return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
            }

            return E_MACHO_FILE_PARSE_OK;
        }
    }

    struct macho_file_parse_slc_flags *const flags_in = parse_info->flags_in;
    enum tbd_platform *const platform_in = parse_info->platform_in;

    if (!flags_in->found_build_version) {
        flags_in->found_build_version = true;
        *platform_in = platform;

        return E_MACHO_FILE_PARSE_OK;
    }

    const enum tbd_platform parsed_platform = *platform_in;
    if (parsed_platform == platform) {
        return E_MACHO_FILE_PARSE_OK;
    }

    if (tbd_uses_archs(info_in->version)) {
        const bool should_continue =
            call_callback(callback,
                          info_in,
                          ERR_MACHO_FILE_PARSE_PLATFORM_CONFLICT,
                          cb_info);

        if (!should_continue) {
            return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
        }

        return E_MACHO_FILE_PARSE_OK;
    }

    /*
     * We only allow duplicate, differing build-version platforms if they're
     * both either a macOS or iosmac platform.
     */

    if (is_mac_or_iosmac_platform(parsed_platform) ||
        is_mac_or_iosmac_platform(platform))
    {
        *platform_in = TBD_PLATFORM_IOSMAC;

        parse_info->flags_in->found_build_version = true;
        parse_info->flags_in->found_iosmac_platform = true;

        return E_MACHO_FILE_PARSE_OK;
    }

    const bool should_continue =
        call_callback(callback,
                      info_in,
                      ERR_MACHO_FILE_PARSE_TARGET_PLATFORM_CONFLICT,
                      cb_info);

    if (!should_continue) {
        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
    }

    return E_MACHO_FILE_PARSE_OK;
}

static enum macho_file_parse_result
parse_version_min_lc(
    const struct macho_file_parse_single_lc_info *__notnull const parse_info,
    struct tbd_create_info *__notnull const info_in,
    const struct load_command load_cmd,
    const enum tbd_platform expected_platform,
    const macho_file_parse_error_callback callback,
    void *const cb_info,
    const struct tbd_parse_options options)
{
    /*
     * If the platform isn't needed, skip the unnecessary parsing.
     */

    if (options.ignore_platform) {
        return E_MACHO_FILE_PARSE_OK;
    }

    /*
     * Ignore the version_min_* load-commands if we already found a
     * build-version load-command.
     */

    if (parse_info->flags_in->found_build_version) {
        return E_MACHO_FILE_PARSE_OK;
    }

    /*
     * All version_min load-commands should have the same cmdsize.
     */

    if (load_cmd.cmdsize != sizeof(struct version_min_command)) {
        return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
    }

    enum tbd_platform *const platform_in = parse_info->platform_in;
    const enum tbd_platform platform = *platform_in;

    if (platform != TBD_PLATFORM_NONE) {
        if (platform != expected_platform) {
            const bool should_continue =
                call_callback(callback,
                              info_in,
                              ERR_MACHO_FILE_PARSE_PLATFORM_CONFLICT,
                              cb_info);

            if (!should_continue) {
                return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
            }
        }
    } else {
        *platform_in = expected_platform;
    }

    return E_MACHO_FILE_PARSE_OK;
}

static enum macho_file_parse_result
parse_export_trie_info(
    const struct macho_file_parse_single_lc_info *__notnull const parse_info,
    struct tbd_create_info *__notnull const info_in,
    uint32_t export_off,
    uint32_t export_size,
    const macho_file_parse_error_callback callback,
    void *const cb_info)
{
    if (parse_info->flags_in->is_big_endian) {
        export_off = swap_uint32(export_off);
        export_size = swap_uint32(export_size);
    }

    uint32_t *const export_off_out = parse_info->export_off_out;
    uint32_t *const export_size_out = parse_info->export_size_out;

    const uint32_t existing_export_off = *export_off_out;
    const uint32_t existing_export_size = *export_size_out;

    if (existing_export_off != 0 || existing_export_size != 0) {
        if (existing_export_off != export_off ||
            existing_export_size != export_size)
        {
            const bool should_continue =
                call_callback(callback,
                              info_in,
                              ERR_MACHO_FILE_PARSE_EXPORT_TRIE_CONFLICT,
                              cb_info);

            if (!should_continue) {
                return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
            }
        }
    } else {
        *export_off_out = export_off;
        *export_size_out = export_size;
    }

    return E_MACHO_FILE_PARSE_OK;
}

enum macho_file_parse_result
macho_file_parse_single_lc(
    struct macho_file_parse_single_lc_info *__notnull const parse_info,
    const macho_file_parse_error_callback callback,
    void *const cb_info)
{
    struct tbd_create_info *const info_in = parse_info->info_in;
    const struct tbd_parse_options tbd_options = parse_info->tbd_options;

    const struct load_command load_cmd = parse_info->load_cmd;
    const uint8_t *const lc_iter = parse_info->lc_iter;

    switch (load_cmd.cmd) {
        case LC_BUILD_VERSION: {
            const enum macho_file_parse_result parse_platform_result =
                parse_bv_platform(parse_info,
                                  info_in,
                                  load_cmd,
                                  lc_iter,
                                  callback,
                                  cb_info,
                                  tbd_options);

            if (parse_platform_result != E_MACHO_FILE_PARSE_OK) {
                return parse_platform_result;
            }

            break;
        }

        case LC_DYLD_EXPORTS_TRIE: {
            /*
             * If exports aren't needed, skip the unnecessary parsing.
             */

            if (tbd_options.ignore_exports) {
                break;
            }

            /*
             * All linkedit-data load-commands should have the same cmdsize.
             */

            if (load_cmd.cmdsize != sizeof(struct linkedit_data_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct linkedit_data_command *const linkedit_data =
                (const struct linkedit_data_command *)lc_iter;

            const enum macho_file_parse_result parse_export_info_result =
                parse_export_trie_info(parse_info,
                                       info_in,
                                       linkedit_data->dataoff,
                                       linkedit_data->datasize,
                                       callback,
                                       cb_info);

            if (parse_export_info_result != E_MACHO_FILE_PARSE_OK) {
                return parse_export_info_result;
            }

            break;
        }

        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            /*
             * If exports aren't needed, skip the unnecessary parsing.
             */

            if (tbd_options.ignore_exports) {
                break;
            }

            /*
             * All dyld_info load-commands should have the same cmdsize.
             */

            if (load_cmd.cmdsize != sizeof(struct dyld_info_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct dyld_info_command *const dyld_info =
                (const struct dyld_info_command *)lc_iter;

            const enum macho_file_parse_result parse_export_info_result =
                parse_export_trie_info(parse_info,
                                       info_in,
                                       dyld_info->export_off,
                                       dyld_info->export_size,
                                       callback,
                                       cb_info);

            if (parse_export_info_result != E_MACHO_FILE_PARSE_OK) {
                return parse_export_info_result;
            }

            break;
        }

        case LC_ID_DYLIB: {
            /*
             * If no information is needed, skip the unnecessary parsing.
             */

            const uint64_t should_ignore =
                (tbd_options.ignore_current_version &&
                 tbd_options.ignore_compat_version &&
                 tbd_options.ignore_install_name);

            if (should_ignore) {
                parse_info->flags_in->found_identification = true;
                break;
            }

            /*
             * Every Dylib-Command needs to store its basic information, in
             * addition to a path-string.
             */

            if (load_cmd.cmdsize < sizeof(struct dylib_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct dylib_command *const dylib_command =
                (const struct dylib_command *)lc_iter;

            struct macho_file_parse_slc_flags *const flags_in =
                parse_info->flags_in;

            const bool is_big_endian = flags_in->is_big_endian;
            const char *install_name = NULL;
            uint32_t length = 0;

            bool ignore_install_name = tbd_options.ignore_install_name;
            if (!ignore_install_name) {
                uint32_t name_offset = dylib_command->dylib.name.offset;
                if (is_big_endian) {
                    name_offset = swap_uint32(name_offset);
                }

                const enum verify_string_result verify_string_result =
                    verify_string_offset(lc_iter,
                                         name_offset,
                                         sizeof(struct dylib_command),
                                         load_cmd.cmdsize,
                                         &length);

                switch (verify_string_result) {
                    case E_VERIFY_STRING_OK:
                        install_name = (const char *)lc_iter + name_offset;
                        break;

                    case E_VERIFY_STRING_INVALID:
                    case E_VERIFY_STRING_EMPTY: {
                        const bool should_continue =
                            call_callback(
                                callback,
                                info_in,
                                ERR_MACHO_FILE_PARSE_INVALID_INSTALL_NAME,
                                cb_info);

                        if (!should_continue) {
                            return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                        }

                        ignore_install_name = true;
                        break;
                    }
                }
            }

            struct dylib dylib = dylib_command->dylib;
            if (!flags_in->found_identification) {
                if (!tbd_options.ignore_current_version) {
                    if (is_big_endian) {
                        dylib.current_version =
                            swap_uint32(dylib.current_version);
                    }

                    info_in->fields.current_version = dylib.current_version;
                }

                if (!tbd_options.ignore_compat_version) {
                    uint32_t compat_version = dylib.compatibility_version;
                    if (is_big_endian) {
                        compat_version = swap_uint32(compat_version);
                    }

                    info_in->fields.compatibility_version = compat_version;
                }

                if (!ignore_install_name) {
                    if (parse_info->options.copy_strings) {
                        install_name = alloc_and_copy(install_name, length);
                        if (install_name == NULL) {
                            return E_MACHO_FILE_PARSE_ALLOC_FAIL;
                        }
                    }

                    if (yaml_c_str_needs_quotes(install_name, length)) {
                        info_in->flags.install_name_needs_quotes = true;
                    }

                    info_in->fields.install_name = install_name;
                    info_in->fields.install_name_length = length;
                }
            } else {
                uint32_t dylib_compat_version =
                    dylib.compatibility_version;

                if (is_big_endian) {
                    dylib.current_version = swap_uint32(dylib.current_version);
                    dylib_compat_version = swap_uint32(dylib_compat_version);
                }

                if (!tbd_options.ignore_current_version &&
                    info_in->fields.current_version != dylib.current_version)
                {
                    const bool should_continue =
                        call_callback(
                            callback,
                            info_in,
                            ERR_MACHO_FILE_PARSE_CURRENT_VERSION_CONFLICT,
                            cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }
                }

                uint32_t info_compat_version =
                    info_in->fields.compatibility_version;

                if (!tbd_options.ignore_compat_version &&
                    info_compat_version != dylib_compat_version)
                {
                    const bool should_continue =
                        call_callback(
                            callback,
                            info_in,
                            ERR_MACHO_FILE_PARSE_COMPAT_VERSION_CONFLICT,
                            cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }
                }

                if (!ignore_install_name) {
                    if (info_in->fields.install_name_length != length) {
                        const bool should_continue =
                            call_callback(
                                callback,
                                info_in,
                                ERR_MACHO_FILE_PARSE_INSTALL_NAME_CONFLICT,
                                cb_info);

                        if (!should_continue) {
                            return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                        }
                    }

                    const char *const info_install_name =
                        info_in->fields.install_name;

                    if (memcmp(info_install_name, install_name, length) != 0) {
                        const bool should_continue =
                            call_callback(
                                callback,
                                info_in,
                                ERR_MACHO_FILE_PARSE_INSTALL_NAME_CONFLICT,
                                cb_info);

                        if (!should_continue) {
                            return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                        }

                        break;
                    }
                }
            }

            flags_in->found_identification = true;
            break;
        }

        case LC_REEXPORT_DYLIB: {
            if (tbd_options.ignore_reexports) {
                break;
            }

            /*
             * Make sure the dylib-command is large enough to store its basic
             * information.
             */

            if (load_cmd.cmdsize < sizeof(struct dylib_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct dylib_command *const reexport_dylib =
                (const struct dylib_command *)lc_iter;

            const enum add_export_result add_reexport_result =
                add_export_to_info(info_in,
                                   parse_info->arch_index,
                                   TBD_SYMBOL_TYPE_REEXPORT,
                                   lc_iter,
                                   reexport_dylib->dylib.name.offset,
                                   sizeof(struct dylib_command),
                                   load_cmd.cmdsize,
                                   parse_info->flags_in->is_big_endian,
                                   tbd_options);

            switch (add_reexport_result) {
                case E_ADD_EXPORT_OK:
                    break;

                case E_ADD_EXPORT_INVALID:
                    return E_MACHO_FILE_PARSE_INVALID_REEXPORT;

                case E_ADD_EXPORT_ADD_FAIL:
                    return E_MACHO_FILE_PARSE_CREATE_SYMBOL_LIST_FAIL;
            }

            break;
        }

        case LC_SUB_CLIENT: {
            /*
             * If no sub-clients are needed, skip the unnecessary parsing.
             */

            if (tbd_options.ignore_clients) {
                break;
            }

            /*
             * Ensure that sub_client_command is large enough to store its
             * basic structure.
             *
             * An exact match cannot be made as sub_client_command includes a
             * full client-string in its cmdsize.
             */

            if (load_cmd.cmdsize < sizeof(struct sub_client_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct sub_client_command *const client_command =
                (const struct sub_client_command *)lc_iter;

            const enum add_export_result add_client_result =
                add_export_to_info(info_in,
                                   parse_info->arch_index,
                                   TBD_SYMBOL_TYPE_CLIENT,
                                   lc_iter,
                                   client_command->client.offset,
                                   sizeof(struct sub_client_command),
                                   load_cmd.cmdsize,
                                   parse_info->flags_in->is_big_endian,
                                   tbd_options);

            switch (add_client_result) {
                case E_ADD_EXPORT_OK:
                    break;

                case E_ADD_EXPORT_INVALID:
                    return E_MACHO_FILE_PARSE_INVALID_CLIENT;

                case E_ADD_EXPORT_ADD_FAIL:
                    return E_MACHO_FILE_PARSE_CREATE_SYMBOL_LIST_FAIL;
            }

            break;
        }

        case LC_SUB_FRAMEWORK: {
            /*
             * If no parent-umbrella is needed, skip the unnecessary parsing.
             */

            if (tbd_options.ignore_parent_umbrellas) {
                break;
            }

            /*
             * Make sure the framework-command is large enough to store its
             * basic information.
             */

            if (load_cmd.cmdsize < sizeof(struct sub_framework_command)) {
                return E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND;
            }

            const struct sub_framework_command *const framework_command =
                (const struct sub_framework_command *)lc_iter;

            uint32_t umbrella_offset = framework_command->umbrella.offset;
            uint32_t length = 0;

            if (parse_info->flags_in->is_big_endian) {
                umbrella_offset = swap_uint32(umbrella_offset);
            }

            const enum verify_string_result verify_string_result =
                verify_string_offset(lc_iter,
                                     umbrella_offset,
                                     sizeof(struct sub_umbrella_command),
                                     load_cmd.cmdsize,
                                     &length);

            switch (verify_string_result) {
                case E_VERIFY_STRING_OK:
                    break;

                case E_VERIFY_STRING_EMPTY:
                    return E_MACHO_FILE_PARSE_OK;

                case E_VERIFY_STRING_INVALID: {
                    const bool should_continue =
                        call_callback(
                            callback,
                            info_in,
                            ERR_MACHO_FILE_PARSE_INVALID_PARENT_UMBRELLA,
                            cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }

                    break;
                }
            }

            const char *const umbrella =
                (const char *)lc_iter + umbrella_offset;

            const enum tbd_ci_add_parent_umbrella_result add_umbrella_result =
                tbd_ci_add_parent_umbrella(info_in,
                                           umbrella,
                                           length,
                                           parse_info->arch_index,
                                           tbd_options);

            switch (add_umbrella_result) {
                case E_TBD_CI_ADD_PARENT_UMBRELLA_OK:
                    break;

                case E_TBD_CI_ADD_PARENT_UMBRELLA_ALLOC_FAIL:
                case E_TBD_CI_ADD_PARENT_UMBRELLA_ARRAY_FAIL:
                    return E_MACHO_FILE_PARSE_CREATE_SYMBOL_LIST_FAIL;

                case E_TBD_CI_ADD_PARENT_UMBRELLA_INFO_CONFLICT: {
                    const bool should_continue =
                        call_callback(
                            callback,
                            info_in,
                            ERR_MACHO_FILE_PARSE_PARENT_UMBRELLA_CONFLICT,
                            cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }

                    break;
                }
            }

            break;
        }

        case LC_SYMTAB: {
            /*
             * If exports and undefineds aren't needed, skip the unnecessary
             * parsing.
             */

            if (tbd_options.ignore_exports && tbd_options.ignore_undefineds) {
                break;
            }

            /*
             * All symtab load-commands should be the same size.
             */

            if (load_cmd.cmdsize != sizeof(struct symtab_command)) {
                return E_MACHO_FILE_PARSE_INVALID_SYMBOL_TABLE;
            }

            struct symtab_command *const existing_symtab =
                parse_info->symtab_out;

            const struct symtab_command *const symtab =
                (const struct symtab_command *)lc_iter;

            if (existing_symtab->cmd != 0) {
                if (memcmp(existing_symtab, symtab, sizeof(*symtab)) != 0) {
                    const bool should_continue =
                        call_callback(
                            callback,
                            info_in,
                            ERR_MACHO_FILE_PARSE_SYMBOL_TABLE_CONFLICT,
                            cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }

                    break;
                }
            }

            *existing_symtab = *symtab;
            break;
        }

        case LC_UUID: {
            /*
             * If uuids aren't needed, skip the unnecessary parsing.
             */

            if (tbd_options.ignore_uuids) {
                break;
            }

            if (load_cmd.cmdsize != sizeof(struct uuid_command)) {
                const bool should_continue =
                    call_callback(callback,
                                  info_in,
                                  ERR_MACHO_FILE_PARSE_INVALID_UUID,
                                  cb_info);

                if (!should_continue) {
                    return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                }

                break;
            }

            const struct uuid_command *const uuid_cmd =
                (const struct uuid_command *)lc_iter;

            if (parse_info->flags_in->found_uuid) {
                const uint8_t *const uuid_cmd_uuid = uuid_cmd->uuid;
                const uint8_t *const found_uuid = parse_info->uuid_in;

                if (memcmp(found_uuid, uuid_cmd_uuid, 16) != 0) {
                    const bool should_continue =
                        call_callback(callback,
                                      info_in,
                                      ERR_MACHO_FILE_PARSE_UUID_CONFLICT,
                                      cb_info);

                    if (!should_continue) {
                        return E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK;
                    }

                    break;
                }
            } else {
                memcpy(parse_info->uuid_in, uuid_cmd->uuid, 16);
                parse_info->flags_in->found_uuid = true;
            }

            break;
        }

        case LC_VERSION_MIN_MACOSX: {
            const enum macho_file_parse_result parse_vm_result =
                parse_version_min_lc(parse_info,
                                     info_in,
                                     load_cmd,
                                     TBD_PLATFORM_MACOS,
                                     callback,
                                     cb_info,
                                     tbd_options);

            if (parse_vm_result != E_MACHO_FILE_PARSE_OK) {
                return parse_vm_result;
            }

            break;
        }

        case LC_VERSION_MIN_IPHONEOS: {
            const enum macho_file_parse_result parse_vm_result =
                parse_version_min_lc(parse_info,
                                     info_in,
                                     load_cmd,
                                     TBD_PLATFORM_IOS,
                                     callback,
                                     cb_info,
                                     tbd_options);

            if (parse_vm_result != E_MACHO_FILE_PARSE_OK) {
                return parse_vm_result;
            }

            break;
        }

        case LC_VERSION_MIN_WATCHOS: {
            const enum macho_file_parse_result parse_vm_result =
                parse_version_min_lc(parse_info,
                                     info_in,
                                     load_cmd,
                                     TBD_PLATFORM_WATCHOS,
                                     callback,
                                     cb_info,
                                     tbd_options);

            if (parse_vm_result != E_MACHO_FILE_PARSE_OK) {
                return parse_vm_result;
            }

            break;
        }

        case LC_VERSION_MIN_TVOS: {
            const enum macho_file_parse_result parse_vm_result =
                parse_version_min_lc(parse_info,
                                     info_in,
                                     load_cmd,
                                     TBD_PLATFORM_TVOS,
                                     callback,
                                     cb_info,
                                     tbd_options);

            if (parse_vm_result != E_MACHO_FILE_PARSE_OK) {
                return parse_vm_result;
            }

            break;
        }

        default:
            break;
    }

    return E_MACHO_FILE_PARSE_OK;
}
