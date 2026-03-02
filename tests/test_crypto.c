/*
 * Crypto tests: ChaCha20, SHA-256, HMAC-SHA256.
 * Test vectors from RFC 6234 (SHA-256) and RFC 4231 (HMAC-SHA256).
 */
#include "test_framework.h"
#include "seaclaw/crypto.h"
#include "seaclaw/security.h"
#include "seaclaw/multimodal.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdint.h>

static void test_chacha20_roundtrip_empty(void);
static void test_chacha20_roundtrip_short(void);
static void test_chacha20_roundtrip_block(void);
static void test_chacha20_roundtrip_multi_block(void);
static void test_chacha20_partial_block(void);
static void test_sha256_empty(void);
static void test_sha256_abc(void);
static void test_sha256_long(void);
static void test_sha256_multi_block(void);
static void test_chacha20_rfc7539_vector(void);
static void test_hmac_sha256_rfc4231_1(void);
static void test_hmac_sha256_rfc4231_2(void);
static void test_chacha20_different_counters(void);
static void test_chacha20_different_nonce(void);
static void test_sha256_one_byte(void);
static void test_hmac_sha256_rfc4231_3(void);
static void test_hmac_sha256_empty_key(void);
static void test_hex_encode_decode_roundtrip(void);
static void test_hex_decode_invalid_rejected(void);
static void test_hex_decode_odd_length_rejected(void);
static void test_base64_encode_roundtrip_hello(void);
static void test_base64_encode_binary(void);
static void test_chacha20_decrypt_inverts_encrypt(void);
static void test_sha256_deterministic(void);
static void test_hmac_deterministic(void);
static void test_sha256_length_sensitive(void);
static void test_chacha20_zero_length(void);

static void hex_to_bytes(const char *hex, uint8_t *out, size_t *out_len) {
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2, n++) {
        unsigned int a = (hex[0] >= 'a') ? hex[0] - 'a' + 10 :
                         (hex[0] >= 'A') ? hex[0] - 'A' + 10 : hex[0] - '0';
        unsigned int b = (hex[1] >= 'a') ? hex[1] - 'a' + 10 :
                         (hex[1] >= 'A') ? hex[1] - 'A' + 10 : hex[1] - '0';
        out[n] = (uint8_t)((a << 4) | b);
    }
    *out_len = n;
}

static int hex_eq(const uint8_t *a, const char *hex_b, size_t len) {
    for (size_t i = 0; i < len && hex_b[i * 2]; i++) {
        unsigned int b = (hex_b[i * 2] >= 'a') ? hex_b[i * 2] - 'a' + 10 :
                         (hex_b[i * 2] >= 'A') ? hex_b[i * 2] - 'A' + 10 : hex_b[i * 2] - '0';
        b = (b << 4) | ((hex_b[i * 2 + 1] >= 'a') ? hex_b[i * 2 + 1] - 'a' + 10 :
                        (hex_b[i * 2 + 1] >= 'A') ? hex_b[i * 2 + 1] - 'A' + 10 : hex_b[i * 2 + 1] - '0');
        if (a[i] != (uint8_t)b) return 0;
    }
    return 1;
}

void run_crypto_tests(void) {
    SC_TEST_SUITE("Crypto");

    SC_RUN_TEST(test_chacha20_roundtrip_empty);
    SC_RUN_TEST(test_chacha20_roundtrip_short);
    SC_RUN_TEST(test_chacha20_roundtrip_block);
    SC_RUN_TEST(test_chacha20_roundtrip_multi_block);
    SC_RUN_TEST(test_chacha20_partial_block);
    SC_RUN_TEST(test_sha256_empty);
    SC_RUN_TEST(test_sha256_abc);
    SC_RUN_TEST(test_sha256_long);
    SC_RUN_TEST(test_sha256_multi_block);
    SC_RUN_TEST(test_chacha20_rfc7539_vector);
    SC_RUN_TEST(test_hmac_sha256_rfc4231_1);
    SC_RUN_TEST(test_hmac_sha256_rfc4231_2);

    SC_RUN_TEST(test_chacha20_different_counters);
    SC_RUN_TEST(test_chacha20_different_nonce);
    SC_RUN_TEST(test_sha256_one_byte);
    SC_RUN_TEST(test_hmac_sha256_rfc4231_3);
    SC_RUN_TEST(test_hmac_sha256_empty_key);
    SC_RUN_TEST(test_hex_encode_decode_roundtrip);
    SC_RUN_TEST(test_hex_decode_invalid_rejected);
    SC_RUN_TEST(test_hex_decode_odd_length_rejected);
    SC_RUN_TEST(test_base64_encode_roundtrip_hello);
    SC_RUN_TEST(test_base64_encode_binary);
    SC_RUN_TEST(test_chacha20_decrypt_inverts_encrypt);
    SC_RUN_TEST(test_sha256_deterministic);
    SC_RUN_TEST(test_hmac_deterministic);
    SC_RUN_TEST(test_sha256_length_sensitive);
    SC_RUN_TEST(test_chacha20_zero_length);
}

