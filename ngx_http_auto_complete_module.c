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

typedef struct _ngx_http_auto_complete_tst {
    tst_node             *root;
    tst_cache_node       *cache_root;
} ngx_http_auto_complete_tst_t;

static ngx_http_auto_complete_tst_t *ngx_http_auto_complete_tst;
static ngx_shm_zone_t               *ngx_http_auto_complete_shm_zone;
static size_t                        ngx_http_auto_complete_shm_size;
static char                         *ngx_http_auto_complete_dict_path;

static char *ngx_http_auto_complete_set_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_auto_complete_init_module_handler(ngx_cycle_t *cycle);

static inline char *ngx_http_auto_complete_str_find_space(char *p);
static inline char *ngx_http_auto_complete_str_find_chs(char *p, size_t *l);

static ngx_int_t ngx_http_auto_complete_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_auto_complete_init_tst(ngx_shm_zone_t *shm_zone);

static inline void ngx_http_auto_complete_json_escapes(char *dst, char *src);
static inline void ngx_http_auto_complete_str_tolower(char *s);
static inline void ngx_unescape_uri_patched(u_char **dst, u_char **src, size_t size, ngx_uint_t type);

static ngx_command_t ngx_http_auto_complete_commands[] = {
    { ngx_string("auto_complete_dict_path"),
        NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
        ngx_http_auto_complete_set_slot,
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

    NULL,                                        /* create location configuration */
    NULL,                                        /* merge location configuration */    
};

ngx_module_t ngx_http_auto_complete_module = {
    NGX_MODULE_V1,
    &ngx_http_auto_complete_module_ctx,          /* module context */
    ngx_http_auto_complete_commands,             /* module directives */
    NGX_HTTP_MODULE,                             /* module type */
    NULL,                                        /* init master */
    ngx_http_auto_complete_init_module_handler,  /* init module */
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
    size_t                  count, len, i;
    char                    ebuf[512], *cache, *cache_p, *cache_last;
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

            while (*word) {
                if (*word != ' ') {
                    break;
                }
                word++;
            }

            while (dst - 1 > word) {
                dst--;
                if (*dst == ' ') {
                    *dst = '\0';
                } else {
                    break;
                }
            }
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

    ngx_str_t hv_name = ngx_string("X-AC-Cached");
    ngx_str_t hv_value = ngx_string("no");
    hv = ngx_list_push(&r->headers_out.headers);

    if (!hv) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    hv->hash = 1;
    hv->key.len = hv_name.len;
    hv->key.data = hv_name.data;
    hv->value.len = hv_value.len;
    hv->value.data = hv_value.data;

    b->pos = (u_char *) "[]";
    b->last = b->pos + sizeof("[]") - 1;

    if (word && *word) {
        ngx_http_auto_complete_str_tolower((char *) word);

        ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)ngx_http_auto_complete_shm_zone->shm.addr;

        ngx_shmtx_lock(&shpool->mutex);
        cache = tst_cache_search(ngx_http_auto_complete_tst->cache_root, (char *) word);
        if (cache) {
            len = strlen(cache) + 1;
            if (cb && *cb) {
                len += ngx_strlen(cb) + sizeof("(\n{\"result\":}\n);");
                b->pos = ngx_pcalloc(r->pool, len);
            } else {
                b->pos = ngx_pcalloc(r->pool, len);
            }

            if (!b->pos) {
                ngx_shmtx_unlock(&shpool->mutex);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            b->last = b->pos;

            if (cb && *cb) {
                b->last = ngx_snprintf(b->last, len, "%s(\n{\"result\":%s}\n);", cb, cache);
            } else {
                b->last = ngx_snprintf(b->last, len, "%s", cache);
            }

            ngx_shmtx_unlock(&shpool->mutex);

            ngx_str_t hv_name = ngx_string("yes");
            hv->value.len = hv_name.len;
            hv->value.data = hv_name.data;

            goto cached;
        }

        result = tst_search(ngx_http_auto_complete_tst->root, (char *) word, r->pool, r->connection->log);
        ngx_shmtx_unlock(&shpool->mutex);

        if (result->count > 1) {
            tst_search_result_sort(result->list, result->tail);
            tst_search_result_uniq(result->list);
        }

        if (!result) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (result->count > 0) {
            count = result->count;
            if (count > TST_MAX_RESULT_COUNT) {
                count = TST_MAX_RESULT_COUNT;
            }

            len = 0;

            node = result->list;
            for (i = 0; node && i < count; i++) {
                len += strlen(node->word);
                node = node->next;
            }

            cache_p = NULL;
            cache_last = NULL;
            if (cb && *cb) {
                b->pos = ngx_pcalloc(r->pool, len * 2 + count * 3 + ngx_strlen(cb) + sizeof("(\n{\"result\":[]}\n);") - 1);
                if (!b->pos) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                b->last = b->pos;
                b->last = ngx_sprintf(b->last, "%s(\n{\"result\":[", cb);
                cache_p = (char *) (b->last - 1);
            } else {
                b->pos = ngx_pcalloc(r->pool, len * 2 + count * 3 + sizeof("[]") - 1);
                if (!b->pos) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                b->last = b->pos;
                b->last = ngx_sprintf(b->pos, "[");
            }

            node = result->list;
            while (node && count > 0) {
                ngx_http_auto_complete_json_escapes(ebuf, node->word);
                b->last = ngx_sprintf(b->last, "\"%s\",", ebuf);

                node = node->next;
                count--;
            }

            *(b->last - 1) = ']';

            if (cb && *cb) {
                cache_last = (char *) b->last;
                b->last = ngx_sprintf(b->last, "}\n);");
            }


            if (result->count == 50 || ngx_strlen((u_char *) word) < 3) {
                if (cb && *cb) {
                    cache = ngx_pcalloc(r->pool, cache_last - cache_p + 1);
                    if (cache) {
                        snprintf(cache, cache_last - cache_p + 1, "%s", cache_p);
                    }
                } else {
                    cache = (char *) b->pos;
                }

                if (cache) {
                    ngx_shmtx_lock(&shpool->mutex);
                    ngx_http_auto_complete_tst->cache_root = tst_cache_insert(ngx_http_auto_complete_tst->cache_root, (char *) word, cache, ngx_http_auto_complete_shm_zone, r->connection->log);
                    ngx_shmtx_unlock(&shpool->mutex);
                }
            }
        }
    }

cached:

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
ngx_http_auto_complete_init_tst(ngx_shm_zone_t *shm_zone)
{
    FILE                      *fp;
    char                      *split, *last;
    char                       wb[512], cwb[31];
    tst_node                  *tst, *node;
    uint64_t                   rank;

    fp = fopen(ngx_http_auto_complete_dict_path, "r");

    tst = NULL;

    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);

    while (fgets(wb, sizeof(wb), fp)) {
        size_t len = strlen(wb) - 1;
        if (wb[len] == '\n') {
            wb[len] = '\0';
        }

        split = strstr(wb, "||");

        if (!split) {
            continue;
        }

        *split = '\0';
        if (strlen(wb) > 20) {
            continue;
        }

        rank = atol(wb);

        split += 2;
        last = split;
        split = strstr(last, "||");

        if (!split) {
            if (strlen(last) > 256) {
                continue;
            }

            snprintf(cwb, sizeof(cwb), "%s", last);
            tst = tst_insert(tst, cwb, last, rank, &node, shm_zone, NULL);

            if (rank > 100 && node) {
                char *p = last;
                size_t l = 0;

                while (1) {
                    p = ngx_http_auto_complete_str_find_chs(p, &l);
                    if (!p) {
                        break;
                    }

                    if (p == last) {
                        p += l;
                        continue;
                    }

                    tst = tst_insert_alias(tst, p, node, rank, shm_zone, NULL);
                    p += l;
                }

                p = last;
                while (1) {
                    p = ngx_http_auto_complete_str_find_space(p);
                    if (!p) {
                        break;
                    }

                    tst = tst_insert_alias(tst, p, node, rank, shm_zone, NULL);
                }
            }


        } else {
            *split = '\0';
            split += 2;

            snprintf(cwb, sizeof(cwb), "%s", split);
            tst_search_node(tst, cwb, &node);
            if (node) {
                tst = tst_insert_alias(tst, last, node, rank, shm_zone, NULL);
            }
        }
    }

    ngx_shmtx_unlock(&shpool->mutex); 

    fclose(fp);

    if (shm_zone->data == NULL) {
        shm_zone->data = ngx_http_auto_complete_tst;
    }

    ngx_http_auto_complete_tst->root = tst;

    return NGX_OK;
}

static ngx_int_t
ngx_http_auto_complete_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    return NGX_OK;
}

