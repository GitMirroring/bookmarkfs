/**
 * bookmarkfs/src/backend_chromium.c
 *
 * Chromium backend for BookmarkFS.
 * ----
 *
 * Copyright (C) 2024  CismonX <admin@cismon.net>
 *
 * This file is part of BookmarkFS.
 *
 * BookmarkFS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BookmarkFS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BookmarkFS.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <iconv.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
#  include <nettle/base16.h>
#  include <nettle/md5.h>
#endif

#include "backend.h"
#include "backend_util.h"
#include "hash.h"
#include "hashmap.h"
#include "ioctl.h"
#include "json.h"
#include "lib.h"
#include "macros.h"
#include "prng.h"
#include "sandbox.h"
#include "uuid.h"
#include "version.h"
#include "watcher.h"
#include "xstd.h"

#if defined(SIZEOF_TIME_T) && (SIZEOF_TIME_T != 8)
#  error "64-bit time_t is required"
#endif

#define BACKEND_FILENAME_GUID  ( 1u << 16 )

// Chromium uses Windows FILETIME epoch instead of Unix epoch.
//
// See Chromium source code: /base/time/time.h
// (`base::Time::kTimeTToMicrosecondsOffset`)
#define EPOCH_DIFF  ( (time_t)((1970 - 1601) * 365 + 89) * 24 * 3600 )

#define BOOKMARKS_ROOT_ID  0

enum {
    ATTR_KEY_NULL,
    ATTR_KEY_DATE_ADDED,
    ATTR_KEY_GUID,
    ATTR_KEY_TITLE,
};

enum dirty_level {
    DIRTY_LEVEL_NONE,
    DIRTY_LEVEL_METADATA,
    DIRTY_LEVEL_DATA,
};

typedef int (node_iter_func) (
    void    *iter_data,
    json_t  *node,
    json_t  *children,
    void   **parent_data_ptr
);

struct assocmap_key {
    uint64_t    parent_id;
    char const *name;
    size_t      name_len;
};

struct node_entry {
    uint64_t id;
    uint64_t parent_id;
    uint8_t  guid[UUID_LEN];

    char const *name;
    size_t      name_len;

    json_t *node;
    json_t *children;
};

struct backend_ctx {
    struct watcher *watcher;
    struct hashmap *id_map;
    struct hashmap *assoc_map;
    struct hashmap *guid_map;

    json_t *store;
    json_t *checksum;
    json_t *roots;
    json_t *fake_root;

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    uint64_t         max_id;
    iconv_t          cd;
    enum dirty_level dirty;
#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

    int         dirfd;
    char const *name;

    uint32_t flags;
    uint32_t watcher_flags;
};

struct bookmark_gcookie {
    json_t *store;
    json_t *checksum;
};

struct bookmark_lcookie {
    json_t *children;
    size_t  idx;       // fsck only
};

struct build_node_ctx {
    uint8_t       guid[UUID_LEN];
    unsigned long hashcode;

    struct bookmarkfs_bookmark_stat *stat_buf;
};

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
struct chksum_iter_ctx {
    struct md5_ctx  mdctx;
    iconv_t         cd;
    char           *buf;
    size_t          buf_len;
};
#endif

struct idmap_iter_ctx {
    struct hashmap *id_map;
    struct hashmap *guid_map;
    struct hashmap *assoc_map;
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    uint64_t        max_id;
#endif
};

struct lookup_ctx {
    unsigned long hashcode;
    unsigned long entry_id;
    uint8_t       guid[UUID_LEN];
};

struct node_iter {
    json_t *children;
    size_t  idx;
    void   *parent_data;
};

struct parsed_mntopts {
    uint32_t watcher_flags;
    uint32_t other_flags;
};

struct parsed_mkfsopts {
    struct timespec btime;
};

// Forward declaration start
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
static int  build_node      (struct backend_ctx *, json_t *, char const **,
                             size_t, bool, struct build_node_ctx *);
static int  build_node_guid (json_t *, struct hashmap const *, uint8_t *,
                             unsigned long *);
static int  build_node_id   (json_t *, uint64_t *);
static int  chksum_iter_cb  (void *, json_t *, json_t *, void **);
static int  chksum_root     (struct backend_ctx *, char *);
static int  chksum_utf16    (struct chksum_iter_ctx *, char const *, size_t);
static int  fsck_apply      (struct backend_ctx *, uint64_t,
                             struct bookmarkfs_fsck_data const *,
                             bookmarkfs_bookmark_fsck_cb *, void *);
static int  init_iconv      (iconv_t *);
static int  node_mtime_now  (json_t *, json_t **);
static int  parse_mkfsopts  (struct bookmarkfs_conf_opt const *,
                             struct parsed_mkfsopts *);
static int  store_new       (struct timespec *, json_t **);
static int  store_save      (struct backend_ctx *);
static void update_guid     (struct node_entry *, struct hashmap *,
                             unsigned long, uint8_t *, unsigned long);
static int  update_node_ts  (json_t *, struct timespec *);
#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

static int  assocmap_comp (union hashmap_key, void const *);
static unsigned long
            assocmap_hash (void const *);
static int  build_maps    (struct backend_ctx *);
static int  build_ts      (struct timespec *, char *, size_t);
static int  build_tsnode  (struct timespec *, json_t **);
static void free_bgcookie (struct bookmark_gcookie *);
static void free_blcookie (struct bookmark_lcookie *);
static void free_entry_cb (void *, void *);
static void free_maps     (struct hashmap *, struct hashmap *,
                           struct hashmap *);
static int  fsck_next     (struct backend_ctx const *, uint64_t, json_t *,
                           size_t *, bookmarkfs_bookmark_fsck_cb *, void *);
static int  get_attr_type (char const *, uint32_t);
static int  get_attr_val  (json_t const *, char const *, uint32_t, json_t **);
static int  guidmap_comp  (union hashmap_key, void const *);
static unsigned long
            guidmap_hash  (void const *);
static unsigned long
            hash_assoc    (uint64_t, char const *, size_t);
static int  idmap_comp    (union hashmap_key, void const *);
static unsigned long
            idmap_hash    (void const *);
static int  init_watcher  (struct backend_ctx *);
static int  iter_node     (json_t *, struct node_iter *, size_t,
                           node_iter_func *, void *, void *);
static int  iter_roots    (json_t *, node_iter_func *, void *, void *);
static struct node_entry *
            lookup_assoc  (struct hashmap const *, uint64_t, char const *,
                           size_t, unsigned long *, unsigned long *);
static struct node_entry *
            lookup_guid   (struct hashmap const *, uint8_t const *,
                           unsigned long *, unsigned long *);
static struct node_entry *
            lookup_id     (struct hashmap const *, uint64_t, unsigned long *,
                           unsigned long *);
static int  lookup_name   (struct backend_ctx const *, uint64_t, char const *,
                           size_t, struct lookup_ctx *, struct node_entry **);
static int  maps_iter_cb  (void *, json_t *, json_t *, void **);
static int  parse_entry   (struct backend_ctx const *, json_t const *, off_t,
                           bool, struct bookmarkfs_bookmark_entry *);
static int  parse_id      (json_t const *, uint64_t *);
static int  parse_guid    (char const *, size_t, uint8_t *);
static int  parse_mntopts (struct bookmarkfs_conf_opt const *, uint32_t,
                           struct parsed_mntopts *);
static int  parse_stats   (struct node_entry const *,
                           struct bookmarkfs_bookmark_stat *);
static int  parse_ts      (char const *, size_t, struct timespec *);
static void print_help    (uint32_t);
static void print_version (void);
static int  store_load    (struct backend_ctx *);
// Forward declaration end

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE

static int
build_node (
    struct backend_ctx     *ctx,
    json_t                 *node,
    char const            **name,
    size_t                  name_len,
    bool                    is_dir,
    struct build_node_ctx  *bctx
) {
    json_t *type;
    if (is_dir) {
        json_object_sset_new(node, "children", json_array());
        type = json_sstring("folder");
    } else {
        json_object_sset_new(node, "url", json_sstring(""));
        type = json_sstring("url");
    }
    json_object_sset_new(node, "type", type);

    struct timespec ts = { .tv_nsec = UTIME_NOW };
    json_t *date_added = NULL;
    if (unlikely(0 != build_tsnode(&ts, &date_added))) {
        return -EIO;
    }
    json_object_sset_new(node, "date_added", date_added);
    json_object_sset_new(node, "date_last_used", json_sstring("0"));

    if (unlikely(0 != build_node_id(node, &ctx->max_id))) {
        return -EIO;
    }

    json_t *name_node = json_stringn(*name, name_len);
    if (name_node == NULL) {
        return -EPERM;
    }
    json_object_sset_new(node, "name", name_node);
    *name = json_string_value(name_node);

    if (ctx->flags & BACKEND_FILENAME_GUID) {
        // No need to validate.  Already done in lookup_name().
        json_object_sset_copy(node, "guid", name_node);
    } else {
        if (unlikely(0 != build_node_guid(node, ctx->guid_map, bctx->guid,
                        &bctx->hashcode))
        ) {
            return -EIO;
        }
    }

    if (bctx->stat_buf != NULL) {
        *bctx->stat_buf = (struct bookmarkfs_bookmark_stat) {
            .id        = ctx->max_id,
            .value_len = is_dir ? -1 : 0,
            .atime     = ts,
            .mtime     = ts,
        };
    }
    return 0;
}

static int
build_node_guid (
    json_t               *node,
    struct hashmap const *guid_map,
    uint8_t              *guid,
    unsigned long        *hashcode_ptr
) {
    do {
        uuid_generate_random(guid);

        union hashmap_key key      = { .ptr = guid };
        unsigned long     hashcode = hash_digest(guid, UUID_LEN);
        if (unlikely(NULL != hashmap_search(guid_map, key, hashcode, NULL))) {
            continue;
        }

        if (hashcode_ptr != NULL) {
            *hashcode_ptr = hashcode;
        }
    } while (0);

    char guid_str_buf[UUID_HEX_LEN];
    uuid_bin2hex(guid_str_buf, guid);

    json_t *guid_node = json_stringn_nocheck(guid_str_buf, UUID_HEX_LEN);
    json_object_sset_new(node, "guid", guid_node);
    return 0;
}

static int
build_node_id (
    json_t   *node,
    uint64_t *max_id_ptr
) {
    uint64_t id_val = *max_id_ptr;
    if (unlikely(++id_val > BOOKMARKFS_MAX_ID)) {
        return -1;
    }

    char id_buf[24];
    int nbytes = snprintf(id_buf, sizeof(id_buf), "%" PRIu64, id_val);
    if (unlikely(nbytes < 0 || (size_t)nbytes >= sizeof(id_buf))) {
        return -1;
    }
    json_object_sset_new(node, "id", json_stringn_nocheck(id_buf, nbytes));

    *max_id_ptr = id_val;
    return 0;
}

// See Chromium source code: /components/bookmarks/browser/bookmark_codec.cc
static int
chksum_iter_cb (
    void    *iter_data,
    json_t  *node,
    json_t  *children,
    void   **UNUSED_VAR(parent_data_ptr)
) {
    struct chksum_iter_ctx *ctx = iter_data;

    json_t const *id_node = json_object_sget(node, "id");
    char const   *id      = json_string_value(id_node);
    size_t        id_len  = json_string_length(id_node);
    debug_assert(id != NULL);
    md5_update(&ctx->mdctx, id_len, (uint8_t const *)id);

    json_t const *name_node = json_object_sget(node, "name");
    char const   *name      = json_string_value(name_node);
    size_t        name_len  = json_string_length(name_node);
    debug_assert(name != NULL);
    if (unlikely(0 != chksum_utf16(ctx, name, name_len))) {
        return -1;
    }

    if (children != NULL) {
        md5_update(&ctx->mdctx, strlen("folder"), (uint8_t const *)"folder");
    } else {
        md5_update(&ctx->mdctx, strlen("url"), (uint8_t const *)"url");

        json_t const *url_node = json_object_sget(node, "url");
        char const   *url      = json_string_value(url_node);
        size_t        url_len  = json_string_length(url_node);
        debug_assert(url != NULL);
        md5_update(&ctx->mdctx, url_len, (uint8_t const *)url);
    }
    return 0;
}

static int
chksum_root (
    struct backend_ctx *ctx,
    char               *buf
) {
    if (ctx->cd == (iconv_t)-1) {
        if (unlikely(0 != init_iconv(&ctx->cd))) {
            return -1;
        }
    }
    int status = -1;

    struct chksum_iter_ctx iter_ctx = {
        .cd = ctx->cd,
    };
    md5_init(&iter_ctx.mdctx);
    if (0 != iter_roots(ctx->roots, chksum_iter_cb, &iter_ctx, NULL)) {
        goto end;
    }

    uint8_t digest[MD5_DIGEST_SIZE];
    md5_digest(&iter_ctx.mdctx, MD5_DIGEST_SIZE, digest);

    base16_encode_update(buf, MD5_DIGEST_SIZE, digest);
    status = 0;

  end:
    free(iter_ctx.buf);
    return status;
}

static int
chksum_utf16 (
    struct chksum_iter_ctx *ctx,
    char const             *str,
    size_t                  str_len
) {
    // A UTF-16 string converted from UTF-8 is at most 2x the length.
    size_t buf_len = str_len * 2 + 1;
    if (ctx->buf_len < buf_len) {
        free(ctx->buf);
        ctx->buf     = xmalloc(buf_len);
        ctx->buf_len = buf_len;
    }

    // A sane iconv() implementation should not write to the input buffer.
    char *in_buf  = (char *)str;
    char *out_buf = ctx->buf;
    if ((size_t)-1 == iconv(ctx->cd, &in_buf, &str_len, &out_buf, &buf_len)) {
        log_printf("iconv(): %s", xstrerror(errno));
        return -1;
    }
    md5_update(&ctx->mdctx, out_buf - ctx->buf, (uint8_t const *)ctx->buf);
    return 0;
}

static int
fsck_apply (
    struct backend_ctx                *ctx,
    uint64_t                           parent_id,
    struct bookmarkfs_fsck_data const *fsck_data,
    bookmarkfs_bookmark_fsck_cb       *callback,
    void                              *user_data
) {
    uint64_t    id    = fsck_data->id;
    uint64_t    extra = 0;
    char const *name  = fsck_data->name;
    int         result;

    struct node_entry *entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (entry == NULL || entry->parent_id != parent_id) {
        return -ENOENT;
    }
    if (entry->name != NULL) {
        // Given ID refers to a valid entry.  No fix shall be performed.
        return 0;
    }

    size_t name_len = strnlen(name, sizeof(fsck_data->name));
    if (0 != validate_filename_fsck(name, name_len, &result, &extra)) {
        goto callback;
    }

    struct hashmap *assoc_map = ctx->assoc_map;
    union hashmap_key key_assoc = {
        .ptr = &(struct assocmap_key) {
            .parent_id = parent_id,
            .name      = name,
            .name_len  = name_len,
        },
    };
    unsigned long hashcode_assoc = hash_assoc(id, name, name_len);
    if (NULL != hashmap_search(assoc_map, key_assoc, hashcode_assoc, NULL)) {
        extra  = entry->id;
        result = BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE;
        goto callback;
    }

    json_t *name_node = json_stringn(name, name_len);
    if (name_node == NULL) {
        extra  = BOOKMARKFS_NAME_INVALID_REASON_NOTUTF8;
        result = BOOKMARKFS_FSCK_RESULT_NAME_INVALID;
        goto callback;
    }
    json_object_sset_new(entry->node, "name", name_node);
    ctx->dirty = DIRTY_LEVEL_DATA;

    entry->name     = json_string_value(name_node);
    entry->name_len = name_len;
    *hashmap_insert(assoc_map, key_assoc, hashcode_assoc) = entry;
    return 0;

  callback:
    return callback(user_data, result, id, extra, name);
}

static int
init_iconv (
    iconv_t *cd_ptr
) {
    iconv_t cd = iconv_open("UTF-16LE", "UTF-8");
    if (unlikely(cd == (iconv_t)-1)) {
        log_printf("iconv_open(): %s", xstrerror(errno));
        return -1;
    }
    *cd_ptr = cd;
    return 0;
}

static int
node_mtime_now (
    json_t  *node,
    json_t **tsnode_ptr
) {
    struct timespec ts = { .tv_nsec = UTIME_NOW };

    json_t *tsnode = NULL;
    if (unlikely(0 != build_tsnode(&ts, &tsnode))) {
        return -1;
    }
    json_object_sset_new(node, "date_modified", tsnode);

    if (tsnode_ptr != NULL) {
        *tsnode_ptr = tsnode;
    }
    return 0;
}

static int
parse_mkfsopts (
    struct bookmarkfs_conf_opt const *opts,
    struct parsed_mkfsopts           *parsed_opts
) {
    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("date_added") {
        BACKEND_OPT_VAL_START
        char const *val = BACKEND_OPT_VAL_STR;
        if (0 != parse_ts(val, strlen(val), &parsed_opts->btime)) {
            return BACKEND_OPT_BAD_VAL();
        }
    }
    BACKEND_OPT_END

    return 0;
}

static int
store_new (
    struct timespec  *btime,
    json_t          **store_ptr
) {
    json_t *date_added = NULL;
    if (unlikely(0 != build_tsnode(btime, &date_added))) {
        return -1;
    }
    json_t *children = json_array();
    json_t *type     = json_sstring("folder");
    json_t *zero_str = json_sstring("0");

    json_t *roots = json_object();
#define INIT_ROOT_NODE(roots, key, id, name, guid)                \
    {                                                             \
        json_t *root_ = json_object();                            \
        json_object_sset_new(root_, "id",   json_sstring(id));    \
        json_object_sset_new(root_, "guid", json_sstring(guid));  \
        json_object_sset_new(root_, "name", json_sstring(name));  \
        json_object_sset(root_, "children",       children);      \
        json_object_sset(root_, "date_added",     date_added);    \
        json_object_sset(root_, "date_last_used", zero_str);      \
        json_object_sset(root_, "date_modified",  zero_str);      \
        json_object_sset(root_, "type",           type);          \
        json_object_sset_new(roots, key, root_);                  \
    }
    INIT_ROOT_NODE(roots, "bookmark_bar", "1", "Bookmarks bar",
            "0bc5d13f-2cba-5d74-951f-3f233fe6c908");
    INIT_ROOT_NODE(roots, "other",        "2", "Other bookmarks",
            "82b081ec-3dd3-529c-8475-ab6c344590dd");
    INIT_ROOT_NODE(roots, "synced",       "3", "Mobile bookmarks",
            "4cf2e351-0e85-532b-bb37-df045d8f8d0f");
#undef INIT_ROOT_NODE

    json_t *store = json_object();
    json_object_sset_new(store, "checksum",
            json_sstring("1e54fbb25d92a354f7aeaf576726429e"));
    json_object_sset_new(store, "roots",   roots);
    json_object_sset_new(store, "version", json_integer(1));

    json_decref(date_added);
    json_decref(children);
    json_decref(type);
    json_decref(zero_str);

    *store_ptr = store;
    return 0;
}

static int
store_save (
    struct backend_ctx *ctx
) {
    if (ctx->dirty == DIRTY_LEVEL_NONE) {
        return 0;
    }

    if (ctx->dirty > DIRTY_LEVEL_METADATA) {
        char buf[MD5_DIGEST_SIZE * 2];
        if (unlikely(0 != chksum_root(ctx, buf))) {
            return -1;
        }

        json_t *checksum = json_stringn_nocheck(buf, MD5_DIGEST_SIZE * 2);
        json_object_sset_new(ctx->store, "checksum", checksum);
    }

    size_t flags = JSON_COMPACT;
    if (0 != json_dump_file_at(ctx->store, ctx->dirfd, ctx->name, flags)) {
        return -1;
    }

    watcher_poll(ctx->watcher);
    ctx->dirty = DIRTY_LEVEL_NONE;
    return 0;
}

static void
update_guid (
    struct node_entry *entry,
    struct hashmap    *guid_map,
    unsigned long      old_entry_id,
    uint8_t           *guid,
    unsigned long      hashcode
) {
    hashmap_entry_delete(guid_map, entry, old_entry_id);

    union hashmap_key key = { .ptr = guid };
    *hashmap_insert(guid_map, key, hashcode) = entry;

    memcpy(entry->guid, guid, UUID_LEN);
}

static int
update_node_ts (
    json_t          *node,
    struct timespec *times
) {
    json_t *ts_node = json_object_sget(node, "date_last_used");
    if (unlikely(0 != build_tsnode(&times[0], &ts_node))) {
        return -1;
    }

    ts_node = json_object_sget(node, "date_modified");
    if (ts_node == NULL) {
        ts_node = json_object_sget(node, "date_added");
        if (unlikely(ts_node == NULL)) {
            return -1;
        }
        json_object_sset_copy(node, "date_modified", ts_node);
    }
    if (unlikely(0 != build_tsnode(&times[1], &ts_node))) {
        return -1;
    }

    return 0;
}

#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

static int
assocmap_comp (
    union hashmap_key  key,
    void const        *entry
) {
    struct assocmap_key const *k = key.ptr;
    struct node_entry const   *e = entry;

    if (k->parent_id != e->parent_id) {
        return -1;
    }
    if (k->name_len != e->name_len) {
        return -1;
    }
    return memcmp(k->name, e->name, e->name_len);
}

static unsigned long
assocmap_hash (
    void const *entry
) {
    struct node_entry const *e = entry;

    return hash_assoc(e->parent_id, e->name, e->name_len);
}

static int
build_maps (
    struct backend_ctx *ctx
) {
    json_t *ts_node = NULL;
    struct timespec now = { .tv_nsec = UTIME_NOW };
    if (unlikely(0 != build_tsnode(&now, &ts_node))) {
        return -1;
    }
    json_t *root     = json_object();
    json_t *children = json_array();
    json_object_sset_new(root, "children", children);
    json_object_sset_new(root, "date_added", ts_node);
    json_object_sset_copy(root, "date_last_used", ts_node);
    json_object_sset_copy(root, "date_modified", ts_node);

    struct hashmap *id_map    = hashmap_create(idmap_comp, idmap_hash);
    struct hashmap *guid_map  = hashmap_create(guidmap_comp, guidmap_hash);
    struct hashmap *assoc_map = NULL;
    if (!(ctx->flags & BACKEND_FILENAME_GUID)) {
        assoc_map = hashmap_create(assocmap_comp, assocmap_hash);
    }

    uint64_t root_id = BOOKMARKS_ROOT_ID;
    struct node_entry *root_entry = xmalloc(sizeof(*root_entry));
    *root_entry = (struct node_entry) {
        .id        = root_id,
        .parent_id = root_id,
        .guid = {
            // `bookmarks::kRootNodeUuid`
            0x25, 0x09, 0xa7, 0xdc, 0x21, 0x5d, 0x52, 0xf7,
            0xa4, 0x29, 0x8d, 0x80, 0x43, 0x1c, 0x6c, 0x75,
        },
        .name     = "",
        .node     = root,
        .children = children,
    };
    union hashmap_key key_id      = { .u64 = root_id };
    unsigned long     hashcode_id = hash_digest(&root_id, sizeof(root_id));
    *hashmap_insert(id_map, key_id, hashcode_id) = root_entry;

    union hashmap_key key_guid      = { .ptr = root_entry->guid };
    unsigned long     hashcode_guid = hash_digest(root_entry->guid, UUID_LEN);
    *hashmap_insert(guid_map, key_guid, hashcode_guid) = root_entry;

    struct idmap_iter_ctx iter_ctx = {
        .id_map    = id_map,
        .guid_map  = guid_map,
        .assoc_map = assoc_map,
    };
    if (0 != iter_roots(ctx->roots, maps_iter_cb, &iter_ctx, root_entry)) {
        goto fail;
    }

    free_maps(ctx->id_map, ctx->assoc_map, ctx->guid_map);
    ctx->id_map    = id_map;
    ctx->guid_map  = guid_map;
    ctx->assoc_map = assoc_map;
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    ctx->max_id    = iter_ctx.max_id;
#endif
    json_decref(ctx->fake_root);
    ctx->fake_root = root;
    return 0;

  fail:
    json_decref(root);
    free_maps(id_map, assoc_map, guid_map);
    return -1;
}

static int
build_ts (
    struct timespec *ts,
    char            *buf,
    size_t           buf_len
) {
    if (ts->tv_nsec == UTIME_OMIT) {
        return 0;
    }
    if (ts->tv_nsec == UTIME_NOW) {
        if (unlikely(0 != clock_gettime(CLOCK_REALTIME, ts))) {
            log_printf("clock_gettime(): %s", xstrerror(errno));
            return -1;
        }
    }

    // XXX: May overflow if system time is badly wrong,
    // but don't bother to check.
    time_t secs  = ts->tv_sec + EPOCH_DIFF;
    long   nsecs = ts->tv_nsec;

    long microsecs = nsecs / 1000;
    if (unlikely(microsecs >= 1000000)) {
        secs      += microsecs / 1000000;
        microsecs %= 1000000;
    }

    int nbytes = snprintf(buf, buf_len, "%" PRIuMAX "%06ld",
            (uintmax_t)secs, microsecs);
    if (unlikely(nbytes < 0) || unlikely((size_t)nbytes >= buf_len)) {
        return -1;
    }
    return nbytes;
}

static int
build_tsnode (
    struct timespec  *ts,
    json_t          **node_ptr
) {
    char buf[32];
    int nbytes = build_ts(ts, buf, 32);
    if (unlikely(nbytes < 0)) {
        return -1;
    }
    if (nbytes == 0) {
        // UTIME_OMIT
        return 0;
    }

    json_t *node = *node_ptr;
    if (node == NULL) {
        *node_ptr = json_stringn_nocheck(buf, nbytes);
    } else {
        json_string_setn_nocheck(node, buf, nbytes);
    }
    return 0;
}

static void
free_bgcookie (
    struct bookmark_gcookie *cookie
) {
    json_decref(cookie->checksum);
    free(cookie);
}

static void
free_blcookie (
    struct bookmark_lcookie *cookie
) {
    json_decref(cookie->children);
    free(cookie);
}

static void
free_entry_cb (
    void *entry,
    void *UNUSED_VAR(user_data)
) {
    free(entry);
}

static void
free_maps (
    struct hashmap *id_map,
    struct hashmap *assoc_map,
    struct hashmap *guid_map
) {
    if (id_map != NULL) {
        hashmap_foreach(id_map, free_entry_cb, NULL);
    }
    hashmap_destroy(id_map);
    hashmap_destroy(assoc_map);
    hashmap_destroy(guid_map);
}

static int
fsck_next (
    struct backend_ctx const    *ctx,
    uint64_t                     parent_id,
    json_t                      *children,
    size_t                      *idx_ptr,
    bookmarkfs_bookmark_fsck_cb *callback,
    void                        *user_data
) {
    int    status = 0;
    size_t idx    = *idx_ptr;
    do {
        json_t const *child = json_array_get(children, idx++);
        if (child == NULL) {
            break;
        }

        uint64_t id;
        debug_assert(0 == parse_id(child, &id));
        struct node_entry const *entry
            = lookup_id(ctx->id_map, id, NULL, NULL);
        // Since `children` could be a copy, the corresponding entry
        // may already be gone.
        if (entry == NULL || entry->name != NULL) {
            continue;
        }

        json_t const *name_node = json_object_sget(child, "name");
        char const   *name      = json_string_value(name_node);
        size_t        name_len  = json_string_length(name_node);
        debug_assert(name != NULL);

        uint64_t extra;
        int      result;
        if (0 == validate_filename_fsck(name, name_len, &result, &extra)) {
            entry = lookup_assoc(ctx->assoc_map, parent_id, name, name_len,
                    NULL, NULL);
            extra = BOOKMARKS_ROOT_ID;
            // The duplicate entry may already be renamed.
            if (entry != NULL) {
                extra = entry->id;
            }
            result = BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE;
        }
        status = callback(user_data, result, id, extra, name);
    } while (status == 0);

    *idx_ptr = idx;
    return status;
}

static int
get_attr_type (
    char const *key,
    uint32_t    flags
) {
    if (key == NULL) {
        return ATTR_KEY_NULL;
    }
    if (0 == strcmp("date_added", key)) {
        return ATTR_KEY_DATE_ADDED;
    }
    if (flags & BACKEND_FILENAME_GUID) {
        if (0 == strcmp("title", key)) {
            return ATTR_KEY_TITLE;
        }
    } else {
        if (0 == strcmp("guid", key)) {
            return ATTR_KEY_GUID;
        }
    }
    return -1;
}

static int
get_attr_val (
    json_t const  *node,
    char const    *attr_key,
    uint32_t       flags,
    json_t       **value_ptr
) {
    json_t *value;

    int key_type = get_attr_type(attr_key, flags);
    switch (key_type) {
      case ATTR_KEY_NULL:
        value = json_object_sget(node, "url");
        break;

      case ATTR_KEY_DATE_ADDED:
        value = json_object_sget(node, "date_added");
        break;

      case ATTR_KEY_GUID:
        value = json_object_sget(node, "guid");
        break;

      case ATTR_KEY_TITLE:
        value = json_object_sget(node, "name");
        break;

      default:
        return -ENOATTR;
    }
    if (value == NULL) {
        if (unlikely(attr_key == NULL)) {
            return -EIO;
        }
        return -EISDIR;
    }
    *value_ptr = value;
    return key_type;
}

static int
guidmap_comp (
    union hashmap_key  key,
    void const        *entry
) {
    struct node_entry const *e = entry;

    return memcmp(key.ptr, e->guid, UUID_LEN);
}

static unsigned long
guidmap_hash (
    void const *entry
) {
    struct node_entry const *e = entry;

    return hash_digest(e->guid, UUID_LEN);
}

static unsigned long
hash_assoc (
    uint64_t    parent_id,
    char const *name,
    size_t      name_len
) {
    struct iovec const bufv[] = {
        { .iov_base = &parent_id,   .iov_len = sizeof(parent_id) },
        { .iov_base = (char *)name, .iov_len = name_len          },
    };
    return hash_digestv(bufv, 2);
}

static int
idmap_comp (
    union hashmap_key  key,
    void const        *entry
) {
    struct node_entry const *e = entry;

    if (key.u64 != e->id) {
        return -1;
    }
    return 0;
}

static unsigned long
idmap_hash (
    void const *entry
) {
    struct node_entry const *e = entry;

    return hash_digest(&e->id, sizeof(uint64_t));
}

static int
init_watcher (
    struct backend_ctx *ctx
) {
    sigset_t old, to_block;
    sigemptyset(&to_block);
    // Block these signals for the worker thread, otherwise libfuse
    // session loop may not be promptly terminated upon signal receipt.
    sigaddset(&to_block, SIGHUP);
    sigaddset(&to_block, SIGINT);
    sigaddset(&to_block, SIGTERM);
    xassert(0 == pthread_sigmask(SIG_BLOCK, &to_block, &old));

    ctx->watcher = watcher_create(ctx->dirfd, ctx->name, ctx->watcher_flags);
    xassert(0 == pthread_sigmask(SIG_SETMASK, &old, NULL));
    if (ctx->watcher == NULL) {
        return -1;
    }
    return 0;
}

static int
iter_node (
    json_t           *node,
    struct node_iter *iter_stack,
    size_t            iter_stack_size,
    node_iter_func   *iter_cb,
    void             *iter_data,
    void             *parent_data
) {
    struct node_iter *iter = iter_stack;
    *iter = (struct node_iter) {
        .parent_data = parent_data,
    };
    goto get_children;

    while (1) {
        node = json_array_get(iter->children, iter->idx++);
        if (node == NULL) {
            if (iter == iter_stack) {
                break;
            }
            --iter;
            continue;
        }

      get_children:
        parent_data = iter->parent_data;
        json_t *children = json_object_sget(node, "children");
        if (0 != iter_cb(iter_data, node, children, &parent_data)) {
            return -1;
        }

        if (children == NULL) {
            continue;
        }
        if (++iter == iter_stack + iter_stack_size) {
            log_printf("bookmark directory is too deeply nested (>%zu)",
                    iter_stack_size);
            return -1;
        }
        *iter = (struct node_iter) {
            .children    = children,
            .parent_data = parent_data,
        };
    }
    return 0;
}

static int
iter_roots (
    json_t         *roots,
    node_iter_func *iter_cb,
    void           *iter_data,
    void           *parent_data
) {
#define MAX_NODE_ITER_DEPTH  128
    struct node_iter *iter_stack
        = xmalloc(sizeof(struct node_iter) * MAX_NODE_ITER_DEPTH);

    int status = -1;
    json_object_foreach_iter(roots, iter) {
        json_t *node = json_object_iter_value(iter);

        status = iter_node(node, iter_stack, MAX_NODE_ITER_DEPTH, iter_cb,
                    iter_data, parent_data);
        if (status != 0) {
            break;
        }
    }
    free(iter_stack);
    return status;
}

static struct node_entry *
lookup_assoc (
    struct hashmap const *assoc_map,
    uint64_t              parent_id,
    char const           *name,
    size_t                name_len,
    unsigned long        *hashcode_ptr,
    unsigned long        *entry_id_ptr
) {
    union hashmap_key key = {
        .ptr = &(struct assocmap_key) {
            .parent_id = parent_id,
            .name      = name,
            .name_len  = name_len,
        },
    };
    unsigned long hashcode = hash_assoc(parent_id, name, name_len);
    if (hashcode_ptr != NULL) {
        *hashcode_ptr = hashcode;
    }
    return hashmap_search(assoc_map, key, hashcode, entry_id_ptr);
}

static struct node_entry *
lookup_guid (
    struct hashmap const  *guid_map,
    uint8_t const         *guid,
    unsigned long         *hashcode_ptr,
    unsigned long         *entry_id_ptr
) {
    union hashmap_key key      = { .ptr = guid };
    unsigned long     hashcode = hash_digest(guid, UUID_LEN);
    if (hashcode_ptr != NULL) {
        *hashcode_ptr = hashcode;
    }

    return hashmap_search(guid_map, key, hashcode, entry_id_ptr);
}

static struct node_entry *
lookup_id (
    struct hashmap const *id_map,
    uint64_t              id,
    unsigned long        *hashcode_ptr,
    unsigned long        *entry_id_ptr
) {
    union hashmap_key key      = { .u64 = id };
    unsigned long     hashcode = hash_digest(&id, sizeof(id));
    if (hashcode_ptr != NULL) {
        *hashcode_ptr = hashcode;
    }

    return hashmap_search(id_map, key, hashcode, entry_id_ptr);
}

static int
lookup_name (
    struct backend_ctx const  *ctx,
    uint64_t                   parent_id,
    char const                *name,
    size_t                     name_len,
    struct lookup_ctx         *lctx,
    struct node_entry        **entry_ptr
) {
    unsigned long hashcode = 0;
    unsigned long entry_id = 0;
    struct node_entry *entry = NULL;

    int status = -1;
    if (ctx->flags & BACKEND_FILENAME_GUID) {
        uint8_t guid_buf[UUID_LEN];
        void *guid = guid_buf;
        if (lctx != NULL) {
            guid = lctx->guid;
        }

        if (unlikely(0 != parse_guid(name, name_len, guid))) {
            goto end;
        }
        entry = lookup_guid(ctx->guid_map, guid, &hashcode, &entry_id);
        if (entry == NULL) {
            goto end;
        }
        // An entry with the same GUID is found in another directory.
        if (entry->parent_id != parent_id) {
            goto end;
        }
    } else {
        entry = lookup_assoc(ctx->assoc_map, parent_id, name, name_len,
                &hashcode, &entry_id);
        if (entry == NULL) {
            goto end;
        }
    }
    status = 0;

  end:
    if (lctx != NULL) {
        lctx->hashcode = hashcode;
        lctx->entry_id = entry_id;
    }
    if (entry_ptr != NULL) {
        *entry_ptr = entry;
    }
    return status;
}

static int
maps_iter_cb (
    void    *iter_data,
    json_t  *node,
    json_t  *children,
    void   **parent_data_ptr
) {
    struct idmap_iter_ctx *ctx    = iter_data;
    struct node_entry     *parent = *parent_data_ptr;

    uint64_t id;
    if (unlikely(0 != parse_id(node, &id))) {
        return -1;
    }

    union hashmap_key key      = { .u64 = id };
    unsigned long     hashcode = hash_digest(&id, sizeof(id));
    if (unlikely(NULL != hashmap_search(ctx->id_map, key, hashcode, NULL))) {
        log_printf("duplicate bookmark ID %" PRIu64, id);
        return -1;
    }

    uint8_t     guid[UUID_LEN];
    json_t     *guid_node = json_object_sget(node, "guid");
    char const *guid_str  = json_string_value(guid_node);
    size_t      guid_len  = json_string_length(guid_node);
    if (unlikely(0 != parse_guid(guid_str, guid_len, guid))) {
        log_printf("bad bookmark GUID %s (ID: %" PRIu64 ")", guid_str, id);
        return -1;
    }
    union hashmap_key key_guid      = { .ptr = guid };
    unsigned long     hashcode_guid = hash_digest(guid, UUID_LEN);
    if (NULL != hashmap_search(ctx->guid_map, key_guid, hashcode_guid, NULL)) {
        log_printf("duplicate bookmark GUID %s (ID: %" PRIu64 ")",
                guid_str, id);
        return -1;
    }

    json_t     *name_node = json_object_sget(node, "name");
    char const *name      = json_string_value(name_node);
    size_t      name_len  = json_string_length(name_node);
    if (unlikely(name == NULL)) {
        log_printf("no name found for bookmark %" PRIu64, id);
        return -1;
    }

    struct node_entry *entry = xmalloc(sizeof(*entry));
    entry->id        = id;
    entry->parent_id = parent->id;
    entry->name      = NULL;
    entry->node      = node;
    entry->children  = children;
    memcpy(entry->guid, guid, UUID_LEN);
    *hashmap_insert(ctx->id_map,   key,      hashcode)      = entry;
    *hashmap_insert(ctx->guid_map, key_guid, hashcode_guid) = entry;

    // bookmark bar, mobile bookmarks, other bookmarks, ...
    if (parent->id == BOOKMARKS_ROOT_ID) {
        json_array_append(parent->children, node);
    }
    struct hashmap *assoc_map = ctx->assoc_map;
    // Do not check for name duplicates if using GUID as filename.
    if (assoc_map == NULL) {
        goto end;
    }
    // If the parent entry is ignored from assoc map,
    // all children should also be ignored.
    if (parent->name == NULL && parent->id != BOOKMARKS_ROOT_ID) {
        goto end;
    }

    if (0 != validate_filename(name, name_len, NULL)) {
        debug_printf("bad bookmark name '%s' (ID: %" PRIu64 ")", name, id);
        goto end;
    }
    union hashmap_key key_assoc = {
        .ptr = &(struct assocmap_key) {
            .parent_id = parent->id,
            .name      = name,
            .name_len  = name_len,
        },
    };
    unsigned long hashcode_assoc = hash_assoc(parent->id, name, name_len);
    if (NULL != hashmap_search(assoc_map, key_assoc, hashcode_assoc, NULL)) {
        debug_printf("duplicate bookmark name '%s' (ID: %" PRIu64 ")",
                name, id);
        goto end;
    }

    entry->name     = name;
    entry->name_len = name_len;
    *hashmap_insert(assoc_map, key_assoc, hashcode_assoc) = entry;

  end:
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    if (id > ctx->max_id) {
        ctx->max_id = id;
    }
#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */
    *parent_data_ptr = entry;
    return 0;
}

