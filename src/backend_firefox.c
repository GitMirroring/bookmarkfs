/**
 * bookmarkfs/src/backend_firefox.c
 *
 * Firefox backend for BookmarkFS.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
#  include <nettle/base64.h>
#  include <uriparser/Uri.h>
#endif

#include "backend.h"
#include "backend_util.h"
#include "db.h"
#include "hash.h"
#include "hashmap.h"
#include "ioctl.h"
#include "lib.h"
#include "macros.h"
#include "prng.h"
#include "sandbox.h"
#include "version.h"
#include "xstd.h"

#define ATTR_KEY_NULL        0
#define ATTR_KEY_DESC        1
#define ATTR_KEY_TITLE       2
#define ATTR_KEY_GUID        3
#define ATTR_KEY_DATE_ADDED  4
#define ATTR_IN_MOZBM_START  ATTR_KEY_TITLE

#define BACKEND_EXCLUSIVE_LOCK         ( 1u << 16 )
#define BACKEND_FILENAME_GUID          ( 1u << 17 )
#define BACKEND_ASSUME_TITLE_DISTINCT  ( 1u << 18 )

#define BOOKMARKFS_BOOKMARK_LOOKUP_VALIDATE_GUID  ( 1u << 8 )

#define GUID_LEN     9
#define GUID_STR_LEN 12

#define BOOKMARKS_ROOT_GUID  "root________"
#define TAGS_ROOT_GUID       "tags________"

#define DO_QUERY(ctx, stmt_ptr, sql, query_cb, query_cb_data,        \
                 result, BEFORE_PREPARE, BEFORE_QUERY, ...)          \
    do {                                                             \
        sqlite3_stmt *stmt_ = *(stmt_ptr);                           \
        BEFORE_PREPARE                                               \
        if (stmt_ == NULL) {                                         \
            stmt_ = db_prepare((ctx)->db, sql, strlen(sql), true);   \
            if (unlikely(stmt_ == NULL)) {                           \
                (result) = -EIO;                                     \
                break;                                               \
            }                                                        \
            *(stmt_ptr) = stmt_;                                     \
        }                                                            \
        struct db_stmt_bind_item const bind_[] = { __VA_ARGS__ };    \
        BEFORE_QUERY                                                 \
        (result) = db_query(stmt_, bind_, DB_BIND_ITEMS_CNT(bind_),  \
                true, (query_cb), (query_cb_data));                  \
    } while (0)
#define MOZBM_MAXPOS(parent_id)  \
    "SELECT max(`position`) FROM `moz_bookmarks` WHERE `parent` = " parent_id

enum {
    STMT_BOOKMARK_GET,
    STMT_BOOKMARK_GET_EX,
    STMT_BOOKMARK_LIST,
    STMT_BOOKMARK_LIST_EX,
    STMT_BOOKMARK_LIST_KEYWORD,
    STMT_BOOKMARK_LIST_KEYWORD_EX,
    STMT_BOOKMARK_LIST_TAG,
    STMT_BOOKMARK_LIST_TAG_EX,
    STMT_BOOKMARK_LOOKUP_ASSOC,
    STMT_BOOKMARK_LOOKUP_ID,
    STMT_BOOKMARK_LOOKUP_KEYWORD,
    STMT_BOOKMARK_LOOKUP_TAG_ASSOC,
    STMT_DATA_VERSION,
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
    PERSISTED_STMT_WRITE_START,
    STMT_BEGIN = PERSISTED_STMT_WRITE_START,
    STMT_COMMIT,
    STMT_ROLLBACK,
    STMT_MOZPLACE_ADDREF,
    STMT_MOZPLACE_ADDREF_ID,
    STMT_MOZPLACE_DELETE,
    STMT_MOZPLACE_DELREF,
    STMT_MOZPLACE_INSERT,
    STMT_MOZPLACE_UPDATE,
    STMT_MOZORIGIN_DELETE,
    STMT_MOZORIGIN_GET,
    STMT_MOZORIGIN_INSERT,
    STMT_MOZBM_DELETE_DIR,
    STMT_MOZBM_DELETE_URL,
    STMT_MOZBM_GET_TITLE,
    STMT_MOZBM_INSERT,
    STMT_MOZBM_LOOKUP,
    STMT_MOZBM_LOOKUP_ID,
    STMT_MOZBM_MOVE,
    STMT_MOZBM_MTIME_UPDATE,
    STMT_MOZBM_POS_SHIFT,
    STMT_MOZBM_POS_UPDATE,
    STMT_MOZBM_UPDATE,
    STMT_MOZKW_DELETE,
    STMT_MOZKW_INSERT,
    STMT_MOZKW_LOOKUP,
    STMT_MOZKW_RENAME,
    STMT_TAG_ENTRY_LOOKUP,
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */
    PERSISTED_STMT_END,
};

struct backend_ctx {
    sqlite3  *db;
    uint64_t  bookmarks_root_id;
    uint64_t  tags_root_id;
    uint32_t  flags;

    struct sqlite3_stmt *stmts[PERSISTED_STMT_END];
};

struct bookmark_gcookie {
    int64_t data_version;
};

struct bookmark_lcookie {
    struct hashmap *dentry_map;
    size_t          idx;         // fsck only
};

struct bookmark_dentry {
    uint64_t id;
    size_t   name_len;
    char     name[];
};

struct bookmark_name_key {
    size_t      len;
    char const *val;
};

struct bookmark_get_ctx {
    uint64_t                    tags_root_id;
    bookmarkfs_bookmark_get_cb *callback;
    void                       *user_data;

    int status;
};

struct bookmark_list_ctx {
    uint64_t           tags_root_id;
    size_t             next;
    struct hashmap    *dentry_map;
    db_query_row_func *row_func;
    union {
        bookmarkfs_bookmark_fsck_cb *fsck;
        bookmarkfs_bookmark_list_cb *list;
    } callback;
    void *user_data;
    bool  check_name;
    bool  with_stat;

    int status;
};

struct bookmark_lookup_ctx {
    uint64_t                         tags_root_id;
    struct bookmarkfs_bookmark_stat *stat_buf;

    int status;
};

struct mozbm_fsck_get_ctx {
    int64_t         id;
    struct hashmap *dentry_map;

    int status;
};

struct mozplace_addref_ctx {
    struct timespec *atime_buf;
    int64_t          id;
};

struct mozbm {
    int64_t     id;
    int64_t     place_id;
    int64_t     parent_id;
    int64_t     pos;
    char const *title;
    size_t      title_len;
    int64_t     date_added;
    int64_t     last_modified;
    char const *guid;
};

struct mozkw {
    int64_t     id;
    char const *keyword;
    size_t      keyword_len;
    int64_t     place_id;
};

struct mozorigin {
    int64_t     id;
    char const *prefix;
    size_t      prefix_len;
    char const *host;
    size_t      host_len;
};

struct mozplace {
    int64_t     id;
    char const *url;
    size_t      url_len;
    int64_t     url_hash;
    char const *rev_host;
    size_t      rev_host_len;
    int64_t     last_visit_date;
    int64_t     origin_id;
    char const *desc;
    size_t      desc_len;
};

struct parsed_mkfsopts {
    int64_t date_added;
};

struct parsed_mntopts {
    uint32_t flags;
};

// Forward declaration start
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
static int     bookmark_do_create (struct backend_ctx *, uint64_t,
                                   char const *, size_t, bool,
                                   struct bookmarkfs_bookmark_stat *);
static int     bookmark_do_delete (struct backend_ctx *, uint64_t,
                                   char const *, size_t, bool);
static int     fsck_apply         (struct backend_ctx *, uint64_t,
                                   struct bookmarkfs_fsck_data const *,
                                   struct bookmark_list_ctx *);
static char *  gen_random_guid    (char *);
static bool    is_valid_guid      (char const *, size_t);
static int     keyword_create     (struct backend_ctx *, char const *, size_t,
                                   struct bookmarkfs_bookmark_stat *);
static int     mozbm_delete       (struct backend_ctx *, int64_t, bool);
static int     mozbm_fsck_get_cb  (void *, sqlite3_stmt *);
static int     mozbm_get_title    (struct backend_ctx *, int64_t, int64_t,
                                   db_query_row_func *, void *);
static int     mozbm_insert       (struct backend_ctx *, struct mozbm *);
static int     mozbm_lookup       (struct backend_ctx *, int64_t,
                                   char const *, size_t, bool, struct mozbm *);
static int     mozbm_lookup_id    (struct backend_ctx *, struct mozbm *);
static int     mozbm_move         (struct backend_ctx *, int64_t, int64_t,
                                   int64_t, char const *, size_t);
static int     mozbm_mtime_update (struct backend_ctx *, int64_t, int64_t *);
static int     mozbm_pos_shift    (struct backend_ctx *, int64_t, int64_t,
                                   int64_t *, enum bookmarkfs_permd_op);
static int     mozbm_pos_update   (struct backend_ctx *, int64_t, int64_t);
static int     mozbm_update       (struct backend_ctx *, struct mozbm *);
static int     mozkw_delete       (struct backend_ctx *, char const *, size_t);
static int     mozkw_insert       (struct backend_ctx *, struct mozkw *);
static int     mozkw_lookup       (struct backend_ctx *, struct mozkw *);
static int     mozkw_rename       (struct backend_ctx *, char const *,
                                   char const *, uint32_t);
static int     mozorigin_delete   (struct backend_ctx *, int64_t);
static int     mozorigin_get      (struct backend_ctx *, char const *, size_t,
                                   char const *, size_t, int64_t *);
static int     mozorigin_insert   (struct backend_ctx *, struct mozorigin *);
static int     mozplace_addref    (struct backend_ctx *, char const *, size_t,
                                   int64_t *, struct timespec *);
static int     mozplace_addref_cb (void *, sqlite3_stmt *);
static int     mozplace_addref_id (struct backend_ctx *, int64_t);
static int     mozplace_delete    (struct backend_ctx *, int64_t, int64_t);
static int     mozplace_delref    (struct backend_ctx *, int64_t);
static int     mozplace_insert    (struct backend_ctx *, struct mozplace *);
static int     mozplace_update    (struct backend_ctx *, struct mozplace *);
static int64_t mozplace_url_hash  (char const *, size_t);
static int64_t msecs_now          (struct timespec *);
static int     parse_mkfsopts     (struct bookmarkfs_conf_opt const *,
                                   struct parsed_mkfsopts *);
static int     parse_mozurl_host  (char const *, size_t, size_t *,
                                   char const **, size_t *);
static int     parse_msecs        (char const *, size_t, int64_t *);
static int     store_new          (sqlite3 *, int64_t);
static int     store_sync         (sqlite3 *);
static int     tag_entry_add      (struct backend_ctx *, uint64_t,
                                   char const *, size_t,
                                   struct bookmarkfs_bookmark_stat *);
static int     tag_entry_delete   (struct backend_ctx *, uint64_t,
                                   char const *, size_t);
static int     tag_entry_lookup   (struct backend_ctx *, struct mozbm *);
static int64_t timespec_to_msecs  (struct timespec const *);
static int     txn_begin          (struct backend_ctx *);
static int     txn_end            (struct backend_ctx *);
static int     txn_rollback       (struct backend_ctx *, int);
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */

static int     bookmark_do_get    (struct backend_ctx *, uint64_t, int,
                                   struct bookmark_get_ctx *);
static int     bookmark_do_list   (struct backend_ctx *, uint64_t, off_t,
                                   uint32_t, struct bookmark_list_ctx *);
static int     bookmark_do_lookup (struct backend_ctx *, uint64_t,
                                   char const *, size_t, uint32_t,
                                   struct bookmarkfs_bookmark_stat *);
static int     bookmark_fsck_cb   (void *, sqlite3_stmt *);
static int     bookmark_get_cb    (void *, sqlite3_stmt *);
static int     bookmark_list_cb   (void *, sqlite3_stmt *);
static int     bookmark_lookup_cb (void *, sqlite3_stmt *);
static int     dentmap_comp       (union hashmap_key, void const *);
static unsigned long
               dentmap_hash       (void const *);
static void    free_blcookie      (struct bookmark_lcookie *);
static void    free_dentmap       (struct hashmap *);
static void    free_dentmap_entry (void *, void *);
static int     get_attr_type      (char const *, uint32_t);
static int64_t get_data_version   (struct backend_ctx *);
static bool    is_valid_id        (int64_t);
static void    msecs_to_timespec  (struct timespec *, int64_t);
static int     parse_mntopts      (struct bookmarkfs_conf_opt const *,
                                   uint32_t, struct parsed_mntopts *);
static void    print_help         (uint32_t);
static void    print_version      (void);
static int     store_init         (sqlite3 *, uint64_t *, uint64_t *);
static int     store_check_cb     (void *, sqlite3_stmt *);
// Forward declaration end

#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE

