#ifndef WPA2_CRYPTO_H
#define WPA2_CRYPTO_H

#include <stdint.h>

void wpa2_sha1(const uint8_t *data, uint32_t length, uint8_t out[20]);
void wpa2_hmac_sha1(const uint8_t *key, uint32_t key_length,
                    const uint8_t *data, uint32_t data_length,
                    uint8_t out[20]);
int wpa2_derive_pmk(const char *passphrase, const uint8_t *ssid,
                    uint32_t ssid_length, uint8_t pmk[32]);
void wpa2_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6],
                     const uint8_t spa[6], const uint8_t anonce[32],
                     const uint8_t snonce[32], uint8_t ptk[64]);
int wpa2_constant_time_equal(const uint8_t *a, const uint8_t *b,
                             uint32_t length);
int wpa2_aes_key_unwrap(const uint8_t kek[16], const uint8_t *ciphertext,
                        uint32_t ciphertext_length, uint8_t *plaintext,
                        uint32_t *plaintext_length);

#endif