static int
parse_entry (
    struct backend_ctx const         *ctx,
    json_t const                     *node,
    off_t                             off,
    bool                              with_stat,
    struct bookmarkfs_bookmark_entry *buf
) {
    uint64_t id;
    debug_assert(0 == parse_id(node, &id));

    struct node_entry const *entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (entry == NULL || entry->name == NULL) {
        return -1;
    }

    buf->name    = entry->name;
    buf->off     = off;
    buf->stat.id = id;
    if (ctx->flags & BACKEND_FILENAME_GUID) {
        json_t const *guid_node = json_object_sget(entry->node, "guid");
        buf->name = json_string_value(guid_node);
        debug_assert(buf->name != NULL);
    }

    if (with_stat) {
        if (unlikely(0 != parse_stats(entry, &buf->stat))) {
            return -1;
        }
    } else {
        buf->stat.value_len = entry->children == NULL ? 0 : -1;
    }
    return 0;
}

static int
parse_id (
    json_t const *node,
    uint64_t     *id_val_ptr
) {
    json_t const *id_node = json_object_sget(node, "id");
    if (unlikely(0 == json_string_length(id_node))) {
        return -1;
    }

    char *end;
    uint64_t id_val = strtoull(json_string_value(id_node), &end, 10);
    if (unlikely(*end != '\0')) {
        return -1;
    }
    if (unlikely(id_val > BOOKMARKFS_MAX_ID)) {
        return -1;
    }

    if (id_val_ptr != NULL) {
        *id_val_ptr = id_val;
    }
    return 0;
}