static int
bookmark_do_create (
    struct backend_ctx              *ctx,
    uint64_t                         parent_id,
    char const                      *name,
    size_t                           name_len,
    bool                             is_dir,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    if (parent_id == ctx->bookmarks_root_id) {
        return -EPERM;
    }

    int status = bookmark_do_lookup(ctx, parent_id, name, name_len,
            BOOKMARK_FLAG(LOOKUP_VALIDATE_GUID), stat_buf);
    if (status == 0) {
        return -EEXIST;
    }
    if (status != -ENOENT) {
        return status;
    }

    stat_buf->value_len = -1;
    int64_t place_id = 0;
    if (!is_dir) {
        char const *url = "about:blank";
        stat_buf->value_len = strlen(url);
        status = mozplace_addref(ctx, url, stat_buf->value_len, &place_id,
                &stat_buf->atime);
        if (status < 0) {
            return status;
        }
    }

    int64_t date_added = msecs_now(&stat_buf->mtime);
    if (unlikely(date_added < 0)) {
        return -EIO;
    }
    char const *guid = name;
    char guid_buf[GUID_STR_LEN];
    if (!(ctx->flags & BACKEND_FILENAME_GUID)) {
        guid = gen_random_guid(guid_buf);
    }
    struct mozbm cols = {
        .place_id   = place_id,
        .parent_id  = parent_id,
        .title      = name,
        .title_len  = name_len,
        .date_added = date_added,
        .guid       = guid,
    };
    status = mozbm_insert(ctx, &cols);
    if (status < 0) {
        return status;
    }
    stat_buf->id = cols.id;

    status = mozbm_mtime_update(ctx, parent_id, &date_added);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
bookmark_do_delete (
    struct backend_ctx *ctx,
    uint64_t            parent_id,
    char const         *name,
    size_t              name_len,
    bool                is_dir
) {
    if (parent_id == ctx->bookmarks_root_id) {
        return -EPERM;
    }

    struct mozbm cols;
    int status = mozbm_lookup(ctx, parent_id, name, name_len, false, &cols);
    if (status < 0) {
        return status;
    }
    if (unlikely((cols.place_id == 0) != is_dir)) {
        return is_dir ? -ENOTDIR : -EPERM;
    }

    status = mozbm_delete(ctx, cols.id, is_dir);
    if (status < 0) {
        return status;
    }
    status = mozbm_mtime_update(ctx, parent_id, NULL);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
fsck_apply (
    struct backend_ctx                *ctx,
    uint64_t                           parent_id,
    struct bookmarkfs_fsck_data const *fsck_data,
    struct bookmark_list_ctx          *fctx
) {
    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }

    struct hashmap *map = fctx->dentry_map;
    uint64_t        id  = fsck_data->id;
    struct mozbm_fsck_get_ctx qctx = {
        .id         = id,
        .dentry_map = map,
    };
    status = mozbm_get_title(ctx, id, parent_id, mozbm_fsck_get_cb, &qctx);
    if (status < 0) {
        goto fail;
    }
    if (qctx.status < 0) {
        status = qctx.status;
        goto fail;
    }
    if (qctx.status > 0) {
        goto end;
    }

    uint64_t    extra    = 0;
    char const *name     = fsck_data->name;
    size_t      name_len = strnlen(name, sizeof(fsck_data->name));
    int         result;
    if (0 != validate_filename_fsck(name, name_len, &result, &extra)) {
        goto callback;
    }

    union hashmap_key key = {
        .ptr = &(struct bookmark_name_key) {
            .val = name,
            .len = name_len,
        },
    };
    unsigned long hashcode = hash_digest(name, name_len);
    if (map == NULL) {
        map = hashmap_create(dentmap_comp, dentmap_hash);
        fctx->dentry_map = map;
        goto update_name;
    }
    struct bookmark_dentry *dentry = hashmap_search(map, key, hashcode, NULL);
    if (dentry != NULL) {
        extra  = dentry->id;
        result = BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE;
        goto callback;
    }

  update_name:  ;
    struct mozbm cols = {
        .id            = id,
        .place_id      = -1,
        .title         = name,
        .title_len     = name_len,
        .date_added    = -1,
        .last_modified = -1,
    };
    status = mozbm_update(ctx, &cols);
    if (status < 0) {
        goto fail;
    }

    dentry = xmalloc(sizeof(*dentry) + name_len);
    dentry->id       = id;
    dentry->name_len = name_len;
    memcpy(dentry->name, name, name_len);

    *hashmap_insert(map, key, hashcode) = dentry;
    goto end;

  callback:
    status = fctx->callback.fsck(fctx->user_data, result, id, extra, name);
    if (status < 0) {
        goto fail;
    }

  end:
    return txn_end(ctx);

  fail:
    return txn_rollback(ctx, status);
}

static char *
gen_random_guid (
    char *out
) {
    struct base64_encode_ctx ctx;
    base64url_encode_init(&ctx);

    uint64_t const buf[] = { prng_rand(), prng_rand() };
    base64_encode_final(&ctx, out +
            base64_encode_update(&ctx, out, GUID_LEN, (uint8_t const *)buf));
    return out;
}

static bool
is_valid_guid (
    char const *str,
    size_t      len
) {
    if (len != GUID_STR_LEN) {
        return false;
    }

    struct base64_decode_ctx ctx;
    base64url_decode_init(&ctx);

    uint8_t buf[BASE64_DECODE_LENGTH(GUID_STR_LEN)];
    if (!base64_decode_update(&ctx, &len, buf, len, str)) {
        return false;
    }
    if (!base64_decode_final(&ctx)) {
        return false;
    }
    return true;
}

static int
keyword_create (
    struct backend_ctx              *ctx,
    char const                      *keyword,
    size_t                           keyword_len,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    struct mozbm bm_cols = {
        .id = stat_buf->id,
    };
    int status = mozbm_lookup_id(ctx, &bm_cols);
    if (status < 0) {
        return status;
    }
    if (bm_cols.place_id == 0) {
        return -EPERM;
    }

    struct mozkw kw_cols = {
        .keyword     = keyword,
        .keyword_len = keyword_len,
        .place_id    = bm_cols.place_id,
    };
    status = mozkw_insert(ctx, &kw_cols);
    if (status < 0) {
        return status;
    }

    status = bookmark_do_lookup(ctx, bm_cols.id, NULL, 0, 0, stat_buf);
    if (status < 0) {
        return status;
    }
    status = mozplace_addref_id(ctx, bm_cols.place_id);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
mozbm_delete (
    struct backend_ctx *ctx,
    int64_t             id,
    bool                is_dir
) {
#define MOZBM_DELETE_     "DELETE FROM `moz_bookmarks` WHERE `id` = ? "
#define MOZBM_DELETE_DIR  MOZBM_DELETE_  \
    "AND `id` NOT IN (SELECT DISTINCT `parent` FROM `moz_bookmarks`)"
#define MOZBM_DELETE_URL  MOZBM_DELETE_ "RETURNING `fk`"

    sqlite3_stmt      **stmt_ptr = &ctx->stmts[STMT_MOZBM_DELETE_URL];
    char const         *sql      = MOZBM_DELETE_URL;
    db_query_row_func  *row_cb   = db_query_i64_cb;
    if (is_dir) {
        stmt_ptr = &ctx->stmts[STMT_MOZBM_DELETE_DIR];
        sql      = MOZBM_DELETE_DIR;
        row_cb   = NULL;
    }

    int64_t place_id;
    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, row_cb, &place_id, nrows, ,
        {
            place_id = 0;
        },
        DB_QUERY_BIND_INT64(id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (is_dir) {
        if (0 == sqlite3_changes(ctx->db)) {
            return -ENOTEMPTY;
        }
        return 0;
    } else {
        xassert(nrows > 0);
        return mozplace_delref(ctx, place_id);
    }
}

static int
mozbm_fsck_get_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct mozbm_fsck_get_ctx *ctx = user_data;

    size_t      name_len = sqlite3_column_bytes(stmt, 0);
    char const *name     = (char const *)sqlite3_column_text(stmt, 0);
    if (unlikely(name == NULL)) {
        name = "";
    }
    if (0 != validate_filename(name, name_len, NULL)) {
        return 1;
    }

    struct hashmap *map = ctx->dentry_map;
    if (map == NULL) {
        ctx->status = 1;
        return 1;
    }
    union hashmap_key key = {
        .ptr = &(struct bookmark_name_key) {
            .val = name,
            .len = name_len,
        },
    };
    unsigned long hashcode = hash_digest(name, name_len);
    struct bookmark_dentry *dentry = hashmap_search(map, key, hashcode, NULL);
    if (dentry == NULL || dentry->id == (uint64_t)ctx->id) {
        // fsck_apply() was given an ID not previously returned by fsck_next().
        ctx->status = -ENOENT;
    }
    return 1;
}

static int
mozbm_get_title (
    struct backend_ctx *ctx,
    int64_t             id,
    int64_t             parent_id,
    db_query_row_func  *row_func,
    void               *user_data
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_GET_TITLE];
    char const *sql = "SELECT `title` FROM `moz_bookmarks` "
        "WHERE `id` = ? AND `parent` = ?";

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, row_func, user_data, nrows, , ,
        DB_QUERY_BIND_INT64(id),
        DB_QUERY_BIND_INT64(parent_id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ESTALE;
    }
    return 0;
}

static int
mozbm_insert (
    struct backend_ctx *ctx,
    struct mozbm       *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_INSERT];
    char const *sql =
        "INSERT INTO `moz_bookmarks` (`parent`, `position`, `title`, "
            "`dateAdded`, `lastModified`, `type`, `fk`, `guid`) "
        "VALUES (?1, safeincr((" MOZBM_MAXPOS("?1") ")), ?2, ?3, ?3, "
            "?4, nullif(?5, -1), ?6)";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, prepare:, ,
        DB_QUERY_BIND_INT64(cols->parent_id),
        DB_QUERY_BIND_TEXT(cols->title, cols->title_len),
        DB_QUERY_BIND_INT64(cols->date_added),
        DB_QUERY_BIND_INT64(cols->place_id == 0 ? 2 : 1),
        DB_QUERY_BIND_INT64(cols->place_id),
        DB_QUERY_BIND_TEXT(cols->guid, GUID_STR_LEN),
    );
    if (status < 0) {
        // duplicate GUID
        if (unlikely(status == -EEXIST)) {
            goto prepare;
        }
        return status;
    }

    cols->id = sqlite3_last_insert_rowid(ctx->db);
    if (!is_valid_id(cols->id)) {
        return -ENOSPC;
    }
    return 0;
}

static int
mozbm_lookup (
    struct backend_ctx *ctx,
    int64_t             parent_id,
    char const         *name,
    size_t              name_len,
    bool                validate_guid,
    struct mozbm       *cols
) {
#define MOZBM_LOOKUP(col)  \
    "SELECT `id`, `fk`, `position` FROM `moz_bookmarks` "  \
    "WHERE `parent` = ? AND `" col "` = ? ORDER BY `position` LIMIT 1"

    struct sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_LOOKUP];
    char const *sql = MOZBM_LOOKUP("title");
    if (ctx->flags & BACKEND_FILENAME_GUID) {
        if (validate_guid && !is_valid_guid(name, name_len)) {
            return -EPERM;
        }
        sql = MOZBM_LOOKUP("guid");
    }

    int64_t values[3];
    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, values, nrows, , ,
        DB_QUERY_BIND_INT64(parent_id),
        DB_QUERY_BIND_TEXT(name, name_len),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ENOENT;
    }
    cols->id       = values[0];
    cols->place_id = values[1];
    cols->pos      = values[2];
    return 0;
}

static int
mozbm_lookup_id (
    struct backend_ctx *ctx,
    struct mozbm       *cols
) {
#define MOZBM_LOOKUP_ID(col)  \
    "SELECT `id`, `fk` FROM `moz_bookmarks` WHERE (`fk`, `" col "`) = "  \
        "(SELECT `fk`, `" col "` FROM `moz_bookmarks` WHERE `id` = ?) "  \
    "ORDER BY `id` LIMIT 1"

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_LOOKUP_ID];
    char const    *sql      = MOZBM_LOOKUP_ID("title");
    if (ctx->flags & BACKEND_FILENAME_GUID) {
        sql = MOZBM_LOOKUP_ID("guid");
    }

    int64_t values[2];
    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, values, nrows, , ,
        DB_QUERY_BIND_INT64(cols->id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ESTALE;
    }
    cols->id       = values[0];
    cols->place_id = values[1];
    return 0;
}

static int
mozbm_move (
    struct backend_ctx *ctx,
    int64_t             id,
    int64_t             new_parent,
    int64_t             new_position,
    char const         *new_name,
    size_t              new_name_len
) {
#define MOZBM_MOVE(col)  "UPDATE `moz_bookmarks` "  \
    "SET (`parent`, `" col "`, `position`) = (?1, ifnull(?2, `" col "`), "  \
        "ifnull(?3, safeincr((" MOZBM_MAXPOS("?1") ")))) "  \
    "WHERE `id` = ?4"

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_MOVE];
    char const *sql = MOZBM_MOVE("title");
    if (ctx->flags & BACKEND_FILENAME_GUID) {
        sql = MOZBM_MOVE("guid");
    }

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(new_parent),
        DB_QUERY_BIND_TEXT(new_name, new_name_len),
        DB_QUERY_BIND_INT64(new_position),
        DB_QUERY_BIND_INT64(id),
    );
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
mozbm_mtime_update (
    struct backend_ctx *ctx,
    int64_t             id,
    int64_t            *msecs_ptr
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_MTIME_UPDATE];
    char const *sql =
        "UPDATE `moz_bookmarks` SET `lastModified` = ? WHERE `id` = ?";

    int64_t msecs = -1;
    if (msecs_ptr != NULL) {
        msecs = *msecs_ptr;
    }
    if (msecs < 0) {
        struct timespec now;
        msecs = msecs_now(&now);
        if (unlikely(msecs < 0)) {
            return -EIO;
        }
    }

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(msecs),
        DB_QUERY_BIND_INT64(id),
    );
    if (status < 0) {
        return status;
    }
    if (msecs_ptr != NULL) {
        *msecs_ptr = msecs;
    }
    return 0;
}

