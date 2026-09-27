#include "mt6592_wifi_crypto.h"

#include <stddef.h>
#include <stdint.h>

/* Small freestanding SHA-1/HMAC/PBKDF2 and AES unwrap implementation. The
 * MT6592 firmware owns CCMP; the host only needs WPA2-PSK key derivation and
 * RFC 3394 GTK unwrapping during the EAPOL four-way handshake.
 *
 * Provenance, and why it differs from the rest of these drivers
 * ------------------------------------------------------------
 * Every other mt6592_wifi_*.c file is checked line by line against MediaTek's
 * own MT6592 sources under Reference/MT6592LK. This one cannot be: there is no
 * counterpart to it in that tree. MediaTek's driver never derives a PMK or
 * unwraps a GTK -- it takes finished keys across the ioctl boundary (the WPA
 * state machine in drv_wlan/mt_wifi/wlan/mgmt/rsn.c consumes keys, it does not
 * compute them), and the code that computes them lives in the wpa_supplicant
 * userspace binary, which is not part of a kernel tree. Grepping the whole of
 * Reference/MT6592LK for "pbkdf2", "Pairwise key expansion" or "aes_unwrap"
 * returns nothing.
 *
 * So the ground truth here is the published standards, and the way to hold the
 * code to them is a known-answer harness rather than a citation to a line of
 * vendor source. Each primitive below carries the vectors it was checked
 * against; all of them pass, byte for byte:
 *
 *   SHA-1        FIPS 180-4 / RFC 3174: "", "abc", the 56-byte 448-bit string,
 *                and 10^6 x 'a'. Plus 119- and 120-byte inputs, which straddle
 *                the point where the length field no longer fits in the final
 *                block and sha1_final has to emit an extra one.
 *   HMAC-SHA1    RFC 2202 test cases 1 through 7, which includes the two
 *                80-byte-key cases -- the only ones that reach the
 *                hash-the-key-first branch. Plus 64- and 65-byte keys, the
 *                exact boundary of that branch.
 *   PBKDF2       IEEE 802.11i-2004 Annex H.4.2, all three vectors:
 *                  "password"       / "IEEE"        -> f42c6fc5 2df0ebef ...
 *                  "ThisIsAPassword"/ "ThisIsASSID" -> 0dc0d6eb 90555ed6 ...
 *                  63 x 'a'         / 32 x 'Z'      -> 2d43d0da bfdd6353 ...
 *   PRF-512      Cross-checked against OpenBSD's ieee80211_prf and
 *                ieee80211_derive_ptk (Reference/openbsd/sys/net80211/
 *                ieee80211_crypto.c:325-349 and :386-415) -- see the note on
 *                the label length at mt6592_wifi_wpa_prf_512.
 *   AES unwrap   RFC 3394 4.1, the same KEK/ciphertext pair OpenBSD regresses
 *                against (Reference/openbsd/regress/lib/libcrypto/aeswrap/
 *                aes_wrap.c:104-123). Also n=4 and n=5 payloads, the sizes a
 *                real GTK KDE arrives in, since 4.1 is only n=2 and would not
 *                catch a wrong wrapping-integer t. And the four rejection
 *                paths: corrupt ciphertext, short, unaligned, no room.
 *
 * The decrypt-side structure was read against OpenBSD's aes_key_unwrap
 * (Reference/openbsd/sys/crypto/key_wrap.c:78-112), which is the same loop:
 * t counts down from 6n as j goes 5..0 and i goes n..1. */

typedef struct
{
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
} wifi_sha1_ctx;

static uint32_t rol32(uint32_t value, uint32_t bits)
{
    return (value << bits) | (value >> (32u - bits));
}

static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) dst[i] = src[i];
}

static void zero_bytes(uint8_t* dst, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) dst[i] = 0u;
}

static uint32_t text_length(const char* text)
{
    uint32_t length = 0u;
    if (!text) return 0u;
    while (text[length] != '\0') ++length;
    return length;
}

