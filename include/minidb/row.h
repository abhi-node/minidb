#ifndef MINIDB_ROW_H
#define MINIDB_ROW_H

#include <stddef.h>
#include <stdint.h>

#include "minidb/schema.h"
#include "minidb/status.h"
#include "minidb/value.h"

/*
 * Binary row serialization.
 *
 * A row is its column values back to back, in schema order, with no padding
 * and no per-row header (the schema says how to read it):
 *
 *   INT      8 bytes, little-endian two's complement
 *   DECIMAL  8 bytes, IEEE-754 double
 *   BOOL     1 byte, 0 or 1
 *   TEXT     u16 length, then that many bytes
 *
 * Ownership: encode writes into a caller-supplied buffer. decode does not
 * copy: TEXT values point into `bytes` and live exactly as long as it does.
 */

#define MINIDB_MAX_TEXT_LENGTH UINT16_MAX

/* Encoded size of `values`, which must match the schema's column types. */
size_t minidb_row_encoded_size(const minidb_schema_t *schema, const minidb_value_t *values);

/* Writes exactly minidb_row_encoded_size() bytes to out_bytes. */
void minidb_row_encode(const minidb_schema_t *schema, const minidb_value_t *values,
                       uint8_t *out_bytes);

/*
 * Fills out_values[0 .. column_count). Returns MINIDB_ERR_CORRUPT if the
 * bytes end early or run long.
 */
minidb_status_t minidb_row_decode(const minidb_schema_t *schema, const uint8_t *bytes,
                                  size_t length, minidb_value_t *out_values);

#endif