static int
mozbm_pos_shift (
    struct backend_ctx       *ctx,
    int64_t                   parent_id,
    int64_t                   pos_start,
    int64_t                  *pos_end_ptr,
    enum bookmarkfs_permd_op  op
) {
    int64_t pos_end = *pos_end_ptr;
    if (unlikely(pos_start == pos_end)) {
        // Somehow two bookmarks share a same position...
        return -EIO;
    }

    int64_t diff = 0;
    if (pos_start < pos_end) {
        if (op == BOOKMARKFS_PERMD_OP_MOVE_BEFORE) {
            if (--pos_end == pos_start) {
                return 0;
            }
            *pos_end_ptr = pos_end;
        }
        ++pos_start;
    } else {
        if (op == BOOKMARKFS_PERMD_OP_MOVE_AFTER) {
            if (++pos_end == pos_start) {
                return 0;
            }
            *pos_end_ptr = pos_end;
        }
        --pos_start;

        diff = 2;
        int64_t tmp = pos_end;
        pos_end = pos_start;
        pos_start = tmp;
    }

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_POS_SHIFT];
    char const *sql =
        "UPDATE `moz_bookmarks` SET `position` = `position` + (? - 1) "
        "WHERE `parent` = ? AND `position` BETWEEN ? AND ?";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(diff),
        DB_QUERY_BIND_INT64(parent_id),
        DB_QUERY_BIND_INT64(pos_start),
        DB_QUERY_BIND_INT64(pos_end),
    );
    if (status < 0) {
        return status;
    }
    return 1;
}

static int
mozbm_pos_update (
    struct backend_ctx *ctx,
    int64_t             id,
    int64_t             new_pos
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_POS_UPDATE];
    char const *sql =
        "UPDATE `moz_bookmarks` SET `position` = ? WHERE `id` = ?";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(new_pos),
        DB_QUERY_BIND_INT64(id),
    );
    if (status < 0) {
        return status;
    }
    return 1;
}

static int
mozbm_update (
    struct backend_ctx *ctx,
    struct mozbm       *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZBM_UPDATE];
    char const *sql = "UPDATE `moz_bookmarks` "
        "SET (`fk`, `title`, `guid`, `dateAdded`, `lastModified`) "
            "= (ifnull(?, `fk`), ifnull(?, `title`), ifnull(?, `guid`), "
                "ifnull(?, `dateAdded`), ifnull(?, `lastModified`)) "
        "WHERE `id` = ? RETURNING `fk`";

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, &cols->place_id, nrows, , ,
        DB_QUERY_BIND_INT64(cols->place_id),
        DB_QUERY_BIND_TEXT(cols->title, cols->title_len),
        DB_QUERY_BIND_TEXT(cols->guid, GUID_STR_LEN),
        DB_QUERY_BIND_INT64(cols->date_added),
        DB_QUERY_BIND_INT64(cols->last_modified),
        DB_QUERY_BIND_INT64(cols->id),
    );
    if (nrows < 0) {
        // duplicate GUID
        if (nrows == -EEXIST) {
            nrows = -EPERM;
        }
        return nrows;
    }
    if (nrows == 0) {
        return -ESTALE;
    }
    return 0;
}

static int
mozkw_delete (
    struct backend_ctx *ctx,
    char const         *name,
    size_t              name_len
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZKW_RENAME];
    char const *sql = "DELETE FROM `moz_keywords` "
        "WHERE `keyword` = ? RETURNING `place_id`";

    int64_t place_id;
    int status;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, &place_id, status, , ,
        DB_QUERY_BIND_TEXT(name, name_len),
    );
    if (status < 0) {
        return status;
    }
    status = mozplace_delref(ctx, place_id);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
mozkw_insert (
    struct backend_ctx *ctx,
    struct mozkw       *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZKW_INSERT];
    char const *sql =
        "INSERT INTO `moz_keywords` (`keyword`, `place_id`) VALUES (?, ?)";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_TEXT(cols->keyword, cols->keyword_len),
        DB_QUERY_BIND_INT64(cols->place_id),
    );
    if (status < 0) {
        // May fail with -EEXIST here.
        return status;
    }

    cols->id = sqlite3_last_insert_rowid(ctx->db);
    return 0;
}

static int
mozkw_lookup (
    struct backend_ctx *ctx,
    struct mozkw       *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZKW_LOOKUP];
    char const *sql =
        "SELECT `id`, `place_id` FROM `moz_keywords` WHERE `keyword` = ?";

    int64_t values[2];
    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, values, nrows, , ,
        DB_QUERY_BIND_TEXT(cols->keyword, cols->keyword_len),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ENOENT;
    }
    cols->id       = values[0];
    cols->place_id = values[1];
    return 0;
}

static int
mozkw_rename (
    struct backend_ctx *ctx,
    char const         *old_name,
    char const         *new_name,
    uint32_t            flags
) {
    struct mozkw old_cols = {
        .keyword     = old_name,
        .keyword_len = strlen(old_name),
    };
    int status = mozkw_lookup(ctx, &old_cols);
    if (status < 0) {
        if (status != -ENOENT) {
            return status;
        }
    } else {
        if (flags & BOOKMARKFS_BOOKMARK_RENAME_NOREPLACE) {
            return -EEXIST;
        }
        status = mozplace_delref(ctx, old_cols.place_id);
        if (status < 0) {
            return status;
        }
    }

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZKW_RENAME];
    char const *sql =
        "UPDATE OR REPLACE `moz_keywords` SET `id` = ? WHERE `keyword` = ?";

    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(old_cols.id),
        DB_QUERY_BIND_TEXT(new_name, strlen(new_name)),
    );
    if (status < 0) {
        return status;
    }
    if (0 == sqlite3_changes(ctx->db)) {
        return -ENOENT;
    }
    return 0;
}

static int
mozorigin_delete (
    struct backend_ctx *ctx,
    int64_t             id
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZORIGIN_DELETE];
    char const *sql = "DELETE FROM `moz_origins` WHERE `id` = ? "
        "AND `id` NOT IN (SELECT DISTINCT `origin_id` FROM `moz_places`)";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(id),
    );
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
mozorigin_get (
    struct backend_ctx *ctx,
    char const         *prefix,
    size_t              prefix_len,
    char const         *host,
    size_t              host_len,
    int64_t            *id_ptr
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZORIGIN_GET];
    char const *sql =
        "SELECT `id` FROM `moz_origins` WHERE `prefix` = ? AND `host` = ?";

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, id_ptr, nrows, , ,
        DB_QUERY_BIND_TEXT(prefix, prefix_len),
        DB_QUERY_BIND_TEXT(host, host_len),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ENOENT;
    }
    return 0;
}

static int
mozorigin_insert (
    struct backend_ctx *ctx,
    struct mozorigin   *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZORIGIN_INSERT];
    char const *sql =
        "INSERT INTO `moz_origins` (`prefix`, `host`, `frecency`, "
            "`recalc_frecency`, `recalc_alt_frecency`) "
        "VALUES (?, ?, 1, 1, 1)";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_TEXT(cols->prefix, cols->prefix_len),
        DB_QUERY_BIND_TEXT(cols->host, cols->host_len),
    );
    if (status < 0) {
        return status;
    }

    cols->id = sqlite3_last_insert_rowid(ctx->db);
    return 0;
}

static int
mozplace_addref (
    struct backend_ctx *ctx,
    char const         *url,
    size_t              url_len,
    int64_t            *id_ptr,
    struct timespec    *atime_buf
) {
    size_t      prefix_len;
    char const *host;
    size_t      host_len;
    if (0 != parse_mozurl_host(url, url_len, &prefix_len, &host, &host_len)) {
        log_puts("parse_mozurl_host(): bad url");
        return -EINVAL;
    }
    int64_t url_hash = mozplace_url_hash(url, url_len);

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_ADDREF];
    char const *sql =
        "UPDATE `moz_places` SET `foreign_count` = `foreign_count` + 1 "
        "WHERE `url_hash` = ? AND `url` = ? RETURNING `id`, `last_visit_date`";

    struct mozplace_addref_ctx qctx;
    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, mozplace_addref_cb, &qctx, nrows, ,
        {
            qctx.atime_buf = atime_buf;
        },
        DB_QUERY_BIND_INT64(url_hash),
        DB_QUERY_BIND_TEXT(url, url_len),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows > 0) {
        *id_ptr = qctx.id;
        return 0;
    }

    int64_t origin_id;
    int status = mozorigin_get(ctx, url, prefix_len, host, host_len,
            &origin_id);
    if (status < 0) do {
        if (status != -ENOENT) {
            return status;
        }

        struct mozorigin cols = {
            .prefix     = url,
            .prefix_len = prefix_len,
            .host       = host,
            .host_len   = host_len,
        };
        status = mozorigin_insert(ctx, &cols);
        if (status < 0) {
            return status;
        }
        origin_id = cols.id;
    } while (0);

    char *rev_host = xmalloc(host_len + 1);
    for (size_t idx = 0; idx < host_len; ++idx) {
        rev_host[idx] = host[host_len - idx - 1];
    }
    rev_host[host_len] = '.';

    struct mozplace cols = {
        .url          = url,
        .url_len      = url_len,
        .url_hash     = url_hash,
        .rev_host     = rev_host,
        .rev_host_len = host_len + 1,
        .origin_id    = origin_id,
    };
    status = mozplace_insert(ctx, &cols);
    if (status < 0) {
        goto end;
    }
    *id_ptr = cols.id;

  end:
    free(rev_host);
    return status;
}

static int
mozplace_addref_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct mozplace_addref_ctx *ctx = user_data;

    ctx->id = sqlite3_column_int64(stmt, 0);
    if (ctx->atime_buf != NULL) {
        msecs_to_timespec(ctx->atime_buf, sqlite3_column_int64(stmt, 1));
    }
    return 1;
}

static int
mozplace_addref_id (
    struct backend_ctx *ctx,
    int64_t             id
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_ADDREF_ID];
    char const *sql = "UPDATE `moz_places` "
        "SET `foreign_count` = `foreign_count` + 1 WHERE `id` = ?";

    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(id),
    );
    if (status < 0) {
        return status;
    }
    if (0 == sqlite3_changes(ctx->db)) {
        return -ESTALE;
    }
    return 0;
}

static int
mozplace_delete (
    struct backend_ctx *ctx,
    int64_t             id,
    int64_t             origin_id
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_DELETE];
    char const *sql = "DELETE FROM `moz_places` WHERE `id` = ?";

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, nrows, , ,
        DB_QUERY_BIND_INT64(id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (unlikely(0 == sqlite3_changes(ctx->db))) {
        return -EIO;
    }
    return mozorigin_delete(ctx, origin_id);
}

static int
mozplace_delref (
    struct backend_ctx *ctx,
    int64_t             id
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_DELREF];
    char const *sql =
        "UPDATE `moz_places` SET `foreign_count` = `foreign_count` - 1 "
        "WHERE `id` = ? RETURNING `foreign_count`, `origin_id`";

    ssize_t nrows;
    int64_t result[2];  // `foreign_count`, `origin_id`
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, result, nrows, , ,
        DB_QUERY_BIND_INT64(id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (unlikely(nrows == 0)) {
        return -EIO;
    }
    if (result[0] > 0) {
        return 0;
    }
    // `foreign_count` reaches 0, delete row.
    return mozplace_delete(ctx, id, result[1]);
}

static int
mozplace_insert (
    struct backend_ctx *ctx,
    struct mozplace    *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_INSERT];
    char const *sql = 
        "INSERT INTO `moz_places` (`url`, `rev_host`, `guid`, `frecency`, "
            "`foreign_count`, `url_hash`, `origin_id`, `recalc_frecency`) "
        "VALUES (?, ?, ?, 1, 1, ?, ?, 1)";

    char guid_buf[GUID_STR_LEN];
    int status;
    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, prepare:, ,
        DB_QUERY_BIND_TEXT(cols->url, cols->url_len),
        DB_QUERY_BIND_TEXT(cols->rev_host, cols->rev_host_len),
        DB_QUERY_BIND_TEXT(gen_random_guid(guid_buf), GUID_STR_LEN),
        DB_QUERY_BIND_INT64(cols->url_hash),
        DB_QUERY_BIND_INT64(cols->origin_id),
    );
    if (status < 0) {
        // duplicate GUID
        if (unlikely(status == -EEXIST)) {
            goto prepare;
        }
        return status;
    }

    cols->id = sqlite3_last_insert_rowid(ctx->db);
    return 0;
}

static int
mozplace_update (
    struct backend_ctx *ctx,
    struct mozplace    *cols
) {
    if (cols->url == NULL) {
        goto do_update;
    }
    int64_t new_id;
    int status = mozplace_addref(ctx, cols->url, cols->url_len, &new_id, NULL);
    if (status < 0) {
        return status;
    }
    status = mozplace_delref(ctx, cols->id);
    if (status < 0) {
        return status;
    }
    cols->id = new_id;

  do_update:  ;
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_MOZPLACE_UPDATE];
    char const *sql = "UPDATE `moz_places` "
        "SET (`last_visit_date`, `description`) "
            "= (ifnull(?, `last_visit_date`), ifnull(?, `description`)) "
        "WHERE `id` = ?";

    DO_QUERY(ctx, stmt_ptr, sql, NULL, NULL, status, , ,
        DB_QUERY_BIND_INT64(cols->last_visit_date),
        DB_QUERY_BIND_TEXT(cols->desc, cols->desc_len),
        DB_QUERY_BIND_INT64(cols->id),
    );
    if (status < 0) {
        return status;
    }
    return 0;
}

