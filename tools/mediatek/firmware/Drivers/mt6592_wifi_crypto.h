#ifndef MT6592_WIFI_CRYPTO_H
#define MT6592_WIFI_CRYPTO_H

#include <stdint.h>

/*
 * The host half of WPA2-PSK. The MT6592 firmware does CCMP itself, so all this
 * layer has to produce is the key material the four-way handshake consumes, in
 * the order the handshake consumes it:
 *
 *   PMK  = mt6592_wifi_pbkdf2_sha1(passphrase, ssid)          once, per network
 *   PTK  = mt6592_wifi_wpa_prf_512(PMK, addrs, nonces)        on message 1/4
 *   MIC  = mt6592_wifi_hmac_sha1(KCK, frame)                  on 2/4 and 4/4
 *   GTK  = mt6592_wifi_aes_unwrap(KEK, key data)              from 3/4
 *
 * Every one of these is checked against published known-answer vectors rather
 * than against MediaTek's sources, which do not contain any of it; the header
 * comment in mt6592_wifi_crypto.c lists the vectors and where they come from.
 *
 * Nothing here is constant-time. It does not need to be: the inputs are a
 * locally held passphrase and locally generated nonces, and there is no
 * attacker-controlled repetition to time. Do not lift it into a context where
 * that stops being true.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 2104 HMAC-SHA1. Any key length; out is always the full 20 bytes, so a
 * caller wanting the 16-byte EAPOL MIC truncates it itself. */
void mt6592_wifi_hmac_sha1(const uint8_t* key, uint32_t key_len,
                           const uint8_t* data, uint32_t data_len,
                           uint8_t out[20]);

/*
 * PSK -> PMK, PBKDF2-HMAC-SHA1 with 802.11i's fixed c=4096 and dkLen=32.
 * passphrase is 8..63 characters, ssid_len is 1..32; -1 on anything else.
 * Roughly 8192 HMAC-SHA1 operations, so it is slow by design -- pass
 * cooperative_yield to get a callback every 128 iterations, or null.
 */
int mt6592_wifi_pbkdf2_sha1(const char* passphrase,
                            const uint8_t* ssid,
                            uint32_t ssid_len,
                            uint8_t out[32],
                            void (*cooperative_yield)(void));

/*
 * PMK -> PTK, 802.11i 8.5.1.2. mac_a/mac_b and nonce_a/nonce_b are sorted
 * internally, so it does not matter which way round the authenticator and
 * supplicant values are passed. The 64 bytes come out laid out as
 *
 *     ptk[ 0..15]  KCK  confirmation key -- keys the EAPOL MIC
 *     ptk[16..31]  KEK  encryption key   -- the kek argument to aes_unwrap
 *     ptk[32..63]  TK   temporal key     -- handed to the firmware
 *
 * (Same split as struct ieee80211_ptk, Reference/openbsd/sys/net80211/
 * ieee80211.h:1170-1175, which is where the 64-byte output length comes from.)
 */
void mt6592_wifi_wpa_prf_512(const uint8_t pmk[32],
                             const uint8_t mac_a[6],
                             const uint8_t mac_b[6],
                             const uint8_t nonce_a[32],
                             const uint8_t nonce_b[32],
                             uint8_t ptk[64]);

/*
 * RFC 3394 AES-128 unwrap. Returns the plaintext byte count, or -1.
 *
 * -1 means either malformed input or a failed integrity check, and the caller
 * must not distinguish: a wrapped key whose IV does not come back as a6a6..a6
 * has been tampered with. plain holds partially decrypted rubbish in that case,
 * never the wrapped bytes and never anything usable, so the return value is the
 * only thing that says whether a key is there.
 */
int mt6592_wifi_aes_unwrap(const uint8_t kek[16],
                           const uint8_t* wrapped,
                           uint32_t wrapped_len,
                           uint8_t* plain,
                           uint32_t plain_capacity);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_WIFI_CRYPTO_H */
