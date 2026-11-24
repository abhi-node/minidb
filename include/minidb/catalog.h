#ifndef MINIDB_CATALOG_H
#define MINIDB_CATALOG_H

#include <stdint.h>

#include "minidb/pager.h"
#include "minidb/schema.h"
#include "minidb/status.h"

/*
 * The catalog: which tables exist, their schemas, and where their B+-trees
 * are rooted.
 *
 * It is itself stored as a B+-tree, rooted at a fixed page, with one entry
 * per table (key = table id, payload = root page + encoded schema). On open
 * the entries are loaded into memory, and lookups never touch the disk.
 *
 * Ownership: the catalog owns every minidb_table_t. Pointers stay valid
 * until the table is dropped or the catalog is closed.
 */

/* The catalog tree always lives at the first page after the file header. */
#define MINIDB_CATALOG_ROOT_PAGE ((minidb_page_id_t)1)

typedef struct minidb_table {
    uint32_t id;
    minidb_page_id_t root_page;  /* root of the B+-tree holding the rows */
    int64_t next_row_id;         /* next hidden key for tables without a primary key */
    minidb_schema_t schema;
} minidb_table_t;

typedef struct minidb_catalog minidb_catalog_t;

/* Loads the catalog, creating an empty one in a brand-new database. */
minidb_status_t minidb_catalog_open(minidb_pager_t *pager, minidb_catalog_t **out_catalog);

void minidb_catalog_close(minidb_catalog_t *catalog);

/* NULL when there is no such table. */
minidb_table_t *minidb_catalog_find(const minidb_catalog_t *catalog, const char *table_name);
minidb_table_t *minidb_catalog_find_by_id(const minidb_catalog_t *catalog, uint32_t table_id);

uint32_t minidb_catalog_table_count(const minidb_catalog_t *catalog);
minidb_table_t *minidb_catalog_table_at(const minidb_catalog_t *catalog, uint32_t index);

/* Creates an empty table. MINIDB_ERR_TABLE_EXISTS if the name is taken. */
minidb_status_t minidb_catalog_create_table(minidb_catalog_t *catalog,
                                            const minidb_schema_t *schema,
                                            minidb_table_t **out_table);

/* Frees the table's pages and forgets it. `table` is invalid afterwards. */
minidb_status_t minidb_catalog_drop_table(minidb_catalog_t *catalog, minidb_table_t *table);

#endif
