/**
 * @file printer_lyb.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief LYB printer for libyang data structure
 *
 * Copyright (c) 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "common.h"
#include "printer.h"
#include "tree_schema.h"
#include "tree_data.h"
#include "resolve.h"
#include "tree_internal.h"

static int
lyb_hash_equal_cb(void *UNUSED(val1_p), void *UNUSED(val2_p), int UNUSED(mod), void *UNUSED(cb_data))
{
    /* for this purpose, if hash matches, the value does also, we do not want 2 values to have the same hash */
    return 1;
}

static int
lyb_ptr_equal_cb(void *val1_p, void *val2_p, int UNUSED(mod), void *UNUSED(cb_data))
{
    struct lys_node *val1 = *(struct lys_node **)val1_p;
    struct lys_node *val2 = *(struct lys_node **)val2_p;

    if (val1 == val2) {
        return 1;
    }
    return 0;
}

/* check that sibling collision hash i is safe to insert into ht
 * return: 0 - no whole hash sequence collision, 1 - whole hash sequence collision, -1 - fatal error
 */
static int
lyb_hash_sequence_check(struct hash_table *ht, struct lys_node *sibling, int ht_col_id, int compare_col_id)
{
    int j;
    struct lys_node **col_node;

    /* get the first node inserted with last hash col ID ht_col_id */
    if (lyht_find(ht, &sibling, lyb_hash(sibling, ht_col_id), (void **)&col_node)) {
        /* there is none. valid situation */
        return 0;
    }

    lyht_set_cb(ht, lyb_ptr_equal_cb);
    do {
        for (j = compare_col_id; j > -1; --j) {
            if (lyb_hash(sibling, j) != lyb_hash(*col_node, j)) {
                /* one non-colliding hash */
                break;
            }
        }
        if (j == -1) {
            /* all whole hash sequences of nodes inserted with last hash col ID compare_col_id collide */
            lyht_set_cb(ht, lyb_hash_equal_cb);
            return 1;
        }

        /* get next node inserted with last hash col ID ht_col_id */
    } while (!lyht_find_next(ht, col_node, lyb_hash(*col_node, ht_col_id), (void **)&col_node));

    lyht_set_cb(ht, lyb_hash_equal_cb);
    return 0;
}

static struct hash_table *
lyb_hash_siblings(struct lys_node *sibling, const struct lys_module **models, int mod_count, int options)
{
    struct hash_table *ht;
    struct lys_node *parent, *iter;
    const struct lys_module *mod;
    int i, j;

    ht = lyht_new(1, sizeof(struct lys_node *), lyb_hash_equal_cb, NULL, 1);
    LY_CHECK_ERR_RETURN(!ht, LOGMEM(sibling->module->ctx), NULL);

    for (parent = lys_parent(sibling);
         parent && (parent->nodetype & (LYS_USES | LYS_CHOICE | LYS_CASE | LYS_INPUT | LYS_OUTPUT));
         parent = lys_parent(parent));
    mod = lys_node_module(sibling);

    sibling = NULL;
    while ((sibling = (struct lys_node *)lys_getnext(sibling, parent, mod, 0))) {
        if (models && !lyb_has_schema_model(sibling, models, mod_count)) {
            /* ignore models not present during printing */
            continue;
        }

        if (options & (LYD_OPT_RPC | LYD_OPT_RPCREPLY)) {
            for (iter = lys_parent(sibling);
                 iter && (iter->nodetype & (LYS_USES | LYS_CASE | LYS_CHOICE));
                 iter = lys_parent(iter));

            if (((options & LYD_OPT_RPC) && (iter->nodetype == LYS_OUTPUT))
                    || ((options & LYD_OPT_RPCREPLY) && (iter->nodetype == LYS_INPUT))) {
                /* skip unused nodes */
                continue;
            }
        }

        /* find the first non-colliding hash (or specifically non-colliding hash sequence) */
        for (i = 0; i < LYB_HASH_BITS; ++i) {
            /* check that we are not colliding with nodes inserted with a lower collision ID than ours */
            for (j = i - 1; j > -1; --j) {
                if (lyb_hash_sequence_check(ht, sibling, j, i)) {
                    break;
                }
            }
            if (j > -1) {
                /* some check failed, we must use a higher collision ID */
                continue;
            }

            /* try to insert node with the current collision ID */
            if (!lyht_insert_with_resize_cb(ht, &sibling, lyb_hash(sibling, i), lyb_ptr_equal_cb)) {
                /* success, no collision */
                break;
            }

            /* make sure we really cannot insert it with this hash col ID (meaning the whole hash sequence is colliding) */
            if (i && !lyb_hash_sequence_check(ht, sibling, i, i)) {
                /* it can be inserted after all, even though there is already a node with the same last collision ID */
                lyht_set_cb(ht, lyb_ptr_equal_cb);
                if (lyht_insert(ht, &sibling, lyb_hash(sibling, i))) {
                    lyht_set_cb(ht, lyb_hash_equal_cb);
                    LOGINT(sibling->module->ctx);
                    lyht_free(ht);
                    return NULL;
                }
                lyht_set_cb(ht, lyb_hash_equal_cb);
                break;
            }
            /* there is still another colliding schema node with the same hash sequence, try higher collision ID */
        }

        if (i == LYB_HASH_BITS) {
            /* wow */
            LOGINT(sibling->module->ctx);
            lyht_free(ht);
            return NULL;
        }
    }

    /* change val equal callback so that the HT is usable for finding value hashes */
    lyht_set_cb(ht, lyb_ptr_equal_cb);

    return ht;
}

