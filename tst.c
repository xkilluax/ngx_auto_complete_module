/*
 * Copyright (C) Looyao
 */

#include "tst.h"

#define TST_MAX_WORD_SIZE 256
#define TST_MAX_RANK 0xffffffffffffffff

static inline tst_node *tst_insert1(tst_node *p, char *word, char *pos, ngx_shm_zone_t *shm_zone, ngx_log_t *log);
static inline tst_node *tst_insert_alias1(tst_node *p, char *pos, char *alias, ngx_shm_zone_t *shm_zone, ngx_log_t *log);
static inline void tst_search1(tst_node *p, char *pos, tst_search_result *result, ngx_pool_t *pool, ngx_log_t *log);

static inline void tst_search_result_add(tst_search_result *result, char *word, uint64_t rank, ngx_pool_t *pool, ngx_log_t *log);
static inline void tst_search_result_sort(tst_search_result_node *left_node, tst_search_result_node *right_node);
static inline void tst_search_result_uniq(tst_search_result_node *node);

static inline tst_cache_node *tst_cache_insert1(tst_cache_node *p, char *pos, char *data, ngx_shm_zone_t *shm_zone, ngx_log_t *log);

tst_node *tst_insert(tst_node *root, char *word, ngx_shm_zone_t *shm_zone, ngx_log_t *log) 
{
    return tst_insert1(root, word, word, shm_zone, log);
}

tst_node *tst_insert_alias(tst_node *root, char *word, char *alias, ngx_shm_zone_t *shm_zone, ngx_log_t *log)
{
    return tst_insert_alias1(root, word, alias, shm_zone, log);    
}

void tst_traverse(tst_node *p, tst_search_result *result, ngx_pool_t *pool, ngx_log_t *log)
{
    tst_search_alias_node *anp;

    if (!p) {
        return; 
    }

    tst_traverse(p->left, result, pool, log); 

    /*if (p->type == tst_node_type_normal && p->alias_type == tst_node_type_normal) { */
    /*if (p->type == tst_node_type_normal && !p->alias) {*/
    if (!p->word && !p->alias) {
        tst_traverse(p->center, result, pool, log); 
    } else {
        if (result) {
            if (!p->alias) {
                tst_search_result_add(result, p->word, p->rank, pool, log);
            } else {
                /*if (p->type == tst_node_type_end) {*/
                if (p->word) {
                    tst_search_result_add(result, p->word, p->rank, pool, log);
                }

                anp = p->alias;
                while (anp) {
                    tst_search_result_add(result, anp->word, 0, pool, log);
                    anp = anp->next;
                }
            }
        }

        if (p->center != 0) {
            tst_traverse(p->center, result, pool, log); 
        }
    }

    tst_traverse(p->right, result, pool, log); 
}

tst_search_result *tst_search(tst_node *root, char *word, ngx_pool_t *pool, ngx_log_t *log)
{
    tst_search_result *result = tst_search_result_init(pool, log);
	if (!result) {
		return NULL;
	}

    tst_search1(root, word, result, pool, log);
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
        ngx_slab_free_locked(shpool, p->word);
    }

    np = p->alias;
    while (np) {
        tmp_np = np;

        np = np->next;

        ngx_slab_free_locked(shpool, tmp_np->word);
        ngx_slab_free_locked(shpool, tmp_np);
    }

    ngx_slab_free_locked(shpool, p);
}

tst_search_result *tst_search_result_init(ngx_pool_t *pool, ngx_log_t *log)
{
    tst_search_result *result = (tst_search_result *)ngx_pcalloc(pool, sizeof(tst_search_result));
    if (!result) {
		if (log) {
			ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: ngx_pcalloc error");
		}
        return NULL;
    }

    result->count = 0;
    result->list = NULL;
    result->tail = NULL;

    return result;
}

