#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../kernel/wpa2_crypto.h"

static int expect_hex(const uint8_t *actual, const char *expected,
                      uint32_t length) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < length; i++)
        if (hex[actual[i] >> 4] != expected[i * 2u] ||
            hex[actual[i] & 15u] != expected[i * 2u + 1u]) return 0;
    return 1;
}

int main(void) {
    uint8_t output[64];
    wpa2_sha1((const uint8_t *)"abc", 3u, output);
    if (!expect_hex(output, "a9993e364706816aba3e25717850c26c9cd0d89d", 20u))
        return 1;
    wpa2_hmac_sha1((const uint8_t *)"key", 3u,
                   (const uint8_t *)"The quick brown fox jumps over the lazy dog",
                   43u, output);
    if (!expect_hex(output, "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9", 20u))
        return 2;
    if (!wpa2_derive_pmk("password", (const uint8_t *)"IEEE", 4u, output))
        return 3;
    if (!expect_hex(output,
                    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
                    32u)) return 4;
    if (!wpa2_constant_time_equal(output, output, 32u)) return 5;
    output[0] ^= 1u;
    if (wpa2_constant_time_equal(output, output + 1u, 16u)) return 6;
    const uint8_t kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    const uint8_t wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
        0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
        0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5
    };
    uint32_t plain_length = sizeof(output);
    if (!wpa2_aes_key_unwrap(kek, wrapped, sizeof(wrapped), output,
                             &plain_length) || plain_length != 16u ||
        !expect_hex(output, "00112233445566778899aabbccddeeff", 16u))
        return 7;
    puts("WPA2 crypto: SHA1/HMAC/PBKDF2/AES-UNWRAP PASS");
    return 0;
}
