/*
 * Copyright (C) Looyao
 */

#ifndef _TST_H_
#define _TST_H_

#include <sys/types.h>
#include <ngx_core.h>


typedef enum _tst_node_type tst_node_type;

enum _tst_node_type {
    tst_node_type_normal = 0,
    tst_node_type_end
};

typedef struct _tst_search_alias_node tst_search_alias_node;

struct _tst_search_alias_node {
    tst_search_alias_node  *next;
    char                   *word;
};

typedef struct _tst_node tst_node;

struct _tst_node {
    tst_node               *left;
    tst_node               *center;
    tst_node               *right;
    tst_search_alias_node  *alias;
    char                   *word;
	uint64_t                rank;
    /*tst_node_type           type;*/
    /*tst_node_type           alias_type;*/
    char                    c;
};

typedef struct _tst_cache_node tst_cache_node;

struct _tst_cache_node {
	tst_cache_node         *left;
    tst_cache_node         *center;
    tst_cache_node         *right;
	char                   *data;
	/*tst_node_type           type;*/
	time_t                  tm;
	char                    c;
};


typedef struct _tst_search_result_node tst_search_result_node;

struct _tst_search_result_node {
    tst_search_result_node *next;
    tst_search_result_node *prev;
    char                   *word;
    uint64_t                rank;
};

typedef struct _tst_search_result tst_search_result;

struct _tst_search_result {
    size_t                  count;
    tst_search_result_node *list;
    tst_search_result_node *tail;
};


tst_node *tst_insert(tst_node *root, char *word, ngx_shm_zone_t *shm_zone, ngx_log_t *log);

tst_node *tst_insert_alias(tst_node *root, char *word, char *alias, ngx_shm_zone_t *shm_zone, ngx_log_t *log);

void tst_traverse(tst_node *p, tst_search_result *result, ngx_pool_t *pool, ngx_log_t *log);

tst_search_result *tst_search(tst_node *root, char *word, ngx_pool_t *pool, ngx_log_t *log);

void tst_destroy(tst_node *p, ngx_shm_zone_t *shm_zone);

tst_search_result *tst_search_result_init(ngx_pool_t *pool, ngx_log_t *log);

/* tst cache */
tst_cache_node *tst_cache_insert(tst_cache_node *root, char *word, char *data, ngx_shm_zone_t *shm_zone, ngx_log_t *log);
char *tst_cache_search(tst_cache_node *p, char *pos);

#endif
