/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

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

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_unescape.h>

#include "tcp.h"
#include "tcp_conn.h"
#include "tcp_config.h"

#include <stdlib.h>

struct flb_in_tcp_config *tcp_config_init(struct flb_input_instance *i_ins)
{
    int ret;
    int len;
    char port[16];
    char *out;
    const char *tmp;
    const char *listen;
    const char *buffer_size;
    const char *chunk_size;
    struct flb_in_tcp_config *ctx;

    /* Allocate plugin context */
    ctx = flb_calloc(1, sizeof(struct flb_in_tcp_config));
    if (!ctx) {
        flb_errno();
        return NULL;
    }
    ctx->format = FLB_TCP_FMT_JSON;

    /* Data format (expected payload) */
    tmp = flb_input_get_property("format", i_ins);
    if (tmp) {
        if (strcasecmp(tmp, "json") == 0) {
            ctx->format = FLB_TCP_FMT_JSON;
        }
        else if (strcasecmp(tmp, "none") == 0) {
            ctx->format = FLB_TCP_FMT_NONE;
        }
        else {
            flb_error("[in_tcp] unrecognized format value '%s'", tmp);
            flb_free(ctx);
            return NULL;
        }
    }

    /* String separator used to split records when using 'format none' */
    tmp = flb_input_get_property("separator", i_ins);
    if (tmp) {
        len = strlen(tmp);
        out = flb_malloc(len + 1);
        if (!out) {
            flb_errno();
            flb_free(ctx);
            return NULL;
        }
        ret = flb_unescape_string(tmp, len, &out);
        if (ret <= 0) {
            flb_error("[in_tcp] invalid separator");
            flb_free(out);
            flb_free(ctx);
            return NULL;
        }

        ctx->separator = flb_sds_create_len(out, ret);
        if (!ctx->separator) {
            flb_free(out);
            flb_free(ctx);
            return NULL;
        }
    }
    if (!ctx->separator) {
        ctx->separator = flb_sds_create_len("\n", 1);
    }


    /* Listen interface (if not set, defaults to 0.0.0.0) */
    if (!i_ins->host.listen) {
        listen = flb_input_get_property("listen", i_ins);
        if (listen) {
            ctx->listen = flb_strdup(listen);
        }
        else {
            ctx->listen = flb_strdup("0.0.0.0");
        }
    }
    else {
        ctx->listen = i_ins->host.listen;
    }

    /* Listener TCP Port */
    if (i_ins->host.port == 0) {
        ctx->tcp_port = flb_strdup("5170");
    }
    else {
        snprintf(port, sizeof(port) - 1, "%d", i_ins->host.port);
        ctx->tcp_port = flb_strdup(port);
    }

    /* Chunk size */
    chunk_size = flb_input_get_property("chunk_size", i_ins);
    if (!chunk_size) {
        ctx->chunk_size = FLB_IN_TCP_CHUNK; /* 32KB */
    }
    else {
        /* Convert KB unit to Bytes */
        ctx->chunk_size  = (atoi(chunk_size) * 1024);
    }

    /* Buffer size */
    buffer_size = flb_input_get_property("buffer_size", i_ins);
    if (!buffer_size) {
        ctx->buffer_size = ctx->chunk_size;
    }
    else {
        /* Convert KB unit to Bytes */
        ctx->buffer_size  = (atoi(buffer_size) * 1024);
    }

    flb_debug("[in_tcp] Listen='%s' TCP_Port=%s",
              ctx->listen, ctx->tcp_port);

    return ctx;
}

int tcp_config_destroy(struct flb_in_tcp_config *ctx)
{
    flb_sds_destroy(ctx->separator);
    flb_free(ctx->listen);
    flb_free(ctx->tcp_port);
    flb_free(ctx);

    return 0;
}