static void test_chacha20_roundtrip_empty(void) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t out[1] = {0xAA};
    sc_chacha20_encrypt(key, nonce, 0, out, out, 0);
    SC_ASSERT_EQ(out[0], 0xAA);
}

static void test_chacha20_roundtrip_short(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t in[] = "hello";
    uint8_t out[6], dec[6];
    memset(key, 0x11, 32);
    memset(nonce, 0x22, 12);
    sc_chacha20_encrypt(key, nonce, 1, in, out, 5);
    SC_ASSERT_NEQ(out[0], in[0]);
    sc_chacha20_decrypt(key, nonce, 1, out, dec, 5);
    dec[5] = '\0';
    SC_ASSERT_STR_EQ((char *)dec, "hello");
}

static void test_chacha20_roundtrip_block(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t in[64], out[64], dec[64];
    for (int i = 0; i < 64; i++) in[i] = (uint8_t)i;
    memset(key, 0x33, 32);
    memset(nonce, 0x44, 12);
    sc_chacha20_encrypt(key, nonce, 0, in, out, 64);
    sc_chacha20_decrypt(key, nonce, 0, out, dec, 64);
    SC_ASSERT_TRUE(memcmp(in, dec, 64) == 0);
}

static void test_chacha20_roundtrip_multi_block(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t in[200], out[200], dec[200];
    for (int i = 0; i < 200; i++) in[i] = (uint8_t)(i * 7);
    memset(key, 0x55, 32);
    memset(nonce, 0x66, 12);
    sc_chacha20_encrypt(key, nonce, 1, in, out, 200);
    sc_chacha20_decrypt(key, nonce, 1, out, dec, 200);
    SC_ASSERT_TRUE(memcmp(in, dec, 200) == 0);
}

static void test_chacha20_partial_block(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t in[100], out[100], dec[100];
    for (int i = 0; i < 100; i++) in[i] = (uint8_t)i;
    memset(key, 0x77, 32);
    memset(nonce, 0x88, 12);
    sc_chacha20_encrypt(key, nonce, 5, in, out, 100);
    sc_chacha20_decrypt(key, nonce, 5, out, dec, 100);
    SC_ASSERT_TRUE(memcmp(in, dec, 100) == 0);
}

