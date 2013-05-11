// This file is part of khash released under the MIT license.
// See the LICENSE file for more information.
// Copyright 2013 Cloudant, Inc <support@cloudant.com>

#include <assert.h>
#include <string.h>

#include "erl_nif.h"
#include "hash.h"


#define KHASH_VERSION 0


typedef struct
{
    ERL_NIF_TERM atom_ok;
    ERL_NIF_TERM atom_error;
    ERL_NIF_TERM atom_shared;
    ERL_NIF_TERM atom_value;
    ERL_NIF_TERM atom_not_found;
    ErlNifResourceType* res_hash;
} khash_priv;


typedef struct
{
    ErlNifEnv* env;
    ERL_NIF_TERM key;
    ERL_NIF_TERM val;
} khnode_t;


typedef struct
{
    int version;
    hash_t* h;
    ErlNifPid p;
    ErlNifMutex* l;
} khash_t;


// This is actually an internal Erlang VM function that
// we're being a bit hacky to get access to. There's a
// pending patch to expose this in the NIF API in newer
// Erlangs.
unsigned int make_hash2(ERL_NIF_TERM term);


static inline ERL_NIF_TERM
make_atom(ErlNifEnv* env, const char* name)
{
    ERL_NIF_TERM ret;
    if(enif_make_existing_atom(env, name, &ret, ERL_NIF_LATIN1)) {
        return ret;
    }
    return enif_make_atom(env, name);
}


static inline ERL_NIF_TERM
make_ok(ErlNifEnv* env, khash_priv* priv, ERL_NIF_TERM value)
{
    return enif_make_tuple2(env, priv->atom_ok, value);
}


static inline ERL_NIF_TERM
make_error(ErlNifEnv* env, khash_priv* priv, ERL_NIF_TERM reason)
{
    return enif_make_tuple2(env, priv->atom_error, reason);
}


static inline int
has_option(ErlNifEnv* env, ERL_NIF_TERM opts, ERL_NIF_TERM opt)
{
    ERL_NIF_TERM head;
    ERL_NIF_TERM tail = opts;
    const ERL_NIF_TERM* elems;
    int arity;

    if(!enif_is_list(env, opts)) {
        return 0;
    }

    while(enif_get_list_cell(env, tail, &head, &tail)) {
        if(enif_compare(head, opt) == 0) {
            return 1;
        }

        if(!enif_is_tuple(env, head)) {
            continue;
        }

        if(!enif_get_tuple(env, head, &arity, &elems)) {
            continue;
        }

        if(arity == 0) {
            continue;
        }

        if(enif_compare(elems[0], opt)) {
            return 1;
        }
    }

    return 0;
}


static inline int
lock_hash(ErlNifEnv* env, khash_t* khash)
{
    ErlNifPid pid;

    // shared hash
    if(khash->l != NULL) {
        enif_mutex_lock(khash->l);
        return 1;
    }

    // Else check that we're being called from the pid
    // that created the hash.
    enif_self(env, &pid);
    if(enif_compare(pid.pid, khash->p.pid) == 0) {
        return 1;
    }

    return 0;
}


static inline void
unlock_hash(ErlNifEnv* env, khash_t* khash)
{
    if(khash->l != NULL) {
        enif_mutex_unlock(khash->l);
    }
}


hnode_t*
khnode_alloc(void* ctx)
{
    hnode_t* ret = (hnode_t*) enif_alloc(sizeof(hnode_t));
    khnode_t* node = (khnode_t*) enif_alloc(sizeof(khnode_t));

    memset(ret, '\0', sizeof(hnode_t));
    memset(node, '\0', sizeof(khnode_t));

    node->env = enif_alloc_env();
    ret->hash_key = node;

   return ret;
}


void
khnode_free(hnode_t* obj, void* ctx)
{
    khnode_t* node = (khnode_t*) kl_hnode_getkey(obj);
    enif_free_env(node->env);
    enif_free(node);
    enif_free(obj);
    return;
}


