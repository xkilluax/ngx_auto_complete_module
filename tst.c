/*
* Copyright (C) Looyao
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "tst.h"

#define TST_MAX_WORD_SIZE 256

static inline tst_node *tst_insert1(tst_node *p, char *word, char *pos, ngx_shm_zone_t *shm_zone);
static inline tst_node *tst_insert_alias1(tst_node *p, char *word, char *pos, char *alias, ngx_shm_zone_t *shm_zone);
static inline void tst_search1(tst_node *p, char *pos, tst_search_result *result, ngx_pool_t *pool);


static inline void tst_search_result_add(tst_search_result *result, char *word, size_t word_len, ngx_pool_t *pool);
static inline void tst_search_result_sort(tst_search_result_node *left_node, tst_search_result_node *right_node);
static inline void tst_search_result_uniq(tst_search_result_node *node);

tst_node *tst_insert(tst_node *root, char *word, ngx_shm_zone_t *shm_zone) 
{
    return tst_insert1(root, word, word, shm_zone);
}

tst_node *tst_insert_alias(tst_node *root, char *word, char *alias, ngx_shm_zone_t *shm_zone)
{
    return tst_insert_alias1(root, word, word, alias, shm_zone);    
}

void tst_traverse(tst_node *p, tst_search_result *result, ngx_pool_t *pool)
{
    tst_search_alias_node *anp;

    if (!p) {
        return; 
    }

    tst_traverse(p->left, result, pool); 

    if (p->type == tst_node_type_normal && p->alias_type == tst_node_type_normal) { 
        tst_traverse(p->center, result, pool); 
    } else {
        if (result) {
            if (!p->alias) {
                tst_search_result_add(result, p->word, p->pos, pool);
            } else {
                if (p->type == tst_node_type_end) {
                    tst_search_result_add(result, p->word, p->pos, pool);
                }

                anp = p->alias;
                while (anp) {
                    tst_search_result_add(result, anp->word, anp->word_len, pool);
                    anp = anp->next;
                }
            }
        }

        if (p->center != 0) {
            tst_traverse(p->center, result, pool); 
        }
    }

    tst_traverse(p->right, result, pool); 
}

tst_search_result *tst_search(tst_node *root, char *word, ngx_pool_t *pool)
{
    tst_search_result *result = tst_search_result_init(pool);

    tst_search1(root, word, result, pool);
    if (result->count > 1) {
        tst_search_result_sort(result->list, result->tail);
        tst_search_result_uniq(result->list);
    }

    return result;
}

void tst_destroy(tst_node *p, ngx_shm_zone_t *shm_zone)
{
    tst_search_alias_node *np, *tmp_np;
    ngx_slab_pool_t *shpool;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        return;
    }

    tst_destroy(p->left, shm_zone);
    tst_destroy(p->center, shm_zone);
    tst_destroy(p->right, shm_zone);

    if (p->word) {
        ngx_slab_free(shpool, p->word);
    }

    np = p->alias;
    while (np) {
        tmp_np = np;

        np = np->next;

        ngx_slab_free(shpool, tmp_np->word);
        ngx_slab_free(shpool, tmp_np);
    }

    ngx_slab_free(shpool, p);
}

tst_search_result *tst_search_result_init(ngx_pool_t *pool)
{
    tst_search_result *result = (tst_search_result *)ngx_pcalloc(pool, sizeof(tst_search_result));
    if (!result) {
        /* TODO: log error */
        return NULL;
    }

    result->count = 0;
    result->list = NULL;
    result->tail = NULL;

    return result;
}

/*void tst_search_result_free(tst_search_result *result)
{
    tst_search_result_node *list = result->list;
    tst_search_result_node *node;
    
    while (list) {
        node = list;
        list = list->next;
        free(node);
    }

    free(result);
}*/

static inline tst_node *tst_insert1(tst_node *p, char *word, char *pos, ngx_shm_zone_t *shm_zone)
{
    ngx_slab_pool_t      *shpool;
    size_t                word_len;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        p = (tst_node *)ngx_slab_alloc(shpool, sizeof(tst_node));

        if (!p) {
            return p;
        }

        p->c = *pos;
        p->pos = pos - word;
        p->left = 0;
        p->center = 0;
        p->right = 0;
        p->type = tst_node_type_normal;
        p->alias_type = tst_node_type_normal;
        p->alias = NULL;
        p->word = NULL;
    }
    
    if (*pos < p->c) {
        p->left = tst_insert1(p->left, word, pos, shm_zone);
    } else if (*pos > p->c) {
        p->right = tst_insert1(p->right, word, pos, shm_zone);
    } else {
        if (*(pos + 1) == 0) {
            p->type = tst_node_type_end;
            if (!p->word) {
                word_len = strlen(word);
                p->word = (char *)ngx_slab_alloc(shpool, word_len + 1);
                snprintf(p->word, word_len + 1, "%s", word);
            }
        } else {
            p->center = tst_insert1(p->center, word, ++pos, shm_zone);
        }
    }

    return p;
}