static void sha1_transform(wifi_sha1_ctx* ctx, const uint8_t block[64])
{
    uint32_t w[80];
    for (uint32_t i = 0; i < 16u; ++i) w[i] = read_be32(block + i * 4u);
    for (uint32_t i = 16u; i < 80u; ++i) w[i] = rol32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    for (uint32_t i = 0; i < 80u; ++i)
    {
        uint32_t f;
        uint32_t k;
        if (i < 20u)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        }
        else if (i < 40u)
        {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        }
        else if (i < 60u)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const uint32_t temp = rol32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30u);
        b = a;
        a = temp;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(wifi_sha1_ctx* ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xefcdab89u;
    ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xc3d2e1f0u;
    ctx->bytes = 0u;
    ctx->used = 0u;
}

static void sha1_update(wifi_sha1_ctx* ctx, const uint8_t* data, uint32_t size)
{
    if (!data || size == 0u) return;
    ctx->bytes += size;
    while (size != 0u)
    {
        uint32_t take = 64u - ctx->used;
        if (take > size) take = size;
        copy_bytes(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        size -= take;
        if (ctx->used == 64u)
        {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0u;
        }
    }
}

static void sha1_final(wifi_sha1_ctx* ctx, uint8_t out[20])
{
    const uint64_t bits = ctx->bytes * 8u;
    ctx->block[ctx->used++] = 0x80u;
    if (ctx->used > 56u)
    {
        while (ctx->used < 64u) ctx->block[ctx->used++] = 0u;
        sha1_transform(ctx, ctx->block);
        ctx->used = 0u;
    }
    while (ctx->used < 56u) ctx->block[ctx->used++] = 0u;
    for (uint32_t i = 0; i < 8u; ++i) ctx->block[56u + i] = (uint8_t)(bits >> (56u - i * 8u));
    sha1_transform(ctx, ctx->block);
    for (uint32_t i = 0; i < 5u; ++i) write_be32(out + i * 4u, ctx->h[i]);
}

/*
 * HMAC-SHA1, RFC 2104 2. Keys longer than the 64-byte block are hashed down
 * first and keys shorter are zero-padded up, which is why key_block is zeroed
 * before either branch writes into it -- the padding is not an afterthought,
 * it is part of the definition. The boundary is "> 64", so a key of exactly 64
 * bytes is used as-is; RFC 2202's test cases 6 and 7 are the ones that pin the
 * other side of it down.
 */
void mt6592_wifi_hmac_sha1(const uint8_t* key, uint32_t key_len,
                           const uint8_t* data, uint32_t data_len,
                           uint8_t out[20])
{
    uint8_t key_block[64];
    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    uint8_t digest[20];
    zero_bytes(key_block, sizeof(key_block));
    if (key_len > sizeof(key_block))
    {
        wifi_sha1_ctx key_hash;
        sha1_init(&key_hash);
        sha1_update(&key_hash, key, key_len);
        sha1_final(&key_hash, digest);
        copy_bytes(key_block, digest, sizeof(digest));
    }
    else if (key && key_len)
    {
        copy_bytes(key_block, key, key_len);
    }
    for (uint32_t i = 0; i < 64u; ++i)
    {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    wifi_sha1_ctx inner;
    sha1_init(&inner);
    sha1_update(&inner, inner_pad, sizeof(inner_pad));
    sha1_update(&inner, data, data_len);
    sha1_final(&inner, digest);

    wifi_sha1_ctx outer;
    sha1_init(&outer);
    sha1_update(&outer, outer_pad, sizeof(outer_pad));
    sha1_update(&outer, digest, sizeof(digest));
    sha1_final(&outer, out);
}

/*
 * PBKDF2-HMAC-SHA1 (RFC 2898 5.2) with the iteration count and output length
 * that IEEE 802.11i-2004 Annex H.4 fixes for a WPA PSK: c = 4096, dkLen = 32.
 * Both are baked in rather than passed, because nothing on this device is
 * allowed to ask for anything else -- an AP does not negotiate them.
 *
 * The bounds are the standard's, not ours: a passphrase is 8..63 printable
 * characters and an SSID is 1..32 octets (Annex H.4.1). salt[36] is exactly
 * 32 + 4, the largest SSID plus the big-endian block index INT(i), so the
 * ssid_len check above is also the bounds check on the buffer.
 *
 * Two blocks are generated for 40 bytes of keying material and the last 8 are
 * dropped: the loop takes 20 from block 1 and 12 from block 2. F() runs the PRF
 * 4096 times per block -- once on salt||INT(i), then 4095 times chaining U -- so
 * the inner loop starts at 1 and stops before 4096, not at 0.
 *
 * cooperative_yield is called every 128 iterations. This is a pre-MMU boot
 * environment with no preemption, and the full derivation is ~8192 HMACs; on a
 * cold MT6592 core that is long enough that a watchdog or a UART drain needs a
 * turn. Nothing about the result depends on it, so it may be null.
 */
int mt6592_wifi_pbkdf2_sha1(const char* passphrase,
                            const uint8_t* ssid,
                            uint32_t ssid_len,
                            uint8_t out[32],
                            void (*cooperative_yield)(void))
{
    const uint32_t pass_len = text_length(passphrase);
    uint8_t salt[36];
    uint8_t u[20];
    uint8_t t[20];
    if (!passphrase || pass_len < 8u || pass_len > 63u || !ssid || ssid_len == 0u || ssid_len > 32u || !out)
        return -1;

    copy_bytes(salt, ssid, ssid_len);
    for (uint32_t block = 1u; block <= 2u; ++block)
    {
        salt[ssid_len + 0u] = 0u;
        salt[ssid_len + 1u] = 0u;
        salt[ssid_len + 2u] = 0u;
        salt[ssid_len + 3u] = (uint8_t)block;
        mt6592_wifi_hmac_sha1((const uint8_t*)passphrase, pass_len, salt, ssid_len + 4u, u);
        copy_bytes(t, u, sizeof(t));
        for (uint32_t iteration = 1u; iteration < 4096u; ++iteration)
        {
            mt6592_wifi_hmac_sha1((const uint8_t*)passphrase, pass_len, u, sizeof(u), u);
            for (uint32_t i = 0; i < sizeof(t); ++i) t[i] ^= u[i];
            if (cooperative_yield && (iteration & 0x7fu) == 0u) cooperative_yield();
        }
        const uint32_t offset = (block - 1u) * 20u;
        const uint32_t take = offset + 20u <= 32u ? 20u : 32u - offset;
        copy_bytes(out + offset, t, take);
    }
    return 0;
}

static int compare_bytes(const uint8_t* a, const uint8_t* b, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

/*
 * PTK = PRF-512(PMK, "Pairwise key expansion",
 *               Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce) || Max(...))
 * -- IEEE 802.11i-2004 8.5.1.2, PRF itself at 8.5.1.1.
 *
 * The one thing about this that is easy to get wrong, and that the spec states
 * in prose rather than in a formula, is that the label is followed by a zero
 * octet and that the zero is *inside* the hashed data. OpenBSD makes the point
 * explicitly by passing the label length two different ways from the same call
 * site (ieee80211_crypto.c:404-413):
 *
 *     ieee80211_kdf(..., "Pairwise key expansion", 22 // KDF omits \0
 *     ieee80211_prf(..., "Pairwise key expansion", 23 // PRF uses \0
 *
 * The SHA-256 KDF used by the 802.11w AKMs hashes 22 bytes; the SHA-1 PRF used
 * by WPA/WPA2-PSK hashes 23. We are the PRF case, so the string's terminator
 * counts: 22 label bytes, then the explicit data[pos++] = 0 below. That makes
 * the hashed buffer 22 + 1 + 12 + 64 + 1 = 100 bytes, which is why data[] is
 * sized 100 and not rounded up -- if the layout is ever changed the array stops
 * fitting, which is the failure mode we want.
 *
 * The counter is a single octet appended last and starting at zero, so 64 bytes
 * come out of four HMACs with the last one truncated to 4 bytes
 * (ieee80211_prf, :334-348).
 *
 * Min/Max is compare_bytes, i.e. memcmp order. Ours branches on <= 0 where
 * OpenBSD branches on < 0 (:395, :400); the two disagree only when the operands
 * are equal, and then both orderings emit the same bytes. A supplicant and an
 * authenticator that swap their arguments must land on the same PTK, so that
 * symmetry is checked directly in the harness rather than reasoned about.
 */
void mt6592_wifi_wpa_prf_512(const uint8_t pmk[32],
                             const uint8_t mac_a[6],
                             const uint8_t mac_b[6],
                             const uint8_t nonce_a[32],
                             const uint8_t nonce_b[32],
                             uint8_t ptk[64])
{
    static const uint8_t label[] = "Pairwise key expansion";
    uint8_t data[100];
    uint8_t digest[20];
    uint32_t pos = 0u;
    copy_bytes(data + pos, label, sizeof(label) - 1u);
    pos += sizeof(label) - 1u;
    data[pos++] = 0u;
    if (compare_bytes(mac_a, mac_b, 6u) <= 0)
    {
        copy_bytes(data + pos, mac_a, 6u);
        copy_bytes(data + pos + 6u, mac_b, 6u);
    }
    else
    {
        copy_bytes(data + pos, mac_b, 6u);
        copy_bytes(data + pos + 6u, mac_a, 6u);
    }
    pos += 12u;
    if (compare_bytes(nonce_a, nonce_b, 32u) <= 0)
    {
        copy_bytes(data + pos, nonce_a, 32u);
        copy_bytes(data + pos + 32u, nonce_b, 32u);
    }
    else
    {
        copy_bytes(data + pos, nonce_b, 32u);
        copy_bytes(data + pos + 32u, nonce_a, 32u);
    }
    pos += 64u;

    uint32_t written = 0u;
    for (uint8_t counter = 0u; written < 64u; ++counter)
    {
        data[pos] = counter;
        mt6592_wifi_hmac_sha1(pmk, 32u, data, pos + 1u, digest);
        uint32_t take = 64u - written;
        if (take > sizeof(digest)) take = sizeof(digest);
        copy_bytes(ptk + written, digest, take);
        written += take;
    }
}

/*
 * AES-128 decryption, enough to run RFC 3394 key unwrap and nothing more.
 *
 * The S-boxes are computed from their definition (FIPS-197 5.1.1) instead of
 * being tabled: the multiplicative inverse in GF(2^8) is x^254 by Fermat, then
 * the affine transform. The forward affine is b ^ rotl(b,1..4) ^ 0x63 and the
 * inverse is rotl(b,1) ^ rotl(b,3) ^ rotl(b,6) ^ 0x05, applied before the
 * inversion rather than after. Two 256-byte tables would be faster, but this
 * stage runs pre-MMU out of a small SRAM window and unwraps one 32- or 40-byte
 * GTK per association -- half a kilobyte of .rodata costs more here than the
 * few thousand cycles do. The tradeoff is only defensible if the generated
 * boxes are actually right, so the harness checks the full AES-128 block
 * against FIPS-197 C.1 before it checks anything built on top of it.
 *
 * aes_decrypt_block is the straightforward inverse cipher of FIPS-197 5.3, not
 * the equivalent-inverse form: InvShiftRows, InvSubBytes, AddRoundKey,
 * InvMixColumns, so the round keys are used in plain order and no separate
 * inverse key schedule is needed.
 */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t out = 0u;
    for (uint32_t i = 0; i < 8u; ++i)
    {
        if (b & 1u) out ^= a;
        const uint8_t high = a & 0x80u;
        a <<= 1u;
        if (high) a ^= 0x1bu;
        b >>= 1u;
    }
    return out;
}

static uint8_t gf_pow(uint8_t value, uint8_t power)
{
    uint8_t result = 1u;
    while (power)
    {
        if (power & 1u) result = gf_mul(result, value);
        value = gf_mul(value, value);
        power >>= 1u;
    }
    return result;
}

static uint8_t rotl8(uint8_t value, uint32_t bits)
{
    return (uint8_t)((value << bits) | (value >> (8u - bits)));
}

static uint8_t aes_sbox(uint8_t value)
{
    const uint8_t inverse = value ? gf_pow(value, 254u) : 0u;
    return (uint8_t)(inverse ^ rotl8(inverse, 1u) ^ rotl8(inverse, 2u) ^
                     rotl8(inverse, 3u) ^ rotl8(inverse, 4u) ^ 0x63u);
}

static uint8_t aes_inverse_sbox(uint8_t value)
{
    const uint8_t affine_inverse = (uint8_t)(rotl8(value, 1u) ^ rotl8(value, 3u) ^
                                             rotl8(value, 6u) ^ 0x05u);
    return affine_inverse ? gf_pow(affine_inverse, 254u) : 0u;
}

static void aes_expand_key(const uint8_t key[16], uint8_t round_keys[176])
{
    static const uint8_t rcon[10] = {0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u, 0x1bu, 0x36u};
    copy_bytes(round_keys, key, 16u);
    uint32_t generated = 16u;
    uint32_t round = 0u;
    uint8_t temp[4];
    while (generated < 176u)
    {
        copy_bytes(temp, round_keys + generated - 4u, 4u);
        if ((generated & 15u) == 0u)
        {
            const uint8_t first = temp[0];
            temp[0] = aes_sbox(temp[1]);
            temp[1] = aes_sbox(temp[2]);
            temp[2] = aes_sbox(temp[3]);
            temp[3] = aes_sbox(first);
            temp[0] ^= rcon[round++];
        }
        for (uint32_t i = 0; i < 4u; ++i)
        {
            round_keys[generated] = (uint8_t)(round_keys[generated - 16u] ^ temp[i]);
            ++generated;
        }
    }
}

static void aes_add_round_key(uint8_t state[16], const uint8_t* round_key)
{
    for (uint32_t i = 0; i < 16u; ++i) state[i] ^= round_key[i];
}

static void aes_inverse_shift_rows(uint8_t state[16])
{
    uint8_t temp;
    temp = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = temp;
    temp = state[2]; state[2] = state[10]; state[10] = temp;
    temp = state[6]; state[6] = state[14]; state[14] = temp;
    temp = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = temp;
}

static void aes_inverse_sub_bytes(uint8_t state[16])
{
    for (uint32_t i = 0; i < 16u; ++i) state[i] = aes_inverse_sbox(state[i]);
}

static void aes_inverse_mix_columns(uint8_t state[16])
{
    for (uint32_t column = 0; column < 4u; ++column)
    {
        uint8_t* p = state + column * 4u;
        const uint8_t a = p[0];
        const uint8_t b = p[1];
        const uint8_t c = p[2];
        const uint8_t d = p[3];
        p[0] = (uint8_t)(gf_mul(a, 14u) ^ gf_mul(b, 11u) ^ gf_mul(c, 13u) ^ gf_mul(d, 9u));
        p[1] = (uint8_t)(gf_mul(a, 9u) ^ gf_mul(b, 14u) ^ gf_mul(c, 11u) ^ gf_mul(d, 13u));
        p[2] = (uint8_t)(gf_mul(a, 13u) ^ gf_mul(b, 9u) ^ gf_mul(c, 14u) ^ gf_mul(d, 11u));
        p[3] = (uint8_t)(gf_mul(a, 11u) ^ gf_mul(b, 13u) ^ gf_mul(c, 9u) ^ gf_mul(d, 14u));
    }
}

static void aes_decrypt_block(const uint8_t round_keys[176], const uint8_t input[16], uint8_t output[16])
{
    uint8_t state[16];
    copy_bytes(state, input, 16u);
    aes_add_round_key(state, round_keys + 160u);
    for (int round = 9; round > 0; --round)
    {
        aes_inverse_shift_rows(state);
        aes_inverse_sub_bytes(state);
        aes_add_round_key(state, round_keys + (uint32_t)round * 16u);
        aes_inverse_mix_columns(state);
    }
    aes_inverse_shift_rows(state);
    aes_inverse_sub_bytes(state);
    aes_add_round_key(state, round_keys);
    copy_bytes(output, state, 16u);
}

/*
 * RFC 3394 2.2.2 key unwrap, the index-based variant. n = (len/8) - 1 eight-byte
 * registers plus the leading A; j counts 5 down to 0, i counts n down to 1, and
 * the wrapping integer t = n*j + i is XORed big-endian into A before each block
 * decrypt. OpenBSD writes the same loop with t seeded at 6n and decremented
 * (key_wrap.c:89-107), which is the same sequence read the other way round.
 *
 * Integrity is the whole point of the function. A wrapped key that has been
 * altered anywhere decrypts to a random A, so the check against the a6a6...a6
 * IV (RFC 3394 2.2.3.1) is what stands between a corrupt or spoofed EAPOL frame
 * and a garbage GTK being installed as if it were real. It runs on every call
 * and there is no way for a caller to skip it: the plaintext length is only
 * returned after A matches, and -1 covers both "malformed input" and "failed
 * authentication" because the caller must treat them identically.
 *
 * The length rules are RFC 3394's: at least 24 bytes (A plus two registers --
 * the construction is undefined for n = 1) and a multiple of 8. plain_capacity
 * is checked before anything is written, so a short output buffer is rejected
 * rather than overrun.
 *
 * Note that plain is filled with ciphertext first and decrypted in place, so on
 * an integrity failure it is left holding rubbish, not the previous contents.
 * That is deliberate -- there is nothing there for a caller to mistake for a
 * key -- but it does mean the -1 path must be honoured, not just logged.
 */
int mt6592_wifi_aes_unwrap(const uint8_t kek[16],
                           const uint8_t* wrapped,
                           uint32_t wrapped_len,
                           uint8_t* plain,
                           uint32_t plain_capacity)
{
    uint8_t round_keys[176];
    uint8_t a[8];
    uint8_t block[16];
    uint8_t decrypted[16];
    if (!kek || !wrapped || !plain || wrapped_len < 24u || (wrapped_len & 7u) != 0u) return -1;
    const uint32_t n = wrapped_len / 8u - 1u;
    if (n * 8u > plain_capacity) return -1;
    aes_expand_key(kek, round_keys);
    copy_bytes(a, wrapped, 8u);
    copy_bytes(plain, wrapped + 8u, n * 8u);

    for (int j = 5; j >= 0; --j)
    {
        for (uint32_t i = n; i > 0u; --i)
        {
            copy_bytes(block, a, 8u);
            const uint64_t t = (uint64_t)n * (uint32_t)j + i;
            for (uint32_t byte = 0; byte < 8u; ++byte)
                block[7u - byte] ^= (uint8_t)(t >> (byte * 8u));
            copy_bytes(block + 8u, plain + (i - 1u) * 8u, 8u);
            aes_decrypt_block(round_keys, block, decrypted);
            copy_bytes(a, decrypted, 8u);
            copy_bytes(plain + (i - 1u) * 8u, decrypted + 8u, 8u);
        }
    }
    for (uint32_t i = 0; i < 8u; ++i)
        if (a[i] != 0xa6u) return -1;
    return (int)(n * 8u);
}
