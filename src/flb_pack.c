/*-*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019      The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdlib.h>
#include <string.h>

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_unescape.h>

#include <msgpack.h>
#include <jsmn/jsmn.h>

#define try_to_write_str  flb_utils_write_str

int flb_json_tokenise(const char *js, size_t len,
                      struct flb_pack_state *state)
{
    int ret;
    int new_tokens = 256;
    size_t old_size;
    size_t new_size;
    void *tmp;

    ret = jsmn_parse(&state->parser, js, len,
                     state->tokens, state->tokens_size);
    while (ret == JSMN_ERROR_NOMEM) {
        /* Get current size of the array in bytes */
        old_size = state->tokens_size * sizeof(jsmntok_t);

        /* New size: add capacity for new 256 entries */
        new_size = old_size + (sizeof(jsmntok_t) * new_tokens);

        tmp = flb_realloc_z(state->tokens, old_size, new_size);
        if (!tmp) {
            flb_errno();
            return -1;
        }
        state->tokens = tmp;
        state->tokens_size += new_tokens;

        ret = jsmn_parse(&state->parser, js, len,
                         state->tokens, state->tokens_size);
    }

    if (ret == JSMN_ERROR_INVAL) {
        return FLB_ERR_JSON_INVAL;
    }

    if (ret == JSMN_ERROR_PART) {
        /* This is a partial JSON message, just stop */
        flb_trace("[json tokenise] incomplete");
        return FLB_ERR_JSON_PART;
    }

    state->tokens_count += ret;
    return 0;
}

static inline int is_float(const char *buf, int len)
{
    const char *end = buf + len;
    const char *p = buf;

    while (p <= end) {
        if (*p == '.') {
            return 1;
        }
        p++;
    }
    return 0;
}

/* Sanitize incoming JSON string */
static inline int pack_string_token(struct flb_pack_state *state,
                                    const char *str, int len,
                                    msgpack_packer *pck)
{
    int s;
    int out_len;
    char *tmp;
    char *out_buf;

    if (state->buf_size < len + 1) {
        s = len + 1;
        tmp = flb_realloc(state->buf_data, s);
        if (!tmp) {
            flb_errno();
            return -1;
        }
        else {
            state->buf_data = tmp;
            state->buf_size = s;
        }
    }
    out_buf = state->buf_data;

    /* Always decode any UTF-8 or special characters */
    out_len = flb_unescape_string_utf8(str, len, out_buf);

    /* Pack decoded text */
    msgpack_pack_str(pck, out_len);
    msgpack_pack_str_body(pck, out_buf, out_len);

    return out_len;
}

/* Receive a tokenized JSON message and convert it to MsgPack */
static char *tokens_to_msgpack(struct flb_pack_state *state,
                               const char *js,
                               int *out_size, int *last_byte)
{
    int i;
    int flen;
    int arr_size;
    const char *p;
    char *buf;
    const jsmntok_t *t;
    msgpack_packer pck;
    msgpack_sbuffer sbuf;
    jsmntok_t *tokens;

    tokens = state->tokens;
    arr_size = state->tokens_count;

    if (arr_size == 0) {
        return NULL;
    }

    /* initialize buffers */
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pck, &sbuf, msgpack_sbuffer_write);

    for (i = 0; i < arr_size ; i++) {
        t = &tokens[i];

        if (t->start == -1 || t->end == -1 || (t->start == 0 && t->end == 0)) {
            break;
        }

        if (t->parent == -1) {
            *last_byte = t->end;
        }

        flen = (t->end - t->start);
        switch (t->type) {
        case JSMN_OBJECT:
            msgpack_pack_map(&pck, t->size);
            break;
        case JSMN_ARRAY:
            msgpack_pack_array(&pck, t->size);
            break;
        case JSMN_STRING:
            pack_string_token(state, js + t->start, flen, &pck);
            break;
        case JSMN_PRIMITIVE:
            p = js + t->start;
            if (*p == 'f') {
                msgpack_pack_false(&pck);
            }
            else if (*p == 't') {
                msgpack_pack_true(&pck);
            }
            else if (*p == 'n') {
                msgpack_pack_nil(&pck);
            }
            else {
                if (is_float(p, flen)) {
                    msgpack_pack_double(&pck, atof(p));
                }
                else {
                    msgpack_pack_int64(&pck, atol(p));
                }
            }
            break;
        case JSMN_UNDEFINED:
            msgpack_sbuffer_destroy(&sbuf);
            return NULL;
        }
    }

    *out_size = sbuf.size;
    buf = sbuf.data;

    return buf;
}

static char *str_copy_replace(const char *src, int len, char search, char replace) {
    char *dst = NULL;
    int i;

    dst = flb_strndup(src, len);

    if (!dst) {
        flb_errno();
        return NULL;
    }

    for(i = 0; i < len; i++) {
        if (dst[i] == search) {
            dst[i] = replace;
        }
    }

    return dst;
}

/*
 * It parse a JSON string and convert it to MessagePack format, this packer is
 * useful when a complete JSON message exists, otherwise it will fail until
 * the message is complete.
 *
 * This routine do not keep a state in the parser, do not use it for big
 * JSON messages.
 */
int flb_pack_json(const char *js, size_t len, char **buffer, size_t *size,
                  int *root_type)

{
    int ret = -1;
    int out;
    int last;
    char *buf = NULL;
    struct flb_pack_state state;

    ret = flb_pack_state_init(&state);
    if (ret != 0) {
        return -1;
    }
    ret = flb_json_tokenise(js, len, &state);
    if (ret != 0) {
        ret = -1;
        goto flb_pack_json_end;
    }

    if (state.tokens_count == 0) {
        ret = -1;
        goto flb_pack_json_end;
    }

    buf = tokens_to_msgpack(&state, js, &out, &last);
    if (!buf) {
        ret = -1;
        goto flb_pack_json_end;
    }

    *root_type = state.tokens[0].type;
    *size = out;
    *buffer = buf;

    ret = 0;

 flb_pack_json_end:
    flb_pack_state_reset(&state);
    return ret;
}