int
khash_cmp_fun(const void* l, const void* r)
{
    khnode_t* left = (khnode_t*) l;
    khnode_t* right = (khnode_t*) r;
    int cmp = enif_compare(left->key, right->key);

    if(cmp < 0) {
        return -1;
    } else if(cmp == 0) {
        return 0;
    } else {
        return 1;
    }
}


hash_val_t
khash_hash_fun(const void* obj)
{
    khnode_t* node = (khnode_t*) obj;
    return (hash_val_t) make_hash2(node->key);
}


static inline khash_t*
khash_create_int(ErlNifEnv* env, khash_priv* priv, ERL_NIF_TERM opts)
{
    khash_t* khash = NULL;

    assert(priv != NULL && "missing private data member");

    khash = (khash_t*) enif_alloc_resource(priv->res_hash, sizeof(khash_t));
    memset(khash, '\0', sizeof(khash_t));
    khash->version = KHASH_VERSION;

    khash->h = kl_hash_create(HASHCOUNT_T_MAX, khash_cmp_fun, khash_hash_fun);

    if(khash->h == NULL ) {
        enif_release_resource(khash);
        return NULL;
    }

    kl_hash_set_allocator(khash->h, khnode_alloc, khnode_free, NULL);
    enif_self(env, &(khash->p));

    if(has_option(env, opts, priv->atom_shared)) {
        khash->l = enif_mutex_create(NULL);
    } else {
        khash->l = NULL;
    }

    return khash;
}


static ERL_NIF_TERM
khash_new(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    ERL_NIF_TERM ret;

    if(argc != 1) {
        return enif_make_badarg(env);
    }

    khash = khash_create_int(env, priv, argv[0]);
    if(khash == NULL) {
        return enif_make_badarg(env);
    }

    ret = enif_make_resource(env, khash);
    enif_release_resource(khash);

    return make_ok(env, priv, ret);
}


static void
khash_free(ErlNifEnv* env, void* obj)
{
    khash_t* khash = (khash_t*) obj;

    if(khash->h != NULL) {
        kl_hash_free_nodes(khash->h);
        kl_hash_destroy(khash->h);
    }

    if(khash->l != NULL) {
        enif_mutex_destroy(khash->l);
    }

    return;
}


static ERL_NIF_TERM
khash_to_list(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = (khash_priv*) enif_priv_data(env);
    ERL_NIF_TERM ret = enif_make_list(env, 0);
    khash_t* khash;
    hscan_t scan;
    hnode_t* entry;
    khnode_t* node;
    ERL_NIF_TERM key;
    ERL_NIF_TERM val;
    ERL_NIF_TERM tuple;

    if(argc != 1) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    kl_hash_scan_begin(&scan, khash->h);

    while((entry = kl_hash_scan_next(&scan)) != NULL) {
        node = (khnode_t*) kl_hnode_getkey(entry);
        key = enif_make_copy(env, node->key);
        val = enif_make_copy(env, node->val);
        tuple = enif_make_tuple2(env, key, val);
        ret = enif_make_list_cell(env, tuple, ret);
    }

    unlock_hash(env, khash);

    return ret;
}


static ERL_NIF_TERM
khash_clear(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;

    if(argc != 1) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    kl_hash_free_nodes(khash->h);

    unlock_hash(env, khash);

    return priv->atom_ok;
}


static inline hnode_t*
khash_lookup_int(ErlNifEnv* env, ERL_NIF_TERM key, khash_t* khash)
{
    khnode_t node;
    node.env = env;
    node.key = key;
    return kl_hash_lookup(khash->h, &node);
}


static ERL_NIF_TERM
khash_lookup(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    hnode_t* entry;
    khnode_t* node;
    ERL_NIF_TERM ret;

    if(argc != 2) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    entry = khash_lookup_int(env, argv[1], khash);
    if(entry == NULL) {
        ret = priv->atom_not_found;
    } else {
        node = (khnode_t*) kl_hnode_getkey(entry);
        ret = enif_make_copy(env, node->val);
        ret = enif_make_tuple2(env, priv->atom_value, ret);
    }

    unlock_hash(env, khash);

    return ret;
}


