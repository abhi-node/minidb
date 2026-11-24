# Representing Expressions

### What is an expression?

Expression will be our representation of evaluation a statement. We use expressions as an arbitrary struct that the AST can use to produce an expression on which the table is evaluated on.

### Requirements
- AST must be able to produce an expression from a statement (i.e. WHERE id > 5 AND id < 10 must become an expression.)
- Expressions must be able to evaluate with some input value (i.e. evaluate an expression where X = "id" and replace X with input value)

### Solution
- We can evaluate AST expressions against a row
- Having a node that represents the actual column name is simplest
- So then, we represent a concept where we literally just have an expression tree where column name is a first class type

i.e. id = 5
      / Column("id") - evaluates and produces the value in the column
Equals
      \ Number(5) - produces number directly


- projection works similarly - we use the same Column type to return the value, and evaluate a set of expressions to build the row:

i.e. SELECT id, name, age FROM users
Row {
  Column("id"),
  Column("name"),
  Column("age")
}

even cooler, we can represent expressions like SELECT (age * 2) + 1 FROM users since it all reduces to an expression!

So we can represent an ASTnode as a tagged union of some of the following:
Number {data: int}, Column {col: string}, Operator {operator: OpType, LeftExpr: ASTnode, RightExpr: ASTnode}

we tokenize an expression like:

SELECT id, name FROM users WHERE age > 20

This would look like something where we store the projection expressions:

expr_t *expressions = { ASTnode: Column , ASTnode: Column} 

keep it simple - SELECT defines proj expr arr, FROM defines table source, WHERE defines filter predicate expr
