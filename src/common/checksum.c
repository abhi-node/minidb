#include "minidb/checksum.h"

#include <stdbool.h>
#include <string.h>

/*
 * Slicing-by-8 CRC-32: instead of one table lookup per input byte, eight
 * tables let the loop consume eight bytes per iteration with independent
 * lookups the CPU can overlap. tables[0] is the classic byte-at-a-time
 * table; tables[k] advances a byte k positions further through the CRC.
 */

#define CRC32_POLYNOMIAL 0xEDB88320u
#define SLICE_COUNT 8

static uint32_t tables[SLICE_COUNT][256];
static bool tables_ready = false;

static void build_tables(void) {
    for (uint32_t byte = 0; byte < 256; byte++) {
        uint32_t remainder = byte;
        for (int bit = 0; bit < 8; bit++) {
            remainder = (remainder & 1) ? (remainder >> 1) ^ CRC32_POLYNOMIAL : remainder >> 1;
        }
        tables[0][byte] = remainder;
    }
    for (uint32_t byte = 0; byte < 256; byte++) {
        for (int slice = 1; slice < SLICE_COUNT; slice++) {
            uint32_t previous = tables[slice - 1][byte];
            tables[slice][byte] = (previous >> 8) ^ tables[0][previous & 0xFF];
        }
    }
    tables_ready = true;
}

uint32_t minidb_crc32(const uint8_t *bytes, size_t length) {
    if (!tables_ready) {
        build_tables();
    }
    uint32_t crc = 0xFFFFFFFFu;

    /* Eight bytes at a time (little-endian: the low word meets the CRC first). */
    while (length >= SLICE_COUNT) {
        uint32_t low_word;
        uint32_t high_word;
        memcpy(&low_word, bytes, sizeof(low_word));
        memcpy(&high_word, bytes + sizeof(low_word), sizeof(high_word));
        low_word ^= crc;

        crc = tables[7][low_word & 0xFF] ^ tables[6][(low_word >> 8) & 0xFF] ^
              tables[5][(low_word >> 16) & 0xFF] ^ tables[4][low_word >> 24] ^
              tables[3][high_word & 0xFF] ^ tables[2][(high_word >> 8) & 0xFF] ^
              tables[1][(high_word >> 16) & 0xFF] ^ tables[0][high_word >> 24];
        bytes += SLICE_COUNT;
        length -= SLICE_COUNT;
    }

    /* The remaining tail, one byte at a time. */
    while (length-- > 0) {
        crc = tables[0][(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}
