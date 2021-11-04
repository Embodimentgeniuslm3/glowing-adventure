//
//  src/macho_file_parse_export_trie.c
//  tbd
//
//  Created by inoahdev on 11/3/19.
//  Copyright © 2019 - 2020 inoahdev. All rights reserved.
//

#include <stdint.h>
#include <string.h>

#include "dsc_image.h"
#include "guard_overflow.h"
#include "likely.h"
#include "macho_file.h"
#include "macho_file_parse_export_trie.h"
#include "our_io.h"
#include "string_buffer.h"

static inline uint8_t uleb_byte_get_has_next(const uint8_t byte) {
    return (byte & 0x80);
}

static inline uint8_t uleb_byte_get_bits(const uint8_t byte) {
    return (byte & 0x7f);
}

const uint8_t *
read_uleb128_32(const uint8_t *__notnull iter,
                const uint8_t *__notnull const end,
                uint32_t *__notnull const result_out)
{
    /*
     * uleb128 format is as follows:
     * Every byte with the MSB set to 1 indicates that the byte and the next
     * byte are part of the uleb128 format.
     *
     * The uleb128's integer contents are stored in the 7 LSBs, and are combined
     * into a single integer by placing every 7 bits right to the left (MSB) of
     * the previous 7 bits stored.
     *
     * Ex:
     *     10000110 10010100 00101000
     *
     * In the example above, the first two bytes have the MSB set, meaning that
     * the same byte, and the byte after, are in that uleb128.
     *
     * In this case, all three bytes are used.
     *
     * The lower 7 bits of the integer are all combined together as described
     * earlier.
     *
     * Parsing into an integer should result in the following:
     *     0101000 0010100 0000110
     *
     * Here, the 7 bits are stored in the order opposite to how they were found.
     *
     * The first byte's 7 bits are stored in the 3rd component, the second
     * byte's 7 bits are in the second component, and the third byte's 7 bits
     * are stored in the 1st component.
     */

    uint8_t byte = *iter;
    uint8_t has_next = uleb_byte_get_has_next(byte);

    iter++;
    if (has_next == 0) {
        *result_out = byte;
        return iter;
    }

    if (unlikely(iter == end)) {
        return NULL;
    }

    uint8_t bits = uleb_byte_get_bits(byte);
    uint32_t result = bits;

    for (uint8_t shift = 7; shift != 28; shift += 7) {
        byte = *iter;
        bits = uleb_byte_get_bits(byte);

        result |= ((uint32_t)bits << shift);
        has_next = uleb_byte_get_has_next(byte);
        iter++;

        if (has_next == 0) {
            *result_out = result;
            return iter;
        }

        if (unlikely(iter == end)) {
            return NULL;
        }
    }

    byte = *iter;
    bits = uleb_byte_get_bits(byte);

    if (bits > 15) {
        return NULL;
    }

    has_next = uleb_byte_get_has_next(byte);
    if (has_next) {
        return NULL;
    }

    iter++;
    result |= ((uint32_t)bits << 28);
    *result_out = result;

    return iter;
}

