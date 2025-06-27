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
   /* transfers ownership of zero buffer */
   uint32s_index_insert_exact(index, 0, zeros, 0);
   RBUF_CLEAR(index->additions); /* scrap first addition, we never want to delete 0s during rewind */
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
   uint32_t *coll = bucket->len < 4 ? bucket->contents.idxs : bucket->contents.vec.idxs;
   for(uint32_t i = 0; i < bucket->len; i++)
   {
      uint32_t idx = coll[i];
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

bool uint32s_bucket_remove(struct uint32s_bucket *bucket, uint32_t idx)
{
   bool small = bucket->len < 4;
   uint32_t *coll = small ? bucket->contents.idxs : bucket->contents.vec.idxs;
   if(idx == 0) /* never remove 0s pattern */
      return false;
   for(int i = 0; i < (int)bucket->len; i++)
   {
      if(coll[i] == idx)
      {
         if(bucket->len == 4)
         {
            memcpy((uint8_t *)bucket->contents.idxs, (uint8_t *)coll, 3*sizeof(uint32_t));
            free(coll);
         }
         else
            memmove((uint8_t*)(coll+i), (uint8_t*)(coll+i+1), (bucket->len-(i+1))*sizeof(uint32_t));
         bucket->len--;
         return true;
      }
   }
   return false;
}

uint32s_insert_result_t uint32s_index_insert(uint32s_index_t *index, uint32_t *object, uint64_t frame)
{
   struct uint32s_bucket *bucket;
   uint32s_insert_result_t result;
   size_t size_bytes = index->object_size * sizeof(uint32_t);
   uint32_t hash = uint32s_hash_bytes((uint8_t *)object, size_bytes);
   uint32_t idx;
   uint32_t *copy, *check;
   uint32_t additions_len = RBUF_LEN(index->additions);
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
   if(additions_len == 0 || index->additions[additions_len-1].frame_counter < frame)
   {
      struct uint32s_frame_addition addition;
      addition.frame_counter = frame;
      addition.first_index = result.index;
      RBUF_PUSH(index->additions, addition);
   }
   return result;
}

bool uint32s_index_insert_exact(uint32s_index_t *index, uint32_t idx, uint32_t *object, uint64_t frame)
{
   struct uint32s_bucket *bucket;
   uint32_t hash;
   size_t size_bytes;
   uint32_t additions_len;
   if(idx != RBUF_LEN(index->objects))
      return false;
   size_bytes = index->object_size * sizeof(uint32_t);
   hash = uint32s_hash_bytes((uint8_t *)object, size_bytes);
   additions_len = RBUF_LEN(index->additions);
   if(RHMAP_HAS(index->index, hash))
   {
      uint32_t _index;
      bucket = RHMAP_PTR(index->index, hash);
      if(uint32s_bucket_get(index, bucket, object, size_bytes, &_index))
         return false;
      uint32s_bucket_expand(bucket, idx);
   }
   else
   {
      struct uint32s_bucket new_bucket;
      new_bucket.len = 1;
      new_bucket.contents.idxs[0] = idx;
      new_bucket.contents.idxs[1] = 0;
      new_bucket.contents.idxs[2] = 0;
      RHMAP_SET(index->index, hash, new_bucket);
   }
   RBUF_PUSH(index->objects, object);
   if(additions_len == 0 || index->additions[additions_len-1].frame_counter < frame)
   {
      struct uint32s_frame_addition addition;
      addition.frame_counter = frame;
      addition.first_index = idx;
      RBUF_PUSH(index->additions, addition);
   }
   return true;
}

uint32_t *uint32s_index_get(uint32s_index_t *index, uint32_t which)
{
   if(which >= RBUF_LEN(index->objects))
      return NULL;
   return index->objects[which];
}

void uint32s_index_pop(uint32s_index_t *index)
{
   uint32_t idx = RBUF_LEN(index->objects)-1;
   uint32_t *object = RBUF_POP(index->objects);
   size_t size_bytes = index->object_size * sizeof(uint32_t);
   uint32_t hash = uint32s_hash_bytes((uint8_t *)object, size_bytes);
   struct uint32s_bucket *bucket = RHMAP_PTR(index->index, hash);
   uint32s_bucket_remove(bucket, idx);
}

/* goes backwards from end of additions */
void uint32s_index_remove_after(uint32s_index_t *index, uint64_t frame)
{
   int i;
   for(i = RBUF_LEN(index->additions)-1; i >= 0; i--)
   {
      struct uint32s_frame_addition add = index->additions[i];
      if(add.frame_counter < frame)
         break;
      while(add.first_index > RBUF_LEN(index->objects))
         uint32s_index_pop(index);
   }
   RBUF_RESIZE(index->additions, i+1);
}

void uint32s_bucket_free(struct uint32s_bucket bucket)
{
   if(bucket.len > 3)
      free(bucket.contents.vec.idxs);
}

/* removes all data from index */
void uint32s_index_clear(uint32s_index_t *index)
{
   size_t i, cap;
   uint32_t *zeros = index->objects[0];
   for(i = 0, cap = RHMAP_CAP(index->index); i != cap; i++)
      if(RHMAP_KEY(index->index, i))
         uint32s_bucket_free(index->index[i]);
   RHMAP_CLEAR(index->index);
   /* don't dealloc all-zeros pattern */
   for(i = 1; i < RBUF_LEN(index->objects); i++)
      free(index->objects[i]);
   RBUF_CLEAR(index->objects);
   uint32s_index_insert_exact(index, 0, zeros, 0);
   /* wipe additions */
   RBUF_CLEAR(index->additions);
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
   RBUF_FREE(index->additions);
   free(index);
}