/**
 * Calculate the 48-bit URL hash for a given string.
 * Unspecified result if the string does not contain a colon.
 *
 * See function `HashURL()` in mozilla-central source code:
 * /toolkit/components/places/Helpers.cpp
 */
static int64_t
mozplace_url_hash (
    char const *url,
    size_t      url_len
) {
#define MAX_URL_HASH_LEN  1500
#define ROTL32(v, b)      ( (v) << (b) | (v) >> (32 - (b)) )

    if (url_len > MAX_URL_HASH_LEN) {
        url_len = MAX_URL_HASH_LEN;
    }

    uint64_t prefix_hash = UINT64_MAX;
    uint32_t str_hash    = 0;
    for (char const *end = url + url_len; url < end; ++url) {
        uint32_t ch = *url;
        if (prefix_hash == UINT64_MAX && ch == ':') {
            prefix_hash = str_hash;
        }
        str_hash = (ROTL32(str_hash, 5) ^ ch) * UINT32_C(0x9e3779b9);
    }
    return (prefix_hash & 0xffff) << 32 | str_hash;
}

static int64_t
msecs_now (
    struct timespec *ts_buf
) {
    if (unlikely(0 != clock_gettime(CLOCK_REALTIME, ts_buf))) {
        log_printf("clock_gettime(): %s", xstrerror(errno));
        return -1;
    }
    return timespec_to_msecs(ts_buf);
}

static int
parse_mkfsopts (
    struct bookmarkfs_conf_opt const *opts,
    struct parsed_mkfsopts           *parsed_opts
) {
    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("date_added") {
        BACKEND_OPT_VAL_START
        char *end;
        int64_t val = strtoll(BACKEND_OPT_VAL_STR, &end, 10);
        if (*end == '\0' || val < 0 || val == LLONG_MAX) {
            return BACKEND_OPT_BAD_VAL();
        }
        parsed_opts->date_added = val;
    }
    BACKEND_OPT_END

    return 0;
}

static int
parse_mozurl_host (
    char const  *url,
    size_t       len,
    size_t      *prefix_len_ptr,
    char const **host_ptr,
    size_t      *host_len_ptr
) {
    UriUriA uri;
    char const *end = url + len;
    if (URI_SUCCESS != uriParseSingleUriExA(&uri, url, end, NULL)) {
        return -1;
    }
    bool has_authority = false;

    int status = -1;
    if (uri.scheme.first == NULL) {
        goto end;
    }

    char const *host_end = end;
    UriPathSegmentA *path = uri.pathHead;
    if (path != NULL) {
        host_end = path->text.first - 1;
    }
    if (uri.hostText.afterLast != NULL) {
        host_end = uri.hostText.afterLast;
        if (host_end < end && *host_end == ']') {
            ++host_end;
        }
    }
    if (uri.portText.afterLast != NULL) {
        host_end = uri.portText.afterLast;
        has_authority = true;
    }

    char const *host = host_end;
    if (uri.hostText.first != NULL) {
        host = uri.hostText.first;
        if (host[-1] == '[') {
            --host;
        }
        if (host_end - host > 0) {
            has_authority = true;
        }
    }

    char const *prefix_end = uri.scheme.afterLast + 1;
    if (uri.userInfo.first != NULL) {
        has_authority = true;
    }
    if (has_authority) {
        prefix_end += 2;
    }

    *prefix_len_ptr = prefix_end - url;
    *host_ptr       = host;
    *host_len_ptr   = host_end - host;
    status = 0;

  end:
    uriFreeUriMembersA(&uri);
    return status;
}

static int
parse_msecs (
    char const *str,
    size_t      str_len,
    int64_t    *msecs_ptr
) {
#define MAX_TIME_STR_LEN  19
    if (str_len > MAX_TIME_STR_LEN) {
        return -1;
    }
    char buf[MAX_TIME_STR_LEN + 1];
#undef MAX_TIME_STR_LEN
    memcpy(buf, str, str_len);
    buf[str_len] = '\0';

    char *end;
    int64_t msecs = strtoll(buf, &end, 10);
    if (*end == '\0' || msecs < 0 || msecs == LLONG_MAX) {
        return -1;
    }

    *msecs_ptr = msecs;
    return 0;
}

static int
store_new (
    sqlite3 *db,
    int64_t  date_added
) {
#define CREATE_TABLE(tbl, cols)  \
    { STR_WITHLEN("CREATE TABLE `moz_" tbl "` (" cols")") }
#define CREATE_INDEX_(u, tbl, idx, suff, cols)  \
    "CREATE " u "INDEX `moz_" tbl "_" idx suff "` ON `moz_" tbl "` (" cols ")"
#define CREATE_INDEX(tbl, idx, cols)  \
    { STR_WITHLEN(CREATE_INDEX_("", tbl, idx, "index", cols)) }
#define CREATE_UINDEX(tbl, idx, cols)  \
    { STR_WITHLEN(CREATE_INDEX_("UNIQUE ", tbl, idx, "_unique", cols)) }

    struct sql_withlen {
        char const *str;
        size_t      len;
    } const tables[] = {
        // moz_bookmarks
        CREATE_TABLE("bookmarks",
            "`id`"                " INTEGER PRIMARY KEY, "
            "`type`"              " INTEGER, "
            "`fk`"                " INTEGER DEFAULT NULL, "
            "`parent`"            " INTEGER, "
            "`position`"          " INTEGER, "
            "`title`"             " LONGVARCHAR, "
            "`keyword_id`"        " INTEGER, "
            "`folder_type`"       " TEXT, "
            "`dateAdded`"         " INTEGER, "
            "`lastModified`"      " INTEGER, "
            "`guid`"              " TEXT, "
            "`syncStatus`"        " INTEGER NOT NULL DEFAULT 0, "
            "`syncChangeCounter`" " INTEGER NOT NULL DEFAULT 1"
        ),
        CREATE_INDEX("bookmarks", "item", "`fk`, `type`"),
        CREATE_INDEX("bookmarks", "parent", "`parent`, `position`"),
        CREATE_INDEX("bookmarks", "itemlastmodified", "`fk`, `lastModified`"),
        CREATE_INDEX("bookmarks", "dateadded", "`dateAdded`"),
        CREATE_UINDEX("bookmarks", "guid", "`guid`"),
        // moz_origins
        CREATE_TABLE("origins",
            "`id`"                  " INTEGER PRIMARY KEY, "
            "`prefix`"              " TEXT NOT NULL, "
            "`host`"                " TEXT NOT NULL, "
            "`frecency`"            " INTEGER NOT NULL, "
            "`recalc_frecency`"     " INTEGER NOT NULL DEFAULT 0, "
            "`alt_frecency`"        " INTEGER, "
            "`recalc_alt_frecency`" " INTEGER NOT NULL DEFAULT 0, "
            "UNIQUE (`prefix`, `host`)"
        ),
        // moz_places
        CREATE_TABLE("places",
            "`id`"                  " INTEGER PRIMARY KEY, "
            "`url`"                 " LONGVARCHAR, "
            "`title`"               " LONGVARCHAR, "
            "`rev_host`"            " LONGVARCHAR, "
            "`visit_count`"         " INTEGER DEFAULT 0, "
            "`hidden`"              " INTEGER DEFAULT 0 NOT NULL, "
            "`typed`"               " INTEGER DEFAULT 0 NOT NULL, "
            "`frecency`"            " INTEGER DEFAULT -1 NOT NULL, "
            "`last_visit_date`"     " INTEGER, "
            "`guid`"                " TEXT, "
            "`foreign_count`"       " INTEGER DEFAULT 0 NOT NULL, "
            "`url_hash`"            " INTEGER DEFAULT 0 NOT NULL, "
            "`description`"         " TEXT, "
            "`preview_image_url`"   " TEXT, "
            "`site_name`"           " TEXT, "
            "`origin_id`"           " INTEGER REFERENCES `moz_origins`(`id`), "
            "`recalc_frecency`"     " INTEGER NOT NULL DEFAULT 0, "
            "`alt_frecency`"        " INTEGER, "
            "`recalc_alt_frecency`" " INTEGER NOT NULL DEFAULT 0"
        ),
        // moz_keywords
        CREATE_TABLE("keywords",
            "`id`"        " INTEGER PRIMARY KEY AUTOINCREMENT, "
            "`keyword`"   " TEXT UNIQUE, "
            "`place_id`"  " INTEGER, "
            "`post_data`" " TEXT"
        ),
        CREATE_UINDEX("keywords", "placepostdata", "`place_id`, `post_data`"),
    };
    for (size_t i = 0; i < sizeof(tables) / sizeof(struct sql_withlen); ++i) {
        struct sql_withlen const *sql = tables + i;

        if (0 != db_exec(db, sql->str, sql->len, NULL, NULL)) {
            return -1;
        }
    }

#define MOZBM_ROOT(id_, parent_, pos_, title_, guid_)  \
    {                                                  \
        .id        = (id_),                            \
        .parent_id = (parent_),                        \
        .pos       = (pos_),                           \
        .title     = (title_),                         \
        .title_len = strlen(title_),                   \
        .guid      = (guid_),                          \
    }
    struct mozbm const bmroots[] = {
        MOZBM_ROOT(1, 0, 0, "",        BOOKMARKS_ROOT_GUID),
        MOZBM_ROOT(2, 1, 0, "menu",    "menu________"),
        MOZBM_ROOT(3, 1, 1, "toolbar", "toolbar_____"),
        MOZBM_ROOT(4, 1, 2, "tags",    TAGS_ROOT_GUID),
        MOZBM_ROOT(5, 1, 3, "unfiled", "unfiled_____"),
        MOZBM_ROOT(6, 1, 4, "mobile",  "mobile______"),
    };
    char const *sql =
        "INSERT INTO `moz_bookmarks` (`id`, `type`, `parent`, `position`, "
            "`title`, `dateAdded`, `lastModified`, `guid`) "
        "VALUES (?1, 2, ?2, ?3, ?4, ?5, ?5, ?6)";
    sqlite3_stmt *stmt = db_prepare(db, sql, strlen(sql), true);
    if (unlikely(stmt == NULL)) {
        return -1;
    }
    int status = -1;

    for (size_t i = 0; i < sizeof(bmroots) / sizeof(struct mozbm); ++i) {
        struct mozbm const *bmroot = bmroots + i;

        struct db_stmt_bind_item const bind_items[] = {
            DB_QUERY_BIND_INT64(bmroot->id),
            DB_QUERY_BIND_INT64(bmroot->parent_id),
            DB_QUERY_BIND_INT64(bmroot->pos),
            DB_QUERY_BIND_TEXT(bmroot->title, bmroot->title_len),
            DB_QUERY_BIND_INT64(date_added),
            DB_QUERY_BIND_TEXT(bmroot->guid, GUID_STR_LEN),
        };
        size_t bind_cnt = DB_BIND_ITEMS_CNT(bind_items);
        if (0 != db_query(stmt, bind_items, bind_cnt, true, NULL, NULL)) {
            goto end;
        }
    }
    status = 0;

  end:
    sqlite3_finalize(stmt);
    return status;
}

static int
store_sync (
    sqlite3 *db
) {
    int err = sqlite3_wal_checkpoint(db, "main");
    if (err != SQLITE_OK) {
        log_printf("sqlite3_wal_checkpoint(): %s", sqlite3_errmsg(db));
        return -db_errno(err);
    }
    return 0;
}

static int
tag_entry_add (
    struct backend_ctx              *ctx,
    uint64_t                         parent_id,
    char const                      *name,
    size_t                           name_len,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    struct mozbm cols = {
        .id        = stat_buf->id,
        .parent_id = parent_id,
    };
    int status = mozbm_lookup_id(ctx, &cols);
    if (status < 0) {
        return status;
    }
    if (cols.place_id == 0) {
        return -EPERM;
    }
    status = tag_entry_lookup(ctx, &cols);
    if (status == 0) {
        return -EEXIST;
    }
    if (status != -ENOENT) {
        return status;
    }

    int64_t date_added = msecs_now(&stat_buf->mtime);
    if (unlikely(date_added < 0)) {
        return -EIO;
    }
    char guid_buf[GUID_STR_LEN];
    cols.date_added = date_added;
    cols.guid       = gen_random_guid(guid_buf);
    status = mozbm_insert(ctx, &cols);
    if (status < 0) {
        return status;
    }

    status = bookmark_do_lookup(ctx, parent_id, name, name_len,
            BOOKMARKFS_BOOKMARK_TYPE(TAG), stat_buf);
    if (status < 0) {
        if (status == ENOENT) {
            status = EPERM;
        }
        return status;
    }
    status = mozplace_addref_id(ctx, cols.place_id);
    if (status < 0) {
        return status;
    }
    status = mozbm_mtime_update(ctx, parent_id, &date_added);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
tag_entry_delete (
    struct backend_ctx *ctx,
    uint64_t            parent_id,
    char const         *name,
    size_t              name_len
) {
    struct bookmarkfs_bookmark_stat stat_buf;
    int status = bookmark_do_lookup(ctx, parent_id, name, name_len,
            BOOKMARKFS_BOOKMARK_TYPE(TAG), &stat_buf);
    if (status < 0) {
        return status;
    }

    struct mozbm cols = {
        .id        = stat_buf.id,
        .parent_id = parent_id,
    };
    status = mozbm_lookup_id(ctx, &cols);
    if (status < 0) {
        return status;
    }
    if (unlikely(cols.place_id == 0)) {
        // Bad bookmark file.  Tag entry should not be a directory.
        return -EIO;
    }
    status = tag_entry_lookup(ctx, &cols);
    if (status < 0) {
        return status;
    }

    status = mozbm_delete(ctx, cols.id, false);
    if (status < 0) {
        return status;
    }
    status = mozbm_mtime_update(ctx, parent_id, NULL);
    if (status < 0) {
        return status;
    }
    return 0;
}

static int
tag_entry_lookup (
    struct backend_ctx *ctx,
    struct mozbm       *cols
) {
    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_TAG_ENTRY_LOOKUP];
    char const *sql = "SELECT `id` FROM `moz_bookmarks` "
        "WHERE `parent` = ? AND `fk` = ? LIMIT 1";

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, db_query_i64_cb, &cols->id, nrows, , ,
        DB_QUERY_BIND_INT64(cols->parent_id),
        DB_QUERY_BIND_INT64(cols->place_id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ENOENT;
    }
    return 0;
}