static inline tst_node *tst_insert_alias1(tst_node *p, char *word, char *pos, char *alias, ngx_shm_zone_t *shm_zone)
{
    ngx_slab_pool_t       *shpool;
    tst_search_alias_node *alias_node, *anp;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        p = (tst_node *)ngx_slab_alloc(shpool, sizeof(tst_node));

        if (!p) {
            return p;
        }

        p->c = *pos;
        p->pos = pos - word;
        p->left = 0;
        p->center = 0;
        p->right = 0;
        p->type = tst_node_type_normal;
        p->alias_type = tst_node_type_normal;
        p->alias = NULL;
        p->word = NULL;
    }
    
    if (*pos < p->c) {
        p->left = tst_insert_alias1(p->left, word, pos, alias, shm_zone);
    } else if (*pos > p->c) {
        p->right = tst_insert_alias1(p->right, word, pos, alias, shm_zone);
    } else {
        if (*(pos + 1) == 0) {
            p->alias_type = tst_node_type_end;

            alias_node = (tst_search_alias_node *)ngx_slab_alloc(shpool, sizeof(tst_search_result_node));

            if (!alias_node) {
                //TODO: log error
                return p;
            }

            alias_node->next = NULL;
            // alias_node->word = strdup(alias);
            alias_node->word_len = strlen(alias);

            alias_node->word = (char *)ngx_slab_alloc(shpool, alias_node->word_len + 1);

            if (!alias_node->word) {
                //TODO: log error
                return p;
            }

            snprintf(alias_node->word, alias_node->word_len + 1, "%s", alias);

            if (!p->alias) {
                p->alias = alias_node;
            } else {
                anp = p->alias;

                while (anp->next) {
                    if (strcmp(anp->word, alias) == 0) {
                        ngx_slab_free(shpool, alias_node->word);
                        ngx_slab_free(shpool, alias_node);
                        alias_node = NULL;
                        break;
                    }

                    anp = anp->next;
                }

                if (alias_node) {
                    anp->next = alias_node;
                }
            }
        } else {
            p->center = tst_insert_alias1(p->center, word, ++pos, alias, shm_zone);
        }
    }

    return p;
}

static inline void tst_search1(tst_node *p, char *pos, tst_search_result *result, ngx_pool_t *pool)
{
    tst_search_alias_node *anp;

    if (!p) {
        return;
    }

    if (*pos < p->c) {
        tst_search1(p->left, pos, result, pool);
    } else if (*pos > p->c) {
        tst_search1(p->right, pos, result, pool);
    } else {
        if (*(pos + 1) == 0) {
            if (p->type == tst_node_type_end || p->alias_type == tst_node_type_end) {
                if (p->type == tst_node_type_end) {
                    tst_search_result_add(result, p->word, p->pos, pool);
                }

                if (p->alias) {
                    anp = p->alias;
                    while (anp) {
                        tst_search_result_add(result, anp->word, anp->word_len, pool);
                        anp = anp->next;
                    }
                }
            }

            tst_traverse(p->center, result, pool);
        } else {
            tst_search1(p->center, ++pos, result, pool);
        }
    }

}

static inline void tst_search_result_add(tst_search_result *result, char *word, size_t word_len, ngx_pool_t *pool)
{
    tst_search_result_node *node = (tst_search_result_node *)ngx_pcalloc(pool, sizeof(tst_search_result_node));
    if (!node) {
        //TODO: log error
        return;
    }

    node->word = word;
    node->word_len = word_len;
    node->next = NULL;
    node->prev = NULL;

    if (!result->tail) {
        result->list = node;
        result->tail = node;
    } else {
        node->prev = result->tail;
        result->tail->next = node;
        result->tail = node;
    }

    result->count++;
}

static inline void tst_search_result_sort(tst_search_result_node *left_node, tst_search_result_node *right_node)
{
    char                  *pivot_data;
    size_t                 pivot_data_len;
    tst_search_result_node *l_node;
    tst_search_result_node *r_node;

    pivot_data = left_node->word;
    pivot_data_len = left_node->word_len;

    l_node = left_node;
    r_node = right_node;

    while (l_node != r_node) {
        while (l_node != r_node && r_node->prev && r_node->word_len >= pivot_data_len) {
            r_node = r_node->prev;
        }
        if (l_node != r_node) {
            l_node->word_len = r_node->word_len;
            l_node->word = r_node->word;
        }

        while (l_node != r_node && l_node->next && l_node->word_len < pivot_data_len) {
            l_node = l_node->next;
        }
        if (l_node != r_node) {
            r_node->word_len = l_node->word_len;
            r_node->word = l_node->word;
        }
    }

    l_node->word_len = pivot_data_len;
    l_node->word = pivot_data;

    if (left_node != l_node) {
        tst_search_result_sort(left_node, l_node->prev);
    }

    if (right_node != l_node) {
        tst_search_result_sort(l_node->next, right_node);
    }
}

static inline void tst_search_result_uniq(tst_search_result_node *node)
{
    char                   *s;
    tst_search_result_node *prev, *next, *p;

    s = node->word;
    node = node->next;

    while (node) {
        if (strcmp(s, node->word) == 0) {
            prev = node->prev;
            next = node->next;

            prev->next = next;
            if (next) {
                next->prev = prev;
            }

            p = node;

            node = node->next;

            free(p);
        } else {
            s = node->word;
            node = node->next;
        }
    }
}