const uint8_t *
read_uleb128_64(const uint8_t *__notnull iter,
                const uint8_t *__notnull const end,
                uint64_t *__notnull const result_out)
{
    /*
     * uleb128 format is as follows:
     * Every byte with the MSB set to 1 indicates that the byte and the next
     * byte are part of the uleb128 format.
     *
     * The uleb128's integer contents are stored in the 7 LSBs, and are combined
     * into a single integer by placing every 7 bits right to the left (MSB) of
     * the previous 7 bits stored.
     *
     * Ex:
     *     10000110 10010100 00101000
     *
     * In the example above, the first two bytes have the MSB set, meaning that
     * the same byte, and the byte after, are in that uleb128.
     *
     * In this case, all three bytes are used.
     *
     * The lower 7 bits of the integer are all combined together as described
     * earlier.
     *
     * Parsing into an integer should result in the following:
     *     0101000 0010100 0000110
     *
     * Here, the 7 bits are stored in the order opposite to how they were found.
     *
     * The first byte's 7 bits are stored in the 3rd component, the second
     * byte's 7 bits are in the second component, and the third byte's 7 bits
     * are stored in the 1st component.
     */

    uint8_t byte = *iter;
    uint8_t has_next = uleb_byte_get_has_next(byte);

    iter++;
    if (has_next == 0) {
        *result_out = byte;
        return iter;
    }

    if (unlikely(iter == end)) {
        return NULL;
    }

    uint8_t bits = uleb_byte_get_bits(byte);
    uint64_t result = bits;

    for (uint8_t shift = 7; shift != 63; shift += 7) {
        byte = *iter;
        bits = uleb_byte_get_bits(byte);

        result |= ((uint64_t)bits << shift);
        has_next = uleb_byte_get_has_next(byte);
        iter++;

        if (has_next == 0) {
            *result_out = result;
            return iter;
        }

        if (unlikely(iter == end)) {
            return NULL;
        }
    }

    byte = *iter;
    bits = uleb_byte_get_bits(byte);

    if (unlikely(bits > 1)) {
        return NULL;
    }

    has_next = uleb_byte_get_has_next(byte);
    if (unlikely(has_next)) {
        return NULL;
    }

    iter++;
    result |= ((uint64_t)bits << 63);
    *result_out = result;

    return iter;
}

const uint8_t *
skip_uleb128(const uint8_t *__notnull iter, const uint8_t *__notnull const end)
{
    for (uint8_t i = 0; i != 9; i++) {
        const uint8_t byte = *iter;
        const uint8_t has_next = uleb_byte_get_has_next(byte);

        iter++;
        if (has_next == 0) {
            return iter;
        }

        if (iter == end) {
            break;
        }
    }

    return NULL;
}

static bool
has_overlapping_range(const struct range list[const 128],
                      const uint64_t count,
                      const struct range range)
{
    const struct range *l_range = list;
    const struct range *const end = list + count;

    for (; l_range != end; l_range++) {
        if (ranges_overlap(*l_range, range)) {
            return true;
        }
    }

    return false;
}