/* Initialize a JSON packer state */
int flb_pack_state_init(struct flb_pack_state *s)
{
    int tokens = 256;
    size_t size = 256;

    jsmn_init(&s->parser);

    size = sizeof(jsmntok_t) * tokens;
    s->tokens = flb_calloc(1, size);
    if (!s->tokens) {
        flb_errno();
        return -1;
    }
    s->tokens_size   = tokens;
    s->tokens_count  = 0;
    s->last_byte     = 0;
    s->multiple      = FLB_FALSE;

    s->buf_data = flb_malloc(size);
    if (!s->buf_data) {
        flb_errno();
        flb_free(s->tokens);
        return -1;
    }
    s->buf_size = size;
    s->buf_len = 0;

    return 0;
}

void flb_pack_state_reset(struct flb_pack_state *s)
{
    flb_free(s->tokens);
    s->tokens_size  = 0;
    s->tokens_count = 0;
    s->last_byte    = 0;
    s->buf_size     = 0;
    flb_free(s->buf_data);
}


/*
 * It parse a JSON string and convert it to MessagePack format. The main
 * difference of this function and the previous flb_pack_json() is that it
 * keeps a parser and tokens state, allowing to process big messages and
 * resume the parsing process instead of start from zero.
 */
int flb_pack_json_state(const char *js, size_t len,
                        char **buffer, int *size,
                        struct flb_pack_state *state)
{
    int ret;
    int out;
    int delim = 0;
    int last =  0;
    char *buf;
    jsmntok_t *t;

    ret = flb_json_tokenise(js, len, state);
    state->multiple = FLB_TRUE;
    if (ret == FLB_ERR_JSON_PART && state->multiple == FLB_TRUE) {
        /*
         * If the caller enabled 'multiple' flag, it means that the incoming
         * JSON message may have multiple messages concatenated and likely
         * the last one is only incomplete.
         *
         * The following routine aims to determinate how many JSON messages
         * are OK in the array of tokens, if any, process them and adjust
         * the JSMN context/buffers.
         */
        int i;
        int found = 0;

        for (i = 1; i < state->tokens_size; i++) {
            t = &state->tokens[i];

            if (t->start < (state->tokens[i - 1]).start) {
                break;
            }

            if (t->parent == -1 && (t->end != 0)) {
                found++;
                delim = i;
            }

        }

        if (found > 0) {
            state->tokens_count += delim;
        }
        else {
            return ret;
        }
    }
    else if (ret != 0) {
        return ret;
    }

    if (state->tokens_count == 0) {
        state->last_byte = last;
        return FLB_ERR_JSON_INVAL;
    }

    buf = tokens_to_msgpack(state, js, &out, &last);
    if (!buf) {
        return -1;
    }

    *size = out;
    *buffer = buf;
    state->last_byte = last;

    return 0;
}

static int pack_print_fluent_record(size_t cnt, msgpack_unpacked result)
{
    double unix_time;
    msgpack_object o;
    msgpack_object *obj;
    msgpack_object root;
    struct flb_time tms;

    root = result.data;
    if (root.type != MSGPACK_OBJECT_ARRAY) {
        return -1;
    }

    /* decode expected timestamp only (integer, float or ext) */
    o = root.via.array.ptr[0];
    if (o.type != MSGPACK_OBJECT_POSITIVE_INTEGER &&
        o.type != MSGPACK_OBJECT_FLOAT &&
        o.type != MSGPACK_OBJECT_EXT) {
        return -1;
    }

    /* This is a Fluent Bit record, just do the proper unpacking/printing */
    flb_time_pop_from_msgpack(&tms, &result, &obj);

    unix_time = flb_time_to_double(&tms);
    fprintf(stdout, "[%zd] [%f, ", cnt, unix_time);
    msgpack_object_print(stdout, *obj);
    fprintf(stdout, "]\n");

    return 0;
}

void flb_pack_print(const char *data, size_t bytes)
{
    int ret;
    msgpack_unpacked result;
    size_t off = 0, cnt = 0;

    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        /* Check if we are processing an internal Fluent Bit record */
        ret = pack_print_fluent_record(cnt, result);
        if (ret == 0) {
            continue;
        }

        printf("[%zd] ", cnt++);
        msgpack_object_print(stdout, result.data);
        printf("\n");
    }
    msgpack_unpacked_destroy(&result);
}


static inline int try_to_write(char *buf, int *off, size_t left,
                               const char *str, size_t str_len)
{
    if (str_len <= 0){
        str_len = strlen(str);
    }
    if (left <= *off+str_len) {
        return FLB_FALSE;
    }
    memcpy(buf+*off, str, str_len);
    *off += str_len;
    return FLB_TRUE;
}

