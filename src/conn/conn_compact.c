/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __compact_server_run_chk --
 *     Check to decide if the compact server should continue running.
 */
static bool
__compact_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_COMPACT));
}

/*
 * __get_compact_stat --
 *     Get the statistics for the given uri.
 */
static WT_BACKGROUND_COMPACT_STAT *
__get_compact_stat(WT_SESSION_IMPL *session, const char *uri)
{
    WT_BACKGROUND_COMPACT_STAT *dsrc_stat;
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session);
    uint64_t bucket, hash;

    hash = __wt_hash_city64(uri, strlen(uri));
    bucket = hash & (conn->hash_size - 1);

    /* Find the uri in the files compacted list. */
    TAILQ_FOREACH (dsrc_stat, &conn->background_compact.compacthash[bucket], hashq)
        if (strcmp(uri, dsrc_stat->uri) == 0)
            return (dsrc_stat);

    return (NULL);
}

/*
 * __should_compact --
 *     Check whether we should proceed with calling compaction on the given file.
 */
static bool
__should_compact(WT_SESSION_IMPL *session, const char *uri)
{
    WT_BACKGROUND_COMPACT_STAT *dsrc_stat;
    WT_CONNECTION_IMPL *conn;
    uint64_t cur_time;

    conn = S2C(session);

    /* The history store file should not be compacted. */
    if (WT_STREQ(uri, WT_HS_URI))
        return (false);

    /* If we haven't seen this file before we should try and compact it. */
    dsrc_stat = __get_compact_stat(session, uri);
    if (dsrc_stat == NULL)
        return (true);

    /* If we have been unsuccessful recently skip this file for some time. */
    cur_time = __wt_clock(session);
    if (WT_CLOCKDIFF_SEC(cur_time, dsrc_stat->last_unsuccessful_compact) < 60) {
        dsrc_stat->skip_count++;
        conn->background_compact.files_skipped++;
        return (false);
    }

    /* If the last compaction pass was less successful than the average. Skip it for some time. */
    if (dsrc_stat->bytes_rewritten < conn->background_compact.bytes_rewritten_ema &&
      WT_CLOCKDIFF_SEC(cur_time, dsrc_stat->start_time) < 60) {
        dsrc_stat->skip_count++;
        conn->background_compact.files_skipped++;
        return (false);
    }

    return (true);
}

/*
 * __compact_background_start --
 *     Pre-fill compact related statistics for the given file.
 */
static int
__compact_background_start(
  WT_SESSION_IMPL *session, const char *uri, WT_BACKGROUND_COMPACT_STAT **compact_stat)
{
    WT_BACKGROUND_COMPACT_STAT *temp_compact_stat;
    WT_BM *bm;
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;

    bm = S2BT(session)->bm;
    conn = S2C(session);

    temp_compact_stat = __get_compact_stat(session, uri);

    /* If the table is not in the list, allocate a new entry and insert it. */
    if (temp_compact_stat == NULL) {
        WT_RET(__wt_calloc_one(session, &temp_compact_stat));
        WT_RET(__wt_strdup(session, uri, &temp_compact_stat->uri));
        hash = __wt_hash_city64(uri, strlen(uri));
        bucket = hash & (conn->hash_size - 1);
        WT_BKG_COMPACT_INSERT(conn, temp_compact_stat, bucket);
    }

    /* Fill starting information prior to running compaction. */
    WT_RET(bm->size(bm, session, &temp_compact_stat->start_size));
    temp_compact_stat->start_time = __wt_clock(session);

    *compact_stat = temp_compact_stat;

    return (0);
}

/*
 * __compact_background_end --
 *     Fill resulting compact statistics in the background compact tracking list for a given file.
 */
