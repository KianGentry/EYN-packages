#pragma once

#include <stddef.h>

void* eyn_mbedtls_calloc(size_t nmemb, size_t size);
void eyn_mbedtls_free(void* ptr);
void eyn_mbedtls_alloc_reset(void);