static int msgpack2json(char *buf, int *off, size_t left,
                        const msgpack_object *o)
{
    int ret = FLB_FALSE;
    int i;
    int loop;

    switch(o->type) {
    case MSGPACK_OBJECT_NIL:
        ret = try_to_write(buf, off, left, "null", 4);
        break;

    case MSGPACK_OBJECT_BOOLEAN:
        ret = try_to_write(buf, off, left,
                           (o->via.boolean ? "true":"false"),0);

        break;

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        {
            char temp[32] = {0};
            i = snprintf(temp, sizeof(temp)-1, "%lu", (unsigned long)o->via.u64);
            ret = try_to_write(buf, off, left, temp, i);
        }
        break;

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        {
            char temp[32] = {0};
            i = snprintf(temp, sizeof(temp)-1, "%ld", (signed long)o->via.i64);
            ret = try_to_write(buf, off, left, temp, i);
        }
        break;
    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
        {
            char temp[32] = {0};
            i = snprintf(temp, sizeof(temp)-1, "%f", o->via.f64);
            ret = try_to_write(buf, off, left, temp, i);
        }
        break;

    case MSGPACK_OBJECT_STR:
        if (try_to_write(buf, off, left, "\"", 1) &&
            (o->via.str.size > 0 ?
             try_to_write_str(buf, off, left, o->via.str.ptr, o->via.str.size)
             : 1/* nothing to do */) &&
            try_to_write(buf, off, left, "\"", 1)) {
            ret = FLB_TRUE;
        }
        break;

    case MSGPACK_OBJECT_BIN:
        if (try_to_write(buf, off, left, "\"", 1) &&
            (o->via.bin.size > 0 ?
             try_to_write_str(buf, off, left, o->via.bin.ptr, o->via.bin.size)
              : 1 /* nothing to do */) &&
            try_to_write(buf, off, left, "\"", 1)) {
            ret = FLB_TRUE;
        }
        break;

    case MSGPACK_OBJECT_EXT:
        if (!try_to_write(buf, off, left, "\"", 1)) {
            goto msg2json_end;
        }
        /* ext body. fortmat is similar to printf(1) */
        {
            char temp[32] = {0};
            int  len;
            loop = o->via.ext.size;
            for(i=0; i<loop; i++) {
                len = snprintf(temp, sizeof(temp)-1, "\\x%02x", (char)o->via.ext.ptr[i]);
                if (!try_to_write(buf, off, left, temp, len)) {
                    goto msg2json_end;
                }
            }
        }
        if (!try_to_write(buf, off, left, "\"", 1)) {
            goto msg2json_end;
        }
        ret = FLB_TRUE;
        break;

    case MSGPACK_OBJECT_ARRAY:
        loop = o->via.array.size;

        if (!try_to_write(buf, off, left, "[", 1)) {
            goto msg2json_end;
        }
        if (loop != 0) {
            msgpack_object* p = o->via.array.ptr;
            if (!msgpack2json(buf, off, left, p)) {
                goto msg2json_end;
            }
            for (i=1; i<loop; i++) {
                if (!try_to_write(buf, off, left, ",", 1) ||
                    !msgpack2json(buf, off, left, p+i)) {
                    goto msg2json_end;
                }
            }
        }

        ret = try_to_write(buf, off, left, "]", 1);
        break;

    case MSGPACK_OBJECT_MAP:
        loop = o->via.map.size;
        if (!try_to_write(buf, off, left, "{", 1)) {
            goto msg2json_end;
        }
        if (loop != 0) {
            msgpack_object_kv *p = o->via.map.ptr;
            if (!msgpack2json(buf, off, left, &p->key) ||
                !try_to_write(buf, off, left, ":", 1)  ||
                !msgpack2json(buf, off, left, &p->val)) {
                goto msg2json_end;
            }
            for (i = 1; i < loop; i++) {
                if (
                    !try_to_write(buf, off, left, ",", 1) ||
                    !msgpack2json(buf, off, left, &(p+i)->key) ||
                    !try_to_write(buf, off, left, ":", 1)  ||
                    !msgpack2json(buf, off, left, &(p+i)->val) ) {
                    goto msg2json_end;

                }
            }
        }

        ret = try_to_write(buf, off, left, "}", 1);
        break;

    default:
        flb_warn("[%s] unknown msgpack type %i", __FUNCTION__, o->type);
    }

 msg2json_end:
    return ret;
}

/**
 *  convert msgpack to JSON string.
 *  This API is similar to snprintf.
 *
 *  @param  json_str  The buffer to fill JSON string.
 *  @param  json_size The size of json_str.
 *  @param  data      The msgpack_unpacked data.
 *  @return success   ? a number characters filled : negative value
 */
int flb_msgpack_to_json(char *json_str, size_t json_size,
                        const msgpack_object *obj)
{
    int ret = -1;
    int off = 0;

    if (json_str == NULL || obj == NULL) {
        return -1;
    }

    ret = msgpack2json(json_str, &off, json_size - 1, obj);
    json_str[off] = '\0';
    return ret ? off: ret;
}

flb_sds_t flb_msgpack_raw_to_json_sds(const void *in_buf, size_t in_size)
{
    int ret;
    size_t off = 0;
    size_t out_size;
    msgpack_unpacked result;
    msgpack_object *root;
    flb_sds_t out_buf;
    flb_sds_t tmp_buf;

    out_size = in_size * 1.5;
    out_buf = flb_sds_create_size(out_size);
    if (!out_buf) {
        flb_errno();
        return NULL;
    }

    msgpack_unpacked_init(&result);
    msgpack_unpack_next(&result, in_buf, in_size, &off);
    root = &result.data;

    while (1) {
        ret = flb_msgpack_to_json(out_buf, out_size, root);
        if (ret <= 0) {
            tmp_buf = flb_sds_increase(out_buf, 256);
            if (tmp_buf) {
                out_buf = tmp_buf;
                out_size += 256;
            }
            else {
                flb_errno();
                flb_sds_destroy(out_buf);
                msgpack_unpacked_destroy(&result);
                return NULL;
            }
        }
        else {
            break;
        }
    }

    msgpack_unpacked_destroy(&result);
    flb_sds_len_set(out_buf, ret);

    return out_buf;
}

/*
 * Given a 'format' string type, return it integer representation. This
 * is used by output plugins that uses pack functions to convert
 * msgpack records to JSON.
 */
int flb_pack_to_json_format_type(const char *str)
{
    if (strcasecmp(str, "msgpack") == 0) {
        return FLB_PACK_JSON_FORMAT_NONE;
    }
    else if (strcasecmp(str, "json") == 0) {
        return FLB_PACK_JSON_FORMAT_JSON;
    }
    else if (strcasecmp(str, "json_stream") == 0) {
        return FLB_PACK_JSON_FORMAT_STREAM;
    }
    else if (strcasecmp(str, "json_lines") == 0) {
        return FLB_PACK_JSON_FORMAT_LINES;
    }

    return -1;
}

/* Given a 'date string type', return it integer representation */
int flb_pack_to_json_date_type(const char *str)
{
    if (strcasecmp(str, "double") == 0) {
        return FLB_PACK_JSON_DATE_DOUBLE;
    }
    else if (strcasecmp(str, "iso8601") == 0) {
        return FLB_PACK_JSON_DATE_ISO8601;
    }
    else if (strcasecmp(str, "epoch") == 0) {
        return FLB_PACK_JSON_DATE_EPOCH;
    }

    return -1;
}