static int
__compact_background_end(WT_SESSION_IMPL *session, WT_BACKGROUND_COMPACT_STAT *compact_stat)
{
    WT_BM *bm;
    WT_CONNECTION_IMPL *conn;

    bm = S2BT(session)->bm;
    conn = S2C(session);

    WT_RET(bm->size(bm, session, &compact_stat->end_size));
    compact_stat->bytes_recovered = compact_stat->start_size - compact_stat->end_size;
    compact_stat->bytes_rewritten = bm->block->compact_bytes_rewritten;

    /*
     * If the file failed to decrease in size, mark as an unsuccessful attempt. We do this check
     * first, because it's possible for compaction to do work (rewriting bytes) while other
     * operations cause the file to increase in size.
     */
    if (compact_stat->bytes_recovered <= 0) {
        compact_stat->last_unsuccessful_compact = __wt_clock(session);
        compact_stat->consecutive_unsuccessful_attempts++;
    } else {
        compact_stat->last_successful_compact = __wt_clock(session);
        compact_stat->consecutive_unsuccessful_attempts = 0;
        conn->background_compact.files_compacted++;

        /*
         * Update the moving average of bytes rewritten across each file compact attempt. A
         * weighting of 10% means that we are effectively considering the last 10 attempts in the
         * average.
         */
        conn->background_compact.bytes_rewritten_ema =
          (uint64_t)(0.1 * bm->block->compact_bytes_rewritten +
            0.9 * conn->background_compact.bytes_rewritten_ema);
    }

    return (0);
}

/*
 * __background_compact_list_cleanup --
 *     Free an entry or all entries in the background compact tracking list.
 */
static void
__background_compact_list_cleanup(WT_SESSION_IMPL *session, bool all)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat, *temp_compact_stat;
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash, cur_time;

    conn = S2C(session);
    cur_time = __wt_clock(session);

    TAILQ_FOREACH_SAFE(compact_stat, &conn->background_compact.compactqh, q, temp_compact_stat)
    {
        if (all || WT_CLOCKDIFF_SEC(cur_time, compact_stat->start_time) > 86400) {
            /* Remove file entry from both the hashtable and list. */
            hash = __wt_hash_city64(compact_stat->uri, strlen(compact_stat->uri));
            bucket = hash & (conn->hash_size - 1);
            WT_BKG_COMPACT_REMOVE(conn, compact_stat, bucket);
            __wt_free(session, compact_stat->uri);
            __wt_free(session, compact_stat);
        }
    }

    if (all)
        __wt_free(session, conn->background_compact.compacthash);
}

/*
 * __compact_server --
 *     The compact server thread.
 */
