/*
 * Copyright (C) Looyao
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "tst.h"

#define TST_MAX_RESULT_COUNT 50

typedef struct {
    ngx_str_t dict_path;
} ngx_http_auto_complete_loc_conf_t;

static tst_node       *ngx_http_auto_complete_tst;
static tst_node       *ngx_http_auto_complete_tst_cache;
static ngx_shm_zone_t *ngx_http_auto_complete_shm_zone;
static ssize_t         ngx_http_auto_complete_shm_size;
static char           *ngx_http_auto_complete_dict_path;

static char *ngx_http_auto_complete_set_dict_path(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char *ngx_http_auto_complete_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, 
        void *conf);


static void *ngx_http_auto_complete_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_auto_complete_merge_loc_conf(ngx_conf_t *cf, void *parent, 
        void *child);

static ngx_int_t ngx_http_auto_complete_init_module(ngx_cycle_t *cycle);

static inline void ngx_http_auto_complete_json_escapes(char *dst, char *src);
static inline void ngx_http_auto_complete_str_tolower(char *s);
static inline void ngx_unescape_uri_patched(u_char **dst, u_char **src, size_t size, ngx_uint_t type);

static ngx_command_t ngx_http_auto_complete_commands[] = {
    { ngx_string("auto_complete_dict_path"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_auto_complete_set_dict_path,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_auto_complete_loc_conf_t, dict_path),
      NULL },
    { ngx_string("auto_complete_shm_size"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_auto_complete_set_shm_size,
      0,
      0,
      NULL },


      ngx_null_command
};

static ngx_http_module_t ngx_http_auto_complete_module_ctx = {
    NULL,                                        /* preconfiguration */
    NULL,                                        /* postconfiguration */

    NULL,                                        /* create main configuration */
    NULL,                                        /* init main configuration */

    NULL,                                        /* create server configuration */
    NULL,                                        /* merge server configuration */

    ngx_http_auto_complete_create_loc_conf,      /* create location configuration */
    ngx_http_auto_complete_merge_loc_conf,       /* merge location configuration */    
};