static LYB_HASH
lyb_hash_find(struct hash_table *ht, struct lys_node *node)
{
    LYB_HASH hash;
    uint32_t i;

    for (i = 0; i < LYB_HASH_BITS; ++i) {
        hash = lyb_hash(node, i);
        if (!hash) {
            LOGINT(node->module->ctx);
            return 0;
        }

        if (!lyht_find(ht, &node, hash, NULL)) {
            /* success, no collision */
            break;
        }
    }
    /* cannot happen, we already calculated the hash */
    if (i == LYB_HASH_BITS) {
        LOGINT(node->module->ctx);
        return 0;
    }

    return hash;
}

/* writing function handles writing size information */
static int
lyb_write(struct lyout *out, const uint8_t *buf, size_t count, struct lyb_state *lybs)
{
    int ret, i, full_chunk_i;
    size_t r, to_write;
    LYB_META meta;

    assert(out && lybs);

    ret = 0;
    while (count) {
        /* check for full data chunks */
        to_write = count;
        full_chunk_i = -1;
        for (i = 0; i < lybs->used; ++i) {
            /* we want the innermost chunks resolved first, so replace previous full chunks */
            if (lybs->written[i] + to_write >= LYB_SIZE_MAX) {
                /* full chunk, do not write more than allowed */
                to_write = LYB_SIZE_MAX - lybs->written[i];
                full_chunk_i = i;
            }
        }

        r = ly_write(out, (char *)buf, to_write);
        if (r < to_write) {
            return -1;
        }

        for (i = 0; i < lybs->used; ++i) {
            /* increase all written counters */
            lybs->written[i] += r;
            assert(lybs->written[i] <= LYB_SIZE_MAX);
        }
        /* decrease count/buf */
        count -= r;
        buf += r;

        ret += r;

        if (full_chunk_i > -1) {
            /* write the meta information (inner chunk count and chunk size) */
            memcpy(&meta, &lybs->written[full_chunk_i], LYB_SIZE_BYTES);
            memcpy(((uint8_t *)&meta) + LYB_SIZE_BYTES, &lybs->inner_chunks[full_chunk_i], LYB_INCHUNK_BYTES);

            r = ly_write_skipped(out, lybs->position[full_chunk_i], (char *)&meta, LYB_META_BYTES);
            if (r < LYB_META_BYTES) {
                return -1;
            }

            /* zero written and inner chunks */
            lybs->written[full_chunk_i] = 0;
            lybs->inner_chunks[full_chunk_i] = 0;

            /* skip space for another chunk size */
            r = ly_write_skip(out, LYB_META_BYTES, &lybs->position[full_chunk_i]);
            if (r < LYB_META_BYTES) {
                return -1;
            }

            ret += r;

            /* increase inner chunk count */
            for (i = 0; i < full_chunk_i; ++i) {
                if (lybs->inner_chunks[i] == LYB_INCHUNK_MAX) {
                    LOGINT(NULL);
                    return -1;
                }
                ++lybs->inner_chunks[i];
            }
        }
    }

    return ret;
}

