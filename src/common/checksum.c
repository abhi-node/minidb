#include "minidb/checksum.h"

#include <stdbool.h>

#define CRC32_POLYNOMIAL 0xEDB88320u

static uint32_t crc_table[256];
static bool crc_table_ready = false;

static void crc_table_build(void) {
    for (uint32_t byte = 0; byte < 256; byte++) {
        uint32_t remainder = byte;
        for (int bit = 0; bit < 8; bit++) {
            remainder = (remainder & 1) ? (remainder >> 1) ^ CRC32_POLYNOMIAL : remainder >> 1;
        }
        crc_table[byte] = remainder;
    }
    crc_table_ready = true;
}

uint32_t minidb_crc32(const uint8_t *bytes, size_t length) {
    if (!crc_table_ready) {
        crc_table_build();
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < length; index++) {
        crc = crc_table[(crc ^ bytes[index]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}