static int64_t
timespec_to_msecs (
    struct timespec const *ts
) {
    if (ts->tv_nsec == UTIME_OMIT) {
        return -1;
    }
    int64_t microsecs = ts->tv_sec * 1000000 + ts->tv_nsec / 1000;

    if (microsecs < 0) {
        microsecs = 0;
    }
    return microsecs;
}

static int
txn_begin (
    struct backend_ctx *ctx
) {
    return db_txn_begin(ctx->db, &ctx->stmts[STMT_BEGIN]);
}

static int
txn_end (
    struct backend_ctx *ctx
) {
    int status = db_txn_commit(ctx->db, &ctx->stmts[STMT_COMMIT]);
    if (status < 0) {
        return txn_rollback(ctx, status);
    }
    return 0;
}

static int
txn_rollback (
    struct backend_ctx *ctx,
    int                 old_status
) {
    db_txn_rollback(ctx->db, &ctx->stmts[STMT_ROLLBACK]);
    return old_status;
}

#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */

static int
bookmark_do_get (
    struct backend_ctx      *ctx,
    uint64_t                 id,
    int                      attr_type,
    struct bookmark_get_ctx *qctx
) {
#define BOOKMARK_GET_(cols, join)  "SELECT CASE ? " cols "END "  \
    "FROM `moz_bookmarks` `b` " join "WHERE `b`.`id` = ?"
#define BOOKMARK_GET(cols)  BOOKMARK_GET_(cols  \
    "WHEN " STRINGIFY(ATTR_KEY_DATE_ADDED) " THEN `b`.`dateAdded` ", )
#define BOOKMARK_GET_EX  BOOKMARK_GET_(  \
        "WHEN " STRINGIFY(ATTR_KEY_NULL) " THEN `p`.`url` "          \
        "WHEN " STRINGIFY(ATTR_KEY_DESC) " THEN `p`.`description` "  \
    , "LEFT JOIN `moz_places` `p` ON `b`.`fk` = `p`.`id` ")
#define BOOKMARK_GET_WITH_GUID  \
    BOOKMARK_GET("WHEN " STRINGIFY(ATTR_KEY_GUID) " THEN `b`.`guid` ")
#define BOOKMARK_GET_WITH_TITLE  \
    BOOKMARK_GET("WHEN " STRINGIFY(ATTR_KEY_TITLE) " THEN `b`.`title` ")

    sqlite3_stmt **stmt_ptr = &ctx->stmts[STMT_BOOKMARK_GET_EX];
    char const    *sql      = BOOKMARK_GET_EX;
    if (attr_type >= ATTR_IN_MOZBM_START) {
        stmt_ptr = &ctx->stmts[STMT_BOOKMARK_GET];
        sql      = BOOKMARK_GET_WITH_GUID;
        if (ctx->flags & BACKEND_FILENAME_GUID) {
            sql = BOOKMARK_GET_WITH_TITLE;
        }
    }

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, bookmark_get_cb, qctx, nrows, ,
        {
            qctx->tags_root_id = ctx->tags_root_id;
        },
        DB_QUERY_BIND_INT64(attr_type),
        DB_QUERY_BIND_INT64(id),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ESTALE;
    }
    if (qctx->status < 0) {
        return qctx->status;
    }
    return 0;
}

static int
bookmark_do_list (
    struct backend_ctx       *ctx,
    uint64_t                  id,
    off_t                     off,
    uint32_t                  flags,
    struct bookmark_list_ctx *qctx
) {
#define BOOKMARK_LIST_(col, join)  \
    "SELECT `b`.`id`, `b`.`position`, " col "FROM `moz_bookmarks` `b` " join  \
    "WHERE `b`.`parent` = ? AND `b`.`position` >= ? ORDER BY `b`.`position`"
#define BOOKMARK_LIST_NOJOIN_(col)  BOOKMARK_LIST_(col ", `b`.`fk` ", )
#define BOOKMARK_LIST_EX_COLS_  \
    "length(`p`.`url`), `b`.`lastModified`, `p`.`last_visit_date`"
#define BOOKMARK_LIST_EX_(col)  \
    BOOKMARK_LIST_(col ", " BOOKMARK_LIST_EX_COLS_ " ",  \
        "LEFT JOIN `moz_places` `p` ON `b`.`fk` = `p`.`id` ")
#define BOOKMARK_LIST_TITLE     BOOKMARK_LIST_NOJOIN_("`b`.`title`")
#define BOOKMARK_LIST_GUID      BOOKMARK_LIST_NOJOIN_("`b`.`guid`")
#define BOOKMARK_LIST_TITLE_EX  BOOKMARK_LIST_EX_("`b`.`title`")
#define BOOKMARK_LIST_GUID_EX   BOOKMARK_LIST_EX_("`b`.`guid`")

#define BOOKMARK_COL_BY_FK_(col)  \
    "SELECT `t`.`" col "` FROM `moz_bookmarks` `t` "  \
    "WHERE `t`.`fk` = `b`.`fk` ORDER BY `t`.`id` LIMIT 1"
#define BOOKMARK_LIST_TAG_TITLE  \
    BOOKMARK_LIST_NOJOIN_("(" BOOKMARK_COL_BY_FK_("title") ")")
#define BOOKMARK_LIST_TAG_GUID  \
    BOOKMARK_LIST_NOJOIN_("(" BOOKMARK_COL_BY_FK_("guid") ")")
#define BOOKMARK_LIST_TAG_TITLE_EX  \
    BOOKMARK_LIST_EX_("(" BOOKMARK_COL_BY_FK_("title") ")")
#define BOOKMARK_LIST_TAG_GUID_EX  \
    BOOKMARK_LIST_EX_("(" BOOKMARK_COL_BY_FK_("guid") ")")

#define BOOKMARK_LIST_KEYWORD_(cols, join)  \
    "SELECT min(`b`.`id`), `k`.`id`, `k`.`keyword`" cols " " \
    "FROM `moz_keywords` `k` " join  \
    "JOIN `moz_bookmarks` `b` ON `k`.`place_id` = `b`.`fk` "  \
    "WHERE `k`.`id` >= ?2 GROUP BY `k`.`place_id` ORDER BY `k`.`id`"
#define BOOKMARK_LIST_KEYWORD     BOOKMARK_LIST_KEYWORD_(,)
#define BOOKMARK_LIST_KEYWORD_EX  \
    BOOKMARK_LIST_KEYWORD_(", " BOOKMARK_LIST_EX_COLS_,  \
        "JOIN `moz_places` `p` ON `k`.`place_id` = `p`.`id` ")

    uint32_t bookmark_type = flags & BOOKMARKFS_BOOKMARK_TYPE_MASK;
    bookmark_type >>= BOOKMARKFS_BOOKMARK_TYPE_SHIFT;
    if (bookmark_type == BOOKMARKFS_BOOKMARK_TYPE_TAG
            && id == ctx->tags_root_id)
    {
        bookmark_type = BOOKMARKFS_BOOKMARK_TYPE_BOOKMARK;
    }

    bool with_stat = flags & BOOKMARK_FLAG(LIST_WITHSTAT);
    int stmt_idx_table[3][2] = {
        { STMT_BOOKMARK_LIST,         STMT_BOOKMARK_LIST_EX         },
        { STMT_BOOKMARK_LIST_TAG,     STMT_BOOKMARK_LIST_TAG_EX     },
        { STMT_BOOKMARK_LIST_KEYWORD, STMT_BOOKMARK_LIST_KEYWORD_EX },
    };
    sqlite3_stmt **stmt_ptr
        = &ctx->stmts[stmt_idx_table[bookmark_type][with_stat]];

    bool filename_is_guid = ctx->flags & BACKEND_FILENAME_GUID;
    char const *sql_table[3][2][2] = {
        { { BOOKMARK_LIST_TITLE,     BOOKMARK_LIST_TITLE_EX     },
          { BOOKMARK_LIST_GUID,      BOOKMARK_LIST_GUID_EX      }, },
        { { BOOKMARK_LIST_TAG_TITLE, BOOKMARK_LIST_TAG_TITLE_EX },
          { BOOKMARK_LIST_TAG_GUID,  BOOKMARK_LIST_TAG_GUID_EX  }, },
        { { BOOKMARK_LIST_KEYWORD,   BOOKMARK_LIST_KEYWORD_EX   },
          { BOOKMARK_LIST_KEYWORD,   BOOKMARK_LIST_KEYWORD_EX   }, },
    };
    char const *sql = sql_table[bookmark_type][filename_is_guid][with_stat];

    ssize_t nrows;
    DO_QUERY(ctx, stmt_ptr, sql, qctx->row_func, qctx, nrows, ,
        {
            bool name_distinct = filename_is_guid
                || !BOOKMARKFS_BOOKMARK_IS_TYPE(flags, BOOKMARK)
                || (ctx->flags & BACKEND_ASSUME_TITLE_DISTINCT);

            qctx->tags_root_id = ctx->tags_root_id;
            qctx->check_name   = !name_distinct;
            qctx->with_stat    = with_stat;
        },
        DB_QUERY_BIND_INT64(id),
        DB_QUERY_BIND_INT64(off),
    );
    if (nrows < 0) {
        return nrows;
    }
    return qctx->status;
}

static int
bookmark_do_lookup (
    struct backend_ctx              *ctx,
    uint64_t                         id,
    char const                      *name,
    size_t                           name_len,
    uint32_t                         flags,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
#define BOOKMARK_LOOKUP_(join, where)  \
    "SELECT `b`.`id`, `b`.`lastModified`, "  \
        "`p`.`last_visit_date`, length(`p`.`url`) "  \
    "FROM `moz_bookmarks` `b` "  \
    join "JOIN `moz_places` `p` ON `b`.`fk` = `p`.`id` WHERE " where
#define BOOKMARK_LOOKUP_ASSOC_(col)  \
    BOOKMARK_LOOKUP_("LEFT ", "`b`.`parent` = ? AND `b`.`" col "` = ? ")  \
    "ORDER BY `b`.`position` LIMIT 1"
#define BOOKMARK_LOOKUP_ID_(join, val)  \
    BOOKMARK_LOOKUP_(join, "`b`.`id` = " val " ")
#define BOOKMARK_LOOKUP_ID     BOOKMARK_LOOKUP_ID_("LEFT ", "?")
#define BOOKMARK_LOOKUP_GUID   BOOKMARK_LOOKUP_ASSOC_("guid")
#define BOOKMARK_LOOKUP_TITLE  BOOKMARK_LOOKUP_ASSOC_("title")

#define BOOKMARK_TAG_BY_PARENT_  \
    "SELECT `fk` FROM `moz_bookmarks` WHERE `parent` = ?"
#define BOOKMARK_ID_BY_TAG_(idx, col)  \
    "SELECT `id` FROM `moz_bookmarks` "  \
    "INDEXED BY `moz_bookmarks_" idx "index` "  \
    "WHERE `fk` IN (" BOOKMARK_TAG_BY_PARENT_ ") AND `" col "` = ? "  \
    "ORDER BY `id` LIMIT 1"
#define BOOKMARK_LOOKUP_TAG_(idx, col)  \
    BOOKMARK_LOOKUP_ID_(, "(" BOOKMARK_ID_BY_TAG_(idx, col) ")")
#define BOOKMARK_LOOKUP_TAG_GUID   BOOKMARK_LOOKUP_TAG_("guid_unique", "guid")
#define BOOKMARK_LOOKUP_TAG_TITLE  BOOKMARK_LOOKUP_TAG_("item", "title")

#define PLACE_ID_BY_KEYWORD(val)  \
    "SELECT `place_id` FROM `moz_keywords` WHERE `keyword` = " val
#define BOOKMARK_LOOKUP_PLACE_ID_(val)  \
    BOOKMARK_LOOKUP_(, "`b`.`fk` = " val " ORDER BY `b`.`id` LIMIT 1")
#define BOOKMARK_LOOKUP_KEYWORD_(val)  \
    BOOKMARK_LOOKUP_PLACE_ID_("(" PLACE_ID_BY_KEYWORD(val) ")")
#define BOOKMARK_LOOKUP_KEYWORD  BOOKMARK_LOOKUP_KEYWORD_("?2")

    int         stmt_idx = STMT_BOOKMARK_LOOKUP_ID;
    char const *sql      = BOOKMARK_LOOKUP_ID;
    if (name == NULL) {
        if (BOOKMARKFS_BOOKMARK_IS_TYPE(flags, KEYWORD)) {
            *stat_buf = (struct bookmarkfs_bookmark_stat) {
                .value_len = -1,
            };
            return 0;
        }
        goto query;
    }

    bool filename_is_guid = ctx->flags & BACKEND_FILENAME_GUID;
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
    if (filename_is_guid && (flags & BOOKMARK_FLAG(LOOKUP_VALIDATE_GUID))) {
        if (!is_valid_guid(name, name_len)) {
            return -EPERM;
        }
    }
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */

    uint32_t bookmark_type = flags & BOOKMARKFS_BOOKMARK_TYPE_MASK;
    switch (bookmark_type) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (id != ctx->tags_root_id) {
            stmt_idx = STMT_BOOKMARK_LOOKUP_TAG_ASSOC;
            break;
        }
        bookmark_type = BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK);
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK):
        stmt_idx = STMT_BOOKMARK_LOOKUP_ASSOC;
        break;

      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        stmt_idx = STMT_BOOKMARK_LOOKUP_KEYWORD;
        break;
    }
    bookmark_type >>= BOOKMARKFS_BOOKMARK_TYPE_SHIFT;

    char const *sql_table[3][2] = {
        { BOOKMARK_LOOKUP_TITLE,     BOOKMARK_LOOKUP_GUID      },
        { BOOKMARK_LOOKUP_TAG_TITLE, BOOKMARK_LOOKUP_TAG_GUID  },
        { BOOKMARK_LOOKUP_KEYWORD,   BOOKMARK_LOOKUP_KEYWORD   },
    };
    sql = sql_table[bookmark_type][filename_is_guid];

  query:  ;
    sqlite3_stmt **stmt_ptr = &ctx->stmts[stmt_idx];

    ssize_t nrows;
    struct bookmark_lookup_ctx qctx;
    DO_QUERY(ctx, stmt_ptr, sql, bookmark_lookup_cb, &qctx, nrows, ,
        {
            qctx.tags_root_id = name == NULL ? UINT64_MAX : ctx->tags_root_id;
            qctx.stat_buf     = stat_buf;
        },
        DB_QUERY_BIND_INT64(id),
        DB_QUERY_BIND_TEXT(name, name_len),
    );
    if (nrows < 0) {
        return nrows;
    }
    if (nrows == 0) {
        return -ENOENT;
    }
    int status = qctx.status;
    if (status == -ENOENT && name == NULL) {
        status = -ESTALE;
    }
    return status;
}