static WT_THREAD_RET
__compact_server(void *arg)
{
    WT_BACKGROUND_COMPACT_STAT *dsrc_stat;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    int exact;
    const char *config, *key, *prefix, *uri;
    bool full_iteration, running, signalled;

    dsrc_stat = NULL;

    session = arg;
    conn = S2C(session);
    wt_session = (WT_SESSION *)session;
    cursor = NULL;
    config = NULL;
    key = NULL;
    uri = NULL;
    /* The compact operation is only applied on URIs with a specific prefix. */
    prefix = "file:";
    exact = 0;
    full_iteration = running = signalled = false;

    WT_STAT_CONN_SET(session, background_compact_running, 0);

    for (;;) {

        /* When the entire metadata file has been parsed, take a break or wait until signalled. */
        if (full_iteration || !running) {

            /*
             * In order to always try to parse all the candidates present in the metadata file even
             * though the compaction server may be stopped at random times, only set the URI to the
             * prefix for the very first iteration and when all the candidates in the metadata file
             * have been parsed.
             */
            if (uri == NULL || full_iteration) {
                full_iteration = false;
                __wt_free(session, uri);
                WT_ERR(__wt_strndup(session, prefix, strlen(prefix), &uri));
                __background_compact_list_cleanup(session, false);
            }

            /* Check every 10 seconds in case the signal was missed. */
            __wt_cond_wait(
              session, conn->background_compact.cond, 10 * WT_MILLION, __compact_server_run_chk);
        }

        /* Check if we're quitting or being reconfigured. */
        if (!__compact_server_run_chk(session))
            break;

        __wt_spin_lock(session, &conn->background_compact.lock);
        running = conn->background_compact.running;
        if (conn->background_compact.signalled) {
            conn->background_compact.signalled = false;
            WT_STAT_CONN_SET(session, background_compact_running, running);
        }
        __wt_spin_unlock(session, &conn->background_compact.lock);

        /*
         * This check is necessary as we may have timed out while waiting on the mutex to be
         * signalled and compaction is not supposed to be executed.
         */
        if (!running)
            continue;

        /* Open a metadata cursor. */
        WT_ERR(__wt_metadata_cursor(session, &cursor));

        cursor->set_key(cursor, uri);
        WT_ERR(cursor->search_near(cursor, &exact));

        /* Make sure not to go backwards. */
        if (exact <= 0)
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);

        /* Find a table to compact. */
        while (ret == 0) {
            WT_ERR(cursor->get_key(cursor, &key));
            /* Check we are still dealing with keys which have the right prefix. */
            if (WT_PREFIX_MATCH(key, prefix)) {
                /*
                 * Check the list of files background compact has tracked statistics for. This
                 * avoids having to open a dhandle for the file if compaction is unlikely to work
                 * efficiently on this file.
                 */
                if (__should_compact(session, key))
                    break;
            } else {
                ret = WT_NOTFOUND;
                break;
            }
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);
        }

        /* All the keys with the specified prefix have been parsed. */
        if (ret == WT_NOTFOUND) {
            WT_ERR(__wt_metadata_cursor_release(session, &cursor));
            full_iteration = true;
            continue;
        }
        /* Make a copy of the key as it can be freed once the cursor is released. */
        __wt_free(session, uri);
        WT_ERR(__wt_strndup(session, key, strlen(key), &uri));

        /* Always close the metadata cursor. */
        WT_ERR(__wt_metadata_cursor_release(session, &cursor));

        /* Compact the file with the latest configuration. */
        __wt_spin_lock(session, &conn->background_compact.lock);
        if (config == NULL || !WT_STREQ(config, conn->background_compact.config)) {
            __wt_free(session, config);
            ret = __wt_strndup(session, conn->background_compact.config,
              strlen(conn->background_compact.config), &config);
        }
        __wt_spin_unlock(session, &conn->background_compact.lock);

        WT_ERR(ret);

        WT_ERR(__wt_session_get_dhandle(session, uri, NULL, NULL, 0));
        WT_ERR(__compact_background_start(session, uri, &dsrc_stat));

        ret = wt_session->compact(wt_session, uri, config);

        WT_ERR(__compact_background_end(session, dsrc_stat));

        /*
         * Compact may return:
         * - EBUSY or WT_ROLLBACK for various reasons.
         * - ENOENT if the underlying file does not exist.
         * - ETIMEDOUT if the configured timer has elapsed.
         * - WT_ERROR if the background compaction has been interrupted.
         */
        if (ret == EBUSY || ret == ENOENT || ret == ETIMEDOUT || ret == WT_ROLLBACK) {
            WT_STAT_CONN_INCR(session, background_compact_fail);

            if (ret == EBUSY && __wt_cache_stuck(session))
                WT_STAT_CONN_INCR(session, background_compact_fail_cache_pressure);

            if (ret == ETIMEDOUT)
                WT_STAT_CONN_INCR(session, background_compact_timeout);

            ret = 0;
        }

        /*
         * WT_ERROR should indicate the server was interrupted, make sure it is no longer running.
         */
        if (ret == WT_ERROR) {
            __wt_spin_lock(session, &conn->background_compact.lock);
            running = conn->background_compact.running;
            __wt_spin_unlock(session, &conn->background_compact.lock);
            if (!running) {
                WT_STAT_CONN_INCR(session, background_compact_interrupted);
                ret = 0;
            }
        }

        WT_ERR(ret);
    }

    WT_STAT_CONN_SET(session, background_compact_running, 0);

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    __background_compact_list_cleanup(session, true);

    __wt_free(session, config);
    __wt_free(session, conn->background_compact.config);
    __wt_free(session, uri);

    if (ret != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "compact server error"));
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_compact_server_create --
 *     Start the compact thread.
 */