static int
lyb_write_stop_subtree(struct lyout *out, struct lyb_state *lybs)
{
    int r;
    LYB_META meta;

    /* write the meta chunk information */
    memcpy(&meta, &lybs->written[lybs->used - 1], LYB_SIZE_BYTES);
    memcpy(((uint8_t *)&meta) + LYB_SIZE_BYTES, &lybs->inner_chunks[lybs->used - 1], LYB_INCHUNK_BYTES);

    r = ly_write_skipped(out, lybs->position[lybs->used - 1], (char *)&meta, LYB_META_BYTES);
    if (r < LYB_META_BYTES) {
        return -1;
    }

    --lybs->used;
    return 0;
}

static int
lyb_write_start_subtree(struct lyout *out, struct lyb_state *lybs)
{
    int i;

    if (lybs->used == lybs->size) {
        lybs->size += LYB_STATE_STEP;
        lybs->written = ly_realloc(lybs->written, lybs->size * sizeof *lybs->written);
        lybs->position = ly_realloc(lybs->position, lybs->size * sizeof *lybs->position);
        lybs->inner_chunks = ly_realloc(lybs->inner_chunks, lybs->size * sizeof *lybs->inner_chunks);
        LY_CHECK_ERR_RETURN(!lybs->written || !lybs->position || !lybs->inner_chunks, LOGMEM(NULL), -1);
    }

    ++lybs->used;
    lybs->written[lybs->used - 1] = 0;
    lybs->inner_chunks[lybs->used - 1] = 0;

    /* another inner chunk */
    for (i = 0; i < lybs->used - 1; ++i) {
        if (lybs->inner_chunks[i] == LYB_INCHUNK_MAX) {
            LOGINT(NULL);
            return -1;
        }
        ++lybs->inner_chunks[i];
    }

    return ly_write_skip(out, LYB_META_BYTES, &lybs->position[lybs->used - 1]);
}

static int
lyb_write_number(uint64_t num, uint64_t max_num, struct lyout *out, struct lyb_state *lybs)
{
    int max_bits, max_bytes, i, ret = 0;
    uint8_t byte;

    for (max_bits = 0; max_num; max_num >>= 1, ++max_bits);
    max_bytes = max_bits / 8 + (max_bits % 8 ? 1 : 0);

    for (i = 0; i < max_bytes; ++i) {
        byte = *(((uint8_t *)&num) + i);
        ret += lyb_write(out, &byte, 1, lybs);
    }

    return ret;
}

