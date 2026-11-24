#ifndef MINIDB_PLANNER_H
#define MINIDB_PLANNER_H

#include "minidb/arena.h"
#include "minidb/ast.h"
#include "minidb/operator.h"
#include "minidb/status.h"
#include "minidb/storage.h"

/*
 * The planner turns a parsed statement into a tree of operators.
 *
 * A SELECT always becomes the same stack, bottom to top, leaving out the
 * stages the query does not use:
 *
 *   Scan -> Join(s) -> Filter -> Sort -> Project -> Limit
 *
 * Along the way it binds every expression (resolving names, checking types)
 * and makes two access-path decisions:
 *
 *   - Comparisons between the primary key and a constant in WHERE become
 *     the scan's key range, so `WHERE id = 7` is one B+-tree descent rather
 *     than a full scan.
 *   - A join whose ON clause equates the right table's primary key with an
 *     expression over the left side becomes an index join; any other join
 *     falls back to a nested loop.
 *
 * Ownership: the plan lives in `arena`. The caller opens, drains and closes
 * the returned operator.
 */

minidb_status_t minidb_plan_select(minidb_storage_t *storage, minidb_arena_t *arena,
                                   minidb_select_statement_t *select,
                                   minidb_operator_t **out_plan, minidb_error_t *error);

/*
 * Plans the row source of an UPDATE or DELETE: a scan of `table`, narrowed
 * by any primary-key bounds in `where`. `where` (which may be NULL) is bound
 * here; the caller still evaluates it against each row the scan returns.
 */
minidb_status_t minidb_plan_table_scan(minidb_storage_t *storage, minidb_arena_t *arena,
                                       const minidb_table_t *table, minidb_expr_t *where,
                                       minidb_operator_t **out_scan, minidb_error_t *error);

#endif