static int
bookmark_fsck_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct bookmark_list_ctx *ctx = user_data;
    debug_assert(ctx->check_name);

    int64_t id = sqlite3_column_int64(stmt, 0);
    if (unlikely(!is_valid_id(id))) {
        log_printf("bad bookmark ID %" PRIi64, id);
        goto fail;
    }
    if (unlikely((uint64_t)id == ctx->tags_root_id)) {
        return 0;
    }

    int64_t position = sqlite3_column_int64(stmt, 1);
    if (unlikely(position < 0)) {
        goto fail;
    }
    ctx->next = position + 1;

    size_t      name_len = sqlite3_column_bytes(stmt, 2);
    char const *name     = (char const *)sqlite3_column_text(stmt, 2);
    if (unlikely(name == NULL)) {
        name = "";
    }
    uint64_t extra;
    int      result;
    if (0 != validate_filename_fsck(name, name_len, &result, &extra)) {
        // Only valid names shall be inserted into dentry map.
        goto found;
    }

    struct hashmap *map = ctx->dentry_map;
    if (map == NULL) {
        return 0;
    }
    union hashmap_key key = {
        .ptr = &(struct bookmark_name_key) {
            .val = name,
            .len = name_len,
        },
    };
    unsigned long hashcode = hash_digest(name, name_len);
    struct bookmark_dentry *dentry = hashmap_search(map, key, hashcode, NULL);
    if (dentry == NULL) {
        dentry = xmalloc(sizeof(*dentry) + name_len);
        dentry->id       = id;
        dentry->name_len = name_len;
        memcpy(dentry->name, name, name_len);

        if (map == NULL) {
            map = hashmap_create(dentmap_comp, dentmap_hash);
            ctx->dentry_map = map;
        }
        *hashmap_insert(map, key, hashcode) = dentry;
        return 0;
    }
    if (dentry->id == (uint64_t)id) {
        return 0;
    }
    extra  = dentry->id;
    result = BOOKMARKFS_FSCK_RESULT_NAME_DUPLICATE;

  found:
    ctx->status = ctx->callback.fsck(ctx->user_data, result, id, extra, name);
    return ctx->status;

  fail:
    ctx->status = -EIO;
    return 1;
}

static int
bookmark_get_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct bookmark_get_ctx *ctx = user_data;

    size_t      len = sqlite3_column_bytes(stmt, 0);
    char const *val = (char const *)sqlite3_column_text(stmt, 0);

    ctx->status = ctx->callback(ctx->user_data, val == NULL ? "" : val, len);
    return 1;
}

static int
bookmark_list_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct bookmark_list_ctx *ctx = user_data;

    struct bookmarkfs_bookmark_entry entry;

    int64_t id = sqlite3_column_int64(stmt, 0);
    if (unlikely(!is_valid_id(id))) {
        log_printf("bad bookmark ID %" PRIi64, id);
        goto fail;
    }
    if (unlikely((uint64_t)id == ctx->tags_root_id)) {
        return 0;
    }
    entry.stat.id = id;

    int64_t position = sqlite3_column_int64(stmt, 1);
    if (unlikely(position < 0)) {
        goto fail;
    }
    entry.next = position + 1;

    ssize_t len = -1;
    if (SQLITE_INTEGER == sqlite3_column_type(stmt, 3)) {
        len = sqlite3_column_int64(stmt, 3);
        if (unlikely(len < 0)) {
            goto fail;
        }
    }
    entry.stat.value_len = len;

    size_t      name_len = sqlite3_column_bytes(stmt, 2);
    char const *name     = (char const *)sqlite3_column_text(stmt, 2);
    if (unlikely(name == NULL)) {
        name = "";
    }
    if (0 != validate_filename(name, name_len, NULL)) {
        debug_printf("bad bookmark name '%s' (ID: %" PRIi64 ")", name, id);
        return 0;
    }

    if (!ctx->check_name) {
        goto set_name;
    }

    struct hashmap *map = ctx->dentry_map;
    if (map == NULL) {
        map = hashmap_create(dentmap_comp, dentmap_hash);
        ctx->dentry_map = map;
    }

    union hashmap_key key = {
        .ptr = &(struct bookmark_name_key) {
            .val = name,
            .len = name_len,
        },
    };
    unsigned long hashcode = hash_digest(name, name_len);
    struct bookmark_dentry *dentry = hashmap_search(map, key, hashcode, NULL);
    if (dentry != NULL) {
        if (dentry->id == (uint64_t)id) {
            goto set_name;
        }
        debug_printf("duplicate bookmark name '%s' (ID: %" PRIi64 ")",
                name, id);
        return 0;
    }

  set_name:
    entry.name = name;

    if (ctx->with_stat) {
        msecs_to_timespec(&entry.stat.mtime, sqlite3_column_int64(stmt, 4));
        msecs_to_timespec(&entry.stat.atime, sqlite3_column_int64(stmt, 5));
    }

    ctx->status = ctx->callback.list(ctx->user_data, &entry);
    if (ctx->status == 0 && ctx->check_name && dentry == NULL) {
        dentry = xmalloc(sizeof(*dentry) + name_len);
        dentry->id       = id;
        dentry->name_len = name_len;
        memcpy(dentry->name, name, name_len);

        *hashmap_insert(map, key, hashcode) = dentry;
    }
    return ctx->status;

  fail:
    ctx->status = -EIO;
    return 1;
}

static int
bookmark_lookup_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct bookmark_lookup_ctx *ctx = user_data;

    struct bookmarkfs_bookmark_stat *stat_buf = ctx->stat_buf;

    int64_t id = sqlite3_column_int64(stmt, 0);
    if (unlikely(!is_valid_id(id))) {
        ctx->status = -EIO;
        return 1;
    }
    if (unlikely((uint64_t)id == ctx->tags_root_id)) {
        ctx->status = -ENOENT;
        return 1;
    }
    stat_buf->id = id;

    msecs_to_timespec(&stat_buf->mtime, sqlite3_column_int64(stmt, 1));
    msecs_to_timespec(&stat_buf->atime, sqlite3_column_int64(stmt, 2));

    ssize_t len = -1;
    if (SQLITE_INTEGER == sqlite3_column_type(stmt, 3)) {
        len = sqlite3_column_int64(stmt, 3);
        if (unlikely(len < 0)) {
            ctx->status = -EIO;
            return 1;
        }
    }
    stat_buf->value_len = len;
    ctx->status = 0;
    return 1;
}

static int
dentmap_comp (
    union hashmap_key  key,
    void const        *entry
) {
    struct bookmark_name_key const *name   = key.ptr;
    struct bookmark_dentry const   *dentry = entry;

    if (name->len != dentry->name_len) {
        return -1;
    }
    return memcmp(name->val, dentry->name, name->len);
}

static unsigned long
dentmap_hash (
    void const *entry
) {
    struct bookmark_dentry const *dentry = entry;

    return hash_digest(dentry->name, dentry->name_len);
}

static void
free_blcookie (
    struct bookmark_lcookie *cookie
) {
    free_dentmap(cookie->dentry_map);
    free(cookie);
}

static void
free_dentmap (
    struct hashmap *dentry_map
) {
    if (dentry_map == NULL) {
        return;
    }

    hashmap_foreach(dentry_map, free_dentmap_entry, NULL);
    hashmap_destroy(dentry_map);
}