flb_sds_t flb_pack_msgpack_to_json_format(const char *data, uint64_t bytes,
                                          int json_format, int date_format,
                                          flb_sds_t date_key)
{
    int i;
    int len;
    int ok = MSGPACK_UNPACK_SUCCESS;
    int records = 0;
    int map_size;
    size_t off = 0;
    char time_formatted[32];
    size_t s;
    flb_sds_t out_tmp;
    flb_sds_t out_js;
    flb_sds_t out_buf = NULL;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object map;
    msgpack_sbuffer tmp_sbuf;
    msgpack_packer tmp_pck;
    msgpack_object *obj;
    struct tm tm;
    struct flb_time tms;

    if (!date_key) {
        return NULL;
    }

    /* Iterate the original buffer and perform adjustments */
    records = flb_mp_count(data, bytes);
    if (records <= 0) {
        return NULL;
    }

    /* For json lines and streams mode we need a pre-allocated buffer */
    if (json_format == FLB_PACK_JSON_FORMAT_LINES ||
        json_format == FLB_PACK_JSON_FORMAT_STREAM) {
        out_buf = flb_sds_create_size(bytes * 1.25);
        if (!out_buf) {
            flb_errno();
            return NULL;
        }
    }

    /* Create temporal msgpack buffer */
    msgpack_sbuffer_init(&tmp_sbuf);
    msgpack_packer_init(&tmp_pck, &tmp_sbuf, msgpack_sbuffer_write);

    /*
     * If the format is the original msgpack style of one big array,
     * registrate the array, otherwise is not necessary. FYI, original format:
     *
     * [
     *   [timestamp, map],
     *   [timestamp, map],
     *   [T, M]...
     * ]
     */
    if (json_format == FLB_PACK_JSON_FORMAT_JSON) {
        msgpack_pack_array(&tmp_pck, records);
    }

    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == ok) {
        /* Each array must have two entries: time and record */
        root = result.data;
        if (root.via.array.size != 2) {
            continue;
        }

        /* Unpack time */
        flb_time_pop_from_msgpack(&tms, &result, &obj);

        /* Get the record/map */
        map = root.via.array.ptr[1];
        map_size = map.via.map.size;
        msgpack_pack_map(&tmp_pck, map_size + 1);

        /* Append date key */
        msgpack_pack_str(&tmp_pck, flb_sds_len(date_key));
        msgpack_pack_str_body(&tmp_pck, date_key, flb_sds_len(date_key));

        /* Append date value */
        switch (date_format) {
        case FLB_PACK_JSON_DATE_DOUBLE:
            msgpack_pack_double(&tmp_pck, flb_time_to_double(&tms));
            break;
        case FLB_PACK_JSON_DATE_ISO8601:
            /* Format the time, use microsecond precision not nanoseconds */
            gmtime_r(&tms.tm.tv_sec, &tm);
            s = strftime(time_formatted, sizeof(time_formatted) - 1,
                         FLB_PACK_JSON_DATE_ISO8601_FMT, &tm);

            len = snprintf(time_formatted + s,
                           sizeof(time_formatted) - 1 - s,
                           ".%06" PRIu64 "Z",
                           (uint64_t) tms.tm.tv_nsec / 1000);
            s += len;
            msgpack_pack_str(&tmp_pck, s);
            msgpack_pack_str_body(&tmp_pck, time_formatted, s);
            break;
        case FLB_PACK_JSON_DATE_EPOCH:
            msgpack_pack_uint64(&tmp_pck, (long long unsigned)(tms.tm.tv_sec));
            break;
        }

        /* Append remaining keys/values */
        for (i = 0; i < map_size; i++) {
            msgpack_object *k = &map.via.map.ptr[i].key;
            msgpack_object *v = &map.via.map.ptr[i].val;

            msgpack_pack_object(&tmp_pck, *k);
            msgpack_pack_object(&tmp_pck, *v);
        }

        /*
         * If the format is the original msgpack style, just continue since
         * we don't care about separator or JSON convertion at this point.
         */
        if (json_format == FLB_PACK_JSON_FORMAT_JSON) {
            continue;
        }

        /*
         * Here we handle two types of records concatenation:
         *
         * FLB_PACK_JSON_FORMAT_LINES: add  breakline (\n) after each record
         *
         *
         *     {'ts':abc,'k1':1}
         *     {'ts':abc,'k1':2}
         *     {N}
         *
         * FLB_PACK_JSON_FORMAT_STREAM: no separators, e.g:
         *
         *     {'ts':abc,'k1':1}{'ts':abc,'k1':2}{N}
         */
        if (json_format == FLB_PACK_JSON_FORMAT_LINES ||
            json_format == FLB_PACK_JSON_FORMAT_STREAM) {

            /* Encode current record into JSON in a temporal variable */
            out_js = flb_msgpack_raw_to_json_sds(tmp_sbuf.data, tmp_sbuf.size);
            if (!out_js) {
                msgpack_sbuffer_destroy(&tmp_sbuf);
                flb_sds_destroy(out_buf);
                return NULL;
            }

            /*
             * One map record has been converted, now append it to the
             * outgoing out_buf sds variable.
             */
            out_tmp = flb_sds_cat(out_buf, out_js, flb_sds_len(out_js));
            if (!out_tmp) {
                msgpack_sbuffer_destroy(&tmp_sbuf);
                flb_sds_destroy(out_js);
                flb_sds_destroy(out_buf);
                return NULL;
            }

            /* Release temporal json sds buffer */
            flb_sds_destroy(out_js);

            /* If a realloc happened, check the returned address */
            if (out_tmp != out_buf) {
                out_buf = out_tmp;
            }

            /* Append the breakline only for json lines mode */
            if (json_format == FLB_PACK_JSON_FORMAT_LINES) {
                out_tmp = flb_sds_cat(out_buf, "\n", 1);
                if (!out_tmp) {
                    msgpack_sbuffer_destroy(&tmp_sbuf);
                    flb_sds_destroy(out_buf);
                    return NULL;
                }
                if (out_tmp != out_buf) {
                    out_buf = out_tmp;
                }
            }
            msgpack_sbuffer_clear(&tmp_sbuf);
        }
    }

    /* Release the unpacker */
    msgpack_unpacked_destroy(&result);

    /* Format to JSON */
    if (json_format == FLB_PACK_JSON_FORMAT_JSON) {
        out_buf = flb_msgpack_raw_to_json_sds(tmp_sbuf.data, tmp_sbuf.size);
        msgpack_sbuffer_destroy(&tmp_sbuf);

        if (!out_buf) {
            return NULL;
        }
    }
    else {
        msgpack_sbuffer_destroy(&tmp_sbuf);
    }

    return out_buf;
}

