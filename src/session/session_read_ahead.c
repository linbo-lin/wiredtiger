/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_session_read_ahead_check --
 *     Check to see whether cursors owned by this session might benefit from doing read ahead
 */
bool
__wt_session_read_ahead_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    if (!S2C(session)->read_ahead_auto_on)
        return (false);

    if (S2C(session)->read_ahead_queue_count > WT_MAX_READ_AHEAD_QUEUE)
        return (false);

    /*
     * Don't deal with internal pages at the moment - finding the right content to preload based on
     * internal pages is hard.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (false);

    if (session->read_ahead_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, block_read_ahead_disk_one);

    /* A single read from disk is common - don't use it to guide read ahead behavior. */
    if (session->read_ahead_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, block_read_ahead_skipped);
        return (false);
    }

    if (session->read_ahead_prev_ref == NULL) {
        WT_STAT_CONN_INCR(session, block_read_ahead_attempts);
        return (true);
    }

    /*
     * If the previous read ahead was using the same home ref, it's already been pre-loaded. Note
     * that this heuristic probably needs to get more sophisticated - ideally it would preload a
     * number of pages, not necessarily all children of the current internal page.
     */
    if (session->read_ahead_prev_ref->page == ref->home) {
        WT_STAT_CONN_INCR(session, block_read_ahead_skipped);
        return (false);
    }

    WT_STAT_CONN_INCR(session, block_read_ahead_attempts);
    return (true);
}