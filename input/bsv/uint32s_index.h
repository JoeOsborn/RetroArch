#ifndef __UINT32S_INDEX__H
#define __UINT32S_INDEX__H

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include <boolean.h>

struct uint32s_bucket
{
   uint32_t len; /* if < 4, contents is idxs. */
   union {
     uint32_t idxs[3];
     struct {
       uint32_t cap;
       uint32_t *idxs;
     } vec;
   } contents;
};
struct uint32s_index
{
   size_t object_size; /* measured in ints */
   struct uint32s_bucket *index; /* an rhmap of buckets for value->index lookup */
   uint32_t **objects;   /* an rbuf of the actual buffers */
};
typedef struct uint32s_index uint32s_index_t;

struct uint32s_insert_result
{
   uint32_t index;
   bool is_new;
};
typedef struct uint32s_insert_result uint32s_insert_result_t;

uint32s_index_t *uint32s_index_new(size_t object_size);
/* Does not take ownership of object */
uint32s_insert_result_t uint32s_index_insert(uint32s_index_t *index, uint32_t *object);
/* Does take ownership, requires idx is the exact next index and object not in index */
bool uint32s_index_insert_exact(uint32s_index_t *index, uint32_t idx, uint32_t *object);
/* Does not grant ownership of return value */
uint32_t *uint32s_index_get(uint32s_index_t *index, uint32_t which);
void uint32s_index_free(uint32s_index_t *index);

#endif /* __UINT32S_INDEX__H */
