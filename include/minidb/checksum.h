#ifndef MINIDB_CHECKSUM_H
#define MINIDB_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3 polynomial), used to detect torn write-ahead log records. */
uint32_t minidb_crc32(const uint8_t *bytes, size_t length);

#endif
