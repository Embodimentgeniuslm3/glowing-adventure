//
//  src/parse_or_list_fields.c
//  tbd
//
//  Created by inoahdev on 12/02/18.
//  Copyright © 2018 - 2020 inoahdev. All rights reserved.
//

#include <stdlib.h>
#include <string.h>

#include "macho_file.h"
#include "parse_or_list_fields.h"
#include "target_list.h"
#include "tbd.h"

struct target_list
parse_architectures_list(int index,
                         const int argc,
                         char *const *__notnull const argv,
                         int *__notnull const index_out)
{
    struct target_list list = {};

    do {
        const char *const arg = argv[index];
        const char arg_front = arg[0];

        /*
         * Quickly check if our arch-string is either a path-string or an option
         * to avoid an unnecessary arch-info lookup.
         */

        if (arg_front == '-' || arg_front == '/') {
            if (list.set_count == 0) {
                fputs("Please provide a list of architectures\n", stderr);
                exit(1);
            }

            break;
        }

        const struct arch_info *const arch = arch_info_for_name(arg);
        if (arch == NULL) {
            /*
             * At least one architecture must be provided for the list.
             */

            if (list.set_count == 0) {
                fprintf(stderr,
                        "Unrecognized architecture (with name %s)\n",
                        arg);

                exit(1);
            }

            break;
        }

        const bool has_arch = target_list_has_arch(&list, arch);
        if (!has_arch) {
            const enum target_list_result add_target_result =
                target_list_add_target(&list, arch, TBD_PLATFORM_NONE);

            if (add_target_result != E_TARGET_LIST_OK) {
                fprintf(stderr,
                        "INTERNAL: Failed to add arch %s to list\n",
                        arg);
                exit(1);
            }
        } else {
            fprintf(stderr, "Note: Arch %s has been provided twice\n", arg);
        }

        index++;
        if (index == argc) {
            break;
        }
    } while (true);


    /*
     * Subtract one from index as we're supposed to end with the index pointing
     * to the last argument.
     */

    *index_out = index - 1;
    return list;
}

struct tbd_flags
parse_flags_list(int index,
                 const int argc,
                 char *const *__notnull const argv,
                 int *__notnull const index_out)
{
    struct tbd_flags flags = {};
    for (; index != argc; index++) {
        const char *const arg = argv[index];
        if (strcmp(arg, "flat_namespace") == 0) {
            if (flags.flat_namespace) {
                fputs("Note: tbd-flag flat_namespace was provided twice\n",
                      stdout);
            } else {
                flags.flat_namespace = true;
            }
        } else if (strcmp(arg, "not_app_extension_safe") == 0) {
            if (flags.not_app_extension_safe) {
                fputs("Note: tbd-flag not_app_extension_safe was provided "
                      "twice\n",
                      stdout);
            } else {
                flags.not_app_extension_safe = true;
            }
        } else {
            if (flags.value != 0) {
                break;
            }

            const char front = arg[0];
            switch (front) {
                case '-':
                case '/':
                    fputs("Please provide a list of tbd-flags\n", stderr);
                    break;

                default:
                    fprintf(stderr, "Unrecognized flag: %s\n", arg);
                    break;
            }

            exit(1);
        }
    }

    /*
     * Subtract one from index as we're supposed to end with the index pointing
     * to the last argument.
     */

    *index_out = index - 1;
    return flags;
}

enum tbd_objc_constraint
parse_objc_constraint(const char *__notnull const constraint) {
    if (strcmp(constraint, "none") == 0) {
        return TBD_OBJC_CONSTRAINT_NONE;
    } else if (strcmp(constraint, "retain_release") == 0) {
        return TBD_OBJC_CONSTRAINT_RETAIN_RELEASE;
    } else if (strcmp(constraint, "retain_release_for_simulator") == 0) {
        return TBD_OBJC_CONSTRAINT_RETAIN_RELEASE_FOR_SIMULATOR;
    } else if (strcmp(constraint, "retain_release_or_gc") == 0) {
        return TBD_OBJC_CONSTRAINT_RETAIN_RELEASE_OR_GC;
    } else if (strcmp(constraint, "gc") == 0) {
        return TBD_OBJC_CONSTRAINT_GC;
    }

    return TBD_OBJC_CONSTRAINT_NO_VALUE;
}

