#include "uint32s_index.h"
#include <string.h>
#include <array/rhmap.h>
#include <array/rbuf.h>

uint32s_index_t *uint32s_index_new(size_t object_size)
{
   uint32_t *zeros = calloc(object_size, sizeof(uint32_t));
   uint32s_index_t *index = malloc(sizeof(uint32s_index_t));
   index->object_size = object_size;
   index->index = NULL;
   index->objects = NULL;
   uint32s_index_insert(index, zeros);
   free(zeros);
   return index;
}

uint32_t uint32s_hash_bytes(uint8_t *bytes, size_t len)
{
   uint32_t hash = (uint32_t)0x811c9dc5;
   for(size_t i = 0; i < len; i++)
      hash = ((hash * (uint32_t)0x01000193) ^ (uint32_t)(bytes[i]));
   return (hash ? hash : 1);
}

bool uint32s_bucket_get(uint32s_index_t *index, struct uint32s_bucket *bucket, uint32_t *object, size_t size_bytes, uint32_t *out_idx)
{
   uint32_t idx;
   bool small = (bucket->len < 4);
   for(uint32_t i = 0; i < bucket->len; i++)
   {
      idx = small ? bucket->contents.idxs[i] : bucket->contents.vec.idxs[i];
      if(memcmp(index->objects[idx], object, size_bytes) == 0)
      {
         *out_idx = idx;
         return true;
      }
   }
   return false;
}

void uint32s_bucket_expand(struct uint32s_bucket *bucket, uint32_t idx)
{
   if(bucket->len < 3)
   {
      bucket->contents.idxs[bucket->len] = idx;
      bucket->len++;
   }
   else if(bucket->len == 3)
   {
      uint32_t *idxs = calloc(8, sizeof(uint32_t));
      memcpy(idxs, bucket->contents.idxs, 3*sizeof(uint32_t));
      bucket->contents.vec.cap = 8;
      bucket->contents.vec.idxs = idxs;
      bucket->contents.vec.idxs[bucket->len] = idx;
      bucket->len++;
   }
   else if(bucket->len < bucket->contents.vec.cap)
   {
      bucket->contents.vec.idxs[bucket->len] = idx;
      bucket->len++;
   }
   else /* bucket->len == bucket->contents.vec.cap */
   {
      bucket->contents.vec.cap *= 2;
      bucket->contents.vec.idxs = realloc(bucket->contents.vec.idxs, bucket->contents.vec.cap*sizeof(uint32_t));
      bucket->contents.vec.idxs[bucket->len] = idx;
      bucket->len++;
   }
}

uint32s_insert_result_t uint32s_index_insert(uint32s_index_t *index, uint32_t *object)
{
   struct uint32s_bucket *bucket;
   uint32s_insert_result_t result;
   size_t size_bytes = index->object_size * sizeof(uint32_t);
   uint32_t hash = uint32s_hash_bytes((uint8_t *)object, size_bytes);
   uint32_t idx;
   uint32_t *copy, *check;
   result.index = 0;
   result.is_new = false;
   if(RHMAP_HAS(index->index, hash))
   {
      bucket = RHMAP_PTR(index->index, hash);
      if(uint32s_bucket_get(index, bucket, object, size_bytes, &result.index))
      {
         result.is_new = false;
         return result;
      }
      idx = RBUF_LEN(index->objects);
      copy = malloc(size_bytes);
      memcpy(copy, object, size_bytes);
      RBUF_PUSH(index->objects, copy);
      result.index = idx;
      result.is_new = true;
      uint32s_bucket_expand(bucket, idx);
   }
   else
   {
      struct uint32s_bucket new_bucket;
      idx = RBUF_LEN(index->objects);
      copy = malloc(size_bytes);
      memcpy(copy, object, size_bytes);
      RBUF_PUSH(index->objects, copy);
      new_bucket.len = 1;
      new_bucket.contents.idxs[0] = idx;
      new_bucket.contents.idxs[1] = 0;
      new_bucket.contents.idxs[2] = 0;
      RHMAP_SET(index->index, hash, new_bucket);
      result.index = idx;
      result.is_new = true;
   }
   return result;
}

uint32_t *uint32s_index_get(uint32s_index_t *index, uint32_t which)
{
   if(which >= RBUF_LEN(index->objects))
      return NULL;
   return index->objects[which];
}

void uint32s_bucket_free(struct uint32s_bucket bucket)
{
   if(bucket.len > 3)
      free(bucket.contents.vec.idxs);
}

void uint32s_index_free(uint32s_index_t *index)
{
   size_t i, cap;
   for(i = 0, cap = RHMAP_CAP(index->index); i != cap; i++)
      if(RHMAP_KEY(index->index, i))
         uint32s_bucket_free(index->index[i]);
   RHMAP_FREE(index->index);
   for(i = 0; i < RBUF_LEN(index->objects); i++)
      free(index->objects[i]);
   RBUF_FREE(index->objects);
}

