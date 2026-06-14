#include "minidb/lexer.h"
#include "minidb/schema.h"

minidb_db_status tokenize(FILE *f, minidb_token_set_t *out_tokens) {
    if (out_tokens == NULL || f == NULL) {
        return MINIDB_ERR_INVALID_CALL;
    }

    int c;
    while ((c = fgetc(f)) != EOF) {

    }



}


/*
 * Goal - produce a lexer capable of capturing each part of the expressions
 * SELECT * FROM users;
 SELECT name, email FROM users;
 SELECT name, email FROM users WHERE age > 18;
 SELECT * FROM users WHERE name = 'Alice' AND active = 1;
 SELECT * FROM users WHERE email LIKE '%@gmail.com';
 SELECT * FROM users ORDER BY created_at DESC LIMIT 10;

 Token types identified:
 SELECT
 FROM
 WHERE
 AND
 OR

 <, >, =
 +, -, /, * (how to represent this as a multiplication and a wildcard? Makes parsing more complex, we can generalize to an ASTERIK token
 'string'

 column token represents variables - we can look into refining this on joins
(we will handle order by and desc and limit later.)

support other statements after easily.
How to invalidate malformed statements
- no matching column
- no matching table
- unterminated statement

If the statement ends without a semicolon, we can invalidate it.
If a column does not match, invalidate
If no matching table, invalidate

clear concept is we just have to handle most of this at parsing, except the semicolon finalizer

return a vector of tokens
- should it be a struct - it's likely easiest to represent that way
 */
