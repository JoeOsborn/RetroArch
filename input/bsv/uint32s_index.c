#include "uint32s_index.h"

uint32s_index_t *uint32s_index_new(uint32_t object_size)
{
   uint32_t *zeros = calloc(object_size, sizeof(uint32_t));
   uint32s_index_t *index = malloc(sizeof(uint32s_index_t));
   index->object_size = object_size;
   index->trie = NULL;
   index->roots = NULL;
   index->objects = NULL;
   uint32s_index_insert(index, zeros);
   free(zeros);
   return index;
}

bool _uint32s_keypart_matches(uint32_t *key, uint32_t *check)
{
  int result = 1;
  for(int i = 0; i < UINT32S_INDEX_KEYLEN; i++)
    {
      result &= key[i] == check[i];
    }
  return result;
}

uint32s_insert_result uint32s_index_insert(uint32s_index_t *index, uint32_t *object)
{
   uint32_t steps = index->object_size / UINT32S_INDEX_KEYLEN;
   uint32_t step  = 0;
   uint32_t *options = index->roots;
   uint32s_insert_result result;
   result.index = 0;
   result.is_new = false;
   for(step = 0; step < steps; step++)
     {
       int opt;
       for(opt = 0; opt < RBUF_LEN(options); opt++)
         {
           uint32_t option = index->trie[options[opt]];
           if(_uint32s_keypart_matches(option.key, object+(step*UINT32S_INDEX_KEYLEN)))
             {
               options = option.children;
               break;
             }
         }
       if(opt >= RBUF_LEN(options))
         {
           uint32_t *copy = malloc(index->object_size*sizeof(uint32_t));
           uint32_t idx = RBUF_LEN(index->objects);
           uint32s_prefix *node = calloc(1, sizeof(uint32s_prefix));
           memcpy(copy, object, index->object_size*sizeof(uint32_t));
           RBUF_PUSH(index->objects, copy);
           result.index = idx;
           result.is_new = true;
           for(int k = 0; k < UINT32S_INDEX_KEYLEN; k++)
             {
               node->key[k] = object[step*UINT32S_INDEX_KEYLEN+k];
             }
           RBUF_PUSH(options, node);
           options = node->children;
           for(; step < steps; step++)
             {
               // TODO
               // push one option into the current options
               // set options = that node's options
             }
           // TODO create value trie node
           
           return result;
         }
     }
   result.index = index->trie[options[0]].key[0];
   return result;
}

uint32_t *uint32s_index_get(uint32s_index_t *index, uint32_t which)
{
  return index->objects[which];
}

void uint32s_prefix_free(uint32s_prefix *prefix)
{
   RBUF_FREE(prefix->children);
}

void uint32s_index_free(uint32s_index_t *index)
{
   for(int i = 0; i < RBUF_LEN(index->trie); i++)
      uint32s_prefix_free(index->trie[i]);
   RBUF_FREE(index->trie);
   RBUF_FREE(index->roots);
   for(int i = 0; i < RBUF_LEN(index->objects))
      free(index->objects[i]);
   RBUF_FREE(index->objects);
}

