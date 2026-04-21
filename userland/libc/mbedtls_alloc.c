#include <mbedtls_alloc.h>

#include <stdint.h>
#include <string.h>

#define EYN_MBEDTLS_POOL_BYTES (128u * 1024u)
#define EYN_MBEDTLS_ALIGN      8u

typedef struct eyn_mbedtls_block {
    size_t size;
    unsigned char free;
    struct eyn_mbedtls_block* next;
    struct eyn_mbedtls_block* prev;
} eyn_mbedtls_block_t;

static union {
    uint64_t align;
    unsigned char bytes[EYN_MBEDTLS_POOL_BYTES];
} g_pool;

static eyn_mbedtls_block_t* g_head = NULL;
static int g_pool_initialized = 0;

static size_t eyn_mbedtls_align_up(size_t value) {
    return (value + (EYN_MBEDTLS_ALIGN - 1u)) & ~(EYN_MBEDTLS_ALIGN - 1u);
}

static void eyn_mbedtls_pool_init(void) {
    g_head = (eyn_mbedtls_block_t*)g_pool.bytes;
    g_head->size = EYN_MBEDTLS_POOL_BYTES - sizeof(eyn_mbedtls_block_t);
    g_head->free = 1;
    g_head->next = NULL;
    g_head->prev = NULL;
    g_pool_initialized = 1;
}

static void eyn_mbedtls_split_block(eyn_mbedtls_block_t* block, size_t need) {
    if (!block) return;

    if (block->size <= need + sizeof(eyn_mbedtls_block_t) + EYN_MBEDTLS_ALIGN) {
        return;
    }

    unsigned char* raw = (unsigned char*)block;
    eyn_mbedtls_block_t* right = (eyn_mbedtls_block_t*)(raw + sizeof(eyn_mbedtls_block_t) + need);
    right->size = block->size - need - sizeof(eyn_mbedtls_block_t);
    right->free = 1;
    right->next = block->next;
    right->prev = block;
    if (right->next) right->next->prev = right;

    block->size = need;
    block->next = right;
}

static void eyn_mbedtls_coalesce_forward(eyn_mbedtls_block_t* block) {
    while (block && block->next && block->next->free) {
        eyn_mbedtls_block_t* right = block->next;
        block->size += sizeof(eyn_mbedtls_block_t) + right->size;
        block->next = right->next;
        if (block->next) block->next->prev = block;
    }
}

void* eyn_mbedtls_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    if (nmemb > ((size_t)-1) / size) return NULL;
    size_t total = eyn_mbedtls_align_up(nmemb * size);
    if (total == 0) return NULL;

    if (!g_pool_initialized) eyn_mbedtls_pool_init();

    eyn_mbedtls_block_t* block = g_head;
    while (block) {
        if (block->free && block->size >= total) {
            eyn_mbedtls_split_block(block, total);
            block->free = 0;

            void* payload = (unsigned char*)block + sizeof(eyn_mbedtls_block_t);
            memset(payload, 0, total);
            return payload;
        }
        block = block->next;
    }

    return NULL;
}

void eyn_mbedtls_free(void* ptr) {
    if (!ptr || !g_pool_initialized) return;

    unsigned char* p = (unsigned char*)ptr;
    unsigned char* pool_start = g_pool.bytes;
    unsigned char* pool_end = g_pool.bytes + EYN_MBEDTLS_POOL_BYTES;

    if (p <= pool_start || p >= pool_end) return;

    eyn_mbedtls_block_t* block = (eyn_mbedtls_block_t*)(p - sizeof(eyn_mbedtls_block_t));
    if ((unsigned char*)block < pool_start || (unsigned char*)block >= pool_end) return;

    block->free = 1;
    eyn_mbedtls_coalesce_forward(block);

    if (block->prev && block->prev->free) {
        eyn_mbedtls_coalesce_forward(block->prev);
    }
}

void eyn_mbedtls_alloc_reset(void) {
    g_pool_initialized = 0;
    g_head = NULL;
}