static inline tst_node *tst_insert1(tst_node *p, char *word, char *pos, ngx_shm_zone_t *shm_zone, ngx_log_t *log)
{
    ngx_slab_pool_t      *shpool;
    size_t                word_len;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        p = (tst_node *)ngx_slab_alloc_locked(shpool, sizeof(tst_node));

        if (!p) {
			if (log) {
				ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: out of shared memory");
			}
            return p;
        }

        p->c = *pos;
        p->left = 0;
        p->center = 0;
        p->right = 0;
        /*p->type = tst_node_type_normal;*/
        /*p->alias_type = tst_node_type_normal;*/
        p->alias = NULL;
        p->word = NULL;
    }
    
    if (*pos < p->c) {
        p->left = tst_insert1(p->left, word, pos, shm_zone, log);
    } else if (*pos > p->c) {
        p->right = tst_insert1(p->right, word, pos, shm_zone, log);
    } else {
        if (*(pos + 1) == 0) {
            /*p->type = tst_node_type_end;*/
            if (!p->word) {
                word_len = strlen(word);
                p->word = (char *)ngx_slab_alloc_locked(shpool, word_len + 1);
				if (p->word) {
                	snprintf(p->word, word_len + 1, "%s", word);
				}
            }
        } else {
            p->center = tst_insert1(p->center, word, ++pos, shm_zone, log);
        }
    }

    return p;
}

static inline tst_node *tst_insert_alias1(tst_node *p, char *pos, char *alias, ngx_shm_zone_t *shm_zone, ngx_log_t *log)
{
    ngx_slab_pool_t       *shpool;
    tst_search_alias_node *alias_node, *anp;
    size_t                 alias_len;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        p = (tst_node *)ngx_slab_alloc_locked(shpool, sizeof(tst_node));

        if (!p) {
            return p;
        }

        p->c = *pos;
        p->left = 0;
        p->center = 0;
        p->right = 0;
        /*p->type = tst_node_type_normal;*/
        /*p->alias_type = tst_node_type_normal;*/
        p->alias = NULL;
        p->word = NULL;
    }
    
    if (*pos < p->c) {
        p->left = tst_insert_alias1(p->left, pos, alias, shm_zone, log);
    } else if (*pos > p->c) {
        p->right = tst_insert_alias1(p->right, pos, alias, shm_zone, log);
    } else {
        if (*(pos + 1) == 0) {
            /*p->alias_type = tst_node_type_end;*/

            alias_node = (tst_search_alias_node *)ngx_slab_alloc_locked(shpool, sizeof(tst_search_result_node));

            if (!alias_node) {
				if (log) {
					ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: out of shared memory");
				}	
                return p;
            }

            alias_node->next = NULL;
            alias_len = strlen(alias);

            alias_node->word = (char *)ngx_slab_alloc_locked(shpool, alias_len + 1);

            if (!alias_node->word) {
				if (log) {
					ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: out of shared memory");
				}	
                return p;
            }

            ngx_snprintf(alias_node->word, alias_len + 1, "%s", alias);

            if (!p->alias) {
                p->alias = alias_node;
            } else {
                anp = p->alias;

                while (anp->next) {
                    if (strcmp(anp->word, alias) == 0) {
                        ngx_slab_free_locked(shpool, alias_node->word);
                        ngx_slab_free_locked(shpool, alias_node);
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
            p->center = tst_insert_alias1(p->center, ++pos, alias, shm_zone, log);
        }
    }

    return p;
}

static inline void tst_search1(tst_node *p, char *pos, tst_search_result *result, ngx_pool_t *pool, ngx_log_t *log)
{
    tst_search_alias_node *anp;

    if (!p) {
        return;
    }

    if (*pos < p->c) {
        tst_search1(p->left, pos, result, pool, log);
    } else if (*pos > p->c) {
        tst_search1(p->right, pos, result, pool, log);
    } else {
        if (*(pos + 1) == 0) {
            /*if (p->type == tst_node_type_end || p->alias_type == tst_node_type_end) {*/
            /*if (p->type == tst_node_type_end || p->alias) {*/
            if (p->word || p->alias) {
                /*if (p->type == tst_node_type_end) {*/
                if (p->word) {
					if (p->rank < TST_MAX_RANK) {
						p->rank++;
					}
                    tst_search_result_add(result, p->word, TST_MAX_RANK, pool, log);
                }

                if (p->alias) {
                    anp = p->alias;
                    while (anp) {
                        tst_search_result_add(result, anp->word, 0, pool, log);
                        anp = anp->next;
                    }
                }
            }

            tst_traverse(p->center, result, pool, log);
        } else {
            tst_search1(p->center, ++pos, result, pool, log);
        }
    }

}

static inline void tst_search_result_add(tst_search_result *result, char *word, uint64_t rank, ngx_pool_t *pool, ngx_log_t *log)
{
    tst_search_result_node *node = (tst_search_result_node *)ngx_pcalloc(pool, sizeof(tst_search_result_node));
    if (!node) {
		if (log) {
			ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: ngx_pcalloc error");
		}
        return;
    }

	if (!word) {
		return;
	}

    node->word = word;
	if (rank < TST_MAX_RANK) {
    	node->rank = 512 - strlen(word) + rank;
	} else {
		node->rank = TST_MAX_RANK;
	}

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
    size_t                 pivot_data_rank;
    tst_search_result_node *l_node;
    tst_search_result_node *r_node;

    pivot_data = left_node->word;
    pivot_data_rank = left_node->rank;

    l_node = left_node;
    r_node = right_node;

    while (l_node != r_node) {
        while (l_node != r_node && r_node->prev && r_node->rank <= pivot_data_rank) {
            r_node = r_node->prev;
        }
        if (l_node != r_node) {
            l_node->rank = r_node->rank;
            l_node->word = r_node->word;
        }

        while (l_node != r_node && l_node->next && l_node->rank > pivot_data_rank) {
            l_node = l_node->next;
        }
        if (l_node != r_node) {
            r_node->rank = l_node->rank;
            r_node->word = l_node->word;
        }
    }

    l_node->rank = pivot_data_rank;
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
        } else {
            s = node->word;
            node = node->next;
        }
    }
}


