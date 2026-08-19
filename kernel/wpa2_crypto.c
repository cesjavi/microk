#include "wpa2_crypto.h"
#include "string.h"

typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
} sha1_context_t;

static uint32_t rol32(uint32_t value, uint32_t count) {
    return (value << count) | (value >> (32u - count));
}

static void sha1_transform(sha1_context_t *ctx, const uint8_t block[64]) {
    uint32_t w[80];
    for (uint32_t i = 0; i < 16u; i++)
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               block[i * 4u + 3u];
    for (uint32_t i = 16u; i < 80u; i++)
        w[i] = rol32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2];
    uint32_t d = ctx->h[3], e = ctx->h[4];
    for (uint32_t i = 0; i < 80u; i++) {
        uint32_t f, k;
        if (i < 20u) {
            f = (b & c) | ((~b) & d); k = 0x5A827999u;
        } else if (i < 40u) {
            f = b ^ c ^ d; k = 0x6ED9EBA1u;
        } else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d; k = 0xCA62C1D6u;
        }
        uint32_t temp = rol32(a, 5u) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30u); b = a; a = temp;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c;
    ctx->h[3] += d; ctx->h[4] += e;
}

static void sha1_init(sha1_context_t *ctx) {
    ctx->h[0] = 0x67452301u; ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu; ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u; ctx->bytes = 0; ctx->used = 0;
}