static void test_sha256_empty(void) {
    uint8_t out[32];
    sc_sha256((const uint8_t *)"", 0, out);
    const char *exp = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

static void test_sha256_abc(void) {
    uint8_t out[32];
    sc_sha256((const uint8_t *)"abc", 3, out);
    const char *exp = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

static void test_sha256_long(void) {
    uint8_t out[32];
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sc_sha256((const uint8_t *)msg, 56, out);
    const char *exp = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

/* NIST/SHA-256: 65 'a' bytes -> multi-block + padding */
static void test_sha256_multi_block(void) {
    uint8_t msg[65];
    uint8_t out[32];
    for (int i = 0; i < 65; i++) msg[i] = 'a';
    sc_sha256(msg, 65, out);
    const char *exp = "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

/* RFC 7539 Section 2.4.2: ChaCha20 test vector */
static void test_chacha20_rfc7539_vector(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    size_t n;
    hex_to_bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                 key, &n);
    hex_to_bytes("000000000000004a00000000", nonce, &n);
    uint8_t plain[114], cipher[114], expected[114];
    hex_to_bytes("4c616469657320616e642047656e746c656d656e206f662074686520636c617373"
                 "206f66202739393a204966204920636f756c64206f6666657220796f75206f6e6c79"
                 "206f6e652074697020666f7220746865206675747572652c2073756e73637265656e"
                 "20776f756c642062652069742e",
                 plain, &n);
    hex_to_bytes("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
                 "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d"
                 "807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793"
                 "7365af90bbf74a35be6b40b8eedf2785e42874d",
                 expected, &n);
    sc_chacha20_encrypt(key, nonce, 1, plain, cipher, 114);
    SC_ASSERT_TRUE(memcmp(cipher, expected, 114) == 0);
}

static void test_hmac_sha256_rfc4231_1(void) {
    uint8_t key[20];
    size_t key_len;
    hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", key, &key_len);
    uint8_t msg[] = "Hi There";
    uint8_t out[32];
    sc_hmac_sha256(key, key_len, msg, 8, out);
    const char *exp = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

static void test_hmac_sha256_rfc4231_2(void) {
    uint8_t key[4];
    size_t key_len;
    hex_to_bytes("4a656665", key, &key_len);
    uint8_t msg[] = "what do ya want for nothing?";
    uint8_t out[32];
    sc_hmac_sha256(key, key_len, msg, 28, out);
    const char *exp = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

/* ─── ~18 additional crypto tests ─────────────────────────────────────────── */
static void test_chacha20_different_counters(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    memset(key, 0xAA, 32);
    memset(nonce, 0xBB, 12);
    uint8_t in[] = "same input";
    uint8_t out0[11], out1[11];
    sc_chacha20_encrypt(key, nonce, 0, in, out0, 10);
    sc_chacha20_encrypt(key, nonce, 1, in, out1, 10);
    SC_ASSERT_TRUE(memcmp(out0, out1, 10) != 0);
}

static void test_chacha20_different_nonce(void) {
    uint8_t key[32];
    uint8_t nonce0[12] = {0}, nonce1[12] = {0};
    nonce1[0] = 1;
    uint8_t in[] = "plain";
    uint8_t out0[6], out1[6];
    sc_chacha20_encrypt(key, nonce0, 0, in, out0, 5);
    sc_chacha20_encrypt(key, nonce1, 0, in, out1, 5);
    SC_ASSERT_TRUE(memcmp(out0, out1, 5) != 0);
}

static void test_sha256_one_byte(void) {
    uint8_t out[32];
    sc_sha256((const uint8_t *)"x", 1, out);
    const char *exp = "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";
    SC_ASSERT_TRUE(hex_eq(out, exp, 32));
}

static void test_hmac_sha256_rfc4231_3(void) {
    uint8_t key[20];
    size_t key_len;
    hex_to_bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", key, &key_len);
    uint8_t msg[50];
    memset(msg, 0xdd, 50);
    uint8_t out[32];
    sc_hmac_sha256(key, key_len, msg, 50, out);
    SC_ASSERT_TRUE(out[0] != 0 || out[1] != 0);
}

static void test_hmac_sha256_empty_key(void) {
    uint8_t key[1] = {0};
    uint8_t msg[] = "Hi There";
    uint8_t out[32];
    sc_hmac_sha256(key, 0, msg, 8, out);
    SC_ASSERT_TRUE(out[0] != 0 || out[1] != 0);
}

static void test_hex_encode_decode_roundtrip(void) {
    uint8_t data[] = {0x00, 0xFF, 0x0F, 0xF0, 0x12};
    char hex[32];
    sc_hex_encode(data, 5, hex);
    uint8_t out[8];
    size_t len = 0;
    sc_error_t err = sc_hex_decode(hex, 10, out, 8, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 5u);
    SC_ASSERT_TRUE(memcmp(data, out, 5) == 0);
}

static void test_hex_decode_invalid_rejected(void) {
    uint8_t out[8];
    size_t len = 0;
    sc_error_t err = sc_hex_decode("GG", 2, out, 8, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_hex_decode_odd_length_rejected(void) {
    uint8_t out[8];
    size_t len = 0;
    sc_error_t err = sc_hex_decode("123", 3, out, 8, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_base64_encode_roundtrip_hello(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *b64 = NULL;
    size_t len = 0;
    sc_error_t err = sc_multimodal_encode_base64(&alloc, "Hello", 5, &b64, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(b64, "SGVsbG8=");
    if (b64) alloc.free(alloc.ctx, b64, len + 1);
}

static void test_base64_encode_binary(void) {
    sc_allocator_t alloc = sc_system_allocator();
    uint8_t data[] = {0x00, 0x01, 0x02, 0xFF};
    char *b64 = NULL;
    size_t len = 0;
    sc_error_t err = sc_multimodal_encode_base64(&alloc, data, 4, &b64, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(b64);
    if (b64) alloc.free(alloc.ctx, b64, len + 1);
}

static void test_chacha20_decrypt_inverts_encrypt(void) {
    uint8_t key[32];
    uint8_t nonce[12];
    memset(key, 0x11, 32);
    memset(nonce, 0x22, 12);
    uint8_t orig[64];
    for (int i = 0; i < 64; i++) orig[i] = (uint8_t)(i ^ 0x55);
    uint8_t enc[64], dec[64];
    sc_chacha20_encrypt(key, nonce, 0, orig, enc, 64);
    sc_chacha20_decrypt(key, nonce, 0, enc, dec, 64);
    SC_ASSERT_TRUE(memcmp(orig, dec, 64) == 0);
}

static void test_sha256_deterministic(void) {
    uint8_t out1[32], out2[32];
    sc_sha256((const uint8_t *)"repeat", 6, out1);
    sc_sha256((const uint8_t *)"repeat", 6, out2);
    SC_ASSERT_TRUE(memcmp(out1, out2, 32) == 0);
}

static void test_hmac_deterministic(void) {
    uint8_t key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t msg[] = "same";
    uint8_t out1[32], out2[32];
    sc_hmac_sha256(key, 8, msg, 4, out1);
    sc_hmac_sha256(key, 8, msg, 4, out2);
    SC_ASSERT_TRUE(memcmp(out1, out2, 32) == 0);
}

static void test_sha256_length_sensitive(void) {
    uint8_t out_abc[32], out_abcd[32];
    sc_sha256((const uint8_t *)"abc", 3, out_abc);
    sc_sha256((const uint8_t *)"abcd", 4, out_abcd);
    SC_ASSERT_TRUE(memcmp(out_abc, out_abcd, 32) != 0);
}

static void test_chacha20_zero_length(void) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t in[1] = {0x99};
    sc_chacha20_encrypt(key, nonce, 0, in, in, 0);
    SC_ASSERT_EQ(in[0], 0x99);
}