static int
parse_guid (
    char const *str,
    size_t      str_len,
    uint8_t    *guid
) {
    if (str_len != UUID_HEX_LEN) {
        return -1;
    }

    return uuid_hex2bin(guid, str);
}

static int
parse_mntopts (
    struct bookmarkfs_conf_opt const *opts,
    uint32_t                          flags,
    struct parsed_mntopts            *parsed_opts
) {
    uint32_t watcher_flags = WATCHER_NOOP;
    uint32_t other_flags   = 0;
    if (flags & BOOKMARKFS_BACKEND_FSCK_ONLY) {
        goto end;
    }
    if (flags & BOOKMARKFS_BACKEND_READONLY) {
        watcher_flags = 0;
    }

    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("filename") {
        BACKEND_OPT_VAL_START
        BACKEND_OPT_VAL("guid") {
            other_flags |= BACKEND_FILENAME_GUID;
        }
        BACKEND_OPT_VAL("title") {
            other_flags &= ~BACKEND_FILENAME_GUID;
        }
        BACKEND_OPT_VAL_END
    }
    BACKEND_OPT_KEY("watcher") {
        BACKEND_OPT_VAL_START
        BACKEND_OPT_VAL("native") {
            watcher_flags = 0;
        }
        BACKEND_OPT_VAL("fallback") {
            watcher_flags = WATCHER_FALLBACK;
        }
        BACKEND_OPT_VAL("none") {
            watcher_flags = WATCHER_NOOP;
        }
        BACKEND_OPT_VAL_END
    }
    BACKEND_OPT_END

  end:
    parsed_opts->watcher_flags = watcher_flags;
    parsed_opts->other_flags   = other_flags;
    return 0;
}