static void sha1_update(sha1_context_t *ctx, const uint8_t *data,
                        uint32_t length) {
    ctx->bytes += length;
    while (length) {
        uint32_t take = 64u - ctx->used;
        if (take > length) take = length;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take; data += take; length -= take;
        if (ctx->used == 64u) {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha1_final(sha1_context_t *ctx, uint8_t out[20]) {
    uint64_t bits = ctx->bytes * 8u;
    ctx->block[ctx->used++] = 0x80u;
    if (ctx->used > 56u) {
        memset(ctx->block + ctx->used, 0, 64u - ctx->used);
        sha1_transform(ctx, ctx->block); ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56u - ctx->used);
    for (uint32_t i = 0; i < 8u; i++)
        ctx->block[63u - i] = (uint8_t)(bits >> (i * 8u));
    sha1_transform(ctx, ctx->block);
    for (uint32_t i = 0; i < 5u; i++) {
        out[i * 4u] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)ctx->h[i];
    }
    memset(ctx, 0, sizeof(*ctx));
}

void wpa2_sha1(const uint8_t *data, uint32_t length, uint8_t out[20]) {
    sha1_context_t ctx; sha1_init(&ctx); sha1_update(&ctx, data, length);
    sha1_final(&ctx, out);
}

void wpa2_hmac_sha1(const uint8_t *key, uint32_t key_length,
                    const uint8_t *data, uint32_t data_length,
                    uint8_t out[20]) {
    uint8_t k[64], inner[20];
    memset(k, 0, sizeof(k));
    if (key_length > 64u) wpa2_sha1(key, key_length, k);
    else memcpy(k, key, key_length);
    for (uint32_t i = 0; i < 64u; i++) k[i] ^= 0x36u;
    sha1_context_t ctx; sha1_init(&ctx); sha1_update(&ctx, k, 64u);
    sha1_update(&ctx, data, data_length); sha1_final(&ctx, inner);
    for (uint32_t i = 0; i < 64u; i++) k[i] ^= 0x36u ^ 0x5Cu;
    sha1_init(&ctx); sha1_update(&ctx, k, 64u); sha1_update(&ctx, inner, 20u);
    sha1_final(&ctx, out); memset(k, 0, sizeof(k)); memset(inner, 0, 20u);
}

static void pbkdf2_block(const uint8_t *pass, uint32_t pass_length,
                         const uint8_t *ssid, uint32_t ssid_length,
                         uint32_t block, uint8_t out[20]) {
    uint8_t salt[36], u[20];
    memcpy(salt, ssid, ssid_length);
    salt[ssid_length] = (uint8_t)(block >> 24);
    salt[ssid_length + 1u] = (uint8_t)(block >> 16);
    salt[ssid_length + 2u] = (uint8_t)(block >> 8);
    salt[ssid_length + 3u] = (uint8_t)block;
    wpa2_hmac_sha1(pass, pass_length, salt, ssid_length + 4u, u);
    memcpy(out, u, 20u);
    for (uint32_t iteration = 1u; iteration < 4096u; iteration++) {
        wpa2_hmac_sha1(pass, pass_length, u, 20u, u);
        for (uint32_t i = 0; i < 20u; i++) out[i] ^= u[i];
    }
    memset(salt, 0, sizeof(salt)); memset(u, 0, sizeof(u));
}

int wpa2_derive_pmk(const char *passphrase, const uint8_t *ssid,
                    uint32_t ssid_length, uint8_t pmk[32]) {
    uint32_t pass_length = passphrase ? strlen(passphrase) : 0u;
    if (pass_length < 8u || pass_length > 63u || !ssid ||
        !ssid_length || ssid_length > 32u || !pmk) return 0;
    uint8_t block[20];
    pbkdf2_block((const uint8_t *)passphrase, pass_length, ssid,
                 ssid_length, 1u, block);
    memcpy(pmk, block, 20u);
    pbkdf2_block((const uint8_t *)passphrase, pass_length, ssid,
                 ssid_length, 2u, block);
    memcpy(pmk + 20u, block, 12u); memset(block, 0, sizeof(block));
    return 1;
}

static int byte_compare(const uint8_t *a, const uint8_t *b, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

void wpa2_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6],
                     const uint8_t spa[6], const uint8_t anonce[32],
                     const uint8_t snonce[32], uint8_t ptk[64]) {
    static const uint8_t label[] = "Pairwise key expansion";
    uint8_t context[76], input[100], digest[20];
    const uint8_t *mac1 = byte_compare(aa, spa, 6u) < 0 ? aa : spa;
    const uint8_t *mac2 = mac1 == aa ? spa : aa;
    const uint8_t *nonce1 = byte_compare(anonce, snonce, 32u) < 0 ?
                            anonce : snonce;
    const uint8_t *nonce2 = nonce1 == anonce ? snonce : anonce;
    memcpy(context, mac1, 6u); memcpy(context + 6u, mac2, 6u);
    memcpy(context + 12u, nonce1, 32u); memcpy(context + 44u, nonce2, 32u);
    memcpy(input, label, sizeof(label) - 1u); input[sizeof(label) - 1u] = 0;
    memcpy(input + sizeof(label), context, sizeof(context));
    for (uint8_t counter = 0; counter < 4u; counter++) {
        input[sizeof(label) + sizeof(context)] = counter;
        wpa2_hmac_sha1(pmk, 32u, input,
                       sizeof(label) + sizeof(context) + 1u, digest);
        uint32_t copy = counter == 3u ? 4u : 20u;
        memcpy(ptk + counter * 20u, digest, copy);
    }
    memset(context, 0, sizeof(context)); memset(input, 0, sizeof(input));
    memset(digest, 0, sizeof(digest));
}

int wpa2_constant_time_equal(const uint8_t *a, const uint8_t *b,
                             uint32_t length) {
    uint8_t difference = 0;
    for (uint32_t i = 0; i < length; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

static const uint8_t aes_inverse_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t aes_multiply(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    while (b) {
        if (b & 1u) result ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1Bu : 0u));
        b >>= 1;
    }
    return result;
}

static uint8_t aes_forward_sbox(uint8_t value) {
    for (uint32_t i = 0; i < 256u; i++)
        if (aes_inverse_sbox[i] == value) return (uint8_t)i;
    return 0;
}

static void aes_expand_key(const uint8_t key[16], uint8_t round_keys[176]) {
    static const uint8_t rcon[10] =
        {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    memcpy(round_keys, key, 16u);
    uint32_t generated = 16u, round = 0;
    uint8_t temp[4];
    while (generated < 176u) {
        memcpy(temp, round_keys + generated - 4u, 4u);
        if ((generated & 15u) == 0) {
            uint8_t first = temp[0];
            temp[0] = aes_forward_sbox(temp[1]) ^ rcon[round++];
            temp[1] = aes_forward_sbox(temp[2]);
            temp[2] = aes_forward_sbox(temp[3]);
            temp[3] = aes_forward_sbox(first);
        }
        for (uint32_t i = 0; i < 4u; i++) {
            round_keys[generated] = round_keys[generated - 16u] ^ temp[i];
            generated++;
        }
    }
}

static void aes_add_round_key(uint8_t state[16], const uint8_t *key) {
    for (uint32_t i = 0; i < 16u; i++) state[i] ^= key[i];
}

static void aes_inverse_shift_rows(uint8_t s[16]) {
    uint8_t t;
    t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
}

static void aes_inverse_mix_columns(uint8_t s[16]) {
    for (uint32_t c = 0; c < 4u; c++) {
        uint8_t *p = s + c * 4u;
        uint8_t a=p[0], b=p[1], d=p[2], e=p[3];
        p[0]=aes_multiply(a,14)^aes_multiply(b,11)^aes_multiply(d,13)^aes_multiply(e,9);
        p[1]=aes_multiply(a,9)^aes_multiply(b,14)^aes_multiply(d,11)^aes_multiply(e,13);
        p[2]=aes_multiply(a,13)^aes_multiply(b,9)^aes_multiply(d,14)^aes_multiply(e,11);
        p[3]=aes_multiply(a,11)^aes_multiply(b,13)^aes_multiply(d,9)^aes_multiply(e,14);
    }
}

static void aes128_decrypt(const uint8_t key[16], const uint8_t input[16],
                           uint8_t output[16]) {
    uint8_t state[16], round_keys[176];
    memcpy(state, input, 16u); aes_expand_key(key, round_keys);
    aes_add_round_key(state, round_keys + 160u);
    for (int round = 9; round > 0; round--) {
        aes_inverse_shift_rows(state);
        for (uint32_t i = 0; i < 16u; i++) state[i] = aes_inverse_sbox[state[i]];
        aes_add_round_key(state, round_keys + (uint32_t)round * 16u);
        aes_inverse_mix_columns(state);
    }
    aes_inverse_shift_rows(state);
    for (uint32_t i = 0; i < 16u; i++) state[i] = aes_inverse_sbox[state[i]];
    aes_add_round_key(state, round_keys);
    memcpy(output, state, 16u); memset(state, 0, sizeof(state));
    memset(round_keys, 0, sizeof(round_keys));
}

int wpa2_aes_key_unwrap(const uint8_t kek[16], const uint8_t *ciphertext,
                        uint32_t ciphertext_length, uint8_t *plaintext,
                        uint32_t *plaintext_length) {
    if (!kek || !ciphertext || !plaintext || !plaintext_length ||
        ciphertext_length < 24u || (ciphertext_length & 7u)) return 0;
    uint32_t n = ciphertext_length / 8u - 1u;
    if (*plaintext_length < n * 8u) return 0;
    uint8_t a[8], block[16], decrypted[16];
    memcpy(a, ciphertext, 8u);
    memcpy(plaintext, ciphertext + 8u, n * 8u);
    for (int j = 5; j >= 0; j--) {
        for (uint32_t reverse = n; reverse > 0u; reverse--) {
            uint64_t t = (uint64_t)n * (uint32_t)j + reverse;
            memcpy(block, a, 8u);
            for (uint32_t byte = 0; byte < 8u; byte++)
                block[7u - byte] ^= (uint8_t)(t >> (byte * 8u));
            memcpy(block + 8u, plaintext + (reverse - 1u) * 8u, 8u);
            aes128_decrypt(kek, block, decrypted);
            memcpy(a, decrypted, 8u);
            memcpy(plaintext + (reverse - 1u) * 8u, decrypted + 8u, 8u);
        }
    }
    static const uint8_t integrity[8] =
        {0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6};
    if (!wpa2_constant_time_equal(a, integrity, 8u)) {
        memset(plaintext, 0, n * 8u); return 0;
    }
    *plaintext_length = n * 8u;
    memset(a, 0, sizeof(a)); memset(block, 0, sizeof(block));
    memset(decrypted, 0, sizeof(decrypted));
    return 1;
}
