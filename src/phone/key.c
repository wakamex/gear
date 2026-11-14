#include "key.h"
#include <string.h>
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

typedef struct { mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg; } rng;

static bool rng_open(rng *r) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    static const char who[] = "gear phone key";
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy, (const unsigned char *)who, sizeof who - 1) == 0;
}

static void rng_close(rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

bool gp_key_new(char *pem, size_t pem_size) {
    rng r;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = rng_open(&r) && mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
              mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &r.drbg) == 0 &&
              mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem, pem_size) == 0;
    mbedtls_pk_free(&pk);
    rng_close(&r);
    return ok;
}

// An SSH string: its length in four bytes, big endian, then the bytes.
static size_t put(unsigned char *at, const void *data, size_t n) {
    at[0] = (unsigned char)(n >> 24), at[1] = (unsigned char)(n >> 16), at[2] = (unsigned char)(n >> 8), at[3] = (unsigned char)n;
    memcpy(at + 4, data, n);
    return 4 + n;
}

// The key from its PEM, with a random generator for what needs one.
static bool parse(mbedtls_pk_context *pk, rng *r, const char *pem) {
    return rng_open(r) &&
           mbedtls_pk_parse_key(pk, (const unsigned char *)pem, strlen(pem) + 1, NULL, 0, mbedtls_ctr_drbg_random, &r->drbg) == 0 &&
           mbedtls_pk_get_type(pk) == MBEDTLS_PK_ECKEY;
}

size_t gp_key_blob(const char *pem, unsigned char *out, size_t size) {
    rng r;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    unsigned char point[65];
    size_t n = 0, len = 0;
    if (parse(&pk, &r, pem) && size >= 104 &&
        mbedtls_ecp_write_public_key(mbedtls_pk_ec(pk), MBEDTLS_ECP_PF_UNCOMPRESSED, &n, point, sizeof point) == 0 && n == 65) {
        // RFC 5656: the key type, the curve's name and the point.
        len = put(out, "ecdsa-sha2-nistp256", 19);
        len += put(out + len, "nistp256", 8);
        len += put(out + len, point, 65);
    }
    mbedtls_pk_free(&pk);
    rng_close(&r);
    return len;
}

bool gp_key_public(const char *pem, const char *comment, char *out, size_t size) {
    unsigned char blob[128];
    size_t len = gp_key_blob(pem, blob, sizeof blob), written = 0;
    static const char type[] = "ecdsa-sha2-nistp256 ";
    size_t head = sizeof type - 1;
    if (!len || size <= head || mbedtls_base64_encode((unsigned char *)out + head, size - head, &written, blob, len)) return false;
    memcpy(out, type, head);
    size_t at = head + written;
    if (comment && *comment && at + 1 + strlen(comment) < size) out[at++] = ' ', strcpy(out + at, comment), at += strlen(comment);
    out[at] = 0;
    return true;
}

// An SSH mpint of a positive number: its big-endian bytes, without leading zeros, and one zero in front
// when the first byte has its high bit set.
static size_t put_mpint(unsigned char *at, const mbedtls_mpi *x) {
    unsigned char b[33];
    size_t n = mbedtls_mpi_size(x);
    b[0] = 0;
    mbedtls_mpi_write_binary(x, b + 1, n);
    size_t lead = (b[1] & 0x80) ? 0 : 1;
    return put(at, b + lead, n + 1 - lead);
}

size_t gp_key_sign(const char *pem, const unsigned char *data, size_t len, unsigned char *out, size_t size) {
    rng r;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_mpi R, S;
    mbedtls_mpi_init(&R), mbedtls_mpi_init(&S);
    unsigned char hash[32];
    size_t n = 0;
    if (parse(&pk, &r, pem) && size >= 2 * (4 + 33) && mbedtls_sha256(data, len, hash, 0) == 0) {
        mbedtls_ecp_keypair *k = mbedtls_pk_ec(pk);
        mbedtls_ecp_group grp;
        mbedtls_mpi d;
        mbedtls_ecp_group_init(&grp), mbedtls_mpi_init(&d);
        if (mbedtls_ecp_export(k, &grp, &d, NULL) == 0 &&
            mbedtls_ecdsa_sign(&grp, &R, &S, &d, hash, sizeof hash, mbedtls_ctr_drbg_random, &r.drbg) == 0) {
            n = put_mpint(out, &R);
            n += put_mpint(out + n, &S);
        }
        mbedtls_ecp_group_free(&grp), mbedtls_mpi_free(&d);
    }
    mbedtls_mpi_free(&R), mbedtls_mpi_free(&S);
    mbedtls_pk_free(&pk);
    rng_close(&r);
    return n;
}
