#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include <stddef.h>

#include "minidb/arena.h"
#include "minidb/ast.h"
#include "minidb/status.h"

/*
 * Recursive-descent SQL parser.
 *
 * Grammar (keywords are case-insensitive):
 *
 *   statement  := select | insert | update | delete | create | drop
 *   select     := SELECT ('*' | item {',' item}) FROM table_ref {join}
 *                 [WHERE expr] [ORDER BY term {',' term}] [LIMIT integer]
 *   item       := expr [[AS] name]
 *   table_ref  := name [[AS] name]
 *   join       := [INNER] JOIN table_ref ON expr
 *   term       := expr [ASC | DESC]
 *   insert     := INSERT INTO name ['(' name {',' name} ')']
 *                 VALUES row {',' row}
 *   row        := '(' expr {',' expr} ')'
 *   update     := UPDATE name SET name '=' expr {',' name '=' expr} [WHERE expr]
 *   delete     := DELETE FROM name [WHERE expr]
 *   create     := CREATE TABLE name '(' column {',' column} ')'
 *   column     := name type [PRIMARY KEY]
 *   type       := INT | INTEGER | DECIMAL | REAL | DOUBLE | FLOAT
 *               | TEXT | VARCHAR ['(' integer ')'] | BOOL | BOOLEAN
 *   drop       := DROP TABLE name
 *
 * Expressions, loosest binding first:
 *
 *   expr       := and {OR and}
 *   and        := not {AND not}
 *   not        := NOT not | comparison
 *   comparison := sum [('=' | '!=' | '<>' | '<' | '<=' | '>' | '>=') sum
 *                     | [NOT] LIKE sum]
 *   sum        := product {('+' | '-') product}
 *   product    := unary {('*' | '/') unary}
 *   unary      := '-' unary | primary
 *   primary    := integer | decimal | string | TRUE | FALSE
 *               | name ['.' name] | '(' expr ')'
 */

/*
 * Parses the first statement in `sql`.
 *
 * *out_consumed is how many bytes it spanned, including its semicolon, so
 * the caller can continue with the next statement. *out_statement is NULL
 * (with MINIDB_OK) when only whitespace, comments or semicolons remained.
 *
 * The statement is allocated in `arena`. String literals are copied, so the
 * AST does not point into `sql`.
 */
minidb_status_t minidb_parse_statement(const char *sql, size_t length, minidb_arena_t *arena,
                                       minidb_statement_t **out_statement, size_t *out_consumed,
                                       minidb_error_t *error);

#endif