enum tbd_platform parse_platform(const char *__notnull const platform) {
    if (strcmp(platform, "macosx") == 0) {
        return TBD_PLATFORM_MACOS;
    } else if (strcmp(platform, "ios") == 0) {
        return TBD_PLATFORM_IOS;
    } else if (strcmp(platform, "watchos") == 0) {
        return TBD_PLATFORM_WATCHOS;
    } else if (strcmp(platform, "tvos") == 0) {
        return TBD_PLATFORM_TVOS;
    } else if (strcmp(platform, "bridgeos") == 0) {
        return TBD_PLATFORM_BRIDGEOS;
    } else if (strcmp(platform, "iosmac") == 0) {
        return TBD_PLATFORM_IOSMAC;
    } else if (strcmp(platform, "driverkit") == 0) {
        return TBD_PLATFORM_DRIVERKIT;
    }

    return TBD_PLATFORM_NONE;
}

static inline bool ch_is_digit(const char ch) {
    return ((uint8_t)(ch - '0') < 10);
}

static inline
uint32_t append_ch_to_number(const uint32_t number, const char ch) {
    return ((number * 10) + (ch & 0xf));
}

uint32_t parse_swift_version(const char *__notnull const arg) {
    if (strcmp(arg, "1.2") == 0) {
        return 2;
    }

    uint32_t version = 0;
    const char *iter = arg;

    for (char ch = *iter; ch != '\0'; ch = *(++iter)) {
        if (!ch_is_digit(ch)) {
            return 0;
        }

        const uint32_t new_version = append_ch_to_number(version, ch);

        /*
         * Check for any overflows when parsing out the number.
         */

        if (new_version < version) {
            return 0;
        }

        version = new_version;
    }

    if (version > 1) {
        version++;
    }

    return version;
}

static char *find_dash(char *const str) {
    char *iter = str;
    for (char ch = *iter; ch != '\0'; ch = *(++iter)) {
        if (ch == '-') {
            return iter;
        }
    }

    return NULL;
}

struct target_list
parse_targets_list(int index,
                   const int argc,
                   char *const *__notnull const argv,
                   int *__notnull index_out)
{
    struct target_list list = {};

    do {
        char *const arg = argv[index];

        /*
         * Catch any path-string or option to avoid unnecessary parsing.
         */

        const char arg_front = *arg;
        if (arg_front == '-' || arg_front == '/') {
            if (list.set_count == 0) {
                fputs("Please provide a list of targets\n", stderr);
                exit(1);
            }

            break;
        }

        char *const sep_dash = find_dash(arg);
        if (sep_dash == NULL) {
            if (list.set_count == 0) {
                fprintf(stderr, "Unrecognized target: %s\n", arg);
                exit(1);
            }

            break;
        }

        /*
         * Set the char at sep_dash to '\0' to give us the arch-string.
         */

        *sep_dash = '\0';
        const struct arch_info *const arch = arch_info_for_name(arg);

        if (arch == NULL) {
            /*
             * We may have a relative path-string that has a dash in the first
             * path-component.
             */

            if (list.set_count == 0) {
                fprintf(stderr, "Unrecognized architecture: %s\n", arg);
                exit(1);
            }

            break;
        }

        *sep_dash = '-';

        const char *const platform_str = sep_dash + 1;
        if (*platform_str == '\0') {
            /*
             * At this point, it's highly unlikely that we have a relative
             * path-string, that starts off with an arch-string followed by a
             * dash.
             *
             * The possibility may still remain, but we choose to ignore it.
             */

            fprintf(stderr,
                    "Please provide a platform for the target %s\n",
                    arg);
            exit(1);
        }

        const enum tbd_platform platform = parse_platform(platform_str);
        if (platform == TBD_PLATFORM_NONE) {
            fprintf(stderr, "Unrecognized platform: %s\n", platform_str);
            exit(1);
        }

        const bool has_target = target_list_has_target(&list, arch, platform);
        if (!has_target) {
            const enum target_list_result add_target_result =
                target_list_add_target(&list, arch, platform);

            if (add_target_result != E_TARGET_LIST_OK) {
                fprintf(stderr,
                        "INTERNAL: Failed to add target %s to list\n",
                        arg);

                exit(1);
            }
        } else {
            fprintf(stderr, "Note: Target %s has been provided twice\n", arg);
        }

        index++;
        if (index == argc) {
            break;
        }
    } while (true);

    *index_out = index - 1;
    return list;
}