ngx_module_t ngx_http_auto_complete_module = {
    NGX_MODULE_V1,
    &ngx_http_auto_complete_module_ctx,          /* module context */
    ngx_http_auto_complete_commands,             /* module directives */
    NGX_HTTP_MODULE,                             /* module type */
    NULL,                                        /* init master */
    NULL,                                        /* init module */
    NULL,                                        /* init process */
    NULL,                                        /* init thread */
    NULL,                                        /* exit thread */
    NULL,                                        /* exit process */
    NULL,                                        /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t 
ngx_http_auto_complete_handler(ngx_http_request_t *r)
{
    ngx_int_t               rc;
    ngx_buf_t              *b;
    ngx_chain_t             out;
    ngx_str_t               value;
    u_char                 *word, *cb, *dst, *src;
    size_t                  count;
    char                    escape_buf[512];
    tst_search_result_node *node;
    tst_search_result      *result;
    ngx_table_elt_t        *hv;

    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (r->uri.data[r->uri.len - 1] == '/') {
        return NGX_DECLINED;
    }

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (!b) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    word = NULL;
    cb = NULL;
    
    if (r->args.len) {
        if (ngx_http_arg(r, (u_char *) "s", 1, &value) == NGX_OK) {
            word = ngx_pcalloc(r->pool, value.len + 1);
            if (!word) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            src = value.data;
            dst = word;

            ngx_unescape_uri_patched(&dst, &src, value.len, NGX_UNESCAPE_URI);

            *dst = 0;
        }

        if (ngx_http_arg(r, (u_char *) "cb", 2, &value) == NGX_OK) {
            if (value.len > 128) {
                return NGX_HTTP_BAD_REQUEST;
            }

            cb = ngx_pcalloc(r->pool, value.len + 1);
            if (!cb) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            src = value.data;
            dst = cb;

            ngx_unescape_uri_patched(&dst, &src, value.len, NGX_UNESCAPE_URI);

            *dst = 0;
        }
    }

    b->pos = "[]";
    b->last = b->pos + sizeof("[]") - 1;

    if (word) {
        ngx_http_auto_complete_str_tolower((char *) word);

        ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)ngx_http_auto_complete_shm_zone->shm.addr;

        ngx_shmtx_lock(&shpool->mutex);
        result = tst_search(ngx_http_auto_complete_tst_cache, (char *) word, r->pool);
        ngx_shmtx_unlock(&shpool->mutex);

        if (result->count == 1) {
            b->pos = result->list->word;
            b->last = result->list->word + result->list->word_len;

            ngx_str_t hv_name = ngx_string("X-TST-CACHE-HIT");
            ngx_str_t hv_value = ngx_string("true");
            hv = ngx_list_push(&r->headers_out.headers);

            if (!hv) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            hv->hash = 1;
            hv->key.len = hv_name.len;
            hv->key.data = hv_name.data;
            hv->value.len =  hv_value.len;
            hv->value.data = hv_value.data;

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "fuck ...");
        } else {
            ngx_shmtx_lock(&shpool->mutex);
            result = tst_search(ngx_http_auto_complete_tst, (char *) word, r->pool);
            ngx_shmtx_unlock(&shpool->mutex);
            if (result->count > 0) {
                count = 0;

                b->pos = ngx_pcalloc(r->pool, result->count * 512 + 147);
                b->last = b->pos;

                if (!b->pos) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                if (cb && *cb) {
                    b->last = ngx_sprintf(b->last, "%s(\n{\"result\":[", cb);
                } else {
                    b->last = ngx_sprintf(b->last, "[");
                }

                node = result->list;
                while (node && count < TST_MAX_RESULT_COUNT) {
                    ngx_http_auto_complete_json_escapes(escape_buf, node->word);
                    b->last = ngx_sprintf(b->last, "\"%s\",", escape_buf);

                    node = node->next;
                    count++;
                }

                *(b->last - 1) = ']';

                if (cb && *cb) {
                    b->last = ngx_sprintf(b->last, "}\n);");
                }


                if (result->count > 50) {
                    ngx_shmtx_lock(&shpool->mutex);
                    ngx_http_auto_complete_tst_cache = tst_insert_alias(ngx_http_auto_complete_tst_cache, (char *) word, b->pos, ngx_http_auto_complete_shm_zone);
                    ngx_shmtx_unlock(&shpool->mutex);
                }
            }
        }
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;
    r->headers_out.content_type.data = (u_char *) "text/plain; charset=utf-8";
    r->headers_out.content_type.len = sizeof("text/plain; charset=utf-8") - 1;

    out.buf = b;
    out.next = NULL;
    b->memory = 1;
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc != NGX_OK) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t
ngx_http_auto_complete_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    FILE                      *fp;
    char                      *path, *split;
    char                       word_buf[512], cut_word_buf[46];

    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    if (access(ngx_http_auto_complete_dict_path, F_OK) != 0) {
        fprintf(stderr, "auto_complete_dict_path can't be access\n");
        return NGX_ERROR;
    }

    fp = fopen(ngx_http_auto_complete_dict_path, "r");

    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex); 
    while (fgets(word_buf, sizeof(word_buf), fp)) {
        size_t len = strlen(word_buf) - 1;
        if (word_buf[len] == '\n') {
            word_buf[len] = '\0';
        }

        split = strstr(word_buf, "||");
        if (!split) {
            if (strlen(word_buf) > 45) {
                ngx_snprintf(cut_word_buf, 46, "%s", word_buf);
                ngx_http_auto_complete_tst = tst_insert_alias(ngx_http_auto_complete_tst, cut_word_buf, word_buf, ngx_http_auto_complete_shm_zone);
            } else {
                ngx_http_auto_complete_tst = tst_insert(ngx_http_auto_complete_tst, word_buf, ngx_http_auto_complete_shm_zone);
            }
        } else {
            *split = 0;
            if (strlen(word_buf) > 46) {
                ngx_snprintf(cut_word_buf, 46, "%s", word_buf);
                ngx_http_auto_complete_tst = tst_insert_alias(ngx_http_auto_complete_tst, cut_word_buf, split + 2, ngx_http_auto_complete_shm_zone);
            } else {
                ngx_http_auto_complete_tst = tst_insert_alias(ngx_http_auto_complete_tst, word_buf, split + 2, ngx_http_auto_complete_shm_zone);
            }
        }
    }
    ngx_shmtx_unlock(&shpool->mutex); 

    fclose(fp);

    return NGX_OK;
}

static char *
ngx_http_auto_complete_set_dict_path(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                 *shm_name;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_http_auto_complete_loc_conf_t   *tlcf;


    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_auto_complete_handler;
    ngx_conf_set_str_slot(cf, cmd, conf);
    
    tlcf = conf;

    if (ngx_http_auto_complete_dict_path) {
        if (ngx_strcmp(tlcf->dict_path.data, ngx_http_auto_complete_dict_path) == 0) {
            return NGX_CONF_OK;
        }
    }

    ngx_http_auto_complete_dict_path = (char *)ngx_pcalloc(cf->pool, tlcf->dict_path.len + 1);
    ngx_sprintf(ngx_http_auto_complete_dict_path, "%V", &tlcf->dict_path);

    shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
    shm_name->len = sizeof("auto_complete") - 1;
    shm_name->data = (unsigned char *) "auto_complete";

    if (ngx_http_auto_complete_shm_size == 0) {
        ngx_http_auto_complete_shm_size = 1024 * 16 * ngx_pagesize;
    }

    ngx_http_auto_complete_shm_zone = ngx_shared_memory_add(cf, shm_name, ngx_http_auto_complete_shm_size, &ngx_http_auto_complete_module);
    if (!ngx_http_auto_complete_shm_zone) {
        return NGX_CONF_ERROR;
    }

    ngx_http_auto_complete_shm_zone->init = ngx_http_auto_complete_init_shm_zone;
    
    return NGX_CONF_OK;
}

