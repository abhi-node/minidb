#include "minidb/catalog.h"

#include <stdlib.h>
#include <string.h>

#include "minidb/btree.h"

/* Catalog entry payload: u32 root page, then the encoded schema. */
#define ENTRY_ROOT_PAGE_SIZE sizeof(uint32_t)

struct minidb_catalog {
    minidb_pager_t *pager;
    minidb_table_t **tables;  /* each table is its own allocation, so pointers are stable */
    uint32_t table_count;
    uint32_t table_slots;     /* allocated length of `tables` */
    uint32_t next_table_id;
};

static minidb_status_t remember_table(minidb_catalog_t *catalog, minidb_table_t *table) {
    if (catalog->table_count == catalog->table_slots) {
        uint32_t slots = catalog->table_slots == 0 ? 8 : catalog->table_slots * 2;
        minidb_table_t **tables = realloc(catalog->tables, slots * sizeof(*tables));
        if (tables == NULL) {
            return MINIDB_ERR_NO_MEMORY;
        }
        catalog->tables = tables;
        catalog->table_slots = slots;
    }
    catalog->tables[catalog->table_count++] = table;
    if (table->id >= catalog->next_table_id) {
        catalog->next_table_id = table->id + 1;
    }
    return MINIDB_OK;
}

/* Builds the in-memory table for one catalog entry. */
static minidb_status_t load_entry(minidb_catalog_t *catalog, int64_t table_id,
                                  const uint8_t *payload, uint32_t payload_length) {
    if (payload_length < ENTRY_ROOT_PAGE_SIZE) {
        return MINIDB_ERR_CORRUPT;
    }
    minidb_table_t *table = calloc(1, sizeof(*table));
    if (table == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    table->id = (uint32_t)table_id;
    memcpy(&table->root_page, payload, ENTRY_ROOT_PAGE_SIZE);

    minidb_status_t status =
        minidb_schema_decode(payload + ENTRY_ROOT_PAGE_SIZE,
                             payload_length - ENTRY_ROOT_PAGE_SIZE, &table->schema);

    /* Hidden row ids continue after the largest one in use. */
    int64_t max_key = 0;
    bool has_rows = false;
    if (status == MINIDB_OK) {
        status = minidb_btree_max_key(catalog->pager, table->root_page, &max_key, &has_rows);
    }
    if (status == MINIDB_OK) {
        table->next_row_id = has_rows ? max_key + 1 : 1;
        status = remember_table(catalog, table);
    }
    if (status != MINIDB_OK) {
        free(table);
    }
    return status;
}

static minidb_status_t load_all_entries(minidb_catalog_t *catalog) {
    minidb_cursor_t cursor;
    minidb_cursor_init(&cursor, catalog->pager);

    minidb_status_t status = minidb_cursor_first(&cursor, MINIDB_CATALOG_ROOT_PAGE);
    while (status == MINIDB_OK && minidb_cursor_is_valid(&cursor)) {
        uint32_t payload_length;
        const uint8_t *payload = minidb_cursor_payload(&cursor, &payload_length);
        status = load_entry(catalog, minidb_cursor_key(&cursor), payload, payload_length);
        if (status == MINIDB_OK) {
            status = minidb_cursor_next(&cursor);
        }
    }
    minidb_cursor_close(&cursor);
    return status;
}

minidb_status_t minidb_catalog_open(minidb_pager_t *pager, minidb_catalog_t **out_catalog) {
    *out_catalog = NULL;
    minidb_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (catalog == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    catalog->pager = pager;
    catalog->next_table_id = 1;

    minidb_status_t status;
    bool is_new_database = minidb_pager_page_count(pager) == 1;
    if (is_new_database) {
        minidb_page_id_t root;
        status = minidb_btree_create(pager, &root);
        if (status == MINIDB_OK && root != MINIDB_CATALOG_ROOT_PAGE) {
            status = MINIDB_ERR_CORRUPT;
        }
    } else {
        status = load_all_entries(catalog);
    }

    if (status != MINIDB_OK) {
        minidb_catalog_close(catalog);
        return status;
    }
    *out_catalog = catalog;
    return MINIDB_OK;
}

void minidb_catalog_close(minidb_catalog_t *catalog) {
    if (catalog == NULL) {
        return;
    }
    for (uint32_t index = 0; index < catalog->table_count; index++) {
        free(catalog->tables[index]);
    }
    free(catalog->tables);
    free(catalog);
}

minidb_table_t *minidb_catalog_find(const minidb_catalog_t *catalog, const char *table_name) {
    for (uint32_t index = 0; index < catalog->table_count; index++) {
        if (strcmp(catalog->tables[index]->schema.table_name, table_name) == 0) {
            return catalog->tables[index];
        }
    }
    return NULL;
}

minidb_table_t *minidb_catalog_find_by_id(const minidb_catalog_t *catalog, uint32_t table_id) {
    for (uint32_t index = 0; index < catalog->table_count; index++) {
        if (catalog->tables[index]->id == table_id) {
            return catalog->tables[index];
        }
    }
    return NULL;
}

uint32_t minidb_catalog_table_count(const minidb_catalog_t *catalog) {
    return catalog->table_count;
}

minidb_table_t *minidb_catalog_table_at(const minidb_catalog_t *catalog, uint32_t index) {
    return catalog->tables[index];
}

minidb_status_t minidb_catalog_create_table(minidb_catalog_t *catalog,
                                            const minidb_schema_t *schema,
                                            minidb_table_t **out_table) {
    *out_table = NULL;
    if (minidb_catalog_find(catalog, schema->table_name) != NULL) {
        return MINIDB_ERR_TABLE_EXISTS;
    }

    uint8_t entry[MINIDB_BTREE_MAX_PAYLOAD];
    size_t entry_length = ENTRY_ROOT_PAGE_SIZE + minidb_schema_encoded_size(schema);
    if (entry_length > sizeof(entry)) {
        return MINIDB_ERR_ROW_TOO_LARGE;
    }

    minidb_table_t *table = calloc(1, sizeof(*table));
    if (table == NULL) {
        return MINIDB_ERR_NO_MEMORY;
    }
    table->id = catalog->next_table_id;
    table->next_row_id = 1;
    table->schema = *schema;

    minidb_status_t status = minidb_btree_create(catalog->pager, &table->root_page);
    if (status == MINIDB_OK) {
        memcpy(entry, &table->root_page, ENTRY_ROOT_PAGE_SIZE);
        minidb_schema_encode(schema, entry + ENTRY_ROOT_PAGE_SIZE);
        status = minidb_btree_insert(catalog->pager, MINIDB_CATALOG_ROOT_PAGE, table->id, entry,
                                     (uint32_t)entry_length);
    }
    if (status == MINIDB_OK) {
        status = remember_table(catalog, table);
    }
    if (status != MINIDB_OK) {
        free(table);
        return status;
    }
    *out_table = table;
    return MINIDB_OK;
}

minidb_status_t minidb_catalog_drop_table(minidb_catalog_t *catalog, minidb_table_t *table) {
    minidb_status_t status = minidb_btree_destroy(catalog->pager, table->root_page);
    if (status == MINIDB_OK) {
        status = minidb_btree_delete(catalog->pager, MINIDB_CATALOG_ROOT_PAGE, table->id);
    }
    if (status != MINIDB_OK) {
        return status;
    }

    for (uint32_t index = 0; index < catalog->table_count; index++) {
        if (catalog->tables[index] == table) {
            catalog->tables[index] = catalog->tables[--catalog->table_count];
            break;
        }
    }
    free(table);
    return MINIDB_OK;
}