enum tbd_version parse_tbd_version(const char *__notnull const version) {
    if (strcmp(version, "v1") == 0) {
        return TBD_VERSION_V1;
    } else if (strcmp(version, "v2") == 0) {
        return TBD_VERSION_V2;
    } else if (strcmp(version, "v3") == 0) {
        return TBD_VERSION_V3;
    } else if (strcmp(version, "v4") == 0) {
        return TBD_VERSION_V4;
    }

    return TBD_VERSION_NONE;
}

static int
parse_component_til_ch(const char *__notnull iter,
                       const char end_ch,
                       const int max,
                       const char **__notnull iter_out)
{
    int result = 0;
    char ch = *iter;

    if (ch == end_ch || ch == '\0') {
        return -1;
    }

    do {
        if (ch_is_digit(ch)) {
            const int new_result = append_ch_to_number(result, ch);

            /*
             * Don't go above max, and don't overflow.
             */

            if (new_result > max || new_result < result) {
                return -1;
            }

            ch = *(++iter);
            result = new_result;

            if (ch == end_ch) {
                iter++;
                break;
            } else if (ch == '\0') {
                iter = NULL;
                break;
            }

            continue;
        }

        return -1;
    } while (true);

    *iter_out = iter;
    return result;
}

static inline uint32_t
create_packed_version(const uint16_t major,
                      const uint8_t minor,
                      const uint8_t revision)
{
    return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | revision;
}

int64_t parse_packed_version(const char *__notnull const version) {
    const char *iter = version;
    const int major = parse_component_til_ch(iter, '.', UINT16_MAX, &iter);

    if (major == -1) {
        return -1;
    }

    if (iter == NULL) {
        return create_packed_version(major, 0, 0);
    }

    const int minor = parse_component_til_ch(iter, '.', UINT8_MAX, &iter);
    if (minor == -1) {
        return -1;
    }

    if (iter == NULL) {
        return create_packed_version(major, minor, 0);
    }

    const int revision = parse_component_til_ch(iter, '\0', UINT8_MAX, &iter);
    if (revision == -1) {
        return -1;
    }

    return create_packed_version(major, minor, revision);
}

void print_arch_info_list(void) {
    const struct arch_info *info = arch_info_get_list();
    for (const char *name = info->name; name != NULL; name = (++info)->name) {
        fprintf(stdout, "%s\n", name);
    }
}

void print_objc_constraint_list(void) {
    fputs("none\n"
          "retain_release\n"
          "retain_release_or_gc\n"
          "retain_release_for_simulator\n"
          "gc\n",
          stdout);
}

void print_platform_list(void) {
    fputs("macosx\n"
          "ios\n"
          "watchos\n"
          "tvos\n"
          "bridgeos\n"
          "iosmac\n"
          "driverkit\n",
          stdout);
}

void print_tbd_flags_list(void) {
    fputs("flat_namespace\n"
          "not_app_extension_safe\n",
          stdout);
}

void print_tbd_version_list(void) {
    fputs("v1\n"
          "v2\n"
          "v3\n"
          "v4\n",
          stdout);
}
