/**
 * bookmarkfs/src/db.c
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

#include "db.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "xstd.h"

struct db_pragma_ctx {
    char const *val;
    size_t      val_len;

    int status;
};

// Forward declaration start
static int  db_check_cb  (void *, sqlite3_stmt *);
static int  db_pragma_cb (void *, sqlite3_stmt *);
static void safeincr     (sqlite3_context *, int, sqlite3_value **);
// Forward declaration end

static int
db_check_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    int *status_ptr = user_data;

    char const *result = (char const *)sqlite3_column_text(stmt, 0);
    xassert(result != NULL);

    if (0 != strcmp("ok", result)) {
        log_printf("%s: expected 'ok', got '%s'", sqlite3_sql(stmt), result);
        *status_ptr = -EIO;
    }
    return 0;
}

static int
db_pragma_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    struct db_pragma_ctx *ctx = user_data;

    size_t               val_len = sqlite3_column_bytes(stmt, 0);
    unsigned char const *val     = sqlite3_column_text(stmt, 0);
    xassert(val != NULL);

    ctx->status = 0;
    if (val_len != ctx->val_len || 0 != memcmp(ctx->val, val, val_len)) {
        log_printf("%s: expected '%s', got '%s'", sqlite3_sql(stmt),
                ctx->val, val);
        ctx->status = -1;
    }
    return 0;
}

static void
safeincr (
    sqlite3_context  *dbctx,
    int               argc,
    sqlite3_value   **argv
) {
    debug_assert(argc == 1);
    sqlite3_value *val = argv[0];

    int64_t ival = 0;
    if (SQLITE_INTEGER == sqlite3_value_type(val)) {
        ival = sqlite3_value_int64(val);
        if (unlikely(ival == INT64_MAX)) {
            sqlite3_result_error(dbctx, "integer overflow", -1);
            return;
        }
        ++ival;
    }
    sqlite3_result_int64(dbctx, ival);
}

int
db_check (
    sqlite3 *db
) {
    sqlite3_stmt *stmt = db_prepare(db, SQL_PRAGMA("quick_check(1)"), false);
    if (stmt == NULL) {
        return -1;
    }

    int status = 0;
    ssize_t nrows = db_query(stmt, NULL, 0, false, db_check_cb, &status);
    if (nrows < 0) {
        return nrows;
    }
    xassert(nrows == 1);
    return status;
}

int
db_config (
    sqlite3                   *db,
    struct db_conf_item const *items,
    size_t                     items_cnt
) {
    for (size_t idx = 0; idx < items_cnt; ++idx) {
        struct db_conf_item const *item = items + idx;

        int result;
        int status = sqlite3_db_config(db, item->op, item->value, &result);
        if (unlikely(status != SQLITE_OK)) {
            log_printf("sqlite3_db_config(): %s", sqlite3_errstr(status));
            return status;
        }
        if (unlikely(result != item->value)) {
            log_puts("sqlite3_db_config() failed");
            return -1;
        }
    }
    return 0;
}

int
db_errno (
    int err
) {
    if ((err & SQLITE_BUSY) == SQLITE_BUSY) {
        return EBUSY;
    }
    if (err == SQLITE_FULL) {
        return ENOSPC;
    }
    if (err == SQLITE_CONSTRAINT_UNIQUE) {
        // NOTE: This is an internal error and should not be
        //       implicitly exposed to the filesystem.
        return EEXIST;
    }
    return EIO;
}

ssize_t
db_exec (
    sqlite3       *db,
    char const    *sql,
    size_t         sql_len,
    sqlite3_stmt **stmt_ptr,
    int64_t       *values_buf
) {
    sqlite3_stmt *stmt = NULL;
    if (stmt_ptr != NULL) {
        stmt = *stmt_ptr;
        if (stmt != NULL) {
            goto query;
        }
    }

    stmt = db_prepare(db, sql, sql_len, stmt_ptr != NULL);
    if (unlikely(stmt == NULL)) {
        return -EIO;
    }
    if (stmt_ptr != NULL) {
        *stmt_ptr = stmt;
    }

  query:
    return db_query(stmt, NULL, 0, stmt_ptr != NULL,
            values_buf == NULL ? NULL : db_query_i64_cb, values_buf);
}

int
db_fcntl (
    sqlite3 *db,
    int      op,
    int      val
) {
    int status = sqlite3_file_control(db, "main", op, &val);
    if (status != SQLITE_OK) {
        log_printf("sqlite3_file_control: %s", sqlite3_errstr(status));
        return status;
    }
    return 0;
}

sqlite3 *
db_open (
    char const *path
) {
    sqlite3 *db;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_EXRESCODE;
    if (SQLITE_OK != sqlite3_open_v2(path, &db, flags, NULL)) {
        log_printf("sqlite3_open_v2(): %s", sqlite3_errmsg(db));
        goto fail;
    }
    if (0 != sqlite3_db_readonly(db, "main")) {
        log_puts("cannot open database for read/write");
        goto fail;
    }
    return db;

  fail:
    sqlite3_close(db);
    return NULL;
}

int
db_pragma (
    sqlite3                     *db,
    struct db_pragma_item const *items,
    size_t                       items_cnt
) {
    for (size_t idx = 0; idx < items_cnt; ++idx) {
        struct db_pragma_item const *item = items + idx;

        char const *sql = item->sql;
        if (sql == NULL) {
            continue;
        }
        size_t sql_len = item->sql_len;
        size_t val_len = item->val_len;

        struct db_pragma_ctx pragma_ctx = {
            .val     = sql + sql_len - val_len,
            .val_len = val_len,
        };
        bool again = true;

      query:  ;
        sqlite3_stmt *stmt = db_prepare(db, sql, sql_len, false);
        if (unlikely(stmt == NULL)) {
            return -1;
        }

        ssize_t nrows
            = db_query(stmt, NULL, 0, false, db_pragma_cb, &pragma_ctx);
        if (nrows > 0) {
            if (pragma_ctx.status != 0) {
                return -1;
            }
        } else if (nrows == 0) {
            if (!again) {
                log_printf("bad pragma: %s", sql);
                return -1;
            }
            again = false;
            // strlen(" = ") == 3
            sql_len = pragma_ctx.val - sql - 3;
            goto query;
        }
    }
    return 0;
}

sqlite3_stmt *
db_prepare (
    sqlite3    *db,
    char const *sql,
    size_t      sql_len,
    bool        persistent
) {
    sqlite3_stmt *stmt;

    int flags = 0;
    if (persistent) {
        flags |= SQLITE_PREPARE_PERSISTENT;
    }
    int err = sqlite3_prepare_v3(db, sql, sql_len, flags, &stmt, NULL);
    if (unlikely(err != SQLITE_OK)) {
        log_printf("sqlite3_prepare_v3(): %s, %s", sql, sqlite3_errmsg(db));
        return NULL;
    }

    return stmt;
}

ssize_t
db_query (
    sqlite3_stmt                   *stmt,
    struct db_stmt_bind_item const *bind_items,
    int                             bind_cnt,
    bool                            persistent,
    db_query_row_func              *row_cb,
    void                           *user_data
) {
    int     err;
    ssize_t result = 0;

    if (bind_items == NULL) {
        goto fetch_rows;
    }
    for (int i = 0; i < bind_cnt; ) {
        struct db_stmt_bind_item const *bind_item = bind_items + i++;

        union db_value val = bind_item->val;
        ssize_t        len = bind_item->len;
        if (len < 0) {
            if (val.i64 < 0) {
                continue;
            }
            err = sqlite3_bind_int64(stmt, i, val.i64);
        } else {
            if (val.text == NULL) {
                continue;
            }
            err = sqlite3_bind_text(stmt, i, val.text, len, SQLITE_STATIC);
        }
        if (unlikely(err != SQLITE_OK)) {
            log_printf("sqlite3_bind(): %d: %s", i, sqlite3_errstr(err));
            result = -err;
            goto end;
        }
    }

  fetch_rows:
#if 1 && defined(BOOKMARKFS_DEBUG)
    {
        char *sql = sqlite3_expanded_sql(stmt);
        debug_printf("%s", sql);
        sqlite3_free(sql);
    }
#endif
    for (size_t nrows = 0; ; ) {
        err = sqlite3_step(stmt);
        switch (err) {
          case SQLITE_ROW:
            break;

          case SQLITE_DONE:
          no_more_rows:
            result = nrows;
            // fallthrough
          default:
            goto end;
        }

        ++nrows;
        if (row_cb == NULL) {
            continue;
        }
        if (0 != row_cb(user_data, stmt)) {
            goto no_more_rows;
        }
    }

  end:
    if (persistent) {
        if (bind_cnt > 0) {
            sqlite3_clear_bindings(stmt);
        }
        err = sqlite3_reset(stmt);
    } else {
        err = sqlite3_finalize(stmt);
    }
    if (err != SQLITE_OK) {
        char const *errmsg;
        if ((err & SQLITE_ERROR) == SQLITE_ERROR) {
            errmsg = sqlite3_errmsg(sqlite3_db_handle(stmt));
        } else {
            errmsg = sqlite3_errstr(err);
        }
        log_printf("sqlite3_step(): %s", errmsg);
        result = -db_errno(err);
    }
    return result;
}

int
db_query_i64_cb (
    void         *user_data,
    sqlite3_stmt *stmt
) {
    int64_t *arr = user_data;

    int ncols = sqlite3_column_count(stmt);
    for (int i = 0; i < ncols; ++i) {
        arr[i] = sqlite3_column_int64(stmt, i);
    }
    return 1;
}

int
db_register_safeincr (
    sqlite3 *db
) {
    int flags = SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS | SQLITE_DIRECTONLY;
    int err = sqlite3_create_function(db, "safeincr", 1, flags | SQLITE_UTF8,
            NULL, safeincr, NULL, NULL);
    if (unlikely(err != SQLITE_OK)) {
        log_printf("sqlite3_create_function(): %s", sqlite3_errstr(err));
        return -1;
    }
    return 0;
}
