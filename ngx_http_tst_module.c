/*
* Copyright (C) Looyao
*/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <syslog.h>
#include <unistd.h>
#include <ctype.h>
#include "tst.h"

#define TST_MAX_RESULT_COUNT 50

typedef struct {
    ngx_str_t dict_path;
} ngx_http_tst_loc_conf_t;


static tst_node *tst;
static ngx_shm_zone_t *tst_shm_zone;
static char *tst_dict_file_path;

static char *ngx_http_tst_dict_path(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static void *ngx_http_tst_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_tst_merge_loc_conf(ngx_conf_t *cf, void *parent, 
    void *child);

static inline void json_escapes(char *dst, char *src);
static inline void str_tolower(char *s);
static inline void ngx_unescape_uri_patched(u_char **dst, u_char **src, size_t size, ngx_uint_t type);

static ngx_command_t ngx_http_tst_commands[] = {
    { ngx_string("tst_dict_path"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tst_dict_path,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tst_loc_conf_t, dict_path),
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_tst_module_ctx = {
    NULL,                              /* preconfiguration */
    NULL,                              /* postconfiguration */

    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */

    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */

    ngx_http_tst_create_loc_conf,      /* create location configuration */
    ngx_http_tst_merge_loc_conf,       /* merge location configuration */    
};

ngx_module_t ngx_http_tst_module = {
    NGX_MODULE_V1,
    &ngx_http_tst_module_ctx,          /* module context */
    ngx_http_tst_commands,             /* module directives */
    NGX_HTTP_MODULE,                   /* module type */
    NULL,                              /* init master */
    NULL,                              /* init module */
    NULL,                              /* init process */
    NULL,                              /* init thread */
    NULL,                              /* exit thread */
    NULL,                              /* exit process */
    NULL,                              /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t 
ngx_http_tst_handler(ngx_http_request_t *r)
{
    ngx_int_t               rc;
    ngx_buf_t              *b;
    ngx_chain_t             out;
    ngx_str_t               value;
    u_char                 *word, *cb, *dst, *src;
    char                   *out_data;
    size_t                  out_data_len, count;
    char                    escape_buf[512];
    tst_search_result_node *node;
    tst_search_result      *result;

    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (r->uri.data[r->uri.len - 1] == '/') {
        return NGX_DECLINED;
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

    out_data = "[]";
    out_data_len = 2;

    if (word) {
        str_tolower((char *) word);

        result = tst_search(tst, (char *) word, r->pool);

        if (result->count > 0) {
            count = 0;

            out_data = ngx_pcalloc(r->pool, result->count * 512 + 147);

            if (!out_data) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            if (cb && *cb) {
                out_data_len = snprintf(out_data, 141, "%s(\n{\"result\":[", cb);
            } else {
                *out_data = '[';
                out_data_len = 1;
            }

            node = result->list;
            while (node && count < TST_MAX_RESULT_COUNT) {
                json_escapes(escape_buf, node->word);
                out_data_len += snprintf(out_data + out_data_len, 512, "\"%s\",", escape_buf);

                node = node->next;
                count++;
            }

            *(out_data + out_data_len - 1) = ']';

            if (cb && *cb) {
                out_data_len += snprintf(out_data + out_data_len, 5, "}\n);");
            }
        }

        /*tst_search_result_free(result);*/
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = out_data_len;
    r->headers_out.content_type.data = (u_char *) "text/plain; charset=utf-8";
    r->headers_out.content_type.len = sizeof("text/plain; charset=utf-8") - 1;

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (!b) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;
    b->pos = (u_char *) out_data;
    b->last = (u_char *) (out_data + out_data_len);
    b->memory = 1;
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc != NGX_OK) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t
ngx_http_tst_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    FILE                      *fp;
    char                      *path, *split;
    char                      word_buf[512];

    fprintf(stderr, "%s\n", "shm init");
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    if (access(tst_dict_file_path, F_OK) != 0) {
        fprintf(stderr, "tst_dict_path can't be access\n");
        free(tst_dict_file_path);
        return NGX_ERROR;
    }

    fp = fopen(tst_dict_file_path, "r");

    while (fgets(word_buf, sizeof(word_buf), fp)) {
        size_t len = strlen(word_buf) - 1;
        if (word_buf[len] == '\n') {
            word_buf[len] = '\0';
        }

        split = strstr(word_buf, "||");
        if (!split) {
            tst = tst_insert(tst, word_buf, tst_shm_zone);
        } else {
            *split = 0;
            tst = tst_insert_alias(tst, word_buf, split + 2, tst_shm_zone);
        }
    }

    fclose(fp);

    return NGX_OK;
}

static char *
ngx_http_tst_dict_path(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    /*FILE                      *fp;
    char                      *path, *split;
    char                      word_buf[512];*/
    ngx_str_t                 *shm_name;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_http_tst_loc_conf_t   *tlcf;


    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_tst_handler;
    ngx_conf_set_str_slot(cf, cmd, conf);
    
    tlcf = conf;

    shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
    shm_name->len = sizeof("tst") - 1;
    shm_name->data = (unsigned char *) "tst";

    tst_shm_zone = ngx_shared_memory_add(cf, shm_name, 1024 * 32 * ngx_pagesize, &ngx_http_tst_module);
    if (!tst_shm_zone) {
        return NGX_CONF_ERROR;
    }

    tst_shm_zone->init = ngx_http_tst_init_shm_zone;

    // path = strndup((const char *) tlcf->dict_path.data, tlcf->dict_path.len);
    tst_dict_file_path = (char *)ngx_pcalloc(cf->pool, tlcf->dict_path.len + 1);
    snprintf(tst_dict_file_path, tlcf->dict_path.len + 1, "%s", tlcf->dict_path.data);
    /*if (access(path, F_OK) != 0) {
        fprintf(stderr, "tst_dict_path can't be access\n");
        free(path);
        return NGX_CONF_ERROR;
    }

    fp = fopen(path, "r");

    while (fgets(word_buf, sizeof(word_buf), fp)) {
        size_t len = strlen(word_buf) - 1;
        if (word_buf[len] == '\n') {
            word_buf[len] = '\0';
        }

        split = strstr(word_buf, "||");
        if (!split) {
            // tst = tst_insert(tst, word_buf, tst_shm_zone);
        } else {
            *split = 0;
            // tst = tst_insert_alias(tst, word_buf, split + 2, tst_shm_zone);
        }
    }

    fclose(fp);

    free(path);*/

    return NGX_CONF_OK;
}

static void *
ngx_http_tst_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_tst_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_tst_loc_conf_t));

    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->dict_path.len = 0;
    conf->dict_path.data = NULL;

    return conf;
}

static char *
ngx_http_tst_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_tst_loc_conf_t *prev = parent;
    ngx_http_tst_loc_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->dict_path, prev->dict_path, "");

    return NGX_CONF_OK;
}

static inline void json_escapes(char *dst, char *src)
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
            default:
                *(p++) = c;
                break;
        }
    }

    *p = 0;
}

static inline void str_tolower(char *s)
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