/**
 *  convert msgpack to JSON string.
 *  This API is similar to snprintf.
 *  @param  size     Estimated length of json str.
 *  @param  data     The msgpack_unpacked data.
 *  @return success  ? allocated json str ptr : NULL
 */
char *flb_msgpack_to_json_str(size_t size, const msgpack_object *obj)
{
    int ret;
    char *buf = NULL;
    char *tmp;

    if (obj == NULL) {
        return NULL;
    }

    if (size <= 0) {
        size = 128;
    }

    buf = flb_malloc(size);
    if (!buf) {
        flb_errno();
        return NULL;
    }

    while (1) {
        ret = flb_msgpack_to_json(buf, size, obj);
        if (ret <= 0) {
            /* buffer is small. retry.*/
            size += 128;
            tmp = flb_realloc(buf, size);
            if (tmp) {
                buf = tmp;
            }
            else {
                flb_free(buf);
                flb_errno();
                return NULL;
            }
        }
        else {
            break;
        }
    }

    return buf;
}

int flb_pack_time_now(msgpack_packer *pck)
{
    int ret;
    struct flb_time t;

    flb_time_get(&t);
    ret = flb_time_append_to_msgpack(&t, pck, 0);

    return ret;
}

int flb_msgpack_expand_map(char *map_data, size_t map_size,
                           msgpack_object_kv **kv_arr, int kv_arr_len,
                           char** out_buf, int* out_size)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pck;
    msgpack_unpacked result;
    size_t off = 0;
    char *ret_buf;
    int map_num;
    int i;
    int len;

    if (map_data == NULL){
        return -1;
    }

    msgpack_unpacked_init(&result);
    if ( (i=msgpack_unpack_next(&result, map_data, map_size, &off)) != MSGPACK_UNPACK_SUCCESS ){
        return -1;
    }
    if (result.data.type != MSGPACK_OBJECT_MAP) {
        msgpack_unpacked_destroy(&result);
        return -1;
    }

    len = result.data.via.map.size;
    map_num = kv_arr_len + len;

    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pck, &sbuf, msgpack_sbuffer_write);
    msgpack_pack_map(&pck, map_num);

    for(i=0; i<len; i++) {
        msgpack_pack_object(&pck, result.data.via.map.ptr[i].key);
        msgpack_pack_object(&pck, result.data.via.map.ptr[i].val);
    }
    for(i=0; i<kv_arr_len; i++){
        msgpack_pack_object(&pck, kv_arr[i]->key);
        msgpack_pack_object(&pck, kv_arr[i]->val);
    }
    msgpack_unpacked_destroy(&result);

    *out_size = sbuf.size;
    ret_buf  = flb_malloc(sbuf.size);
    *out_buf = ret_buf;
    if (*out_buf == NULL) {
        flb_errno();
        msgpack_sbuffer_destroy(&sbuf);
        return -1;
    }
    memcpy(*out_buf, sbuf.data, sbuf.size);
    msgpack_sbuffer_destroy(&sbuf);

    return 0;
}