/*
 * The export-trie is a compressed tree designed to store symbols and other info
 * in an efficient fashion.
 *
 * To better understand the format, let's create a trie of the symbols
 * "_symbol", "__symcol", and "_abcdef", "_abcghi", and work backwrds.
 *
 * Here's a tree showing how the compressed trie would look like:
 *              "_"
 *            /     \
 *       "sym"       "abc"
 *       /   \        /  \
 *  "bol"    "col" "def"  "ghi"
 *
 * As you can see, the symbols are parsed to find common prefixes, and then
 * placed in a tree where they can later be reconstructed back into full-fledged
 * symbols.
 *
 * To store them in binary, each node on the tree is stored in a tree-node.
 *
 * Every tree-node contains two properties - its structure (terminal) size and
 * the children-count. The structure size and children-count are both stored in
 * the uleb128 format described above.
 *
 * So in C, a basic tree-node may look something like the following:
 *
 * struct tree_node {
 *     uleb128 terminal_size; // structure-size
 *     uleb128 children_count;
 * }
 *
 * If the tree-node's terminal-size is zero, the tree-node simply contains the
 * children-count, and an array of children mini-nodes that follow directly
 * after the tree-node.
 *
 * Every tree-node is pointed to by a child (mini-node) belonging to another
 * tree-node, except the first tree-node. Until the terminal-size is a non-zero
 * value, indicating that tree-node has reached its end, creating a full symbol,
 * the tree of nodes will keep extending down.
 *
 * Each child (mini-node) contains a label (the string, as shown in the tree
 * above), and a uleb128 index to the next node in that path. The uleb128 index
 * is relative to the start of the export-trie, and not relative to the start of
 * the current tree-node.
 *
 * So for the tree provided above, the top-level node would have one child,
 * whose label is '_', and an index to another tree-node, which provides info
 * for the '_' label.
 *
 * That tree-node would have two children, for both the "sym" and the "abc"
 * components, and these two children would each have their two children for the
 * "bol", "col", "def", "ghi" components.
 *
 * However, if the terminal-size is non-zero, the previous node encountered gave
 * the last bit of string we needed to create the symbol.
 *
 * In this case, the present node would be the final node to parse, and would
 * contain information on the symbol.
 *
 * The final export-node structure stores a uleb128 flags field.
 *
 * The flags field stores the 'kind' of the symbol, whether its a normal symbol,
 * weakly-defined, or a thread-local symbol.
 *
 * The flags field also specifies whether the export is a re-export or not, and
 * as such what format the fields after the flags field are in.
 *
 * For a re-export export-node, a dylib-ordinal uleb128 immediately follows the
 * flags field.
 *
 * The dylib-ordinal is the number (starting from 1) of the dylibs imported via
 * the LC_REEXPORT_DYLIB and other load-commands. The ordinal describes where
 * the symbol is re-exported from.
 *
 * Following the dylib-ordinal is the re-export string. If the string is not
 * empty, the string describes what symbol the export-node's is re-exported
 * from.
 *
 * Re-exports are used to provide additional symbols, to one that already exist,
 * for example, the export-node _bzero is re-exported from _platform_bzero in
 * another library file.
 *
 * On the other hand, a non-reexport export-node has an address uleb128 field
 * follow it.
 *
 * If the export is non-reexport, as well as a stub-resolver, the address of the
 * stub-resolver's own stub-resolver is given in another uleb128.
 *
 * Basically, the structures can be imagined in the following:
 *
 * struct final_export_node_reexport {
 *     uleb128 terminal_size;
 *     uleb128 flags;
 *     uleb128 dylib_ordinal;
 *     char reexport_symbol[]; // may be empty.
 * };
 *
 * struct final_export_node_non_reexport {
 *     uleb128 terminal_size;
 *     uleb128 flags;
 *     uleb128 address;
 * };
 *
 * struct final_export_node_non_reexport_stub_resolver {
 *     uleb128 terminal_size;
 *     uleb128 flags;
 *     uleb128 address;
 *     uleb128 address_of_self_stub_resolver;
 * };
 *
 * Every export-node has an array at the very end of the data-structure. Each
 * entry in this array has a null-terminated string, to append to the cumulative
 * string. In addition, each entry has an uleb128 offset to the next uleb128,
 * relative to the export-trie base.
 *
 * Basically, this is a child:
 *     struct export_node_child {
 *         char string[];
 *         uleb128 next;
 *     };
 */