static int
parse_stats (
    struct node_entry const         *entry,
    struct bookmarkfs_bookmark_stat *buf
) {
    json_t const *node = entry->node;

    json_t const *atime_node = json_object_sget(node, "date_last_used");
    char const *atime = json_string_value(atime_node);
    if (unlikely(atime == NULL)) {
        return -EIO;
    }
    size_t atime_len = json_string_length(atime_node);
    if (unlikely(0 != parse_ts(atime, atime_len, &buf->atime))) {
        return -EIO;
    }

    json_t const *mtime_node = json_object_sget(node, "date_modified");
    // If the bookmark has not been modified, this field does not exist.
    if (mtime_node == NULL) {
        mtime_node = json_object_sget(node, "date_added");
    }
    char const *mtime = json_string_value(mtime_node);
    if (unlikely(mtime == NULL)) {
        return -EIO;
    }
    size_t mtime_len = json_string_length(mtime_node);
    if (unlikely(0 != parse_ts(mtime, mtime_len, &buf->mtime))) {
        return -EIO;
    }

    if (entry->children == NULL) {
        json_t *url = json_object_sget(node, "url");
        buf->value_len = json_string_length(url);
    } else {
        buf->value_len = -1;
    }

    return 0;
}

static int
parse_ts (
    char const      *str,
    size_t           str_len,
    struct timespec *buf
) {
    time_t secs = 0;
    long   microsecs;

    char *end;
    if (likely(str_len > 6)) {
        str_len -= 6;
        if (unlikely(str_len > 15)) {
            return -1;
        }
        char tmp[16];
        memcpy(tmp, str, str_len);
        tmp[str_len] = '\0';
        secs = strtoll(tmp, &end, 10);

        if (*end != '\0' || secs < 0 || secs == LLONG_MAX) {
            return -1;
        }
        str += str_len;
    }
    microsecs = strtol(str, &end, 10);
    if (*end != '\0' || microsecs < 0 || microsecs == LONG_MAX) {
        return -1;
    }

    if (buf != NULL) {
        if (unlikely(secs < EPOCH_DIFF)) {
            // Stay away from negative tv_sec
            secs = EPOCH_DIFF;
        }
        buf->tv_sec  = secs - EPOCH_DIFF;
        buf->tv_nsec = microsecs * 1000;
    }
    return 0;
}