/*
 * cache
 */
tst_cache_node *tst_cache_insert(tst_cache_node *root, char *word, char *data, ngx_shm_zone_t *shm_zone, ngx_log_t *log)
{
	return tst_cache_insert1(root, word, data, shm_zone, log);
}

static inline tst_cache_node *tst_cache_insert1(tst_cache_node *p, char *pos, char *data, ngx_shm_zone_t *shm_zone, ngx_log_t *log)
{
	ngx_slab_pool_t       *shpool;
    size_t                 data_len;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        p = (tst_cache_node *)ngx_slab_alloc_locked(shpool, sizeof(tst_cache_node));

        if (!p) {
			if (log) {
				ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: out of shared memory");
			}	
            return p;
        }

        p->c = *pos;
        p->left = 0;
        p->center = 0;
        p->right = 0;
		/*p->type = tst_node_type_normal;*/
        p->data = NULL;
		p->tm = 0;
    }
    
    if (*pos < p->c) {
        p->left = tst_cache_insert1(p->left, pos, data, shm_zone, log);
    } else if (*pos > p->c) {
        p->right = tst_cache_insert1(p->right, pos, data, shm_zone, log);
    } else {
        if (*(pos + 1) == 0) {
			data_len = strlen(data);

			if (p->data && strlen(p->data) < data_len) {
				ngx_slab_free_locked(shpool, p->data);
				p->data = NULL;
			}

			if (!p->data) {
				/*p->type = tst_node_type_end;*/
				p->data = (char *)ngx_slab_alloc_locked(shpool, data_len + 1);
				if (!p->data) {
					if (log) {
						ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_auto_complete_module: out of shared memory");
					}
					return p;
				}
			}

			ngx_snprintf(p->data, data_len + 1, "%s", data);

			p->tm = ngx_time();
        } else {
            p->center = tst_cache_insert1(p->center, ++pos, data, shm_zone, log);
        }
    }

    return p;

}

char *tst_cache_search(tst_cache_node *p, char *pos)
{
    if (!p) {
        return NULL;
    }

    if (*pos < p->c) {
        tst_cache_search(p->left, pos);
    } else if (*pos > p->c) {
        tst_cache_search(p->right, pos);
    } else {
        if (*(pos + 1) == 0) {
            /*if (p->type == tst_node_type_end) {*/
            if (p->data) {
				if (ngx_time() - p->tm < 2) {
					return p->data;
				} else {
					return NULL;
				}
            }
        } else {
            tst_cache_search(p->center, ++pos);
        }
    }
}

void tst_cache_destroy(tst_cache_node *p, ngx_shm_zone_t *shm_zone)
{
    ngx_slab_pool_t *shpool;

    shpool = (ngx_slab_pool_t *)shm_zone->shm.addr;

    if (!p) {
        return;
    }

    tst_cache_destroy(p->left, shm_zone);
    tst_cache_destroy(p->center, shm_zone);
    tst_cache_destroy(p->right, shm_zone);

    if (p->data) {
        ngx_slab_free_locked(shpool, p->data);
    }

    ngx_slab_free_locked(shpool, p);
}