enum macho_file_parse_result
parse_trie_node(struct tbd_create_info *__notnull const info_in,
                const uint64_t arch_index,
                const uint8_t *__notnull const start,
                const uint32_t offset,
                const uint8_t *__notnull const end,
                struct range node_ranges[const 128],
                uint8_t node_ranges_count,
                const uint32_t export_size,
                struct string_buffer *__notnull const sb_buffer,
                const struct tbd_parse_options options)
{
    const uint8_t *iter = start + offset;
    uint64_t iter_size = 0;

    if ((iter = read_uleb128_64(iter, end, &iter_size)) == NULL) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    if (unlikely(iter == end)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    uint32_t iter_off_end = offset;
    if (guard_overflow_add(&iter_off_end, iter_size)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    const uint8_t *const node_start = iter;
    const uint8_t *const children = iter + iter_size;

    if (unlikely(children > end)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    const struct range export_range = {
        .begin = offset,
        .end = offset + iter_size
    };

    const bool has_overlap_range =
        has_overlapping_range(node_ranges, node_ranges_count, export_range);

    if (unlikely(has_overlap_range)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    node_ranges[node_ranges_count] = export_range;
    node_ranges_count++;

    const bool is_export_info = (iter_size != 0);
    if (is_export_info) {
        /*
         * This should only occur if the first tree-node is an export-node, but
         * check anyways as this is invalid behavior.
         */

        if (sb_buffer->length == 0) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        uint64_t flags = 0;
        if ((iter = read_uleb128_64(iter, end, &flags)) == NULL) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        if (unlikely(iter == end)) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        const uint8_t kind = (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK);
        if (flags != 0) {
            if (kind != EXPORT_SYMBOL_FLAGS_KIND_REGULAR &&
                kind != EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE &&
                kind != EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL)
            {
                return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
            }
        }

        enum tbd_symbol_meta_type meta_type = TBD_SYMBOL_META_TYPE_EXPORT;
        if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
            if ((iter = skip_uleb128(iter, end)) == NULL) {
                return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
            }

            if (unlikely(iter == end)) {
                return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
            }

            if (unlikely(*iter != '\0')) {
                iter++;

                const uint32_t maxlen = (uint32_t)(end - iter);
                const uint32_t length = (uint32_t)strnlen((char *)iter, maxlen);

                /*
                 * We can't have a re-export whose install-name reaches the end
                 * of the export-trie.
                 */

                if (unlikely(length == maxlen)) {
                    return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
                }

                /*
                 * Skip past the null-terminator.
                 */

                iter += (length + 1);
            } else {
                iter++;
            }

            meta_type = TBD_SYMBOL_META_TYPE_REEXPORT;
        } else {
            if ((iter = skip_uleb128(iter, end)) == NULL) {
                return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
            }

            if (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
                if (unlikely(iter == end)) {
                    return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
                }

                if ((iter = skip_uleb128(iter, end)) == NULL) {
                    return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
                }
            }
        }

        const uint8_t *const expected_end = node_start + iter_size;
        if (unlikely(iter != expected_end)) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        enum tbd_symbol_type predefined_type = TBD_SYMBOL_TYPE_NONE;
        switch (kind) {
            case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                if (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION) {
                    predefined_type = TBD_SYMBOL_TYPE_WEAK_DEF;
                }

                break;

            case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
                predefined_type = TBD_SYMBOL_TYPE_THREAD_LOCAL;
                break;

            default:
                break;
        }

        const enum tbd_ci_add_data_result add_symbol_result =
            tbd_ci_add_symbol_with_info_and_len(info_in,
                                                sb_buffer->data,
                                                sb_buffer->length,
                                                arch_index,
                                                predefined_type,
                                                meta_type,
                                                true,
                                                options);

        if (add_symbol_result != E_TBD_CI_ADD_DATA_OK) {
            return E_MACHO_FILE_PARSE_CREATE_SYMBOL_LIST_FAIL;
        }
    }

    const uint8_t children_count = *iter;
    if (children_count == 0) {
        return E_MACHO_FILE_PARSE_OK;
    }

    /*
     * From dyld, don't parse an export-trie that gets too deep.
     */

    if (unlikely(node_ranges_count == 128)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    iter++;
    if (unlikely(iter == end)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    /*
     * Every child shares only the same symbol-prefix, and only the same
     * node-ranges, both of which we need to restore to its original amount
     * after every loop.
     */

    const uint32_t orig_buff_length = (uint32_t)sb_buffer->length;
    const uint8_t orig_node_ranges_count = (uint8_t)node_ranges_count;

    for (uint8_t i = 0; i != children_count; i++) {
        /*
         * Pass the length-calculation of the string to strnlen in the hopes of
         * better performance.
         */

        const uint32_t max_length = (uint32_t)(end - iter);
        const uint32_t length = (uint32_t)strnlen((char *)iter, max_length);

        /*
         * We can't have the string reach the end of the export-trie.
         */

        if (unlikely(length == max_length)) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        const enum string_buffer_result add_c_str_result =
            sb_add_c_str(sb_buffer, (char *)iter, length);

        if (unlikely(add_c_str_result != E_STRING_BUFFER_OK)) {
            return E_MACHO_FILE_PARSE_ALLOC_FAIL;
        }

        /*
         * Skip past the null-terminator.
         */

        iter += (length + 1);

        uint32_t next = 0;
        if ((iter = read_uleb128_32(iter, end, &next)) == NULL) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        if (unlikely(iter == end)) {
            if (i != children_count - 1) {
                return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
            }
        }

        if (unlikely(next >= export_size)) {
            return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
        }

        const enum macho_file_parse_result parse_export_result =
            parse_trie_node(info_in,
                            arch_index,
                            start,
                            next,
                            end,
                            node_ranges,
                            node_ranges_count,
                            export_size,
                            sb_buffer,
                            options);

        if (unlikely(parse_export_result != E_MACHO_FILE_PARSE_OK)) {
            return parse_export_result;
        }

        node_ranges_count = orig_node_ranges_count;
        sb_buffer->length = orig_buff_length;
    }

    return E_MACHO_FILE_PARSE_OK;
}

enum macho_file_parse_result
macho_file_parse_export_trie_from_file(
    const struct macho_file_parse_export_trie_args args,
    const int fd,
    const uint64_t base_offset)
{
    /*
     * Validate the dyld-info exports-range.
     */

    if (args.export_size < 2) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    uint64_t export_end = args.export_off;
    if (guard_overflow_add(&export_end, args.export_size)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    uint64_t full_export_off = base_offset;
    if (guard_overflow_add(&full_export_off, args.export_off)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    uint64_t full_export_end = base_offset;
    if (guard_overflow_add(&full_export_end, export_end)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    const struct range full_export_range = {
        .begin = full_export_off,
        .end = full_export_end
    };

    if (!range_contains_other(args.available_range, full_export_range)) {
        return E_MACHO_FILE_PARSE_INVALID_RANGE;
    }

    if (our_lseek(fd, full_export_off, SEEK_SET) < 0) {
        return E_MACHO_FILE_PARSE_SEEK_FAIL;
    }

    uint8_t *const export_trie = malloc(args.export_size);
    if (export_trie == NULL) {
        return E_MACHO_FILE_PARSE_ALLOC_FAIL;
    }

    if (our_read(fd, export_trie, args.export_size) < 0) {
        free(export_trie);
        return E_MACHO_FILE_PARSE_READ_FAIL;
    }

    struct range node_ranges[128] = {};

    const uint8_t node_ranges_count = 0;
    const uint8_t *const end = export_trie + args.export_size;

    const enum macho_file_parse_result parse_node_result =
        parse_trie_node(args.info_in,
                        args.arch_index,
                        export_trie,
                        0,
                        end,
                        node_ranges,
                        node_ranges_count,
                        args.export_size,
                        args.sb_buffer,
                        args.tbd_options);

    free(export_trie);

    if (parse_node_result != E_MACHO_FILE_PARSE_OK) {
        return parse_node_result;
    }

    return E_MACHO_FILE_PARSE_OK;
}

enum macho_file_parse_result
macho_file_parse_export_trie_from_map(
    const struct macho_file_parse_export_trie_args args,
    const uint8_t *__notnull map)
{
    /*
     * Validate the dyld-info exports-range.
     */

    if (args.export_size < 2) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    uint64_t export_end = args.export_off;
    if (guard_overflow_add(&export_end, args.export_size)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    const struct range export_range = {
        .begin = args.export_off,
        .end = export_end
    };

    if (!range_contains_other(args.available_range, export_range)) {
        return E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE;
    }

    const uint8_t *const export_trie = map + args.export_off;
    const uint8_t *const end = export_trie + args.export_size;

    struct range node_ranges[128] = {};
    uint8_t node_ranges_count = 0;

    const enum macho_file_parse_result parse_node_result =
        parse_trie_node(args.info_in,
                        args.arch_index,
                        export_trie,
                        0,
                        end,
                        node_ranges,
                        node_ranges_count,
                        args.export_size,
                        args.sb_buffer,
                        args.tbd_options);

    if (parse_node_result != E_MACHO_FILE_PARSE_OK) {
        return parse_node_result;
    }

    return E_MACHO_FILE_PARSE_OK;
}