static char *
ngx_http_auto_complete_set_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                  shm_name, s, *value;
    ngx_http_core_loc_conf_t  *clcf;
    u_char                    *p;
    size_t                     shm_size, i; 

    shm_size = 0;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_auto_complete_handler;

    value = cf->args->elts;

    ngx_http_auto_complete_dict_path = (char *)ngx_pcalloc(cf->pool, value[1].len + 1);
    ngx_memcpy(ngx_http_auto_complete_dict_path, value[1].data, value[1].len);
    ngx_http_auto_complete_dict_path[value[1].len + 1] = '\0';

    if (access(ngx_http_auto_complete_dict_path, F_OK) != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s can't be access", ngx_http_auto_complete_dict_path);
        return NGX_CONF_ERROR;
    }

    for (i = 2; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "shm_zone=", 9) == 0) {
            shm_name.data = value[i].data + 9;

            p = (u_char *) ngx_strchr(shm_name.data, ':');

            if (p) {
                *p = '\0';

                shm_name.len = p - shm_name.data;

                p++;

                s.len = value[i].data + value[i].len - p;
                s.data = p;

                shm_size = ngx_parse_size(&s);

                shm_size = ngx_align(shm_size, ngx_pagesize);

                if (shm_size < 8 * (size_t)ngx_pagesize) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "shm_zone size must be at least %uKB", (8 * ngx_pagesize) >> 10);
                    return NGX_CONF_ERROR;
                }
            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid shm_zone size");
                return NGX_CONF_ERROR;
            }
        }
    }

    if (ngx_http_auto_complete_shm_size && ngx_http_auto_complete_shm_size != shm_size) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 1, "cannot change memory area size without restart, ignoring change");
    } else {
        ngx_http_auto_complete_shm_size = shm_size;
    }

    ngx_http_auto_complete_shm_zone = ngx_shared_memory_add(cf, &shm_name, ngx_http_auto_complete_shm_size, &ngx_http_auto_complete_module);
    if (!ngx_http_auto_complete_shm_zone) {
        return NGX_CONF_ERROR;
    }

    ngx_http_auto_complete_shm_zone->init = ngx_http_auto_complete_init_shm_zone;

    return NGX_CONF_OK;
}