static void
free_dentmap_entry (
    void *entry,
    void *UNUSED_VAR(user_data)
) {
    struct bookmark_dentry *dentry = entry;

    free(dentry);
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
    if (0 == strcmp("description", key)) {
        return ATTR_KEY_DESC;
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

static int64_t
get_data_version (
    struct backend_ctx *ctx
) {
    int64_t data_version = -1;
    ssize_t nrows = db_exec(ctx->db, SQL_PRAGMA("data_version"),
            &ctx->stmts[STMT_DATA_VERSION], &data_version);
    if (nrows < 0) {
        return nrows;
    }
    if (unlikely(data_version < 0)) {
        log_printf("bad data version %" PRIi64 ", should be of uint32 value",
                data_version);
    }
    return data_version;
}

static bool
is_valid_id (
    int64_t id
) {
    if (id <= 0) {
        return false;
    }
    if ((uint64_t)id > BOOKMARKFS_MAX_ID) {
        return false;
    }
    return true;
}

static void
msecs_to_timespec (
    struct timespec *ts_buf,
    int64_t          microsecs
) {
    if (unlikely(microsecs < 0)) {
        microsecs = 0;
    }

    ts_buf->tv_sec  = microsecs / 1000000;
    ts_buf->tv_nsec = (microsecs % 1000000) * 1000;
}

static int
parse_mntopts (
    struct bookmarkfs_conf_opt const *opts,
    uint32_t                          flags,
    struct parsed_mntopts            *parsed_opts
) {
    unsigned backend_flags = BACKEND_EXCLUSIVE_LOCK;
    if (flags & BOOKMARKFS_BACKEND_FSCK_ONLY) {
        goto end;
    }
    if (flags & BOOKMARKFS_BACKEND_READONLY) {
        backend_flags = 0;
    }

    BACKEND_OPT_START(opts)
    BACKEND_OPT_KEY("filename") {
        BACKEND_OPT_VAL_START
        BACKEND_OPT_VAL("guid") {
            backend_flags |= BACKEND_FILENAME_GUID;
        }
        BACKEND_OPT_VAL("title") {
            backend_flags &= ~BACKEND_FILENAME_GUID;
        }
        BACKEND_OPT_VAL_END
    }
    BACKEND_OPT_KEY("lock") {
        BACKEND_OPT_VAL_START
        BACKEND_OPT_VAL("exclusive") {
            backend_flags |= BACKEND_EXCLUSIVE_LOCK;
        }
        BACKEND_OPT_VAL("normal") {
            backend_flags &= ~BACKEND_EXCLUSIVE_LOCK;
        }
        BACKEND_OPT_VAL_END
    }
    BACKEND_OPT_KEY("assume_title_distinct") {
        BACKEND_OPT_NO_VAL
        backend_flags |= BACKEND_ASSUME_TITLE_DISTINCT;
    }
    BACKEND_OPT_END

  end:
    parsed_opts->flags = backend_flags;
    return 0;
}

static void
print_help (
    uint32_t flags
) {
    char const *options = "";
    if (flags & BOOKMARKFS_FRONTEND_MOUNT) {
        options = "Options:\n"
            "  filename=<title/guid>      Bookmark file name origin\n"
            "  lock=<exclusive/normal>    Database connection locking mode\n"
            "  assume_title_distinct\n"
            "\n";
    } else if (flags & BOOKMARKFS_FRONTEND_MKFS) {
        options = "Options:\n"
            "  date_added=<timestamp>    Override date_added attribute\n"
            "\n";
    }
    printf("Firefox backend for BookmarkFS\n\n"
            "%s"
            "Run 'info bookmarkfs' for more information.\n\n"
            "Project homepage: <" BOOKMARKFS_HOMEPAGE_URL ">.\n", options);
}

static void
print_version (void)
{
    printf("bookmarkfs-backend-firefox %d.%d.%d\n",
            BOOKMARKFS_VER_MAJOR, BOOKMARKFS_VER_MINOR, BOOKMARKFS_VER_PATCH);
    puts(BOOKMARKFS_FEATURE_STRING(DEBUG,                 "debug"));
    puts(BOOKMARKFS_FEATURE_STRING(BACKEND_FIREFOX_WRITE, "write"));

    bookmarkfs_print_lib_version("\n");
}

static int
store_init (
    sqlite3  *db,
    uint64_t *bookmarks_root_id_ptr,
    uint64_t *tags_root_id_ptr
) {
    int status = db_check(db);
    if (status < 0) {
        return status;
    }
    status = -EIO;

    char const *sql = "SELECT `id` FROM `moz_bookmarks` WHERE `guid` = ?";
    sqlite3_stmt *stmt = db_prepare(db, sql, strlen(sql), false);
    if (unlikely(stmt == NULL)) {
        return status;
    }

    struct store_check_args {
        struct db_stmt_bind_item bind_item;
        uint64_t                 id;
    } check_args[] = {
        { .bind_item = DB_QUERY_BIND_TEXT(BOOKMARKS_ROOT_GUID, GUID_STR_LEN) },
        { .bind_item = DB_QUERY_BIND_TEXT(TAGS_ROOT_GUID,      GUID_STR_LEN) },
    };
    size_t num_args = sizeof(check_args) / sizeof(struct store_check_args);

    for (size_t idx = 0; idx < num_args; ++idx) {
        struct store_check_args *args = check_args + idx;

        ssize_t nrows = db_query(stmt, &args->bind_item, 1, true,
                store_check_cb, &args->id);
        if (nrows <= 0) {
            if (nrows == 0) {
                log_puts("bookmark root not found");
            }
            goto end;
        }
    }

    if (!is_valid_id(check_args[0].id) || !is_valid_id(check_args[1].id)) {
        goto end;
    }
    *bookmarks_root_id_ptr = check_args[0].id;
    *tags_root_id_ptr      = check_args[1].id;
    status = 0;

  end:
    sqlite3_finalize(stmt);
    return status;
}

static int
store_check_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    int64_t *id_ptr = user_data;

    *id_ptr = sqlite3_column_int64(stmt, 0);
    return 1;
}

static int
backend_create (
    struct bookmarkfs_backend_conf const *conf,
    struct bookmarkfs_backend_init_resp  *resp
) {
    bool readonly = conf->flags & BOOKMARKFS_BACKEND_READONLY;
    if (!readonly) {
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
        int minver = 3035000;  // required for the RETURNING clause
        int vernum = sqlite3_libversion_number();
        if (vernum < minver) {
            log_printf("sqlite version too low (%d<%d)", vernum, minver);
            return -1;
        }
#else  /* !defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */
        log_puts("write support is not enabled on this build");
        return -1;
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */
    }

    struct parsed_mntopts opts = { 0 };
    if (0 != parse_mntopts(conf->opts, conf->flags, &opts)) {
        return -1;
    }

    sqlite3 *db = db_open(conf->store_path, readonly);
    if (db == NULL) {
        return -1;
    }

    struct db_conf_item const conf_items[] = {
        { SQLITE_DBCONFIG_DEFENSIVE,      1         },
        // Trigger is required for foreign keys to work
        { SQLITE_DBCONFIG_ENABLE_TRIGGER, !readonly },
        { SQLITE_DBCONFIG_ENABLE_VIEW,    0         },
        { SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0         },
    };
    if (0 != db_config(db, conf_items, DB_CONFIG_ITEMS_CNT(conf_items))) {
        goto close_db;
    }

    struct db_pragma_item pragmas[5] = {
        SQL_PRAGMA_ITEM("locking_mode", "normal"),
        SQL_PRAGMA_ITEM("journal_mode", "wal"),
        SQL_PRAGMA_ITEM("synchronous",  "1"),
    };
    if (opts.flags & BACKEND_EXCLUSIVE_LOCK) {
        pragmas[0] = SQL_PRAGMA_ITEM("locking_mode", "exclusive");
    }
    if (!(conf->flags & BOOKMARKFS_BACKEND_NO_SANDBOX)) {
        // Cannot use file as temp store in sandbox mode,
        // since SQLite does not use *at() for filesystem operations.
        pragmas[3] = SQL_PRAGMA_ITEM("temp_store", "2");
    }
    if (!readonly) {
        // moz_places_extra, moz_places_metadata, ...
        pragmas[4] = SQL_PRAGMA_ITEM("foreign_keys", "1");
    }
    if (0 != db_pragma(db, pragmas, DB_PRAGMA_ITEMS_CNT(pragmas))) {
        goto close_db;
    }

    uint64_t bookmarks_root_id = UINT64_MAX;
    uint64_t tags_root_id     = UINT64_MAX;
    if (conf->flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        // Defer initialization in sandbox mode, so that
        // user-provided data is only read after entering sandbox.
        if (0 != store_init(db, &bookmarks_root_id, &tags_root_id)) {
            goto close_db;
        }
    } else {
        // Persist -wal and -shm files in sandbox mode,
        // since we're unable to unlink them.
        if (0 != db_fcntl(db, SQLITE_FCNTL_PERSIST_WAL, 1)) {
            goto close_db;
        }
    }

    if (0 != db_register_safeincr(db)) {
        goto close_db;
    }

    struct backend_ctx *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct backend_ctx) {
        .db                = db,
        .bookmarks_root_id = bookmarks_root_id,
        .tags_root_id      = tags_root_id,
        .flags             = conf->flags | opts.flags,
    };

    uint32_t resp_flags = BOOKMARKFS_BACKEND_HAS_KEYWORD;
    if (opts.flags & BACKEND_EXCLUSIVE_LOCK) {
        resp_flags |= BOOKMARKFS_BACKEND_EXCLUSIVE;
    }

    char const *bookmark_attrs = "guid\0date_added\0description\0";
    if (opts.flags & BACKEND_FILENAME_GUID) {
        bookmark_attrs = "title\0date_added\0description\0";
    }

    resp->name              = "firefox";
    resp->backend_ctx       = ctx;
    resp->bookmarks_root_id = bookmarks_root_id;
    resp->tags_root_id      = tags_root_id;
    resp->bookmark_attrs    = bookmark_attrs;
    resp->flags             = resp_flags;
    return 0;

  close_db:
    sqlite3_close(db);
    return -1;
}

static void
backend_free (
    void *backend_ctx
) {
    struct backend_ctx *ctx = backend_ctx;
    if (ctx == NULL) {
        return;
    }

    int end = PERSISTED_STMT_END;
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
    if (ctx->flags & BOOKMARKFS_BACKEND_READONLY) {
        end = PERSISTED_STMT_WRITE_START;
    }
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */
    for (int idx = 0; idx < end; ++idx) {
        sqlite3_stmt *stmt = ctx->stmts[idx];
        if (stmt != NULL) {
            sqlite3_finalize(stmt);
        }
    }

#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
    if (!(ctx->flags & BOOKMARKFS_BACKEND_READONLY)) {
        store_sync(ctx->db);
    }
#endif
    sqlite3_close(ctx->db);
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
    if (!(flags & BOOKMARKFS_BACKEND_LIB_READY)
            && !(flags & BOOKMARKFS_FRONTEND_MKFS)
    ) {
        if (0 != bookmarkfs_lib_init()) {
            return -1;
        }
    }

    // If you wish to use the backend in a multi-threaded environment,
    // change the config to SQLITE_CONFIG_MULTITHREAD,
    // thus it is MT-Safe as long as functions are never called concurrently
    // with the same `backend_ctx`.
    int status = sqlite3_config(SQLITE_CONFIG_SINGLETHREAD);
    if (unlikely(status != SQLITE_OK)) {
        log_printf("sqlite3_config(): %s", sqlite3_errstr(status));
        return -1;
    }

    return 0;
}

static int
backend_sandbox (
    void                                *backend_ctx,
    int                                  fusefd,
    struct bookmarkfs_backend_init_resp *resp
) {
    struct backend_ctx *ctx = backend_ctx;

    if (ctx->flags & BOOKMARKFS_BACKEND_NO_SANDBOX) {
        return 0;
    }

    // Currently there is no way to retrieve the file descriptors of the
    // open database/-wal/-shm/... files using the SQLite3 public API,
    // thus we're unable to exert fine-grained control over their capabilities.
    if (unlikely(0 != sandbox_enter(fusefd, -1, 0))) {
        return -1;
    }

    // Deferred db init (see backend_create()).
    // Processing untrusted data before entering sandbox is a potential
    // vulnerability, and should be avoided if possible.
    if (0 != store_init(ctx->db, &ctx->bookmarks_root_id,
                &ctx->tags_root_id)
    ) {
        return -1;
    }
    resp->bookmarks_root_id = ctx->bookmarks_root_id;
    resp->tags_root_id      = ctx->tags_root_id;
    return 0;
}

static int
bookmark_fsck (
    void                               *backend_ctx,
    uint64_t                            id,
    struct bookmarkfs_fsck_data const  *fsck_data,
    uint32_t                            flags,
    bookmarkfs_bookmark_fsck_cb        *callback,
    void                               *user_data,
    void                              **cookie_ptr
) {
    struct backend_ctx *ctx = backend_ctx;

    if (ctx->flags & BACKEND_FILENAME_GUID) {
        return -EPERM;
    }
    switch (flags & BOOKMARKFS_BOOKMARK_TYPE_MASK) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (id == ctx->tags_root_id) {
            break;
        }
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        // TODO: support keyword fsck
        return -EPERM;
    }

    struct hashmap *dentry_map = NULL;
    struct bookmark_lcookie *cookie;
    size_t idx = 0;
    if (cookie_ptr != NULL) {
        cookie = *cookie_ptr;
        if (cookie != NULL) {
            dentry_map = cookie->dentry_map;
            idx        = cookie->idx;
        }
    }

    int status = 0;
    if (callback == NULL) {
        // fsck_rewind
        free_dentmap(dentry_map);
        dentry_map = NULL;
        idx = 0;
        goto end;
    }

    struct bookmark_list_ctx qctx;
    qctx.dentry_map    = dentry_map;
    qctx.next          = idx;
    qctx.row_func      = bookmark_fsck_cb;
    qctx.callback.fsck = callback;
    qctx.user_data     = user_data;
    if (fsck_data == NULL) {
        qctx.status = 0;
        status = bookmark_do_list(ctx, id, idx, flags, &qctx);
    } else {
        debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));
#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
        status = fsck_apply(ctx, id, fsck_data, &qctx);
#endif
    }
    if (status < 0) {
        if (dentry_map == NULL) {
            free_dentmap(qctx.dentry_map);
        }
        return status;
    }
    dentry_map = qctx.dentry_map;
    idx        = qctx.next;

  end:
    if (cookie_ptr != NULL) {
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            *cookie_ptr = cookie;
        }
        cookie->dentry_map = dentry_map;
        cookie->idx        = idx;
    } else {
        free_dentmap(dentry_map);
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

    int attr_type = get_attr_type(attr_key, ctx->flags);
    if (attr_type < 0) {
        return -ENOATTR;
    }

    if (cookie_ptr == NULL) {
        goto query;
    }
    int64_t data_version = get_data_version(ctx);
    if (data_version < 0) {
        return data_version;
    }
    struct bookmark_gcookie *cookie = *cookie_ptr;
    if (cookie != NULL) {
        if (cookie->data_version == data_version) {
            return -EAGAIN;
        }
    }

  query:  ;
    struct bookmark_get_ctx qctx;
    qctx.callback  = callback;
    qctx.user_data = user_data;
    int status = bookmark_do_get(ctx, id, attr_type, &qctx);
    if (status < 0) {
        return status;
    }

    if (cookie_ptr != NULL) {
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            *cookie_ptr = cookie;
        }
        cookie->data_version = data_version;
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

    struct hashmap *dentry_map = NULL;
    struct bookmark_lcookie *cookie;
    if (cookie_ptr != NULL) {
        cookie = *cookie_ptr;
        if (cookie != NULL) {
            dentry_map = cookie->dentry_map;
        }
    }

    int status = 0;
    if (callback == NULL) {
        goto end;
    }
    if (off == 0) {
        // rewinddir()
        free_dentmap(dentry_map);
        dentry_map = NULL;
    }

    struct bookmark_list_ctx qctx;
    qctx.dentry_map    = dentry_map;
    qctx.row_func      = bookmark_list_cb;
    qctx.callback.list = callback;
    qctx.user_data     = user_data;
    status = bookmark_do_list(ctx, id, off, flags, &qctx);
    if (status < 0) {
        if (dentry_map == NULL) {
            free_dentmap(qctx.dentry_map);
        }
        return status;
    }
    dentry_map = qctx.dentry_map;

  end:
    if (cookie_ptr != NULL) {
        if (cookie == NULL) {
            cookie = xmalloc(sizeof(*cookie));
            cookie->idx = 0;
            *cookie_ptr = cookie;
        }
        cookie->dentry_map = dentry_map;
    } else {
        free_dentmap(dentry_map);
    }
    return status;
}

