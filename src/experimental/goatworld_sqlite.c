#include "g1_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int sql_error(g1_store_t *s, int code) {
    (void)s;
    errno = (code & 255) == SQLITE_FULL ? ENOSPC : (code & 255) == SQLITE_BUSY ? EBUSY : EIO;
    return -1;
}
static int exec_sql(g1_store_t *s, const char *text) {
    int rc = sqlite3_exec(s->sql, text, NULL, NULL, NULL);
    return rc == SQLITE_OK ? 0 : sql_error(s, rc);
}
static int pragma_is(g1_store_t *s, const char *sql, const char *expected) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s->sql, sql, -1, &st, NULL);
    bool good = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) &&
                !strcmp((const char *)sqlite3_column_text(st, 0), expected);
    int done = sqlite3_finalize(st);
    return good && done == SQLITE_OK ? 0 : sql_error(s, rc);
}
static int row_digest(uint32_t dim, int32_t x, int32_t z, uint64_t gen, g1_profile_t p, const uint8_t *raw,
                      size_t n, uint8_t out[32]) {
    uint8_t b[80] = {0};
    g1_p32(b, dim);
    g1_p32(b + 4, (uint32_t)x);
    g1_p32(b + 8, (uint32_t)z);
    g1_p32(b + 12, (uint32_t)n);
    g1_p64(b + 16, gen);
    g1_p32(b + 24, p.codec);
    g1_p32(b + 28, p.framed);
    g1_p32(b + 32, (uint32_t)(int32_t)p.level);
    return g1_sha(raw, n, b + 40) || g1_sha(b, sizeof(b), out) ? -1 : 0;
}
static int world_identity(g1_store_t *s, bool create) {
    if (create) {
        if (exec_sql(s, "CREATE TABLE meta(id INTEGER PRIMARY KEY CHECK(id=1),world_id BLOB NOT NULL "
                        "CHECK(length(world_id)=16),data_version INTEGER NOT NULL CHECK(data_version=4788))"))
            return -1;
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(s->sql, "INSERT INTO meta VALUES(1,?,4788)", -1, &st, NULL);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_blob(st, 1, s->id, 16, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK)
            rc = sqlite3_step(st);
        int closed = sqlite3_finalize(st);
        return rc == SQLITE_DONE && closed == SQLITE_OK ? 0 : sql_error(s, rc);
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s->sql, "SELECT world_id,data_version FROM meta WHERE id=1", -1, &st, NULL);
    bool valid = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
                 sqlite3_column_type(st, 0) == SQLITE_BLOB && sqlite3_column_bytes(st, 0) == 16 &&
                 !memcmp(sqlite3_column_blob(st, 0), s->id, 16) &&
                 sqlite3_column_type(st, 1) == SQLITE_INTEGER && sqlite3_column_int(st, 1) == 4788 &&
                 sqlite3_step(st) == SQLITE_DONE;
    int closed = sqlite3_finalize(st);
    return valid && closed == SQLITE_OK ? 0 : sql_error(s, rc);
}
int g1_sql_check(g1_store_t *s) { return pragma_is(s, "PRAGMA integrity_check", "ok"); }
int g1_sql_open(g1_store_t *s, bool create) {
    if (!create) {
        const char *names[] = {"chunks.sqlite", "chunks.sqlite-wal", "chunks.sqlite-shm"};
        for (unsigned i = 0; i < 3; i++) {
            struct stat st;
            if (fstatat(s->root, names[i], &st, AT_SYMLINK_NOFOLLOW)) {
                if (i && errno == ENOENT)
                    continue;
                return -1;
            }
            if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
                errno = EINVAL;
                return -1;
            }
        }
    }
    char path[4096];
    if (g1_path(s, "chunks.sqlite", path))
        return -1;
    int flags = (s->writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) |
                (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(path, &s->sql, flags, NULL);
    if (rc != SQLITE_OK)
        return sql_error(s, rc);
    sqlite3_limit(s->sql, SQLITE_LIMIT_LENGTH, G1_MAX_RAW * 2 + 65536);
    sqlite3_extended_result_codes(s->sql, 1);
    sqlite3_busy_timeout(s->sql, 0);
    if (exec_sql(s, "PRAGMA trusted_schema=OFF; PRAGMA mmap_size=0; PRAGMA cache_size=-4096;") ||
        pragma_is(s, "PRAGMA cache_size", "-4096"))
        return -1;
    if (!create &&
        (pragma_is(s, "PRAGMA application_id", "1196896561") || pragma_is(s, "PRAGMA user_version", "1")))
        return -1;
    if (!create && world_identity(s, false))
        return -1;
    if (s->writable) {
        if (pragma_is(s, "PRAGMA journal_mode=WAL", "wal") ||
            exec_sql(s, "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;") ||
            pragma_is(s, "PRAGMA synchronous", "2") || pragma_is(s, "PRAGMA wal_autocheckpoint", "0"))
            return -1;
    }
    if (create) {
        if (exec_sql(s, "PRAGMA application_id=1196896561; PRAGMA user_version=1; CREATE TABLE chunks(d "
                        "INTEGER NOT NULL,x INTEGER NOT NULL,z INTEGER NOT NULL,g INTEGER NOT NULL "
                        "CHECK(g>0),c INTEGER NOT NULL,f INTEGER NOT NULL,l INTEGER NOT NULL,n INTEGER NOT "
                        "NULL CHECK(n>0 AND n<=8388608),sha BLOB NOT NULL CHECK(length(sha)=32),payload BLOB "
                        "NOT NULL,PRIMARY KEY(d,x,z)) WITHOUT ROWID;") ||
            world_identity(s, true) || g1_sql_finish(s, false) || g1_sync(s->root))
            return -1;
    } else if (pragma_is(s, "PRAGMA application_id", "1196896561") ||
               pragma_is(s, "PRAGMA user_version", "1"))
        return -1;
    return 0;
}
int g1_sql_read(g1_store_t *s, uint32_t dim, int32_t x, int32_t z, uint8_t **raw, size_t *len,
                uint64_t *gen) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s->sql, "SELECT g,c,f,l,n,sha,payload FROM chunks WHERE d=? AND x=? AND z=?",
                                -1, &st, NULL);
    if (rc != SQLITE_OK)
        return sql_error(s, rc);
    sqlite3_bind_int(st, 1, (int)dim);
    sqlite3_bind_int(st, 2, x);
    sqlite3_bind_int(st, 3, z);
    int step = sqlite3_step(st);
    int result = -1;
    if (step == SQLITE_DONE)
        result = 1;
    else if (step == SQLITE_ROW) {
        sqlite3_int64 g = sqlite3_column_int64(st, 0);
        int n = sqlite3_column_int(st, 4), bytes = sqlite3_column_bytes(st, 6);
        bool ints = true;
        for (int i = 0; i < 5; i++)
            if (sqlite3_column_type(st, i) != SQLITE_INTEGER)
                ints = false;
        int c = sqlite3_column_int(st, 1), f = sqlite3_column_int(st, 2), level = sqlite3_column_int(st, 3);
        if (ints && g > 0 && n > 0 && n <= (int)G1_MAX_RAW && bytes > 0 && bytes <= (int)G1_MAX_RAW * 2 &&
            c >= 0 && c <= 3 && f >= 0 && f <= 3 && level >= 0 && level <= 9 &&
            sqlite3_column_type(st, 5) == SQLITE_BLOB && sqlite3_column_type(st, 6) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 5) == 32) {
            g1_profile_t p = {(uint8_t)c, (uint8_t)f, (int8_t)level};
            if (!g1_decode(sqlite3_column_blob(st, 6), (size_t)bytes, (size_t)n, p, raw)) {
                uint8_t hash[32];
                if (!row_digest(dim, x, z, (uint64_t)g, p, *raw, (size_t)n, hash) &&
                    !memcmp(hash, sqlite3_column_blob(st, 5), 32)) {
                    *len = (size_t)n;
                    *gen = (uint64_t)g;
                    result = 0;
                } else {
                    free(*raw);
                    *raw = NULL;
                }
            }
        }
    }
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK || result < 0)
        return sql_error(s, rc);
    return result;
}
int g1_sql_write(g1_store_t *s, uint32_t dim, const g1_update_t *u, size_t n, g1_profile_t p) {
    if (exec_sql(s, "BEGIN IMMEDIATE"))
        return -1;
    G1_HOOK("sql-begin");
    int rc = 0;
    const char *text = "INSERT INTO chunks(d,x,z,g,c,f,l,n,sha,payload) VALUES(?,?,?,?,?,?,?,?,?,?) ON "
                       "CONFLICT(d,x,z) DO UPDATE SET "
                       "g=excluded.g,c=excluded.c,f=excluded.f,l=excluded.l,n=excluded.n,sha=excluded.sha,"
                       "payload=excluded.payload WHERE chunks.g < excluded.g";
    sqlite3_stmt *st = NULL;
    int code = sqlite3_prepare_v2(s->sql, text, -1, &st, NULL);
    if (code != SQLITE_OK)
        rc = -1;
    for (size_t i = 0; !rc && i < n; i++) {
        uint8_t hash[32], *encoded = NULL;
        size_t len = 0;
        if (row_digest(dim, u[i].x, u[i].z, u[i].generation, p, u[i].raw, u[i].length, hash) ||
            g1_encode(u[i].raw, u[i].length, p, &encoded, &len)) {
            rc = -1;
            break;
        }
        sqlite3_bind_int(st, 1, (int)dim);
        sqlite3_bind_int(st, 2, u[i].x);
        sqlite3_bind_int(st, 3, u[i].z);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)u[i].generation);
        sqlite3_bind_int(st, 5, p.codec);
        sqlite3_bind_int(st, 6, p.framed);
        sqlite3_bind_int(st, 7, p.level);
        sqlite3_bind_int(st, 8, (int)u[i].length);
        sqlite3_bind_blob(st, 9, hash, 32, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 10, encoded, (int)len, SQLITE_TRANSIENT);
        free(encoded);
        code = sqlite3_step(st);
        if (code != SQLITE_DONE || sqlite3_changes(s->sql) != 1)
            rc = -1;
        if (sqlite3_reset(st) != SQLITE_OK)
            rc = -1;
        if (sqlite3_clear_bindings(st) != SQLITE_OK)
            rc = -1;
    }
    if (sqlite3_finalize(st) != SQLITE_OK)
        rc = -1;
    G1_HOOK("sql-before-commit");
    if (!rc)
        rc = exec_sql(s, "COMMIT");
    else
        exec_sql(s, "ROLLBACK");
    if (!rc) {
        G1_HOOK("sql-committed");
        if (g1_sync(s->root))
            return G1_UNCERTAIN;
        G1_HOOK("durable");
    }
    if (rc)
        sql_error(s, code);
    return rc;
}
int g1_sql_keys(g1_store_t *s, uint32_t **d, int32_t **x, int32_t **z, size_t *n) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s->sql, "SELECT d,x,z FROM chunks ORDER BY d,x,z", -1, &st, NULL);
    if (rc != SQLITE_OK)
        return sql_error(s, rc);
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        if (*n >= 100000) {
            rc = SQLITE_TOOBIG;
            break;
        }
        sqlite3_int64 dim = sqlite3_column_int64(st, 0), a = sqlite3_column_int64(st, 1),
                      b = sqlite3_column_int64(st, 2);
        if (dim < 0 || dim > 2 || a < INT32_MIN || a > INT32_MAX || b < INT32_MIN || b > INT32_MAX) {
            rc = SQLITE_CORRUPT;
            break;
        }
        size_t bytes = (*n + 1) * sizeof(uint32_t);
        uint32_t *ds = realloc(*d, bytes);
        if (!ds) {
            rc = SQLITE_NOMEM;
            break;
        }
        *d = ds;
        int32_t *xs = realloc(*x, bytes);
        if (!xs) {
            rc = SQLITE_NOMEM;
            break;
        }
        *x = xs;
        int32_t *zs = realloc(*z, bytes);
        if (!zs) {
            rc = SQLITE_NOMEM;
            break;
        }
        *z = zs;
        (*d)[*n] = (uint32_t)dim;
        (*x)[*n] = (int32_t)a;
        (*z)[*n] = (int32_t)b;
        (*n)++;
    }
    if (step != SQLITE_DONE)
        rc = SQLITE_CORRUPT;
    if (sqlite3_finalize(st) != SQLITE_OK)
        rc = SQLITE_ERROR;
    if (rc != SQLITE_OK) {
        free(*d);
        free(*x);
        free(*z);
        *d = NULL;
        *x = *z = NULL;
        *n = 0;
        return sql_error(s, rc);
    }
    return 0;
}
int g1_sql_finish(g1_store_t *s, bool compact) {
    G1_HOOK("checkpoint-before");
    int log = 0, done = 0;
    int rc = sqlite3_wal_checkpoint_v2(s->sql, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &done);
    if (rc != SQLITE_OK)
        return sql_error(s, rc);
    G1_HOOK("checkpoint-after");
    if (compact) {
        G1_HOOK("compact-copy");
        if (exec_sql(s, "VACUUM"))
            return -1;
        G1_HOOK("compact-published");
        rc = sqlite3_wal_checkpoint_v2(s->sql, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &done);
        if (rc != SQLITE_OK)
            return sql_error(s, rc);
    }
    return g1_sync(s->root);
}
