# V1 Support set.

**SELECT** — read rows
```sql
SELECT * FROM users;
SELECT name, email FROM users;
SELECT name, email FROM users WHERE age > 18;
SELECT * FROM users WHERE name = 'Alice' AND active = 1;
SELECT * FROM users WHERE email LIKE '%@gmail.com';
SELECT * FROM users ORDER BY created_at DESC LIMIT 10;
```

**INSERT** — add rows
```sql
INSERT INTO users (name, email, age) VALUES ('Alice', 'alice@example.com', 30);
INSERT INTO users (name, email) VALUES ('Bob', 'bob@x.com'), ('Carol', 'carol@x.com');
```

**UPDATE** — modify rows
```sql
UPDATE users SET age = 31 WHERE name = 'Alice';
UPDATE users SET active = 0 WHERE last_login < '2024-01-01';
```

**DELETE** — remove rows
```sql
DELETE FROM users WHERE id = 5;
DELETE FROM users WHERE active = 0;
```

**CREATE / DROP TABLE** — schema
```sql
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  email TEXT UNIQUE,
  age INTEGER,
  active INTEGER DEFAULT 1,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

DROP TABLE users;
```

Select:
- First priority is select - ensure select statements work as expected
- requires an in depth refactor of the schema module - problem is generic select statements are fragile
- Utilize a system 
  - Support generic functions in Schema instead of muddled adhoc select statements
  - Scan; Pulls all data into structured, filterable memory - done
  - Filter; applies a predicate row by row, only pulls rows that fulfill said predicate
  - Project; Transforms each row into a new row with selected columns


Core findings:
- volcano query model - create generic *operator* struct with functions open, next, close
- open represents starting the iterator for traversal.
- next represents moving to the next row - out row pointer, and pass in operator
- close represents flushing the iterator
- We utilize a concept of dynamic state - can be represented with a tagged union, we represent as a void pointer
- Utilize a concept of a child operator, allowing dynamic composition of methods
- Scan (retrieve row) -> Filter (retrieve row where expression valid) -> Project (choose specific columns)
- Clean model - allows composable new operators in the future

Potential optimization:
- create a concept of row batching - selecting a set of rows will decrease function call concept


Next Steps:
- represent the concept of an expression
