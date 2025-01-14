/**
 * bookmarkfs/src/db.h
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

#ifndef BOOKMARKFS_DB_H_
#define BOOKMARKFS_DB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

#include <sqlite3.h>

#include "macros.h"

#define SQL_PRAGMA(p)   STR_WITHLEN("PRAGMA " p)
#define SQL_PRAGMA_ITEM(p, v)  \
    (struct db_pragma_item) { SQL_PRAGMA(p " = " v), strlen(v) }

#define DB_ITEMS_CNT_(arr, name)  ( sizeof(arr) / sizeof(struct name##_item) )
#define DB_PRAGMA_ITEMS_CNT(arr)  DB_ITEMS_CNT_(arr, db_pragma)
#define DB_CONFIG_ITEMS_CNT(arr)  DB_ITEMS_CNT_(arr, db_conf)
#define DB_BIND_ITEMS_CNT(arr)    DB_ITEMS_CNT_(arr, db_stmt_bind)

#define DB_QUERY_BIND_TEXT(s, l)  { { .text = (s)  }, (l) }
#define DB_QUERY_BIND_INT64(v)    { { .i64  = (v)  }, -1  }

#define db_txn_begin(db, stmt_ptr)  \
    db_exec(db, STR_WITHLEN("BEGIN IMMEDIATE"), stmt_ptr, NULL)
#define db_txn_commit(db, stmt_ptr)  \
    db_exec(db, STR_WITHLEN("END"), stmt_ptr, NULL)
#define db_txn_rollback(db, stmt_ptr)  \
    db_exec(db, STR_WITHLEN("ROLLBACK"), stmt_ptr, NULL)

typedef int (db_query_row_func) (
    void         *user_data,
    sqlite3_stmt *stmt
);

union db_value {
    int64_t     i64;
    char const *text;
};

struct db_conf_item {
    int op;
    int value;
};

struct db_pragma_item {
    char const *sql;
    size_t      sql_len;
    size_t      val_len;
};

struct db_stmt_bind_item {
    union db_value val;
    ssize_t        len;
};

int
db_check (
    sqlite3 *db
);

int
db_config (
    sqlite3                   *db,
    struct db_conf_item const *items,
    size_t                     items_cnt
);

int
db_errno (
    int err
);

ssize_t
db_exec (
    sqlite3       *db,
    char const    *sql,
    size_t         sql_len,
    sqlite3_stmt **stmt_ptr,
    int64_t       *values_buf
);

int
db_fcntl (
    sqlite3 *db,
    int      op,
    int      val
);

sqlite3 *
db_open (
    char const *path
);

int
db_pragma (
    sqlite3                     *db,
    struct db_pragma_item const *items,
    size_t                       items_cnt
);

sqlite3_stmt *
db_prepare (
    sqlite3    *db,
    char const *sql,
    size_t      sql_len,
    bool        persistent
);

ssize_t
db_query (
    sqlite3_stmt                   *stmt,
    struct db_stmt_bind_item const *bind_items,
    int                             bind_cnt,
    bool                            persistent,
    db_query_row_func              *row_cb,
    void                           *user_data
);

int
db_query_i64_cb (
    void         *user_data,
    sqlite3_stmt *stmt
);

/**
 * Registers an application-defined function `safeincr()`
 * to the given db connection.
 *
 * Use `safeincr(val)` in place of `(val + 1)`, otherwise
 * SQLite converts the result to REAL upon integer overflow.
 * See: <https://www.sqlite.org/datatype3.html#operators>.
 *
 * The `safeincr()` function fails if given a non-integer
 * argument, or if the argument value equals to INT64_MAX.
 */
int
db_register_safeincr (
    sqlite3 *db
);

#endif  /* !defined(BOOKMARKFS_DB_H_) */