static char *ngx_http_auto_complete_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t        new_shm_size;
    ngx_str_t     *value;

    value = cf->args->elts;

    new_shm_size = ngx_parse_size(&value[1]);
    if (new_shm_size == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    new_shm_size = ngx_align(new_shm_size, ngx_pagesize);

    if (new_shm_size < 8 * (ssize_t)ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The auto_complete_shm_size value must be at least %uKB", (8 * ngx_pagesize) >> 10);
        new_shm_size = 8 * ngx_pagesize;
    }

    if (ngx_http_auto_complete_shm_size && ngx_http_auto_complete_shm_size != (ngx_uint_t)new_shm_size) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
    } else {
        ngx_http_auto_complete_shm_size = new_shm_size;
    }

    return NGX_CONF_OK;
}

static void *
ngx_http_auto_complete_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_auto_complete_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auto_complete_loc_conf_t));

    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->dict_path.len = 0;
    conf->dict_path.data = NULL;

    return conf;
}

static char *
ngx_http_auto_complete_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_auto_complete_loc_conf_t *prev = parent;
    ngx_http_auto_complete_loc_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->dict_path, prev->dict_path, "");

    return NGX_CONF_OK;
}

static inline void 
ngx_http_auto_complete_json_escapes(char *dst, char *src)
{
    char  *p = dst;
    char   c;

    while ((c = *(src++))) {
        switch (c) {
            case '\b': 
                *(p++) = '\\';
                *(p++) = 'b';
                break;
            case '\t': 
                *(p++) = '\\';
                *(p++) = 't';
                break;
            case '\n': 
                *(p++) = '\\';
                *(p++) = 'n';
                break;
            case '\f': 
                *(p++) = '\\';
                *(p++) = 'f';
                break;
            case '\r': 
                *(p++) = '\\';
                *(p++) = 'r';
                break;
            case '/': 
                *(p++) = '\\';
                *(p++) = '/';
                break;
            case '"':
                *(p++) = '\\';
                *(p++) = '"';
                break;
            case '\\':
                *(p++) = '\\';
                *(p++) = '\\';
            default:
                *(p++) = c;
                break;
        }
    }

    *p = 0;
}

static inline void ngx_http_auto_complete_str_tolower(char *s)
{
    while (*s) {
        *s = tolower(*s); 
        s++;
    }
}

static inline void
ngx_unescape_uri_patched(u_char **dst, u_char **src, size_t size,
        ngx_uint_t type)
{
    u_char  *d, *s, ch, c, decoded;
    enum {
        sw_usual = 0,
        sw_quoted,
        sw_quoted_second
    } state;

    d = *dst;
    s = *src;

    state = 0;
    decoded = 0;

    while (size--) {

        ch = *s++;

        switch (state) {
            case sw_usual:
                if (ch == '?'
                    && (type & (NGX_UNESCAPE_URI|NGX_UNESCAPE_REDIRECT)))
                {
                    *d++ = ch;
                    goto done;
                }

                if (ch == '%') {
                    state = sw_quoted;
                    break;
                }

                if (ch == '+') {
                    *d++ = ' ';
                    break;
                }

                *d++ = ch;
                break;

            case sw_quoted:

                if (ch >= '0' && ch <= '9') {
                    decoded = (u_char) (ch - '0');
                    state = sw_quoted_second;
                    break;
                }

                c = (u_char) (ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    decoded = (u_char) (c - 'a' + 10);
                    state = sw_quoted_second;
                    break;
                }

                /* the invalid quoted character */

                state = sw_usual;

                *d++ = ch;

                break;

            case sw_quoted_second:

                state = sw_usual;

                if (ch >= '0' && ch <= '9') {
                    ch = (u_char) ((decoded << 4) + ch - '0');

                    if (type & NGX_UNESCAPE_REDIRECT) {
                        if (ch > '%' && ch < 0x7f) {
                            *d++ = ch;
                            break;
                        }

                        *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);

                        break;
                    }

                    *d++ = ch;

                    break;
                }

                c = (u_char) (ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    ch = (u_char) ((decoded << 4) + c - 'a' + 10);

                    if (type & NGX_UNESCAPE_URI) {
                        if (ch == '?') {
                            *d++ = ch;
                            goto done;
                        }

                        *d++ = ch;
                        break;
                    }

                    if (type & NGX_UNESCAPE_REDIRECT) {
                        if (ch == '?') {
                            *d++ = ch;
                            goto done;
                        }

                        if (ch > '%' && ch < 0x7f) {
                            *d++ = ch;
                            break;
                        }

                        *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
                        break;
                    }

                    *d++ = ch;

                    break;
                }

            /* the invalid quoted character */

            break;
        }
    }

done:

    *dst = d;
    *src = s;
}