static void
print_help (
    uint32_t flags
) {
    char const *options = "";
    if (flags & BOOKMARKFS_FRONTEND_MOUNT) {
        options = "Options:\n"
            "  filename=<title/guid>             Bookmark file name origin\n"
            "  watcher=<native/fallback/none>    File watcher type\n"
            "\n";
    } else if (flags & BOOKMARKFS_FRONTEND_MKFS) {
        options = "Options:\n"
            "  date_added=<timestamp>    Override date_added attribute\n"
            "\n";
    }
    printf("Chromium backend for BookmarkFS\n\n"
            "%s"
            "Run 'info bookmarkfs' for more information.\n\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.\n", options);
}

static void
print_version (void)
{
    printf("bookmarkfs-backend-chromium %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG,                  "debug"));
    puts(BOOKMARKFS_FEATURE_STRING(BACKEND_CHROMIUM_WRITE, "write"));

    bookmarkfs_print_lib_version("\n");
}

static int
store_load (
    struct backend_ctx *ctx
) {
    struct watcher *watcher = ctx->watcher;
    if (unlikely(watcher == NULL)) {
        if (0 != init_watcher(ctx)) {
            return -1;
        }
        goto do_load;
    }

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    // Prioritize client-side modification to the store.
    // Changes made by other processes will be lost.
    if (ctx->dirty > DIRTY_LEVEL_NONE) {
        return 0;
    }
#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

    switch (watcher_poll(watcher)) {
      case WATCHER_POLL_ERR:
        return -1;

      case WATCHER_POLL_NOCHANGE:
        if (ctx->store == NULL) {
            break;
        }
        return 0;

      case WATCHER_POLL_CHANGED:
        json_decref(ctx->store);
        ctx->store = NULL;
        break;

      default:
        unreachable();
    }

  do_load:
    debug_puts("loading store");

    size_t flags = JSON_REJECT_DUPLICATES;
    json_t *store = json_load_file_at(ctx->dirfd, ctx->name, flags);
    if (store == NULL) {
        return -1;
    }

    json_t *checksum = json_object_sget(store, "checksum");
    if (unlikely(checksum == NULL)) {
        log_puts("bad bookmark store: no checksum");
        goto fail;
    }
    json_t *version = json_object_sget(store, "version");
    if (unlikely(1 != json_integer_value(version))) {
        log_puts("bad bookmark store: bad version");
        goto fail;
    }
    json_t *roots = json_object_sget(store, "roots");
    if (unlikely(roots == NULL)) {
        log_puts("bad bookmark store: no roots");
        goto fail;
    }

    ctx->roots = roots;
    if (0 != build_maps(ctx)) {
        goto fail;
    }
    ctx->store    = store;
    ctx->checksum = checksum;

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    if (!(ctx->flags & BOOKMARKFS_BACKEND_READONLY)) {
        ctx->dirty = DIRTY_LEVEL_NONE;
    }
#endif
    return 0;

  fail:
    json_decref(store);
    return -1;
}

static int
backend_create (
    struct bookmarkfs_backend_conf const  *conf,
    struct bookmarkfs_backend_create_resp *resp
) {
#ifndef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    if (!(conf->flags & BOOKMARKFS_READONLY)) {
        log_puts("write support is not enabled on this build");
        return -1;
    }
#endif

    struct parsed_mntopts opts = { 0 };
    if (0 != parse_mntopts(conf->opts, conf->flags, &opts)) {
        return -1;
    }

    char *name;
    int dirfd = basename_opendir(conf->store_path, &name);
    if (dirfd < 0) {
        return -1;
    }

    uint32_t sandbox_flags = 0;
#if defined(__linux__)
    if (conf->flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        sandbox_flags |= SANDBOX_NOOP;
    } else if (conf->flags & BOOKMARKFS_BACKEND_NO_LANDLOCK) {
        sandbox_flags |= SANDBOX_NO_LANDLOCK;
    }
#elif defined(__FreeBSD__)
    // Do not enable sandbox in the watcher, since cap_enter()
    // applies to the entire process, not just the current thread.
    // Let the main thread do the favor (after opening the FUSE device).
    sandbox_flags |= SANDBOX_NOOP;
#else
#  error "not implemented"
#endif  /* defined(__linux__) || defined(__FreeBSD__) */
    uint32_t watcher_flags
        = opts.watcher_flags | (sandbox_flags << WATCHER_SANDBOX_FLAGS_OFFSET);

    struct backend_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct backend_ctx) {
        .name  = name,
        .dirfd = dirfd,
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
        .cd = (iconv_t)-1,
#endif
        .flags         = conf->flags | opts.other_flags,
        .watcher_flags = watcher_flags,
    };

    uint32_t resp_flags = 0;
    if (watcher_flags & WATCHER_NOOP) {
        resp_flags |= BOOKMARKFS_BACKEND_EXCLUSIVE;
    }

    char const *bookmark_attrs = "guid\0date_added\0";
    if (opts.other_flags & BACKEND_FILENAME_GUID) {
        bookmark_attrs = "title\0date_added\0";
    }

    resp->name              = "chromium";
    resp->backend_ctx       = ctx;
    resp->bookmarks_root_id = BOOKMARKS_ROOT_ID;
    resp->flags             = resp_flags;
    resp->bookmark_attrs    = bookmark_attrs;
    return 0;
}