static ERL_NIF_TERM
khash_get(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    hnode_t* entry;
    khnode_t* node;
    ERL_NIF_TERM ret;

    if(argc != 3) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    entry = khash_lookup_int(env, argv[1], khash);
    if(entry == NULL) {
        ret = argv[2];
    } else {
        node = (khnode_t*) kl_hnode_getkey(entry);
        ret = enif_make_copy(env, node->val);
    }

    unlock_hash(env, khash);

    return ret;
}


static ERL_NIF_TERM
khash_put(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    hnode_t* entry;
    khnode_t* node;

    if(argc != 3) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    entry = khash_lookup_int(env, argv[1], khash);
    if(entry == NULL) {
        entry = khnode_alloc(NULL);
        node = (khnode_t*) kl_hnode_getkey(entry);
        node->key = enif_make_copy(node->env, argv[1]);
        node->val = enif_make_copy(node->env, argv[2]);
        kl_hash_insert(khash->h, entry, node);
    } else {
        node = (khnode_t*) kl_hnode_getkey(entry);
        enif_clear_env(node->env);
        node->key = enif_make_copy(node->env, argv[1]);
        node->val = enif_make_copy(node->env, argv[2]);
    }

    unlock_hash(env, khash);

    return priv->atom_ok;
}


static ERL_NIF_TERM
khash_del(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    hnode_t* entry;
    ERL_NIF_TERM ret;

    if(argc != 2) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    entry = khash_lookup_int(env, argv[1], khash);
    if(entry == NULL) {
        ret = priv->atom_not_found;
    } else {
        kl_hash_delete_free(khash->h, entry);
        ret = priv->atom_ok;
    }

    unlock_hash(env, khash);

    return ret;
}


static ERL_NIF_TERM
khash_size(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    khash_priv* priv = enif_priv_data(env);
    khash_t* khash;
    ERL_NIF_TERM ret;

    if(argc != 1) {
        return enif_make_badarg(env);
    }

    if(!enif_get_resource(env, argv[0], priv->res_hash, (void**) &khash)) {
        return enif_make_badarg(env);
    }

    if(!lock_hash(env, khash)) {
        return enif_make_badarg(env);
    }

    ret = enif_make_uint64(env, kl_hash_count(khash->h));

    unlock_hash(env, khash);

    return ret;
}


static int
load(ErlNifEnv* env, void** priv, ERL_NIF_TERM info)
{
    const char* mod = "khash";
    int flags = ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER;
    ErlNifResourceType* res;

    khash_priv* new_priv = (khash_priv*) enif_alloc(sizeof(khash_priv));
    if(new_priv == NULL) {
        return 1;
    }

    res = enif_open_resource_type(env, mod, mod, khash_free, flags, NULL);
    if(res == NULL) {
        return 1;
    }

    new_priv->res_hash = res;

    new_priv->atom_ok = make_atom(env, "ok");
    new_priv->atom_error = make_atom(env, "error");
    new_priv->atom_shared = make_atom(env, "shared");
    new_priv->atom_value = make_atom(env, "value");
    new_priv->atom_not_found = make_atom(env, "not_found");

    *priv = (void*) new_priv;

    return 0;
}


static int
reload(ErlNifEnv* env, void** priv, ERL_NIF_TERM info)
{
    return 0;
}


static int
upgrade(ErlNifEnv* env, void** priv, void** old_priv, ERL_NIF_TERM info)
{
    return load(env, priv, info);
}


static void
unload(ErlNifEnv* env, void* priv)
{
    enif_free(priv);
    return;
}


static ErlNifFunc funcs[] = {
    {"new", 1, khash_new},
    {"to_list", 1, khash_to_list},
    {"clear", 1, khash_clear},
    {"lookup", 2, khash_lookup},
    {"get", 3, khash_get},
    {"put", 3, khash_put},
    {"del", 2, khash_del},
    {"size", 1, khash_size},
};


ERL_NIF_INIT(khash, funcs, &load, &reload, &upgrade, &unload);