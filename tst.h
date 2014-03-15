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

typedef struct _tst_node tst_node;

struct _tst_node {
    tst_node               *left;
    tst_node               *center;
    tst_node               *right;
    tst_search_alias_node  *alias;
    char                   *word;
    uint32_t                rank;
    char                    c;
};


struct _tst_search_alias_node {
    tst_search_alias_node  *next;
    tst_node               *tnode;
};


typedef struct _tst_cache_node tst_cache_node;

struct _tst_cache_node {
    tst_cache_node         *left;
    tst_cache_node         *center;
    tst_cache_node         *right;
    char                   *data;
    /*time_t                  tm;*/
    char                    c;
};


typedef struct _tst_search_result_node tst_search_result_node;

struct _tst_search_result_node {
    tst_search_result_node *next;
    tst_search_result_node *prev;
    char                   *word;
    uint32_t                rank;
};

typedef struct _tst_search_result tst_search_result;

struct _tst_search_result {
    size_t                  count;
    tst_search_result_node *list;
    tst_search_result_node *tail;
};


tst_node *tst_insert(tst_node *root, char *c_word, char *word, uint32_t rank, tst_node **node, ngx_shm_zone_t *shm_zone, ngx_log_t *log);

tst_node *tst_insert_alias(tst_node *root, char *word, tst_node *alias, uint32_t rank, ngx_shm_zone_t *shm_zone, ngx_log_t *log);

void tst_traverse(tst_node *p, tst_search_result *result, ngx_pool_t *pool, ngx_log_t *log);

tst_search_result *tst_search(tst_node *root, char *word, ngx_pool_t *pool, ngx_log_t *log);

void tst_search_node(tst_node *p, char *pos, tst_node **node);

void tst_destroy(tst_node *p, ngx_shm_zone_t *shm_zone);

tst_search_result *tst_search_result_init(ngx_pool_t *pool, ngx_log_t *log);

void tst_search_result_sort(tst_search_result_node *left_node, tst_search_result_node *right_node);
void tst_search_result_uniq(tst_search_result_node *node);

/* tst cache */
tst_cache_node *tst_cache_insert(tst_cache_node *root, char *word, char *data, ngx_shm_zone_t *shm_zone, ngx_log_t *log);
char *tst_cache_search(tst_cache_node *p, char *pos);
void tst_cache_destroy(tst_cache_node *p, ngx_shm_zone_t *shm_zone);

#endif