static int
bookmark_lookup (
    void                            *backend_ctx,
    uint64_t                         id,
    char const                      *name,
    uint32_t                         flags,
    struct bookmarkfs_bookmark_stat *stat_buf
) {
    struct backend_ctx *ctx = backend_ctx;

    size_t name_len = 0;
    if (name != NULL) {
        name_len = strlen(name);
    }
    return bookmark_do_lookup(ctx, id, name, name_len, flags, stat_buf);
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
        free(object);
        break;

      case BOOKMARKFS_OBJECT_TYPE_BLCOOKIE:
        free_blcookie(object);
        break;

      default:
        unreachable();
    }
}

#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE

static int
backend_mkfs (
    struct bookmarkfs_backend_conf const *conf
) {
    struct parsed_mkfsopts opts = {
        .date_added = -1,
    };
    if (0 != parse_mkfsopts(conf->opts, &opts)) {
        return -1;
    }
    if (opts.date_added < 0) {
        struct timespec now;
        opts.date_added = msecs_now(&now);
        if (unlikely(opts.date_added < 0)) {
            return -1;
        }
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

    // XXX: SQLite does not support opening a database file by fd.
    //
    // If any path component changes before the file actually gets opened,
    // we may be writing to a different file than the one `fd` represents.
    //
    // Do not bother with /dev/fd/*, since on Linux, that file is a
    // symbolic link, and SQLite choose to resolve it with readlink(2)
    // instead of directly open it (see unixFullPathname() in src/os_unix.c),
    // which does not solve the TOCTOU problem.  Also, /dev/fd is not portable
    // (e.g. FreeBSD does not mount fdescfs by default).
    //
    // Theoretically we could implement a "VFS shim" to workaround this
    // problem, but that does not seem to be worthwhile.
    //
    // See also:
    // - <https://sqlite.org/forum/forumpost/c15bf2e7df289a5f>
    // - <https://sqlite.org/forum/forumpost/680cd395b4bc97c6>
    sqlite3 *db = db_open(conf->store_path, false);
    if (db == NULL) {
        goto end;
    }

    struct db_pragma_item const pragmas[] = {
        SQL_PRAGMA_ITEM("locking_mode", "exclusive"),
        SQL_PRAGMA_ITEM("journal_mode", "memory"),
        SQL_PRAGMA_ITEM("synchronous",  "0"),
    };
    if (0 != db_pragma(db, pragmas, DB_PRAGMA_ITEMS_CNT(pragmas))) {
        goto end;
    }

    if (0 != db_txn_begin(db, NULL)) {
        goto end;
    }
    if (0 != store_new(db, opts.date_added)) {
        goto end;
    }
    status = db_txn_commit(db, NULL);

  end:
    sqlite3_close(db);
    if (status == 0) {
        status = xfsync(fd);
    }
    close(fd);
    return status;
}

static int
backend_sync (
    void *backend_ctx
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    return store_sync(ctx->db);
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
    debug_assert(name != NULL);

    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }

    size_t name_len = strlen(name);
    bool   is_dir   = flags & BOOKMARK_FLAG(CREATE_DIR);
    switch (flags & BOOKMARKFS_BOOKMARK_TYPE_MASK) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (!is_dir) {
            status = tag_entry_add(ctx, parent_id, name, name_len, stat_buf);
            break;
        }
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK):
        status = bookmark_do_create(ctx, parent_id, name, name_len, is_dir,
                stat_buf);
        break;

      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        status = keyword_create(ctx, name, name_len, stat_buf);
        break;

      default:
        unreachable();
    }
    if (status < 0) {
        return txn_rollback(ctx, status);
    }

    return txn_end(ctx);
}

static int
bookmark_delete (
    void       *backend_ctx,
    uint64_t    parent_id,
    char const *name,
    uint32_t    flags
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));
    debug_assert(name != NULL);

    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }

    size_t   name_len = strlen(name);
    bool     is_dir   = flags & BOOKMARK_FLAG(CREATE_DIR);
    switch (flags & BOOKMARKFS_BOOKMARK_TYPE_MASK) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (!is_dir) {
            status = tag_entry_delete(ctx, parent_id, name, name_len);
            break;
        }
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK):
        status = bookmark_do_delete(ctx, parent_id, name, name_len, is_dir);
        break;

      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        status = mozkw_delete(ctx, name, name_len);
        break;

      default:
        unreachable();
    }
    if (status < 0) {
        return txn_rollback(ctx, status);
    }

    return txn_end(ctx);
}

static int
bookmark_permute (
    void                     *backend_ctx,
    uint64_t                  parent_id,
    enum bookmarkfs_permd_op  op,
    char const               *name1,
    char const               *name2,
    uint32_t                  flags
) {
    struct backend_ctx *ctx = backend_ctx;
    debug_assert(!(ctx->flags & BOOKMARKFS_BACKEND_READONLY));

    switch (flags & BOOKMARKFS_BOOKMARK_TYPE_MASK) {
      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        if (parent_id == ctx->tags_root_id) {
            break;
        }
        // fallthrough
      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        return -EPERM;
    }

    size_t name1_len = strnlen(name1, NAME_MAX + 1);
    if (0 != validate_filename(name1, name1_len, NULL)) {
        return -EINVAL;
    }
    size_t name2_len = strnlen(name2, NAME_MAX + 1);
    if (0 != validate_filename(name2, name2_len, NULL)) {
        return -EINVAL;
    }

    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }

    struct mozbm cols1;
    status = mozbm_lookup(ctx, parent_id, name1, name1_len, false, &cols1);
    if (status < 0) {
        goto fail;
    }

    struct mozbm cols2;
    status = mozbm_lookup(ctx, parent_id, name2, name2_len, false, &cols2);
    if (status < 0) {
        goto fail;
    }
    if (cols1.id == cols2.id) {
        goto fail;
    }

    if (op == BOOKMARKFS_PERMD_OP_SWAP) {
        status = mozbm_pos_update(ctx, cols2.id, cols1.pos);
    } else {
        status = mozbm_pos_shift(ctx, parent_id, cols1.pos, &cols2.pos, op);
    }
    if (status <= 0) {
        goto fail;
    }

    status = mozbm_pos_update(ctx, cols1.id, cols2.pos);
    if (status < 0) {
        goto fail;
    }

    status = mozbm_mtime_update(ctx, parent_id, NULL);
    if (status < 0) {
        goto fail;
    }

    return txn_end(ctx);

  fail:
    return txn_rollback(ctx, status);
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

    int bookmark_type = flags & BOOKMARKFS_BOOKMARK_TYPE_MASK;
    switch (bookmark_type) {
      case BOOKMARKFS_BOOKMARK_TYPE(BOOKMARK):
        if (old_parent_id == ctx->bookmarks_root_id
                || new_parent_id == ctx->bookmarks_root_id
        ) {
            return -EPERM;
        }
        break;

      case BOOKMARKFS_BOOKMARK_TYPE(TAG):
        debug_assert(old_parent_id == ctx->tags_root_id);
        debug_assert(old_parent_id == new_parent_id);
        break;

      default:
        unreachable();
    }

    bool name_changed = true;
    size_t old_name_len = strlen(old_name);
    size_t new_name_len = strlen(new_name);
    if (old_name_len == new_name_len
            && 0 == memcmp(old_name, new_name, old_name_len)
    ) {
        if (old_parent_id == new_parent_id) {
            return 0;
        }
        name_changed = false;
    }

    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }

    switch (bookmark_type) {
      case BOOKMARKFS_BOOKMARK_TYPE(KEYWORD):
        if (name_changed) {
            status = mozkw_rename(ctx, old_name, new_name, flags);
            if (status < 0) {
                goto fail;
            }
        }
        goto end;
    }

    struct mozbm old_cols;
    status = mozbm_lookup(ctx, old_parent_id, old_name, old_name_len, false,
            &old_cols);
    if (status < 0) {
        goto fail;
    }

    struct mozbm new_cols;
    new_cols.pos = -1;
    status = mozbm_lookup(ctx, new_parent_id, new_name, new_name_len, true,
            &new_cols);
    if (status < 0) {
        // move
        if (status != -ENOENT) {
            goto fail;
        }
    } else {
        // replace
        if (flags & BOOKMARKFS_BOOKMARK_RENAME_NOREPLACE) {
            status = -EEXIST;
            goto fail;
        }
        if ((old_cols.place_id == 0) != (new_cols.place_id == 0)) {
            status = (old_cols.place_id == 0) ? -ENOTDIR : -EISDIR;
            goto fail;
        }
        status = mozbm_delete(ctx, new_cols.id, old_cols.place_id == 0);
        if (status < 0) {
            goto fail;
        }
    }

    status = mozbm_move(ctx, old_cols.id, new_parent_id, new_cols.pos,
            name_changed ? new_name : NULL, new_name_len);
    if (status < 0) {
        if (status == -EEXIST) {
            // duplicate GUID
            status = -EPERM;
        }
        goto fail;
    }

    int64_t mtime = -1;
    status = mozbm_mtime_update(ctx, old_parent_id, &mtime);
    if (status < 0) {
        goto fail;
    }
    if (old_parent_id != new_parent_id) {
        status = mozbm_mtime_update(ctx, new_parent_id, &mtime);
        if (status < 0) {
            goto fail;
        }
    }
    if (ctx->flags & BOOKMARKFS_BACKEND_CTIME) {
        status = mozbm_mtime_update(ctx, old_cols.id, &mtime);
        if (status < 0) {
            goto fail;
        }
    }

  end:
    return txn_end(ctx);

  fail:
    return txn_rollback(ctx, status);
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

    struct mozbm bm_cols = {
        .id            = id,
        .place_id      = -1,
        .date_added    = -1,
        .last_modified = -1,
    };
    struct mozplace place_cols = {
        .last_visit_date = -1,
    };

    int attr_type = ATTR_IN_MOZBM_START;
    if (flags & BOOKMARK_FLAG(SET_TIME)) {
        debug_assert(val_len == 2);

        struct timespec const *times = val;
        place_cols.last_visit_date = timespec_to_msecs(&times[0]);
        bm_cols.last_modified      = timespec_to_msecs(&times[1]);
        if (place_cols.last_visit_date >= 0) {
            --attr_type;
        }
    } else {
        attr_type = get_attr_type(attr_key, ctx->flags);
        switch (attr_type) {
          case ATTR_KEY_NULL:
            place_cols.url     = val;
            place_cols.url_len = val_len;
            break;

          case ATTR_KEY_DESC:
            place_cols.desc     = val;
            place_cols.desc_len = val_len;
            break;

          case ATTR_KEY_TITLE:
            if (0 != validate_filename(val, val_len, NULL)) {
                return -EINVAL;
            }
            bm_cols.title     = val;
            bm_cols.title_len = val_len;
            break;

          case ATTR_KEY_GUID:
            if (!is_valid_guid(val, val_len)) {
                return -EINVAL;
            }
            bm_cols.guid = val;
            break;

          case ATTR_KEY_DATE_ADDED:
            if (0 != parse_msecs(val, val_len, &bm_cols.date_added)) {
                return -EINVAL;
            }
            break;

          default:
            return -ENOATTR;
        }

        if (attr_type != ATTR_KEY_NULL
                && ctx->flags & BOOKMARKFS_BACKEND_CTIME
        ) {
            struct timespec now;
            bm_cols.last_modified = msecs_now(&now);
        }
    }

    int status = txn_begin(ctx);
    if (unlikely(status < 0)) {
        return status;
    }
    status = mozbm_update(ctx, &bm_cols);
    if (status < 0) {
        goto fail;
    }
    if (bm_cols.place_id == 0) {
        // Attempting to update moz_places fields on a directory.
        status = -EPERM;
        goto fail;
    }
    if (attr_type >= ATTR_IN_MOZBM_START) {
        goto end;
    }

    place_cols.id = bm_cols.place_id;
    status = mozplace_update(ctx, &place_cols);
    if (status < 0) {
        goto fail;
    }
    if (place_cols.id == bm_cols.place_id) {
        goto end;
    }

    bm_cols.place_id = place_cols.id;
    status = mozbm_update(ctx, &bm_cols);
    if (status < 0) {
        goto fail;
    }

  end:
    return txn_end(ctx);

  fail:
    return txn_rollback(ctx, status);
}

#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */

BOOKMARKFS_API
struct bookmarkfs_backend const bookmarkfs_backend_firefox = {
    .backend_create  = backend_create,
    .backend_free    = backend_free,
    .backend_info    = backend_info,
    .backend_init    = backend_init,
    .backend_sandbox = backend_sandbox,

    .bookmark_fsck   = bookmark_fsck,
    .bookmark_get    = bookmark_get,
    .bookmark_list   = bookmark_list,
    .bookmark_lookup = bookmark_lookup,

    .object_free = object_free,

#ifdef BOOKMARKFS_BACKEND_FIREFOX_WRITE
    .backend_mkfs = backend_mkfs,
    .backend_sync = backend_sync,

    .bookmark_create  = bookmark_create,
    .bookmark_delete  = bookmark_delete,
    .bookmark_permute = bookmark_permute,
    .bookmark_rename  = bookmark_rename,
    .bookmark_set     = bookmark_set,
#endif  /* defined(BOOKMARKFS_BACKEND_FIREFOX_WRITE) */
};
