// The phone's SSH key: an ECDSA P-256 key made on the phone, its private half as PEM (kept with
// gs_secret) and its public half as an OpenSSH line for the server's authorized_keys.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// A new key: its private half as PEM in `pem` (zero-terminated).
bool gp_key_new(char *pem, size_t pem_size);
// The public half of the key in `pem`, as "ecdsa-sha2-nistp256 BASE64 COMMENT".
bool gp_key_public(const char *pem, const char *comment, char *out, size_t size);
// The same as the binary blob SSH sends (RFC 5656), 104 bytes; its length, or 0.
size_t gp_key_blob(const char *pem, unsigned char *out, size_t size);
// A signature of `data` with the key, as SSH's publickey authentication takes it for this key type:
// the r and s of ECDSA over SHA-256, each as an SSH mpint. Its length, or 0.
size_t gp_key_sign(const char *pem, const unsigned char *data, size_t len, unsigned char *out, size_t size);