static int
lyb_write_string(const char *str, size_t str_len, int with_length, struct lyout *out, struct lyb_state *lybs)
{
    int r, ret = 0;

    if (!str_len) {
        str_len = strlen(str);
    }
    if (str_len > UINT16_MAX) {
        LOGINT(NULL);
        return -1;
    }

    if (with_length) {
        /* print length on 2 bytes */
        ret += (r = lyb_write(out, (uint8_t *)&str_len, 2, lybs));
        if (r < 0) {
            return -1;
        }
    }

    ret += (r = lyb_write(out, (const uint8_t *)str, str_len, lybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

static int
lyb_print_model(struct lyout *out, const struct lys_module *mod, struct lyb_state *lybs)
{
    int r, ret = 0;
    uint16_t revision;

    /* model name length and model name */
    ret += (r = lyb_write_string(mod->name, 0, 1, out, lybs));
    if (r < 0) {
        return -1;
    }

    /* model revision as XXXX XXXX XXXX XXXX (2B) (year is offset from 2000)
     *                   YYYY YYYM MMMD DDDD */
    revision = 0;
    if (mod->rev_size) {
        r = atoi(mod->rev[0].date);
        r -= 2000;
        r <<= 9;

        revision |= r;

        r = atoi(mod->rev[0].date + 5);
        r <<= 5;

        revision |= r;

        r = atoi(mod->rev[0].date + 8);

        revision |= r;
    }

    ret += (r = lyb_write(out, (uint8_t *)&revision, sizeof revision, lybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

static int
is_added_model(const struct lys_module **models, size_t mod_count, const struct lys_module *mod)
{
    size_t i;

    for (i = 0; i < mod_count; ++i) {
        if (models[i] == mod) {
            return 1;
        }
    }

    return 0;
}

static void
add_model(const struct lys_module ***models, size_t *mod_count, const struct lys_module *mod)
{
    if (is_added_model(*models, *mod_count, mod)) {
        return;
    }

    *models = ly_realloc(*models, ++(*mod_count) * sizeof **models);
    (*models)[*mod_count - 1] = mod;
}

static int
lyb_print_data_models(struct lyout *out, const struct lyd_node *root, struct lyb_state *lybs)
{
    int ret = 0;
    const struct lys_module **models = NULL, *mod;
    const struct lys_submodule *submod;
    const struct lyd_node *node;
    size_t mod_count = 0;
    uint32_t idx = 0, i, j;

    /* first, collect all data node modules */
    LY_TREE_FOR(root, node) {
        mod = lyd_node_module(node);
        add_model(&models, &mod_count, mod);
    }

    /* then add all models augmenting or deviating the used models */
    idx = ly_ctx_internal_modules_count(root->schema->module->ctx);
    while ((mod = ly_ctx_get_module_iter(root->schema->module->ctx, &idx))) {
        if (!mod->implemented) {
next_mod:
            continue;
        }

        for (i = 0; i < mod->deviation_size; ++i) {
            if (mod->deviation[i].orig_node && is_added_model(models, mod_count, lys_node_module(mod->deviation[i].orig_node))) {
                add_model(&models, &mod_count, mod);
                goto next_mod;
            }
        }
        for (i = 0; i < mod->augment_size; ++i) {
            if (is_added_model(models, mod_count, lys_node_module(mod->augment[i].target))) {
                add_model(&models, &mod_count, mod);
                goto next_mod;
            }
        }

        /* submodules */
        for (j = 0; j < mod->inc_size; ++j) {
            submod = mod->inc[j].submodule;

            for (i = 0; i < submod->deviation_size; ++i) {
                if (submod->deviation[i].orig_node && is_added_model(models, mod_count, lys_node_module(submod->deviation[i].orig_node))) {
                    add_model(&models, &mod_count, mod);
                    goto next_mod;
                }
            }
            for (i = 0; i < submod->augment_size; ++i) {
                if (is_added_model(models, mod_count, lys_node_module(submod->augment[i].target))) {
                    add_model(&models, &mod_count, mod);
                    goto next_mod;
                }
            }
        }
    }

    /* now write module count on 2 bytes */
    ret += lyb_write(out, (uint8_t *)&mod_count, 2, lybs);

    /* and all the used models */
    for (i = 0; i < mod_count; ++i) {
        ret += lyb_print_model(out, models[i], lybs);
    }

    free(models);
    return ret;
}

static int
lyb_print_header(struct lyout *out)
{
    int ret = 0;
    uint8_t byte = 0;

    /* TODO version, some other flags? */
    ret += ly_write(out, (char *)&byte, sizeof byte);

    return ret;
}

static int
lyb_print_anydata(struct lyd_node_anydata *anydata, struct lyout *out, struct lyb_state *lybs)
{
    int ret = 0, len;
    char *buf;
    LYD_ANYDATA_VALUETYPE type;

    switch (anydata->value_type) {
    case LYD_ANYDATA_XML:
        /* transform XML into CONSTSTRING */
        lyxml_print_mem(&buf, anydata->value.xml, LYXML_PRINT_SIBLINGS);
        lyxml_free(anydata->schema->module->ctx, anydata->value.xml);

        anydata->value_type = LYD_ANYDATA_CONSTSTRING;
        anydata->value.str = lydict_insert_zc(anydata->schema->module->ctx, buf);
        /* fallthrough */
    case LYD_ANYDATA_DATATREE:
    case LYD_ANYDATA_JSON:
    case LYD_ANYDATA_SXML:
    case LYD_ANYDATA_CONSTSTRING:
    case LYD_ANYDATA_LYB:
        type = anydata->value_type;
        break;
    default:
        LOGERR(anydata->schema->module->ctx, LY_EINT, "Unsupported anydata value type to print.");
        return -1;
    }

    /* first byte is type */
    ret += lyb_write(out, (uint8_t *)&type, sizeof type, lybs);

    /* followed by the content */
    if (type == LYD_ANYDATA_DATATREE) {
        ret += lyb_print_data(out, anydata->value.tree, 0);
    } else if (type == LYD_ANYDATA_LYB) {
        len = lyd_lyb_data_length(anydata->value.mem);
        if (len > -1) {
            ret += lyb_write_string(anydata->value.str, (size_t)len, 0, out, lybs);
        }
    } else {
        ret += lyb_write_string(anydata->value.str, 0, 0, out, lybs);
    }

    return ret;
}

static int
lyb_print_value(const struct lys_type *type, const char *value_str, lyd_val value, LY_DATA_TYPE value_type,
                uint8_t value_flags, uint8_t dflt, struct lyout *out, struct lyb_state *lybs)
{
    int ret = 0;
    uint8_t byte = 0;
    size_t count, i, bits_i;
    LY_DATA_TYPE dtype;

    /* value type byte - ABCD DDDD
     *
     * A - dflt flag
     * B - user type flag
     * C - unres flag
     * D (5b) - data type value
     */
    if (dflt) {
        byte |= 0x80;
    }
    if (value_flags & LY_VALUE_USER) {
        byte |= 0x40;
    }
    if (value_flags & LY_VALUE_UNRES) {
        byte |= 0x20;
    }

    /* we have only 5b available, must be enough */
    assert((value_type & 0x1f) == value_type);

    if ((value_flags & LY_VALUE_USER) || (type->base == LY_TYPE_UNION)) {
        value_type = LY_TYPE_STRING;
    } else if (value_type == LY_TYPE_LEAFREF) {
        assert(!(value_flags & LY_VALUE_UNRES));
        /* find the leafref target type */
        while (type->base == LY_TYPE_LEAFREF) {
            type = &type->info.lref.target->type;
        }
        value_type = type->base;

        /* and also use its value */
        value = ((struct lyd_node_leaf_list *)value.leafref)->value;
    }

    /* store the value type */
    byte |= value_type & 0x1f;

    /* write value type byte */
    ret += lyb_write(out, &byte, sizeof byte, lybs);

    /* print value itself */
    if (value_flags & LY_VALUE_USER) {
        dtype = LY_TYPE_STRING;
    } else {
        dtype = value_type;
    }
    switch (dtype) {
    case LY_TYPE_BINARY:
    case LY_TYPE_INST:
    case LY_TYPE_STRING:
    case LY_TYPE_UNION:
    case LY_TYPE_IDENT:
    case LY_TYPE_UNKNOWN:
        /* store string */
        ret += lyb_write_string(value_str, 0, 0, out, lybs);
        break;
    case LY_TYPE_BITS:
        /* find the correct structure */
        for (; !type->info.bits.count; type = &type->der->type);

        /* store a bitfield */
        bits_i = 0;

        for (count = type->info.bits.count / 8; count; --count) {
            /* will be a full byte */
            for (byte = 0, i = 0; i < 8; ++i) {
                if (value.bit[bits_i + i]) {
                    byte |= 0x80;
                }
                byte >>= 1;
            }
            ret += lyb_write(out, &byte, sizeof byte, lybs);
            bits_i += 8;
        }

        /* store the remainder */
        if (type->info.bits.count % 8) {
            for (byte = 0, i = 0; i < type->info.bits.count % 8; ++i) {
                if (value.bit[bits_i + i]) {
                    byte |= 0x80;
                }
                byte >>= 1;
            }
            byte >>= 8 - (i + 1);
            ret += lyb_write(out, &byte, sizeof byte, lybs);
        }
        break;
    case LY_TYPE_BOOL:
        /* store the whole byte */
        byte = 0;
        if (value.bln) {
            byte = 1;
        }
        ret += lyb_write(out, &byte, sizeof byte, lybs);
        break;
    case LY_TYPE_EMPTY:
        /* nothing to store */
        break;
    case LY_TYPE_ENUM:
        /* find the correct structure */
        for (; !type->info.enums.count; type = &type->der->type);

        /* store the enum index (save bytes if possible) */
        i = value.enm - type->info.enums.enm;
        ret += lyb_write_number(i, type->info.enums.count, out, lybs);
        break;
    case LY_TYPE_INT8:
    case LY_TYPE_UINT8:
        ret += lyb_write_number(value.uint8, UINT8_MAX, out, lybs);
        break;
    case LY_TYPE_INT16:
    case LY_TYPE_UINT16:
        ret += lyb_write_number(value.uint16, UINT16_MAX, out, lybs);
        break;
    case LY_TYPE_INT32:
    case LY_TYPE_UINT32:
        ret += lyb_write_number(value.uint32, UINT32_MAX, out, lybs);
        break;
    case LY_TYPE_DEC64:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT64:
        ret += lyb_write_number(value.uint64, UINT64_MAX, out, lybs);
        break;
    default:
        return 0;
    }

    return ret;
}

static int
lyb_print_attributes(struct lyout *out, struct lyd_attr *attr, struct lyb_state *lybs)
{
    int r, ret = 0;
    uint8_t count;
    struct lyd_attr *iter;
    struct lys_type *type;

    /* count attributes */
    for (count = 0, iter = attr; iter; ++count, iter = iter->next) {
        if (count == UINT8_MAX) {
            LOGERR(NULL, LY_EINT, "Maximum supported number of data node attributes is %u.", UINT8_MAX);
            return -1;
        }
    }

    /* write number of attributes on 1 byte */
    ret += (r = lyb_write(out, &count, 1, lybs));
    if (r < 0) {
        return -1;
    }

    /* write all the attributes */
    LY_TREE_FOR(attr, iter) {
        /* each attribute is a subtree */
        ret += (r = lyb_write_start_subtree(out, lybs));
        if (r < 0) {
            return -1;
        }

        /* model */
        ret += (r = lyb_print_model(out, attr->annotation->module, lybs));
        if (r < 0) {
            return -1;
        }

        /* annotation name with length */
        ret += (r = lyb_write_string(attr->annotation->arg_value, 0, 1, out, lybs));
        if (r < 0) {
            return -1;
        }

        /* get the type */
        type = *(struct lys_type **)lys_ext_complex_get_substmt(LY_STMT_TYPE, attr->annotation, NULL);

        /* attribute value */
        ret += (r = lyb_print_value(type, attr->value_str, attr->value, attr->value_type, attr->value_flags, 0, out, lybs));
        if (r < 0) {
            return -1;
        }

        /* finish attribute subtree */
        ret += (r = lyb_write_stop_subtree(out, lybs));
        if (r < 0) {
            return -1;
        }
    }

    return ret;
}

static int
lyb_print_schema_hash(struct lyout *out, struct lys_node *schema, struct hash_table **sibling_ht, struct lyb_state *lybs,
                      int options)
{
    int r, ret = 0;
    void *mem;
    uint32_t i;
    LYB_HASH hash;
    struct lys_node *first_sibling, *parent, *iter;

    /* create whole sibling HT if not already created and saved */
    if (!*sibling_ht) {
        /* get first schema data sibling */
        for (parent = lys_parent(schema);
             parent && (parent->nodetype & (LYS_USES | LYS_CASE | LYS_CHOICE | LYS_INPUT | LYS_OUTPUT));
             parent = lys_parent(parent)) {

            /* we have checked this before */
            assert(!(options & LYD_OPT_RPC) || (parent->nodetype != LYS_OUTPUT));
            assert(!(options & LYD_OPT_RPCREPLY) || (parent->nodetype != LYS_INPUT));
        }

        first_sibling = (struct lys_node *)lys_getnext(NULL, parent, lys_node_module(schema), 0);
        if (options & (LYD_OPT_RPC | LYD_OPT_RPCREPLY)) {
check_inout:
            for (iter = lys_parent(first_sibling);
                 iter && (iter->nodetype & (LYS_USES | LYS_CASE | LYS_CHOICE));
                 iter = lys_parent(iter));

            if (((options & LYD_OPT_RPC) && (iter->nodetype == LYS_OUTPUT))
                    || ((options & LYD_OPT_RPCREPLY) && (iter->nodetype == LYS_INPUT))) {
                first_sibling = (struct lys_node *)lys_getnext(first_sibling, NULL, NULL, 0);
                goto check_inout;
            }
        }

        for (r = 0; r < lybs->sib_ht_count; ++r) {
            if (lybs->sib_ht[r].first_sibling == first_sibling) {
                /* we have already created a hash table for these siblings */
                *sibling_ht = lybs->sib_ht[r].ht;
                break;
            }
        }

        if (!*sibling_ht) {
            /* we must create sibling hash table */
            *sibling_ht = lyb_hash_siblings(first_sibling, NULL, 0, options);
            if (!*sibling_ht) {
                return -1;
            }

            /* and save it */
            ++lybs->sib_ht_count;
            mem = realloc(lybs->sib_ht, lybs->sib_ht_count * sizeof *lybs->sib_ht);
            LY_CHECK_ERR_RETURN(!mem, LOGMEM(schema->module->ctx), -1);
            lybs->sib_ht = mem;

            lybs->sib_ht[lybs->sib_ht_count - 1].first_sibling = first_sibling;
            lybs->sib_ht[lybs->sib_ht_count - 1].ht = *sibling_ht;
        }
    }

    /* get our hash */
    hash = lyb_hash_find(*sibling_ht, schema);
    if (!hash) {
        return -1;
    }

    /* write the hash */
    ret += (r = lyb_write(out, &hash, sizeof hash, lybs));
    if (r < 0) {
        return -1;
    }

    if (hash & LYB_HASH_COLLISION_ID) {
        /* no collision for this hash, we are done */
        return ret;
    }

    /* written hash was a collision, write also all the preceding hashes */
    for (i = 0; !(hash & (LYB_HASH_COLLISION_ID >> i)); ++i);

    for (; i; --i) {
        hash = lyb_hash(schema, i - 1);
        if (!hash) {
            return -1;
        }
        assert(hash & (LYB_HASH_COLLISION_ID >> (i - 1)));

        ret += (r = lyb_write(out, &hash, sizeof hash, lybs));
        if (r < 0) {
            return -1;
        }
    }

    return ret;
}

static int
lyb_print_subtree(struct lyout *out, const struct lyd_node *node, struct hash_table **sibling_ht, struct lyb_state *lybs,
                  int options, int top_level)
{
    int r, ret = 0;
    struct lyd_node_leaf_list *leaf;
    struct lys_node *sparent;
    struct hash_table *child_ht = NULL;

    /* skip nodes that should not be printed */
    if (options & (LYD_OPT_RPC | LYD_OPT_RPCREPLY)) {
        for (sparent = lys_parent(node->schema);
            sparent && (sparent->nodetype & (LYS_USES | LYS_CASE | LYS_CHOICE));
            sparent = lys_parent(sparent));

        if ((options & LYD_OPT_RPC) && (sparent->nodetype == LYS_OUTPUT)) {
            return 0;
        }
        if ((options & LYD_OPT_RPCREPLY) && (sparent->nodetype == LYS_INPUT)) {
            return 0;
        }
    }

    /* register a new subtree */
    ret += (r = lyb_write_start_subtree(out, lybs));
    if (r < 0) {
        return -1;
    }

    /*
     * write the node information
     */
    if (top_level) {
        /* write model info first */
        ret += (r = lyb_print_model(out, lyd_node_module(node), lybs));
        if (r < 0) {
            return -1;
        }
    }

    ret += (r = lyb_print_schema_hash(out, node->schema, sibling_ht, lybs, options));
    if (r < 0) {
        return -1;
    }

    ret += (r = lyb_print_attributes(out, node->attr, lybs));
    if (r < 0) {
        return -1;
    }

    /* write node content */
    switch (node->schema->nodetype) {
    case LYS_CONTAINER:
    case LYS_LIST:
    case LYS_NOTIF:
    case LYS_RPC:
    case LYS_ACTION:
        /* nothing to write */
        break;
    case LYS_LEAF:
    case LYS_LEAFLIST:
        leaf = (struct lyd_node_leaf_list *)node;
        ret += (r = lyb_print_value(&((struct lys_node_leaf *)leaf->schema)->type, leaf->value_str, leaf->value,
                                    leaf->value_type, leaf->value_flags, leaf->dflt, out, lybs));
        if (r < 0) {
            return -1;
        }
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        ret += (r = lyb_print_anydata((struct lyd_node_anydata *)node, out, lybs));
        if (r < 0) {
            return -1;
        }
        break;
    default:
        return -1;
    }

    /* recursively write all the descendants */
    r = 0;
    if (node->schema->nodetype & (LYS_CONTAINER | LYS_LIST | LYS_NOTIF | LYS_RPC | LYS_ACTION)) {
        LY_TREE_FOR(node->child, node) {
            ret += (r = lyb_print_subtree(out, node, &child_ht, lybs, options, 0));
            if (r < 0) {
                break;
            }
        }
    }
    if (r < 0) {
        return -1;
    }

    /* finish this subtree */
    ret += (r = lyb_write_stop_subtree(out, lybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

int
lyb_print_data(struct lyout *out, const struct lyd_node *root, int options)
{
    int r, ret = 0, rc = EXIT_SUCCESS;
    uint8_t zero = 0;
    struct hash_table *top_sibling_ht = NULL;
    const struct lys_module *prev_mod = NULL;
    struct lyb_state lybs;

    memset(&lybs, 0, sizeof lybs);

    /* LYB header */
    ret += (r = lyb_print_header(out));
    if (r < 0) {
        rc = EXIT_FAILURE;
        goto finish;
    }

    /* all used models */
    ret += (r = lyb_print_data_models(out, root, &lybs));
    if (r < 0) {
        rc = EXIT_FAILURE;
        goto finish;
    }

    LY_TREE_FOR(root, root) {
        /* do not reuse sibling hash tables from different modules */
        if (lyd_node_module(root) != prev_mod) {
            top_sibling_ht = NULL;
            prev_mod = lyd_node_module(root);
        }

        ret += (r = lyb_print_subtree(out, root, &top_sibling_ht, &lybs, options, 1));
        if (r < 0) {
            rc = EXIT_FAILURE;
            goto finish;
        }

        if (!(options & LYP_WITHSIBLINGS)) {
            break;
        }
    }

    /* ending zero byte */
    ret += (r = lyb_write(out, &zero, sizeof zero, &lybs));
    if (r < 0) {
        rc = EXIT_FAILURE;
    }

finish:
    free(lybs.written);
    free(lybs.position);
    free(lybs.inner_chunks);
    for (r = 0; r < lybs.sib_ht_count; ++r) {
        lyht_free(lybs.sib_ht[r].ht);
    }
    free(lybs.sib_ht);

    return rc;
}