static void
backend_destroy (
    void *backend_ctx
) {
    struct backend_ctx *ctx = backend_ctx;
    if (ctx == NULL) {
        return;
    }

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    if (!(ctx->flags & BOOKMARKFS_BACKEND_READONLY)) {
        store_save(ctx);
    }
    if (ctx->cd != (iconv_t)-1) {
        iconv_close(ctx->cd);
    }
#endif

    watcher_destroy(ctx->watcher);
    free_maps(ctx->id_map, ctx->assoc_map, ctx->guid_map);
    json_decref(ctx->store);
    json_decref(ctx->fake_root);

    if (ctx->dirfd >= 0) {
        close(ctx->dirfd);
    }
    free(ctx);
}

static void
backend_info (
    uint32_t flags
) {
    if (flags & BOOKMARKFS_BACKEND_INFO_HELP) {
        print_help(flags);
    } else if (flags & BOOKMARKFS_BACKEND_INFO_VERSION) {
        print_version();
    }
}

static int
backend_init (
    uint32_t flags
) {
    if (!(flags & BOOKMARKFS_BACKEND_LIB_READY)) {
        if (0 != bookmarkfs_lib_init()) {
            return -1;
        }
    }

    // Using xmalloc() ensures that most json_*() function calls
    // are always successful (otherwise they will abort()),
    // so that we don't have to make assertions everywhere.
    json_set_alloc_funcs(xmalloc, free);
    json_object_seed(prng_rand());
    return 0;
}

static int
backend_sandbox (
    void                                  *backend_ctx,
    struct bookmarkfs_backend_create_resp *UNUSED_VAR(resp)
) {
    struct backend_ctx *ctx = backend_ctx;

    if (ctx->flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        return 0;
    }

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    if (!(ctx->flags & BOOKMARKFS_BACKEND_READONLY)) {
        // Do not lazy-init iconv in sandbox mode,
        // since it may want to load modules (e.g., from /usr/lib/gconv).
        if (0 != init_iconv(&ctx->cd)) {
            return -1;
        }
    }
#endif

    // Watcher cannot be lazy-initialized in sandbox mode.
    // Neither can it be initialized in backend_create(),
    // since the calling process may fork per fuse_daemonize().
    if (unlikely(0 != init_watcher(ctx))) {
        return -1;
    }

    uint32_t sandbox_flags = 0;
    if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
        sandbox_flags |= SANDBOX_READONLY;
    }
    if (ctx->flags & BOOKMARKFS_BACKEND_NO_LANDLOCK) {
        sandbox_flags |= SANDBOX_NO_LANDLOCK;
    }
    return sandbox_enter(ctx->dirfd, sandbox_flags);
}

static int
bookmark_fsck (
    void                               *backend_ctx,
    uint64_t                            id,
    struct bookmarkfs_fsck_data const  *fsck_data,
    uint32_t                            UNUSED_VAR(flags),
    bookmarkfs_bookmark_fsck_cb        *callback,
    void                               *user_data,
    void                              **cookie_ptr
) {
    struct backend_ctx *ctx = backend_ctx;

    if (ctx->flags & BACKEND_FILENAME_GUID) {
        return -EPERM;
    }
    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    int status = 0;
    struct bookmark_lcookie *cookie;
    size_t idx = 0;
    if (cookie_ptr != NULL) {
        cookie = *cookie_ptr;
        if (cookie != NULL) {
            idx = cookie->idx;
        }
    }
    // Unlike bookmark_list(), always fetch the latest entries during fsck.
    struct node_entry const *entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (unlikely(entry == NULL || entry->name == NULL)) {
        return -ESTALE;
    }
    json_t *children = entry->children;
    if (children == NULL) {
        return -ENOTDIR;
    }

    // fsck_rewind
    if (callback == NULL) {
        idx = 0;
        goto end;
    }
    if (fsck_data == NULL) {
        status = fsck_next(ctx, id, children, &idx, callback, user_data);
    } else {
        debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));
#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
        status = fsck_apply(ctx, id, fsck_data, callback, user_data);
#endif
    }

  end:
    if (cookie_ptr != NULL) {
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            cookie->children = json_incref(children);
            *cookie_ptr = cookie;
        }
        cookie->idx = idx;
    }
    return status;
}

static int
bookmark_get (
    void                        *backend_ctx,
    uint64_t                     id,
    char const                  *attr_key,
    bookmarkfs_bookmark_get_cb  *callback,
    void                        *user_data,
    void                       **cookie_ptr
) {
    struct backend_ctx *ctx = backend_ctx;

    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    if (cookie_ptr == NULL) {
        goto lookup;
    }
    struct bookmark_gcookie *cookie = *cookie_ptr;
    if (cookie == NULL) {
        goto lookup;
    }
    if (cookie->store != ctx->store) {
        if (attr_key != NULL) {
            goto lookup;
        }
        // Checksum only applies to directory structure and URL values.
        if (!json_equal(cookie->checksum, ctx->checksum)) {
            goto lookup;
        }
    }
    return -EAGAIN;

  lookup:  ;
    struct node_entry const *entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (unlikely(entry == NULL || entry->name == NULL)) {
        return -ESTALE;
    }
    if (entry->id == BOOKMARKS_ROOT_ID) {
        return attr_key == NULL ? -EISDIR : -ENOATTR;
    }

    json_t *value_node;
    int status = get_attr_val(entry->node, attr_key, ctx->flags, &value_node);
    if (status < 0) {
        return status;
    }

    if (callback == NULL) {
        goto end;
    }
    char const *value = json_string_value(value_node);
    if (unlikely(value == NULL)) {
        return -EIO;
    }
    status = callback(user_data, value, json_string_length(value_node));
    if (status < 0) {
        return status;
    }

  end:
    if (cookie_ptr != NULL) {
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            *cookie_ptr = cookie;
        } else {
            json_decref(cookie->checksum);
        }
        cookie->store    = ctx->store;
        cookie->checksum = json_incref(ctx->checksum);
    }
    return status;
}

static int
bookmark_list (
    void                         *backend_ctx,
    uint64_t                      id,
    off_t                         off,
    uint32_t                      flags,
    bookmarkfs_bookmark_list_cb  *callback,
    void                         *user_data,
    void                        **cookie_ptr
) {
    struct backend_ctx *ctx = backend_ctx;

    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    int status = 0;
    struct bookmark_lcookie *cookie;
    struct node_entry const *entry = NULL;
    json_t *children;
    if (cookie_ptr != NULL) {
        cookie = *cookie_ptr;
        if (cookie != NULL) {
            // `off == 0` implies rewinddir()
            if (off > 0) {
                children = cookie->children;
                goto do_list;
            }
        }
    }
    entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (unlikely(entry == NULL || entry->name == NULL)) {
        return -ESTALE;
    }
    children = entry->children;
    if (children == NULL) {
        return -ENOTDIR;
    }

  do_list:
    if (callback == NULL) {
        goto end;
    }

    bool with_stat = flags & BOOKMARK_FLAG(LIST_WITHSTAT);
    for (size_t idx = off; ; ++idx) {
        json_t const *child = json_array_get(children, idx);
        if (child == NULL) {
            break;
        }

        struct bookmarkfs_bookmark_entry buf;
        if (0 != parse_entry(ctx, child, idx + 1, with_stat, &buf)) {
            // Silently ignore bookmark entries with bad or duplicate names.
            continue;
        }
        status = callback(user_data, &buf);
        if (status != 0) {
            break;
        }
    }

  end:
    if (cookie_ptr != NULL && entry != NULL) {
        if (callback == NULL) {
            // No need to copy.  Subsequent call with this cookie
            // will have to re-fetch the children entries nonetheless.
            json_incref(children);
        } else {
            children = json_copy(children);
        }
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            cookie->idx = 0;
            *cookie_ptr = cookie;
        } else {
            json_decref(cookie->children);
        }
        cookie->children = children;
    }
    return status;
}

