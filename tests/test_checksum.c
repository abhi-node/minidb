#include "test.h"

#include "minidb/checksum.h"

/* Bit-at-a-time CRC-32, straight from the definition: slow but obviously right. */
static uint32_t reference_crc32(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void test_known_value(void) {
    /* The standard CRC-32 check value. */
    CHECK(minidb_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(minidb_crc32(NULL, 0) == 0);
}

static void test_matches_reference_at_every_length_and_alignment(void) {
    uint8_t buffer[512];
    uint32_t state = 12345;
    for (size_t index = 0; index < sizeof(buffer); index++) {
        state = state * 1664525u + 1013904223u;
        buffer[index] = (uint8_t)(state >> 24);
    }
    for (size_t offset = 0; offset < 9; offset++) {
        for (size_t length = 0; length + offset <= 300; length++) {
            CHECK(minidb_crc32(buffer + offset, length) ==
                  reference_crc32(buffer + offset, length));
        }
    }
}

int main(void) {
    RUN(test_known_value);
    RUN(test_matches_reference_at_every_length_and_alignment);
    return EXIT_SUCCESS;
}
