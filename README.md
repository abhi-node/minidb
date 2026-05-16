# Mini DB

## An experiment to produce a relational database engine in C

### Requirements
- SQL parsing engine
- Joins/Relational algebra
- B Tree indexes
- WAL
- Concurrency (read / write locks)
- Full server capabilities
- Insertion, retrieval, update, delete, joins
- Foreign key / check constraints

### Rationale
Project based learning initiative; The idea is that with the help of proctored, supplementary AI, you can learn advanced languages much quicker by treating AI as a adaptive tutor / documentation tool.

#### What AI will do
- Produce accurate documentation to reference
- Explain concepts / best code practices in languages
- Teach different mechanics of languages

#### What AI will **not** do
- Write code
- Solve errors and bugs
- Write tests
- Automate any part of the design process

The point of this project is to learn. It is counter inuitive to use AI for this process - the best engineers understand the internals and can guide AI to produce high quality output after rugged experience in the field. AI is an enhancement tool, not a replacement.

### Building the project

```
cmake -S . -B build      configure: read CMakeLists.txt, generate build files
cmake --build build      build: actually compile and link
ctest --test-dir build   run tests
```

### Project Layout

- The src/ folder is where implementation files live
- The include/ folder is where the header files live
- The tests/ folder is where the test harness lives