int
bookmark_lookup (
    void                            *backend_ctx,
    uint64_t                         id,
    char const                      *name,
    uint32_t                         UNUSED_VAR(flags),
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    struct backend_ctx *ctx = backend_ctx;

    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    struct node_entry *entry = lookup_id(ctx->id_map, id, NULL, NULL);
    if (unlikely(entry == NULL || entry->name == NULL)) {
        return -ESTALE;
    }

    if (name != NULL) {
        if (entry->children == NULL) {
            return -ENOTDIR;
        }
        if (0 != lookup_name(ctx, id, name, strlen(name), NULL, &entry)) {
            return -ENOENT;
        }
    }
    json_t const *node = entry->node;

    if (stat_buf == NULL) {
        return 0;
    }
    if (name != NULL) {
        if (unlikely(0 != parse_id(node, &id))) {
            return -EIO;
        }
    }
    stat_buf->id = id;
    return parse_stats(entry, stat_buf);
}

static void
object_free (
    void                        *UNUSED_VAR(backend_ctx),
    void                        *object,
    enum bookmarkfs_object_type  object_type
) {
    if (object == NULL) {
        return;
    }

    switch (object_type) {
      case BOOKMARKFS_OBJECT_TYPE_BGCOOKIE:
        free_bgcookie(object);
        break;

      case BOOKMARKFS_OBJECT_TYPE_BLCOOKIE:
        free_blcookie(object);
        break;

      default:
        unreachable();
    }
}

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE

static int
backend_mkfs (
    struct bookmarkfs_backend_conf const *conf
) {
    struct parsed_mkfsopts opts = {
        .btime = { .tv_nsec = UTIME_NOW },
    };
    if (0 != parse_mkfsopts(conf->opts, &opts)) {
        return -1;
    }

    int open_flags = O_CREAT | O_WRONLY | O_TRUNC;
    if (!(conf->flags & BOOKMARKFS_BACKEND_MKFS_FORCE)) {
        open_flags |= O_EXCL;
    }
    int fd = open(conf->store_path, open_flags, 0600);
    if (fd < 0) {
        log_printf("open(): %s: %s", conf->store_path, xstrerror(errno));
        return -1;
    }
    int status = -1;

    json_t *store;
    if (0 != store_new(&opts.btime, &store)) {
        goto end;
    }
    status = json_dumpfd_ex(store, fd, JSON_COMPACT);
    json_decref(store);

  end:
    close(fd);
    return status;
}

static int
bookmark_create (
    void                            *backend_ctx,
    uint64_t                         parent_id,
    char const                      *name,
    uint32_t                         flags,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    if (parent_id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }
    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    // Lookup parent entry
    struct node_entry *parent_entry
        = lookup_id(ctx->id_map, parent_id, NULL, NULL);
    if (unlikely(parent_entry == NULL || parent_entry->name == NULL)) {
        return -ESTALE;
    }

    // Check if entry can be created
    if (parent_entry->children == NULL) {
        return -ENOTDIR;
    }

    size_t name_len = strlen(name);
    struct lookup_ctx lctx;
    struct node_entry *entry;
    if (0 == lookup_name(ctx, parent_id, name, name_len, &lctx, &entry)) {
        if (stat_buf != NULL) {
            json_t *node = entry->node;
            if (unlikely(0 != parse_id(node, &stat_buf->id))) {
                return -EIO;
            }
            if (unlikely(0 != parse_stats(entry, stat_buf))) {
                return -EIO;
            }
        }
        return -EEXIST;
    }
    if (entry != NULL) {
        // duplicate GUID
        return -EPERM;
    }
    json_t *siblings = parent_entry->children;

    // Build node
    json_t *node   = json_object();
    bool    is_dir = flags & BOOKMARK_FLAG(CREATE_DIR);
    struct build_node_ctx bctx = {
        .stat_buf = stat_buf,
    };
    int status = build_node(ctx, node, &name, name_len, is_dir, &bctx);
    if (unlikely(status != 0)) {
        json_decref(node);
        return status;
    }
    json_array_append_new(siblings, node);

    // Build lookup entry
    entry = xmalloc(sizeof(*entry));
    uint64_t id = stat_buf->id;
    *entry = (struct node_entry) {
        .id        = id,
        .parent_id = parent_id,
        .name      = name,
        .name_len  = name_len,
        .node      = node,
        .children  = is_dir ? json_object_sget(node, "children") : NULL,
    };

    union hashmap_key key = { .u64 = id };
    *hashmap_insert(ctx->id_map, key, hash_digest(&id, sizeof(id))) = entry;

    void          *guid          = lctx.guid;
    unsigned long  hashcode_guid = lctx.hashcode;
    if (!(ctx->flags & BACKEND_FILENAME_GUID)) {
        guid          = bctx.guid;
        hashcode_guid = bctx.hashcode;

        key.ptr = &(struct assocmap_key) {
            .parent_id = parent_id,
            .name      = name,
            .name_len  = name_len,
        };
        *hashmap_insert(ctx->assoc_map, key, lctx.hashcode) = entry;
    }

    key.ptr = guid;
    *hashmap_insert(ctx->guid_map, key, hashcode_guid) = entry;

    memcpy(entry->guid, guid, UUID_LEN);
    ctx->dirty = DIRTY_LEVEL_DATA;

    if (unlikely(0 != node_mtime_now(parent_entry->node, NULL))) {
        return -EIO;
    }
    return 0;
}

static int
bookmark_delete (
    void       *backend_ctx,
    uint64_t    parent_id,
    char const *name,
    uint32_t    UNUSED_VAR(flags)
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    if (parent_id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }
    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    // Lookup parent entry
    struct node_entry *parent_entry
        = lookup_id(ctx->id_map, parent_id, NULL, NULL);
    if (unlikely(parent_entry == NULL || parent_entry->name == NULL)) {
        return -ESTALE;
    }
    if (parent_entry->children == NULL) {
        return -ENOTDIR;
    }

    // Lookup entry to delete
    struct lookup_ctx lctx;
    struct node_entry *entry;
    if (0 != lookup_name(ctx, parent_id, name, strlen(name), &lctx, &entry)) {
        return -ENOENT;
    };
    json_t *siblings   = parent_entry->children;
    json_t const *node = entry->node;

    // Check if entry can be deleted
    json_t const *children = json_object_sget(node, "children");
    if (children != NULL) {
        if (0 != json_array_size(children)) {
            return -ENOTEMPTY;
        }
    }

    // Remove from maps
    long guidmap_entry_id = lctx.entry_id;
    if (!(ctx->flags & BACKEND_FILENAME_GUID)) {
        guidmap_entry_id = -1;
        hashmap_entry_delete(ctx->assoc_map, entry, lctx.entry_id);
    }
    hashmap_entry_delete(ctx->guid_map, entry, guidmap_entry_id);
    hashmap_entry_delete(ctx->id_map, entry, -1);
    free(entry);

    // Remove from store
    json_array_remove(siblings, json_array_search(siblings, node));

    ctx->dirty = DIRTY_LEVEL_DATA;

    if (unlikely(0 != node_mtime_now(parent_entry->node, NULL))) {
        return -EIO;
    }
    return 0;
}

static int
bookmark_permute (
    void                     *backend_ctx,
    uint64_t                  parent_id,
    enum bookmarkfs_permd_op  op,
    char const               *name1,
    char const               *name2,
    uint32_t                  UNUSED_VAR(flags)
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    if (parent_id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }
    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    size_t name1_len = strnlen(name1, NAME_MAX + 1);
    if (0 != validate_filename(name1, name1_len, NULL)) {
        return -EINVAL;
    }
    size_t name2_len = strnlen(name2, NAME_MAX + 1);
    if (0 != validate_filename(name2, name2_len, NULL)) {
        return -EINVAL;
    }

    struct node_entry const *parent_entry
        = lookup_id(ctx->id_map, parent_id, NULL, NULL);
    if (unlikely(parent_entry == NULL || parent_entry->name == NULL)) {
        return -ESTALE;
    }
    json_t *children = parent_entry->children;
    if (children == NULL) {
        return -ENOTDIR;
    }

    struct node_entry *entry1;
    if (0 != lookup_name(ctx, parent_id, name1, name1_len, NULL, &entry1)) {
        return -ENOENT;
    }

    struct node_entry *entry2;
    if (0 != lookup_name(ctx, parent_id, name2, name2_len, NULL, &entry2)) {
        return -ENOENT;
    }
    if (entry2 == entry1) {
        return 0;
    }
    size_t idx1 = json_array_search(children, entry1->node);
    size_t idx2 = json_array_search(children, entry2->node);

    switch (op) {
      case BOOKMARKFS_PERMD_OP_SWAP:
        debug_assert(entry2 != NULL);
        json_incref(entry1->node);
        json_array_set(children, idx1, entry2->node);
        json_array_set(children, idx2, entry1->node);
        json_decref(entry1->node);
        break;

      case BOOKMARKFS_PERMD_OP_MOVE_AFTER:
        ++idx2;
        // fallthrough
      case BOOKMARKFS_PERMD_OP_MOVE_BEFORE:
        if (idx1 == idx2) {
            return 0;
        }
        json_incref(entry1->node);
        if (idx1 > idx2) {
            json_array_remove(children, idx1);
        }
        json_array_insert_new(children, idx2, entry1->node);
        if (idx1 < idx2) {
            json_array_remove(children, idx1);
        }
        break;

      default:
        return -EINVAL;
    }
    ctx->dirty = DIRTY_LEVEL_DATA;

    if (unlikely(0 != node_mtime_now(parent_entry->node, NULL))) {
        return -EIO;
    }
    return 0;
}