int
__wt_compact_server_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t i;
    uint32_t session_flags;

    conn = S2C(session);

    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_COMPACT);

    TAILQ_INIT(&conn->background_compact.compactqh);

    WT_RET(__wt_calloc_def(session, conn->hash_size, &conn->background_compact.compacthash));
    for (i = 0; i < conn->hash_size; i++)
        TAILQ_INIT(&conn->background_compact.compacthash[i]);

    /*
     * Compaction does enough I/O it may be called upon to perform slow operations for the block
     * manager.
     */
    session_flags = WT_SESSION_CAN_WAIT;
    WT_RET(__wt_open_internal_session(
      conn, "compact-server", true, session_flags, 0, &conn->background_compact.session));
    session = conn->background_compact.session;

    WT_RET(__wt_cond_alloc(session, "compact server", &conn->background_compact.cond));

    WT_RET(__wt_thread_create(session, &conn->background_compact.tid, __compact_server, session));
    conn->background_compact.tid_set = true;

    return (0);
}

/*
 * __wt_compact_server_destroy --
 *     Destroy the background compaction server thread.
 */
int
__wt_compact_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_COMPACT);
    if (conn->background_compact.tid_set) {
        conn->background_compact.running = false;
        __wt_cond_signal(session, conn->background_compact.cond);
        WT_TRET(__wt_thread_join(session, &conn->background_compact.tid));
        conn->background_compact.tid_set = false;
    }
    __wt_cond_destroy(session, &conn->background_compact.cond);

    /* Close the server thread's session. */
    if (conn->background_compact.session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->background_compact.session));
        conn->background_compact.session = NULL;
    }

    return (ret);
}

/*
 * __wt_compact_signal --
 *     Signal the compact thread. Return an error if the background compaction server has not
 *     processed a previous signal yet or because of an invalid configuration.
 */
int
__wt_compact_signal(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *cfg[3] = {NULL, NULL, NULL}, *stripped_config;
    bool running;

    conn = S2C(session);
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_compact);
    cfg[1] = config;
    cfg[2] = NULL;
    stripped_config = NULL;

    /* The background compaction server is not compatible with in-memory or readonly databases. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY)) {
        __wt_verbose_warning(session, WT_VERB_COMPACT, "%s",
          "Background compact cannot be configured for in-memory or readonly databases.");
        return (ENOTSUP);
    }

    /* Wait for any previous signal to be processed first. */
    __wt_spin_lock(session, &conn->background_compact.lock);
    if (conn->background_compact.signalled) {
        ret = EBUSY;
        goto err;
    }

    running = conn->background_compact.running;

    WT_ERR(__wt_config_getones(session, config, "background", &cval));
    if (cval.val == running)
        /*
         * This is an error as we are already in the same state and reconfiguration is not allowed.
         */
        WT_ERR_MSG(
          session, EINVAL, "Background compaction is already %s", running ? "enabled" : "disabled");
    conn->background_compact.running = !running;

    /* Strip the background field from the configuration now it has been parsed. */
    WT_ERR(__wt_config_merge(session, cfg, "background=", &stripped_config));
    __wt_free(session, conn->background_compact.config);
    conn->background_compact.config = stripped_config;

    conn->background_compact.signalled = true;

err:
    __wt_spin_unlock(session, &conn->background_compact.lock);
    if (ret == 0)
        __wt_cond_signal(session, conn->background_compact.cond);
    return (ret);
}
