#include <stdint.h>
#include "crc.h"

void crc(const void *msg, size_t len, const struct crc_params *config,
         struct bigint *checksum)
{
    static const uint8_t msb_to_lsb_table[8] = {
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
    };
    static const uint8_t lsb_to_msb_table[8] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
    };
    size_t i;
    const uint8_t *bits, *u8msg = msg;

    /* Reflect the input bits */
    bits = (config->reflect_in) ? lsb_to_msb_table : msb_to_lsb_table;

    /* Initial XOR value */
    bigint_xor(checksum, &config->init);

    /* Process the rest of the input bits */
    for (i = 0; i < len * 8; i++) {
        int bit = bigint_msb(checksum) ^ !!(u8msg[i/8] & bits[i%8]);
        bigint_shl_1(checksum);
        if (bit) bigint_xor(checksum, &config->poly);
    }

    /* Final XOR value */
    bigint_xor(checksum, &config->xor_out);

    /* Reflect the output CRC value */
    if (config->reflect_out)
        bigint_reflect(checksum);
}

void crc_append(const void *msg, size_t len, const struct crc_params *config,
                struct bigint *checksum)
{
    if (config->reflect_out)
        bigint_reflect(checksum);
    bigint_xor(checksum, &config->xor_out);
    bigint_xor(checksum, &config->init);
    crc(msg, len, config, checksum);
}