static int
bookmark_rename (
    void       *backend_ctx,
    uint64_t    old_parent_id,
    char const *old_name,
    uint64_t    new_parent_id,
    char const *new_name,
    uint32_t    flags
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    if (old_parent_id == BOOKMARKS_ROOT_ID
            || new_parent_id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }
    if (0 != store_load(ctx)) {
        return -EIO;
    }

    // Lookup old entry
    struct node_entry *old_parent
        = lookup_id(ctx->id_map, old_parent_id, NULL, NULL);
    if (unlikely(old_parent == NULL || old_parent->name == NULL)) {
        return -ESTALE;
    }
    if (old_parent->children == NULL) {
        return -ENOTDIR;
    }

    size_t old_name_len = strlen(old_name);
    struct lookup_ctx old_lctx;
    struct node_entry *old_entry;
    if (0 != lookup_name(ctx, old_parent_id, old_name, old_name_len,
                &old_lctx, &old_entry))
    {
        return -ENOENT;
    }
    json_t *old_node = old_entry->node;

    // Lookup new entry
    struct node_entry *new_parent = old_parent;
    if (old_parent_id != new_parent_id) {
        new_parent = lookup_id(ctx->id_map, new_parent_id, NULL, NULL);
        if (unlikely(new_parent == NULL || new_parent->name == NULL)) {
            return -ESTALE;
        }
        if (new_parent->children == NULL) {
            return -ENOTDIR;
        }
    }

    size_t new_name_len = strlen(new_name);
    struct lookup_ctx new_lctx;
    struct node_entry *new_entry;
    if (0 == lookup_name(ctx, new_parent_id, new_name, new_name_len,
                &new_lctx, &new_entry))
    {
        if (flags & BOOKMARKFS_BOOKMARK_RENAME_NOREPLACE) {
            return -EEXIST;
        }
        if (new_entry == old_entry) {
            return 0;
        }
        bool new_is_dir = new_entry->children != NULL;
        if (new_is_dir != (old_entry->children != NULL)) {
            return new_is_dir ? -EISDIR : -ENOTDIR;
        }
        if (new_is_dir && 0 != json_array_size(new_entry->children)) {
            return -ENOTEMPTY;
        }
    } else {
        if (new_entry != NULL) {
            // duplicate GUID
            return -EPERM;
        }
    }

    if (old_name_len == new_name_len) {
        if (0 == memcmp(old_name, new_name, new_name_len)) {
            debug_assert(old_parent != new_parent);
            goto move_node;
        }
    }

    // Update node name
    json_t *new_name_node = json_stringn(new_name, new_name_len);
    if (new_name_node == NULL) {
        return -EPERM;
    }
    json_object_sset_new(old_node, "name", new_name_node);
    old_entry->name     = json_string_value(new_name_node);
    old_entry->name_len = json_string_length(new_name_node);

    if (old_parent != new_parent) {
      move_node:
        old_entry->parent_id = new_parent->id;
        if (new_entry == NULL) {
            json_array_append(new_parent->children, old_node);
        } else {
            json_object_update(new_entry->node, old_node);
            old_entry->node = new_entry->node;
        }
        size_t old_idx = json_array_search(old_parent->children, old_node);
        json_array_remove(old_parent->children, old_idx);
    }

    bool filename_is_guid = ctx->flags & BACKEND_FILENAME_GUID;
    if (new_entry != NULL) {
        long new_guidmap_entry_id = new_lctx.entry_id;
        if (!filename_is_guid) {
            new_guidmap_entry_id = -1;
            hashmap_entry_delete(ctx->assoc_map, new_entry, new_lctx.entry_id);
        }
        hashmap_entry_delete(ctx->guid_map, new_entry, new_guidmap_entry_id);
        hashmap_entry_delete(ctx->id_map, new_entry, -1);
        free(new_entry);
    }

    if (filename_is_guid) {
        update_guid(old_entry, ctx->guid_map, old_lctx.entry_id, new_lctx.guid,
                new_lctx.hashcode);
    } else {
        hashmap_entry_delete(ctx->assoc_map, old_entry, old_lctx.entry_id);

        union hashmap_key key = {
            .ptr = &(struct assocmap_key) {
                .parent_id = new_parent_id,
                .name      = old_entry->name,
                .name_len  = new_name_len,
            },
        };
        *hashmap_insert(ctx->assoc_map, key, new_lctx.hashcode) = old_entry;
    }
    ctx->dirty = DIRTY_LEVEL_DATA;

    json_t *tsnode_now;
    if (unlikely(0 != node_mtime_now(old_parent->node, &tsnode_now))) {
        return -EIO;
    }
    if (old_parent != new_parent) {
        json_object_sset_copy(new_parent->node, "date_modified", tsnode_now);
    }
    if (ctx->flags & BOOKMARKFS_BACKEND_CTIME) {
        json_object_sset_copy(old_node, "date_modified", tsnode_now);
    }
    return 0;
}

static int
bookmark_set (
    void       *backend_ctx,
    uint64_t    id,
    char const *attr_key,
    uint32_t    flags,
    void const *val,
    size_t      val_len
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    if (id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }
    if (unlikely(0 != store_load(ctx))) {
        return -EIO;
    }

    unsigned long entry_id;
    struct node_entry *entry = lookup_id(ctx->id_map, id, NULL, &entry_id);
    if (unlikely(entry == NULL || entry->name == NULL)) {
        return -ESTALE;
    }
    if (entry->parent_id == BOOKMARKS_ROOT_ID) {
        return -EPERM;
    }

    if (flags & BOOKMARK_FLAG(SET_TIME)) {
        debug_assert(val_len == 2);
        // Without UTIME_NOW, it is safe to cast away the const qualifier.
        struct timespec *times = (struct timespec *)val;
        if (unlikely(0 != update_node_ts(entry->node, times))) {
            return -EIO;
        }
        if (ctx->dirty < DIRTY_LEVEL_METADATA) {
            ctx->dirty = DIRTY_LEVEL_METADATA;
        }
        return 0;
    }

    json_t *val_node;
    int key_type = get_attr_val(entry->node, attr_key, ctx->flags, &val_node);
    if (key_type < 0) {
        return key_type;
    }

    bool nocheck = true;
    switch (key_type) {
      case ATTR_KEY_DATE_ADDED:
        if (0 != parse_ts(val, val_len, NULL)) {
            return -EINVAL;
        }
        break;

      case ATTR_KEY_GUID:  ;
        uint8_t guid[UUID_LEN];
        if (0 != parse_guid(val, val_len, guid)) {
            return -EINVAL;
        }
        unsigned long hashcode;
        struct node_entry *entry_found
            = lookup_guid(ctx->guid_map, guid, &hashcode, NULL);
        if (entry_found != NULL) {
            // Must not overwrite existing entry.
            return entry_found->id == id ? 0 : -EPERM;
        }
        update_guid(entry, ctx->guid_map, entry_id, guid, hashcode);
        break;

      case ATTR_KEY_TITLE:
        if (NULL != memchr(val, '\0', val_len)) {
            return -EINVAL;
        }
        nocheck = false;
        break;
    }

    if (nocheck) {
        json_string_setn_nocheck(val_node, val, val_len);
    } else {
        if (0 != json_string_setn(val_node, val, val_len)) {
            // The new value is not valid UTF-8 and cannot fit in JSON.
            return -EINVAL;
        }
    }
    ctx->dirty = DIRTY_LEVEL_DATA;

    if (key_type != ATTR_KEY_NULL && ctx->flags & BOOKMARKFS_BACKEND_CTIME) {
        if (unlikely(0 != node_mtime_now(entry->node, NULL))) {
            return -EIO;
        }
    }
    return 0;
}

static int
bookmark_sync (
    void *backend_ctx
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    return store_save(ctx);
}

#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */

BOOKMARKFS_API
struct bookmarkfs_backend const bookmarkfs_backend_chromium = {
    .backend_create  = backend_create,
    .backend_destroy = backend_destroy,
    .backend_info    = backend_info,
    .backend_init    = backend_init,
    .backend_sandbox = backend_sandbox,

    .bookmark_fsck   = bookmark_fsck,
    .bookmark_get    = bookmark_get,
    .bookmark_list   = bookmark_list,
    .bookmark_lookup = bookmark_lookup,

    .object_free = object_free,

#ifdef BOOKMARKFS_BACKEND_CHROMIUM_WRITE
    .backend_mkfs = backend_mkfs,

    .bookmark_create  = bookmark_create,
    .bookmark_delete  = bookmark_delete,
    .bookmark_permute = bookmark_permute,
    .bookmark_rename  = bookmark_rename,
    .bookmark_set     = bookmark_set,
    .bookmark_sync    = bookmark_sync,
#endif  /* defined(BOOKMARKFS_BACKEND_CHROMIUM_WRITE) */
};
