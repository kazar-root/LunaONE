/* LunaCore macOS port, 2026-11-18. */
#include "utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"

int utils_random_string(char* s, const int len) {
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  const size_t symbols = sizeof(alphanum) - 1;
  const size_t cutoff = 256 - (256 % symbols);
  unsigned char bytes[MBEDTLS_ENTROPY_BLOCK_SIZE];
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);

  int written = 0;
  while (written < len) {
    if (mbedtls_entropy_func(&entropy, bytes, sizeof(bytes)) != 0) {
      mbedtls_entropy_free(&entropy);
      s[0] = '\0';
      return -1;
    }
    for (size_t i = 0; i < sizeof(bytes) && written < len; ++i) {
      if (bytes[i] < cutoff)
        s[written++] = alphanum[bytes[i] % symbols];
    }
  }

  s[len] = '\0';
  mbedtls_entropy_free(&entropy);
  return 0;
}

void utils_get_hmac_sha1(const char* input, size_t input_len, const char* key, size_t key_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA1;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, key_len);
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_hmac_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}

void utils_get_md5(const char* input, size_t input_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_MD5;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}