static flb_sds_t flb_msgpack_gelf_key(flb_sds_t *s, int in_array,
                                      const char *prefix_key, int prefix_key_len,
                                      int concat,
                                      const char *key, int key_len)
{
    int i;
    flb_sds_t tmp;
    static char valid_char[256] = {
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0,
       1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
       1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
       0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
       1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char *prefix_key_copy = NULL;
    char *key_copy = NULL;
    flb_sds_t ret;

    if (prefix_key_len > 0) {
        prefix_key_copy = str_copy_replace(prefix_key, prefix_key_len, '/', '_');
        if (!prefix_key_copy) {
            ret = NULL;
            goto cleanup;
        }
    }

    if (key_len > 0) {
        key_copy = str_copy_replace(key, key_len, '/', '_');
        if (!key_copy) {
            ret = NULL;
            goto cleanup;
        }
    }

    /* check valid key char [A-Za-z0-9_\.\-] */
    for(i=0; i < prefix_key_len; i++) {
        if (!valid_char[(unsigned char)prefix_key_copy[i]]) {
            flb_error("[%s] invalid prefix key char at pos %d: '%.*s'",  __FUNCTION__,
                      i, prefix_key_len, prefix_key);
            ret = NULL;
            goto cleanup;
        }
    }
    for(i=0; i < key_len; i++) {
        if (!valid_char[(unsigned char)key_copy[i]]) {
            flb_error("[%s] invalid key char at pos %d: '%.*s'",  __FUNCTION__,
                      i, key_len, key);
            ret = NULL;
            goto cleanup;
        }
    }

    if (in_array == FLB_FALSE) {
        tmp = flb_sds_cat(*s, ", \"", 3);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    }

    if (prefix_key_len > 0) {
        tmp = flb_sds_cat(*s, prefix_key_copy, prefix_key_len);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    }

    if (concat == FLB_TRUE) {
        tmp = flb_sds_cat(*s, "_", 1);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    }

    if (key_len > 0) {
        tmp = flb_sds_cat(*s, key_copy, key_len);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    }

    if (in_array == FLB_FALSE) {
        tmp = flb_sds_cat(*s, "\":", 2);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    } else {
        tmp = flb_sds_cat(*s, "=", 1);
        if (tmp == NULL) {
            ret = NULL;
            goto cleanup;
        }
        *s = tmp;
    }

    ret = *s;

cleanup:
    if (prefix_key_copy) {
        flb_free(prefix_key_copy);
    }
    if (key_copy) {
        flb_free(key_copy);
    }

    return ret;
}

static flb_sds_t flb_msgpack_gelf_value(flb_sds_t *s, int quote,
                                        const char *val, int val_len)
{
    flb_sds_t tmp;

    if (quote == FLB_TRUE) {
        tmp = flb_sds_cat(*s, "\"", 1);
        if (tmp == NULL) return NULL;
        *s = tmp;

        if (val_len > 0) {
            tmp = flb_sds_cat_utf8(s, val, val_len);
            if (tmp == NULL) return NULL;
            *s = tmp;
        }

        tmp = flb_sds_cat(*s, "\"", 1);
        if (tmp == NULL) return NULL;
        *s = tmp;
    } else {
        tmp = flb_sds_cat(*s, val, val_len);
        if (tmp == NULL) return NULL;
        *s = tmp;
    }

    return *s;
}

static flb_sds_t flb_msgpack_gelf_value_ext(flb_sds_t *s, int quote,
                                            const char *val, int val_len)
{
    static const char int2hex[] = "0123456789abcdef";
    flb_sds_t tmp;

    if (quote == FLB_TRUE) {
        tmp = flb_sds_cat(*s, "\"", 1);
        if (tmp == NULL) return NULL;
        *s = tmp;
    }
    /* ext body. fortmat is similar to printf(1) */
    {
        int i;
        char temp[5];
        for(i=0; i < val_len; i++) {
            char c = (char)val[i];
            temp[0] = '\\';
            temp[1] = 'x';
            temp[2] = int2hex[ (unsigned char) ((c & 0xf0) >> 4)];
            temp[3] = int2hex[ (unsigned char) (c & 0x0f)];
            temp[4] = '\0';
            tmp = flb_sds_cat(*s, temp, 4);
            if (tmp == NULL) return NULL;
            *s = tmp;
        }
    }
    if (quote == FLB_TRUE) {
        tmp = flb_sds_cat(*s, "\"", 1);
        if (tmp == NULL) return NULL;
        *s = tmp;
    }

    return *s;
}

static flb_sds_t flb_msgpack_gelf_flatten(flb_sds_t *s, msgpack_object *o,
                                          const char *prefix, int prefix_len,
                                          int in_array)
{
    int i;
    int loop;
    flb_sds_t tmp;

    switch(o->type) {
    case MSGPACK_OBJECT_NIL:
        tmp = flb_sds_cat(*s, "null", 4);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_BOOLEAN:
        if (o->via.boolean) {
            tmp = flb_msgpack_gelf_value(s, !in_array, "true", 4);
        } else {
            tmp = flb_msgpack_gelf_value(s, !in_array, "false", 5);
        }
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        tmp = flb_sds_printf(s, "%lu", (unsigned long)o->via.u64);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        tmp = flb_sds_printf(s, "%ld", (signed long)o->via.i64);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
        tmp = flb_sds_printf(s, "%f", o->via.f64);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_STR:
        tmp = flb_msgpack_gelf_value(s, !in_array,
                                     o->via.str.ptr,
                                     o->via.str.size);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_BIN:
        tmp = flb_msgpack_gelf_value(s, !in_array,
                                     o->via.bin.ptr,
                                     o->via.bin.size);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_EXT:
        tmp = flb_msgpack_gelf_value_ext(s, !in_array,
                                         o->via.ext.ptr,
                                         o->via.ext.size);
        if (tmp == NULL) return NULL;
        *s = tmp;
        break;

    case MSGPACK_OBJECT_ARRAY:
        loop = o->via.array.size;

        if (!in_array) {
            tmp = flb_sds_cat(*s, "\"", 1);
            if (tmp == NULL) NULL;
            *s = tmp;
        }
        if (loop != 0) {
            msgpack_object* p = o->via.array.ptr;
            for (i=0; i<loop; i++) {
                if (i > 0) {
                     tmp = flb_sds_cat(*s, ", ", 2);
                     if (tmp == NULL) return NULL;
                     *s = tmp;
                }
                tmp = flb_msgpack_gelf_flatten(s, p+i,
                                               prefix, prefix_len,
                                               FLB_TRUE);
                if (tmp == NULL) return NULL;
                *s = tmp;
            }
        }

        if (!in_array) {
            tmp = flb_sds_cat(*s, "\"", 1);
            if (tmp == NULL) return NULL;
            *s = tmp;
        }
        break;

    case MSGPACK_OBJECT_MAP:
        loop = o->via.map.size;
        if (loop != 0) {
            msgpack_object_kv *p = o->via.map.ptr;
            for (i = 0; i < loop; i++) {
                msgpack_object *k = &((p+i)->key);
                msgpack_object *v = &((p+i)->val);

                const char *key = k->via.str.ptr;
                int key_len = k->via.str.size;

                if (v->type == MSGPACK_OBJECT_MAP) {
                    char *obj_prefix = NULL;
                    int obj_prefix_len = 0;

                    obj_prefix_len = key_len;
                    if (prefix_len > 0) {
                        obj_prefix_len += prefix_len + 1;
                    }

                    obj_prefix = flb_malloc(obj_prefix_len + 1);
                    if (obj_prefix == NULL) {
                       return NULL;
                    }

                    if (prefix_len > 0) {
                        memcpy(obj_prefix, prefix, prefix_len);
                        obj_prefix[prefix_len] = '_';
                        memcpy(obj_prefix + prefix_len + 1, key, key_len);
                    } else {
                        memcpy(obj_prefix, key, key_len);
                    }
                    obj_prefix[obj_prefix_len] = '\0';

                    tmp = flb_msgpack_gelf_flatten(s, v,
                                                   obj_prefix, obj_prefix_len,
                                                   in_array);
                    if (tmp == NULL) {
                        flb_free(obj_prefix);
                        return NULL;
                    }
                    *s = tmp;

                    flb_free(obj_prefix);
                } else {
                    if (in_array == FLB_TRUE && i > 0) {
                        tmp = flb_sds_cat(*s, " ", 1);
                        if (tmp == NULL) return NULL;
                        *s = tmp;
                    }
                    if (in_array && prefix_len <= 0) {
                        tmp = flb_msgpack_gelf_key(s, in_array,
                                                   NULL, 0,
                                                   FLB_FALSE,
                                                   key, key_len);
                    } else {
                        tmp = flb_msgpack_gelf_key(s, in_array,
                                                   prefix, prefix_len,
                                                   FLB_TRUE,
                                                   key, key_len);
                    }
                    if (tmp == NULL) return NULL;
                    *s = tmp;

                    tmp = flb_msgpack_gelf_flatten(s, v, NULL, 0, in_array);
                    if (tmp == NULL) return NULL;
                    *s = tmp;
                }
            }
        }
        break;

    default:
        flb_warn("[%s] unknown msgpack type %i", __FUNCTION__, o->type);
    }

    return *s;
}

flb_sds_t flb_msgpack_to_gelf(flb_sds_t *s, msgpack_object *o,
                              struct flb_time *tm,
                              struct flb_gelf_fields *fields)
{
    int i;
    int loop;
    flb_sds_t tmp;

    int host_key_found = FLB_FALSE;
    int timestamp_key_found = FLB_FALSE;
    int level_key_found = FLB_FALSE;
    int short_message_key_found = FLB_FALSE;
    int full_message_key_found = FLB_FALSE;

    char *host_key = NULL;
    char *timestamp_key = NULL;
    char *level_key = NULL;
    char *short_message_key = NULL;
    char *full_message_key = NULL;

    int host_key_len = 0;
    int timestamp_key_len = false;
    int level_key_len = 0;
    int short_message_key_len = 0;
    int full_message_key_len = 0;

    if (s == NULL || o == NULL) {
        return NULL;
    }

    if (fields != NULL && fields->host_key != NULL) {
        host_key = fields->host_key;
        host_key_len = flb_sds_len(fields->host_key);
    }
    else {
        host_key = "host";
        host_key_len = 4;
    }

    if (fields != NULL && fields->timestamp_key != NULL) {
        timestamp_key = fields->timestamp_key;
        timestamp_key_len = flb_sds_len(fields->timestamp_key);
    }
    else {
        timestamp_key = "timestamp";
        timestamp_key_len = 9;
    }

    if (fields != NULL && fields->level_key != NULL) {
        level_key = fields->level_key;
        level_key_len = flb_sds_len(fields->level_key);
    }
    else {
        level_key = "level";
        level_key_len = 5;
    }

    if (fields != NULL && fields->short_message_key != NULL) {
        short_message_key = fields->short_message_key;
        short_message_key_len = flb_sds_len(fields->short_message_key);
    }
    else {
        short_message_key = "short_message";
        short_message_key_len = 13;
    }

    if (fields != NULL && fields->full_message_key != NULL) {
        full_message_key = fields->full_message_key;
        full_message_key_len = flb_sds_len(fields->full_message_key);
    }
    else {
        full_message_key = "full_message";
        full_message_key_len = 12;
    }

    tmp = flb_sds_cat(*s, "{\"version\":\"1.1\"", 16);
    if (tmp == NULL) return NULL;
    *s = tmp;

    loop = o->via.map.size;
    if (loop != 0) {
        msgpack_object_kv *p = o->via.map.ptr;

        for (i = 0; i < loop; i++) {
            const char *key = NULL;
            int key_len;
            const char *val = NULL;
            int val_len;
            int quote = FLB_FALSE;
            int custom_key = FLB_FALSE;

            msgpack_object *k = &p[i].key;
            msgpack_object *v = &p[i].val;

            if (k->type != MSGPACK_OBJECT_BIN && k->type != MSGPACK_OBJECT_STR) {
                continue;
            }

            if (k->type == MSGPACK_OBJECT_STR) {
                key = k->via.str.ptr;
                key_len = k->via.str.size;
            }
            else {
                key = k->via.bin.ptr;
                key_len = k->via.bin.size;
            }

            if ((key_len == host_key_len) &&
                !strncmp(key, host_key, host_key_len)) {
                if (host_key_found == FLB_TRUE) continue;
                host_key_found = FLB_TRUE;
                key = "host";
                key_len = 4;
            }
            else if ((key_len == short_message_key_len) &&
                     !strncmp(key, short_message_key, short_message_key_len)) {
                if (short_message_key_found == FLB_TRUE) continue;
                short_message_key_found = FLB_TRUE;
                key = "short_message";
                key_len = 13;
            }
            else if ((key_len == timestamp_key_len) &&
                     !strncmp(key, timestamp_key, timestamp_key_len)) {
                if (timestamp_key_found == FLB_TRUE) continue;
                timestamp_key_found = FLB_TRUE;
                key = "timestamp";
                key_len = 9;
            }
            else if ((key_len == level_key_len) &&
                     !strncmp(key, level_key, level_key_len )) {
                if (level_key_found == FLB_TRUE) continue;
                level_key_found = FLB_TRUE;
                key = "level";
                key_len = 5;
                if (v->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
                        if ( v->via.u64 > 7 ) {
                            flb_error("[flb_msgpack_to_gelf] level is %" PRIu64 ", but should be in 0..7", v->via.u64);
                            return NULL;
                        }
                } else if (v->type == MSGPACK_OBJECT_STR){
                    val     = v->via.str.ptr;
                    val_len = v->via.str.size;
                    if ( val_len != 1 || val[0] < '0' || val[0] > '7' ) {
                            flb_error("[flb_msgpack_to_gelf] level is '%.*s', but should be in 0..7", val_len, val);
                            return NULL;
                    }
                }
            }
            else if ((key_len == full_message_key_len) &&
                     !strncmp(key, full_message_key, full_message_key_len)) {
                if (full_message_key_found == FLB_TRUE) continue;
                full_message_key_found = FLB_TRUE;
                key = "full_message";
                key_len = 12;
            }
            else if ((key_len == 2)  && !strncmp(key, "id", 2)) {
                /* _id key not allowed */
                continue;
            }
            else {
                custom_key = FLB_TRUE;
            }

            if (v->type == MSGPACK_OBJECT_MAP) {
                char *prefix = NULL;
                int prefix_len = 0;

                prefix_len = key_len + 1;
                prefix = flb_malloc(prefix_len + 1);
                if (prefix == NULL) {
                    return NULL;
                }

                prefix[0] = '_';
                strncpy(prefix + 1, key, key_len);
                prefix[prefix_len] = '\0';

                tmp = flb_msgpack_gelf_flatten (s, v,
                                                prefix, prefix_len, FLB_FALSE);
                if (tmp == NULL) {
                    flb_free(prefix);
                    return NULL;
                }
                *s = tmp;
                flb_free(prefix);

            }
            else if (v->type == MSGPACK_OBJECT_ARRAY) {
                if (custom_key == FLB_TRUE) {
                    tmp = flb_msgpack_gelf_key(s, FLB_FALSE, "_", 1, FLB_FALSE,
                                             key, key_len);
                }
                else {
                    tmp = flb_msgpack_gelf_key(s, FLB_FALSE, NULL, 0, FLB_FALSE,
                                             key, key_len);
                }
                if (tmp == NULL) return NULL;
                *s = tmp;

                tmp = flb_msgpack_gelf_flatten(s, v, NULL, 0, FLB_FALSE);
                if (tmp == NULL) return NULL;
                *s = tmp;
            }
            else {
                char temp[48] = {0};
                if (v->type == MSGPACK_OBJECT_NIL) {
                    val = "null";
                    val_len = 4;
                    continue;
                }
                else if (v->type == MSGPACK_OBJECT_BOOLEAN) {
                    quote   = FLB_TRUE;
                    val = v->via.boolean ? "true" : "false";
                    val_len = v->via.boolean ? 4 : 5;
                }
                else if (v->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
                    val = temp;
                    val_len = snprintf(temp, sizeof(temp) - 1,
                                       "%" PRIu64, v->via.u64);
                }
                else if (v->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) {
                    val = temp;
                    val_len = snprintf(temp, sizeof(temp) - 1,
                                       "%" PRId64, v->via.i64);
                }
                else if (v->type == MSGPACK_OBJECT_FLOAT) {
                    val = temp;
                    val_len = snprintf(temp, sizeof(temp) - 1,
                                       "%f", v->via.f64);
                }
                else if (v->type == MSGPACK_OBJECT_STR) {
                    /* String value */
                    quote   = FLB_TRUE;
                    val     = v->via.str.ptr;
                    val_len = v->via.str.size;
                }
                else if (v->type == MSGPACK_OBJECT_BIN) {
                    /* Bin value */
                    quote   = FLB_TRUE;
                    val     = v->via.bin.ptr;
                    val_len = v->via.bin.size;
                }
                else if (v->type == MSGPACK_OBJECT_EXT) {
                    quote   = FLB_TRUE;
                    val     = o->via.ext.ptr;
                    val_len = o->via.ext.size;
                }

                if (!val || !key) {
                  continue;
                }

                if (custom_key == FLB_TRUE) {
                    tmp = flb_msgpack_gelf_key(s, FLB_FALSE, "_", 1, FLB_FALSE,
                                             key, key_len);
                }
                else {
                    tmp = flb_msgpack_gelf_key(s, FLB_FALSE, NULL, 0, FLB_FALSE,
                                             key, key_len);
                }
                if (tmp == NULL) return NULL;
                *s = tmp;

                if (v->type == MSGPACK_OBJECT_EXT) {
                    tmp = flb_msgpack_gelf_value_ext(s, quote, val, val_len);
                }
                else {
                    tmp = flb_msgpack_gelf_value(s, quote, val, val_len);
                }
                if (tmp == NULL) return NULL;
                *s = tmp;
            }
        }
    }

    if (timestamp_key_found == FLB_FALSE && tm != NULL) {
        tmp = flb_msgpack_gelf_key(s, FLB_FALSE, NULL, 0, FLB_FALSE,
                                   "timestamp", 9);
        if (tmp == NULL) return NULL;
        *s = tmp;

        tmp = flb_sds_printf(s, "%f", flb_time_to_double(tm));
        if (tmp == NULL) return NULL;
        *s = tmp;
    }

    if (short_message_key_found == FLB_FALSE) {
        flb_error("[flb_msgpack_to_gelf] missing short_message key");
        return NULL;
    }

    tmp = flb_sds_cat(*s, "}", 1);
    if (tmp == NULL) return NULL;
    *s = tmp;

    return *s;
}

flb_sds_t flb_msgpack_raw_to_gelf(char *buf, size_t buf_size,
   struct flb_time *tm, struct flb_gelf_fields *fields)
{
    int ret;
    size_t off = 0;
    size_t gelf_size;
    msgpack_unpacked result;
    flb_sds_t s;
    flb_sds_t tmp;

    if (!buf || buf_size <= 0) {
        return NULL;
    }

    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, buf, buf_size, &off);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        return NULL;
    }

    gelf_size = (buf_size * 1.3);
    s = flb_sds_create_size(gelf_size);
    if (s == NULL) {
        msgpack_unpacked_destroy(&result);
        return NULL;
    }

    tmp = flb_msgpack_to_gelf(&s, &result.data, tm, fields);
    if (tmp == NULL) {
        flb_sds_destroy(s);
        msgpack_unpacked_destroy(&result);
        return NULL;
    }
    s = tmp;

    msgpack_unpacked_destroy(&result);

    return s;
}