static ngx_int_t 
ngx_http_auto_complete_init_module_handler(ngx_cycle_t *cycle)
{
    if (ngx_http_auto_complete_tst) {
        ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)ngx_http_auto_complete_shm_zone->shm.addr;

        if (ngx_http_auto_complete_tst->root) {
            ngx_shmtx_lock(&shpool->mutex);
            tst_destroy(ngx_http_auto_complete_tst->root, ngx_http_auto_complete_shm_zone);
            ngx_http_auto_complete_tst->root = NULL;
            ngx_shmtx_unlock(&shpool->mutex);

            ngx_http_auto_complete_init_tst(ngx_http_auto_complete_shm_zone);
        }

        if (ngx_http_auto_complete_tst->cache_root) {
            ngx_shmtx_lock(&shpool->mutex);
            tst_cache_destroy(ngx_http_auto_complete_tst->cache_root, ngx_http_auto_complete_shm_zone);
            ngx_http_auto_complete_tst->cache_root = NULL;
            ngx_shmtx_unlock(&shpool->mutex);
        }

        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "Yeah! auto complete tst reload finished!");
    } else if (ngx_http_auto_complete_shm_zone != NULL) {
        ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)ngx_http_auto_complete_shm_zone->shm.addr;
        ngx_http_auto_complete_tst = (ngx_http_auto_complete_tst_t *)ngx_slab_alloc(shpool, sizeof(ngx_http_auto_complete_tst_t));
        ngx_http_auto_complete_init_tst(ngx_http_auto_complete_shm_zone);
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "Yeah! auto complete tst init finished!");
    }


    return NGX_OK;
}

static inline char *
ngx_http_auto_complete_str_find_space(char *p)
{
    char   *last;

    last = strchr(p, ' ');
    if (last) {
        last++;
        while (*last && *last == ' ') {
            last++;
        }
    }

    return last;
}

static inline char *
ngx_http_auto_complete_str_find_chs(char *p, size_t *l)
{
    char   *last = p + strlen(p);

    while (*p) {
        if (*p & 0x80) {
            if ((*p & 0xf8) == 0xf0 && last - p >= 4) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80 && (*(p + 3) & 0xc0) == 0x80) {
                    *l = 4;
                    break;
                }
            } else if ((*p & 0xf0) == 0xe0 && last - p >= 3) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80) {
                    *l = 3;
                    break;
                }
            } else if ((*p & 0xe0) == 0xc0 && last - p >= 2) {
                if ((*(p + 1) & 0xc0) == 0x80) {
                    *l = 2;
                    break;
                }
            } else {
                p = NULL;
                break;
            }
        } else {
            *l = 1;
            p++;
        }
    }

    if (*p == '\0') {
        p = NULL;
    }

    return p;
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

static inline void 
ngx_http_auto_complete_str_tolower(char *s)
{
    while (*s) {
        *s = ngx_tolower(*s); 
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
