#ifndef __UINT32S_INDEX__H
#define __UINT32S_INDEX__H

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include <boolean.h>

#define UINT32S_INDEX_KEYLEN 8

struct uint32s_prefix
{
   uint32_t key[UINT32S_INDEX_KEYLEN]; /* key for prefixes up to INDEX_KEYLEN, value for terminal nodes */
   uint32_t *children; /* an rbuf of offsets within the index's trie */
};
struct uint32s_index
{
   uint32_t object_size; /* measured in ints */
   struct uint32s_prefix *trie; /* an rbuf of trie nodes for value->index lookup */
   uint32_t *roots;      /* an rbuf of root nodes of the trie */
   uint32_t **objects;   /* an rbuf of the actual buffers */
};
typedef struct uint32s_index uint32s_index_t;

struct uint32s_insert_result
{
   uint32_t index;
   bool is_new;
};

uint32s_index_t *uint32s_index_new(uint32_t object_size);
uint32s_insert_result uint32s_index_insert(uint32s_index_t *index, uint32_t *object);
uint32_t *uint32s_index_get(uint32s_index_t *index, uint32_t which);
void uint32s_index_free(uint32s_index_t *index);

#endif /* __UINT32S_INDEX__H */
