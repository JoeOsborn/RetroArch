#include <retro_endianness.h>
#include "../../retroarch.h"
#include "../../state_manager.h"
#include "../../verbosity.h"
#include "../input_driver.h"
#include "../../tasks/task_content.h"
#include "../../libretro-db/rmsgpack.h"
#include "../../libretro-db/rmsgpack_dom.h"
#ifdef HAVE_CHEEVOS
#include "../../cheevos/cheevos.h"
#endif

#define BSV_IFRAME_START_TOKEN 0x00
/* after START:
   frame counter uint
   state size (uncompressed) uint
   new block and new superblock data (see below)
   superblock seq (see below)
 */
#define BSV_IFRAME_NEW_BLOCK_TOKEN 0x01
/* after NEW_BLOCK:
   index uint
   binary
 */
#define BSV_IFRAME_NEW_SUPERBLOCK_TOKEN 0x02
/* after NEW_SUPERBLOCK:
   index uint
   array of uints
*/
#define BSV_IFRAME_SUPERBLOCK_SEQ_TOKEN 0x03
/* after SUPERBLOCK_SEQ:
   array of uints
   */

/* Later, tokens for pframes */

/* Forward declaration */
void bsv_movie_free(bsv_movie_t*);

void bsv_movie_enqueue(input_driver_state_t *input_st,
      bsv_movie_t * state, enum bsv_flags flags)
{
   if (input_st->bsv_movie_state_next_handle)
      bsv_movie_free(input_st->bsv_movie_state_next_handle);
   input_st->bsv_movie_state_next_handle    = state;
   input_st->bsv_movie_state.flags          = flags;
}

void bsv_movie_deinit(input_driver_state_t *input_st)
{
   if (input_st->bsv_movie_state_handle)
      bsv_movie_free(input_st->bsv_movie_state_handle);
   input_st->bsv_movie_state_handle = NULL;
}

void bsv_movie_deinit_full(input_driver_state_t *input_st)
{
   bsv_movie_deinit(input_st);
   if (input_st->bsv_movie_state_next_handle)
      bsv_movie_free(input_st->bsv_movie_state_next_handle);
   input_st->bsv_movie_state_next_handle = NULL;
}

void bsv_movie_frame_rewind()
{
   input_driver_state_t *input_st = input_state_get_ptr();
   bsv_movie_t          *handle   = input_st->bsv_movie_state_handle;
   bool recording = (input_st->bsv_movie_state.flags
         & BSV_FLAG_MOVIE_RECORDING) ? true : false;

   if (!handle)
      return;

   handle->did_rewind = true;

   if (     ( (handle->frame_counter & handle->frame_mask) <= 1)
         && (handle->frame_pos[0] == handle->min_file_pos))
   {
      /* If we're at the beginning... */
      handle->frame_counter = 0;
      intfstream_seek(handle->file, (int)handle->min_file_pos, SEEK_SET);
      // TODO: clear or reset incremental checkpoint table data
      if (recording)
         intfstream_truncate(handle->file, (int)handle->min_file_pos);
      else
         bsv_movie_read_next_events(handle, false);
   }
   else
   {
      /* First time rewind is performed, the old frame is simply replayed.
       * However, playing back that frame caused us to read data, and push
       * data to the ring buffer.
       *
       * Successively rewinding frames, we need to rewind past the read data,
       * plus another. */
      uint8_t delta = handle->first_rewind ? 1 : 2;
      if (handle->frame_counter >= delta)
         handle->frame_counter -= delta;
      else
         handle->frame_counter = 0;
      intfstream_seek(handle->file, (int)handle->frame_pos[handle->frame_counter & handle->frame_mask], SEEK_SET);
      // TODO: update incremental checkpoint table data by dropping data from later frames
      if (recording)
         intfstream_truncate(handle->file, (int)handle->frame_pos[handle->frame_counter & handle->frame_mask]);
      else
        bsv_movie_read_next_events(handle, false);
   }

   if (intfstream_tell(handle->file) <= (long)handle->min_file_pos)
   {
      /* We rewound past the beginning. */
      // TODO: clear or reset incremental checkpoint table data

      if (handle->playback)
      {
         intfstream_seek(handle->file, (int)handle->min_file_pos, SEEK_SET);
         bsv_movie_read_next_events(handle, false);
      }
      else
      {
         retro_ctx_serialize_info_t serial_info;

         /* If recording, we simply reset
          * the starting point. Nice and easy. */

         intfstream_seek(handle->file, 4 * sizeof(uint32_t), SEEK_SET);
         intfstream_truncate(handle->file, 4 * sizeof(uint32_t));

         serial_info.data = handle->state;
         serial_info.size = handle->state_size;

         core_serialize(&serial_info);

         intfstream_write(handle->file, handle->state, handle->state_size);
      }
   }
}

void bsv_movie_push_key_event(bsv_movie_t *movie,
      uint8_t down, uint16_t mod, uint32_t code, uint32_t character)
{
   bsv_key_data_t data;
   data.down                                 = down;
   data._padding                             = 0;
   data.mod                                  = swap_if_big16(mod);
   data.code                                 = swap_if_big32(code);
   data.character                            = swap_if_big32(character);
   movie->key_events[movie->key_event_count] = data;
   movie->key_event_count++;
}

void bsv_movie_push_input_event(bsv_movie_t *movie,
     uint8_t port, uint8_t dev, uint8_t idx, uint16_t id, int16_t val)
{
   bsv_input_data_t data;
   data.port                          = port;
   data.device                        = dev;
   data.idx                           = idx;
   data._padding                      = 0;
   data.id                            = swap_if_big16(id);
   data.value                         = swap_if_big16(val);
   movie->input_events[movie->input_event_count] = data;
   movie->input_event_count++;
}

bool bsv_movie_handle_read_input_event(bsv_movie_t *movie,
     uint8_t port, uint8_t dev, uint8_t idx, uint16_t id, int16_t* val)
{
   int i;
   /* if movie is old, just read two bytes and hope for the best */
   if (movie->version == 0)
   {
      int64_t read = intfstream_read(movie->file, val, 2);
      *val         = swap_if_big16(*val);
      return (read == 2);
   }
   for (i = 0; i < movie->input_event_count; i++)
   {
      bsv_input_data_t evt = movie->input_events[i];
      if (   (evt.port   == port)
          && (evt.device == dev)
          && (evt.idx    == idx)
          && (evt.id     == id))
      {
         *val = swap_if_big16(evt.value);
         return true;
      }
   }
   return false;
}

void bsv_movie_finish_rewind(input_driver_state_t *input_st)
{
   bsv_movie_t *handle    = input_st->bsv_movie_state_handle;
   if (!handle)
      return;
   handle->frame_counter += 1;
   handle->first_rewind   = !handle->did_rewind;
   handle->did_rewind     = false;
}

bool bsv_movie_read_next_events(bsv_movie_t *handle, bool skip_checkpoints)
{
   input_driver_state_t *input_st = input_state_get_ptr();
   if (intfstream_read(handle->file, &(handle->key_event_count), 1) == 1)
   {
      int i;
      for (i = 0; i < handle->key_event_count; i++)
      {
         if (intfstream_read(handle->file, &(handle->key_events[i]),
                  sizeof(bsv_key_data_t)) != sizeof(bsv_key_data_t))
         {
            /* Unnatural EOF */
            RARCH_ERR("[Replay] Keyboard replay ran out of keyboard inputs too early\n");
            input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
            return false;
         }
      }
   }
   else
   {
      RARCH_LOG("[Replay] EOF after buttons\n");
      /* Natural(?) EOF */
      input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
      return false;
   }
   if (handle->version > 0)
   {
      if (intfstream_read(handle->file, &(handle->input_event_count), 2) == 2)
      {
         int i;
         handle->input_event_count = swap_if_big16(handle->input_event_count);
         for (i = 0; i < handle->input_event_count; i++)
         {
            if (intfstream_read(handle->file, &(handle->input_events[i]),
                     sizeof(bsv_input_data_t)) != sizeof(bsv_input_data_t))
            {
               /* Unnatural EOF */
               RARCH_ERR("[Replay] Input replay ran out of inputs too early\n");
               input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
               return false;
            }
         }
      }
      else
      {
         RARCH_LOG("[Replay] EOF after inputs\n");
         /* Natural(?) EOF */
         input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
         return false;
      }
   }

   {
      uint8_t next_frame_type=REPLAY_TOKEN_INVALID;
      if (intfstream_read(handle->file, (uint8_t *)(&next_frame_type),
               sizeof(uint8_t)) != sizeof(uint8_t))
      {
         /* Unnatural EOF */
         RARCH_ERR("[Replay] Replay ran out of frames\n");
         input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
         return false;
      }
      else if (next_frame_type == REPLAY_TOKEN_CHECKPOINT_FRAME)
      {
         uint64_t size;
         uint8_t *st;
         retro_ctx_serialize_info_t serial_info;

         if (intfstream_read(handle->file, &(size),
             sizeof(uint64_t)) != sizeof(uint64_t))
         {
            RARCH_ERR("[Replay] Replay ran out of frames\n");
            input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
            return false;
         }

         size = swap_if_big64(size);
         if(skip_checkpoints)
         {
            intfstream_seek(handle->file, size, SEEK_CUR);
         }
         else
         {
            st   = (uint8_t*)malloc(size);
            if (intfstream_read(handle->file, st, size) != (int64_t)size)
            {
               RARCH_ERR("[Replay] Replay checkpoint truncated\n");
               input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
               free(st);
               return false;
            }
            // incremental decode
            serial_info.data_const = st;
            serial_info.size       = size;
            core_unserialize(&serial_info);
            free(st);
         }
      }
   }
   return true;
}

void bsv_movie_scan_from_start(input_driver_state_t *input_st, int32_t len)
{
   bsv_movie_t *movie = input_st->bsv_movie_state_handle;
   if(movie->version == 0)
     return; /* Old movies don't store enough information to fixup the frame counters. */
   // TODO: update the checkpoint reference data in the movie structure since we have traveled forward in time
   // if state_size, use the initial state to configure the data structures (maybe add a helper in task_movie.c:133, or have task_movie.c call something in here)
   intfstream_seek(movie->file, movie->min_file_pos, SEEK_SET);
   movie->frame_counter = 0;
   movie->frame_pos[0] = intfstream_tell(movie->file);
   while(intfstream_tell(movie->file) < len && bsv_movie_read_next_events(movie, true))
   {
      movie->frame_counter += 1;
      movie->frame_pos[movie->frame_counter & movie->frame_mask] = intfstream_tell(movie->file);
   }
}

void bsv_movie_next_frame(input_driver_state_t *input_st)
{
   unsigned checkpoint_interval   = config_get_ptr()->uints.replay_checkpoint_interval;
   /* if bsv_movie_state_next_handle is not null, deinit and set
      bsv_movie_state_handle to bsv_movie_state_next_handle and clear
      next_handle */
   bsv_movie_t         *handle    = input_st->bsv_movie_state_handle;
   if (input_st->bsv_movie_state_next_handle)
   {
      if (handle)
         bsv_movie_deinit(input_st);
      handle = input_st->bsv_movie_state_next_handle;
      input_st->bsv_movie_state_handle = handle;
      input_st->bsv_movie_state_next_handle = NULL;
   }

   if (!handle)
      return;
#ifdef HAVE_REWIND
   if (state_manager_frame_is_reversed())
      return;
#endif

   if (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_RECORDING)
   {
      int i;
      uint16_t evt_count = swap_if_big16(handle->input_event_count);
      /* write key events, frame is over */
      intfstream_write(handle->file, &(handle->key_event_count), 1);
      for (i = 0; i < handle->key_event_count; i++)
         intfstream_write(handle->file, &(handle->key_events[i]),
               sizeof(bsv_key_data_t));
      /* Zero out key events when playing back or recording */
      handle->key_event_count = 0;
      /* write input events, frame is over */
      intfstream_write(handle->file, &evt_count, 2);
      for (i = 0; i < handle->input_event_count; i++)
         intfstream_write(handle->file, &(handle->input_events[i]),
               sizeof(bsv_input_data_t));
      /* Zero out input events when playing back or recording */
      handle->input_event_count = 0;

      /* Maybe record checkpoint */
      if (     (checkpoint_interval != 0)
            && (handle->frame_counter > 0)
            && (handle->frame_counter % (checkpoint_interval*60) == 0))
      {
         retro_ctx_serialize_info_t serial_info;
         uint8_t frame_tok = REPLAY_TOKEN_CHECKPOINT_FRAME;
         size_t _len       = core_serialize_size();
         uint64_t size     = swap_if_big64(_len);
         uint8_t *st       = (uint8_t*)malloc(_len);
         serial_info.data  = st;
         serial_info.size  = _len;
         core_serialize(&serial_info);
         // TODO incremental encode
         /* "next frame is a checkpoint" */
         intfstream_write(handle->file, (uint8_t *)(&frame_tok), sizeof(uint8_t));
         intfstream_write(handle->file, &size, sizeof(uint64_t));
         intfstream_write(handle->file, st, _len);
         free(st);
      }
      else
      {
         uint8_t frame_tok = REPLAY_TOKEN_REGULAR_FRAME;
         /* write "next frame is not a checkpoint" */
         intfstream_write(handle->file, (uint8_t *)(&frame_tok), sizeof(uint8_t));
      }
   }

   if (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_PLAYBACK)
      bsv_movie_read_next_events(handle, false);
   handle->frame_pos[handle->frame_counter & handle->frame_mask] = intfstream_tell(handle->file);
}

size_t replay_get_serialize_size(void)
{
   input_driver_state_t *input_st = input_state_get_ptr();
   if (input_st->bsv_movie_state.flags & (BSV_FLAG_MOVIE_RECORDING | BSV_FLAG_MOVIE_PLAYBACK))
      return sizeof(int32_t)+intfstream_tell(input_st->bsv_movie_state_handle->file);
   return 0;
}

bool replay_get_serialized_data(void* buffer)
{
   input_driver_state_t *input_st = input_state_get_ptr();
   bsv_movie_t *handle            = input_st->bsv_movie_state_handle;

   if (input_st->bsv_movie_state.flags & (BSV_FLAG_MOVIE_RECORDING | BSV_FLAG_MOVIE_PLAYBACK))
   {
      int64_t file_end        = intfstream_tell(handle->file);
      int64_t read_amt        = 0;
      long file_end_lil       = swap_if_big32(file_end);
      uint8_t *file_end_bytes = (uint8_t *)(&file_end_lil);
      uint8_t *buf            = buffer;
      buf[0]                  = file_end_bytes[0];
      buf[1]                  = file_end_bytes[1];
      buf[2]                  = file_end_bytes[2];
      buf[3]                  = file_end_bytes[3];
      buf                    += 4;
      intfstream_rewind(handle->file);
      read_amt                = intfstream_read(handle->file, (void *)buf, file_end);
      if (read_amt != file_end)
         RARCH_ERR("[Replay] Failed to write correct number of replay bytes into state file: %d / %d\n",
               read_amt, file_end);
   }
   return true;
}

bool replay_set_serialized_data(void* buf)
{
   uint8_t *buffer                = buf;
   input_driver_state_t *input_st = input_state_get_ptr();
   bool playback                  = (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_PLAYBACK)  ? true : false;
   bool recording                 = (input_st->bsv_movie_state.flags & BSV_FLAG_MOVIE_RECORDING) ? true : false;

   /* If there is no current replay, ignore this entirely.
      TODO/FIXME: Later, consider loading up the replay
      and allow the user to continue it?
      Or would that be better done from the replay hotkeys?
    */
   if (!(playback || recording))
      return true;

   if (!buffer)
   {
      if (recording)
      {
         const char *_msg = msg_hash_to_str(MSG_REPLAY_LOAD_STATE_FAILED_INCOMPAT);
         runloop_msg_queue_push(_msg, strlen(_msg), 1, 180, true, NULL,
               MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
         RARCH_ERR("[Replay] %s.\n", _msg);
         return false;
      }

      if (playback)
      {
         const char *_msg = msg_hash_to_str(MSG_REPLAY_LOAD_STATE_HALT_INCOMPAT);
         runloop_msg_queue_push(_msg, sizeof(_msg), 1, 180, true, NULL,
               MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
         RARCH_WARN("[Replay] %s.\n", _msg);
         movie_stop(input_st);
      }
   }
   else
   {
      /* TODO: should factor the next few lines away, magic numbers ahoy */
      uint32_t *header         = (uint32_t *)(buffer + sizeof(int32_t));
      int64_t *ident_spot      = (int64_t *)(header + 4);
      int64_t ident            = swap_if_big64(*ident_spot);

      if (ident == input_st->bsv_movie_state_handle->identifier) /* is compatible? */
      {
         int32_t loaded_len    = swap_if_big32(((int32_t *)buffer)[0]);
         int64_t handle_idx    = intfstream_tell(input_st->bsv_movie_state_handle->file);
         /* If the state is part of this replay, go back to that state
            and fast forward/rewind the replay.

            If the savestate movie is after the current replay
            length we can replace the current replay data with it,
            but if it's earlier we can rewind the replay to the
            savestate movie time point.

            This can truncate the current replay if we're in recording mode.
          */
         if (loaded_len > handle_idx)
         {
            /* TODO: Really, to be very careful, we should be
               checking that the events in the loaded state are the
               same up to handle_idx. Right? */
            intfstream_rewind(input_st->bsv_movie_state_handle->file);
            intfstream_write(input_st->bsv_movie_state_handle->file, buffer+sizeof(int32_t), loaded_len);
            /* also need to update/reinit frame_pos, frame_counter--rewind won't work properly unless we do. */
            bsv_movie_scan_from_start(input_st, loaded_len);
         }
         else
         {
            intfstream_seek(input_st->bsv_movie_state_handle->file, loaded_len, SEEK_SET);
            /* also need to update/reinit frame_pos, frame_counter--rewind won't work properly unless we do. */
            bsv_movie_scan_from_start(input_st, loaded_len);
            if (recording)
               intfstream_truncate(input_st->bsv_movie_state_handle->file, loaded_len);
         }
      }
      else
      {
         /* otherwise, if recording do not allow the load */
         if (recording)
         {
            const char *_msg = msg_hash_to_str(MSG_REPLAY_LOAD_STATE_FAILED_INCOMPAT);
            runloop_msg_queue_push(_msg, strlen(_msg), 1, 180, true, NULL,
                  MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
            RARCH_ERR("[Replay] %s.\n", _msg);
            return false;
         }
         /* if in playback, halt playback and go to that state normally */
         if (playback)
         {
            const char *_msg = msg_hash_to_str(MSG_REPLAY_LOAD_STATE_HALT_INCOMPAT);
            runloop_msg_queue_push(_msg, strlen(_msg), 1, 180, true, NULL,
                  MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
            RARCH_WARN("[Replay] %s.\n", _msg);
            movie_stop(input_st);
         }
      }
   }
   return true;
}


void bsv_movie_poll(input_driver_state_t *input_st) {
   runloop_state_t *runloop_st   = runloop_state_get_ptr();
   retro_keyboard_event_t *key_event                 = &runloop_st->key_event;

   if (*key_event && *key_event == runloop_st->frontend_key_event)
   {
      int i;
      bsv_key_data_t k;
      for (i = 0; i < input_st->bsv_movie_state_handle->key_event_count; i++)
      {
#ifdef HAVE_CHEEVOS
         rcheevos_pause_hardcore();
#endif
         k = input_st->bsv_movie_state_handle->key_events[i];
         input_keyboard_event(k.down, swap_if_big32(k.code),
                              swap_if_big32(k.character), swap_if_big16(k.mod),
                              RETRO_DEVICE_KEYBOARD);
      }
      /* Have to clear here so we don't double-apply key events */
      /* Zero out key events when playing back or recording */
      input_st->bsv_movie_state_handle->key_event_count = 0;
   }
}

int16_t bsv_movie_read_state(input_driver_state_t *input_st,
                             unsigned port, unsigned device,
                             unsigned idx, unsigned id) {
   int16_t bsv_result = 0;
   bsv_movie_t *movie = input_st->bsv_movie_state_handle;
   if (bsv_movie_handle_read_input_event(movie, port, device, idx, id, &bsv_result))
   {
#ifdef HAVE_CHEEVOS
      rcheevos_pause_hardcore();
#endif
      return bsv_result;
   }
   input_st->bsv_movie_state.flags |= BSV_FLAG_MOVIE_END;
   return 0;
}

size_t bsv_movie_write_deduped_state(bsv_movie_t *movie, uint8_t *state, size_t state_size)
{
   size_t block_size = movie->blocks->object_size;
   size_t block_byte_size = movie->blocks->object_size*4;
   size_t superblock_size = movie->superblocks->object_size;
   size_t superblock_byte_size = superblock_size*block_byte_size;
   size_t superblock_count = state_size / superblock_byte_size + (state_size % superblock_byte_size != 0);
   uint32_t *superblock_seq = calloc(superblock_count, sizeof(uint32_t));
   uint32_t *superblock_buf = calloc(superblock_size, sizeof(uint32_t));
   uint8_t *padded_block = NULL;
   rmsgpack_write_uint(movie->file, BSV_IFRAME_START_TOKEN);
   rmsgpack_write_uint(movie->file, movie->frame_counter);
   rmsgpack_write_uint(movie->file, state_size);
   for(size_t superblock = 0; superblock < superblock_count; superblock++)
   {
      uint32s_insert_result_t found_block;
      for(size_t block = 0; block < superblock_size; block++)
      {
         size_t block_start = superblock*superblock_byte_size+block*block_byte_size;
         if(block_start > state_size)
         {
            /* pad superblocks with zero blocks */
            found_block.index = 0;
            found_block.is_new = false;
         }
         else if(block_start + block_byte_size > state_size)
         {
            if(!padded_block)
               padded_block = calloc(block_byte_size, sizeof(uint8_t));
            else
               memset(padded_block,0,block_byte_size);
            memcpy(padded_block, state+block_start, state_size - block_start);
            found_block = uint32s_index_insert(movie->blocks, (uint32_t*)padded_block);
         }
         else
         {
            found_block = uint32s_index_insert(movie->blocks, (uint32_t*)(state+block_start));
         }
         if(found_block.is_new)
         {
            /* write "here is a new block" and new block to file */
            rmsgpack_write_uint(movie->file, BSV_IFRAME_NEW_BLOCK_TOKEN);
            rmsgpack_write_uint(movie->file, found_block.index);
            rmsgpack_write_bin(movie->file, state+block_start, block_byte_size);
         }
         superblock_buf[block] = found_block.index;
        }
      found_block = uint32s_index_insert(movie->superblocks, superblock_buf);
      if(found_block.is_new)
      {
         /* write "here is a new superblock" and new superblock to file */
         rmsgpack_write_uint(movie->file, BSV_IFRAME_NEW_SUPERBLOCK_TOKEN);
         rmsgpack_write_uint(movie->file, found_block.index);
         rmsgpack_write_array_header(movie->file, superblock_size);
         for(uint32_t i = 0; i < superblock_size; i++)
         {
            rmsgpack_write_uint(movie->file, superblock_buf[i]);
         }
      }
      superblock_seq[superblock] = found_block.index;
   }
   /* write "here is the superblock seq" and superblock seq to file */
   rmsgpack_write_uint(movie->file, BSV_IFRAME_SUPERBLOCK_SEQ_TOKEN);
   rmsgpack_write_array_header(movie->file, superblock_count);
   for(uint32_t i = 0; i < superblock_count; i++)
   {
       rmsgpack_write_uint(movie->file, superblock_seq[i]);
   }
   free(superblock_buf); 
   free(superblock_seq);
   if(padded_block)
      free(padded_block);

   return 0;
}

bool bsv_movie_read_deduped_state(bsv_movie_t *movie)
{
   retro_ctx_serialize_info_t serial_info;
   bool ret = false;
   uint32_t frame_counter = 0;
   size_t state_size = 0;
   struct rmsgpack_dom_value item;
   uint8_t *state_data = NULL;
   size_t block_byte_size = movie->blocks->object_size*4;
   size_t superblock_byte_size = movie->superblocks->object_size*block_byte_size;
   rmsgpack_dom_read(movie->file, &item);
   if(item.type != RDT_UINT)
   {
      RARCH_ERR("[STATESTREAM] start token type is wrong\n");
      goto exit;
   }
   if(item.val.uint_ != BSV_IFRAME_START_TOKEN)
   {
      RARCH_ERR("[STATESTREAM] start token value is wrong\n");
      goto exit;
   }
   rmsgpack_dom_read(movie->file, &item);
   if(item.type != RDT_UINT)
   {
      RARCH_ERR("[STATESTREAM] frame counter type is wrong\n");
      goto exit;
   }
   frame_counter = item.val.uint_;
   RARCH_LOG("[STATESTREAM] load incremental checkpoint at frame %d\n", frame_counter);
   rmsgpack_dom_read(movie->file, &item);
   if(item.type != RDT_UINT)
   {
      RARCH_ERR("[STATESTREAM] state size type is wrong\n");
      goto exit;
   }
   state_size = item.val.uint_;
   state_data = calloc(state_size, sizeof(uint8_t));
   while(rmsgpack_dom_read(movie->file, &item) > 0)
   {
      uint32_t index, *superblock;
      size_t len;
      if(item.type != RDT_UINT)
      {
         RARCH_ERR("[STATESTREAM] state update chunk token type is wrong\n");
         goto exit;
      }
      switch(item.val.uint_) {
      case BSV_IFRAME_NEW_BLOCK_TOKEN:
         rmsgpack_dom_read(movie->file, &item);
         if(item.type != RDT_UINT)
         {
            RARCH_ERR("[STATESTREAM] new block index type is wrong\n");
            goto exit;
         }
         index = item.val.uint_;
         rmsgpack_dom_read(movie->file, &item);
         if(item.type != RDT_BINARY)
         {
            RARCH_ERR("[STATESTREAM] new block value type is wrong\n");
            rmsgpack_dom_value_free(&item);
            goto exit;
         }
         if(item.val.binary.len != block_byte_size)
         {
            RARCH_ERR("[STATESTREAM] new block binary length is wrong\n");
            rmsgpack_dom_value_free(&item);
            goto exit;
         }
         if(!uint32s_index_insert_exact(movie->blocks, index, (uint32_t *)item.val.binary.buff))
         {
            RARCH_ERR("[STATESTREAM] couldn't insert new block at right index\n");
            rmsgpack_dom_value_free(&item);
            goto exit;
         }
         /* do not free binary rmsgpack item since insert_exact takes its allocation */
         break;
      case BSV_IFRAME_NEW_SUPERBLOCK_TOKEN:
         rmsgpack_dom_read(movie->file, &item);
         if(item.type != RDT_UINT)
         {
            RARCH_ERR("[STATESTREAM] new superblock index type is wrong\n");
            goto exit;
         }
         index = item.val.uint_;
         rmsgpack_dom_read(movie->file, &item);
         if(item.type != RDT_ARRAY)
         {
            RARCH_ERR("[STATESTREAM] new superblock contents type is wrong\n");
            goto exit;
         }
         if(item.val.array.len != movie->superblocks->object_size)
         {
            RARCH_ERR("[STATESTREAM] new superblock contents length is wrong\n");
            goto exit;
         }
         len = movie->superblocks->object_size;
         superblock = calloc(len, sizeof(uint32_t));
         for(size_t i = 0; i < len; i++)
         {
            struct rmsgpack_dom_value inner_item = item.val.array.items[i];
            /* assert(inner_item.type == RDT_UINT); */
            superblock[i] = inner_item.val.uint_;
         }
         if(!uint32s_index_insert_exact(movie->superblocks, index, superblock))
         {
            RARCH_ERR("[STATESTREAM] new superblock couldn't be inserted at right index\n");
            rmsgpack_dom_value_free(&item);
            free(superblock);
            goto exit;
         }
         rmsgpack_dom_value_free(&item);
         break;
      case BSV_IFRAME_SUPERBLOCK_SEQ_TOKEN:
         rmsgpack_dom_read(movie->file, &item);
         if(item.type != RDT_ARRAY)
         {
            RARCH_ERR("[STATESTREAM] superblock seq type is wrong\n");
            goto exit;
         }
         len = item.val.array.len;
         for(size_t i = 0; i < len; i++)
         {
            struct rmsgpack_dom_value inner_item = item.val.array.items[i];
            /* assert(inner_item.type == RDT_UINT); */
            uint32_t superblock_idx = inner_item.val.uint_;
            uint32_t *superblock = uint32s_index_get(movie->superblocks, superblock_idx);
            for(size_t j = 0; j < movie->superblocks->object_size; j++)
            {
               uint32_t block_idx = superblock[j];
               size_t block_start = MIN(superblock_idx*superblock_byte_size+block_idx*block_byte_size, state_size);
               size_t block_end = MIN(block_start+block_byte_size, state_size);
               uint32_t *block;
               /* This (==) can only happen in the last superblock, if it was padded with extra blocks. */
               if(block_end <= block_start) { break; }
               block = uint32s_index_get(movie->blocks, block_idx);
               memcpy(state_data+block_start, (uint8_t*)block, block_end-block_start);
            }
         }
         rmsgpack_dom_value_free(&item);
         serial_info.data_const = (void*)state_data;
         serial_info.size       = state_size;
         ret = core_unserialize(&serial_info);
         goto exit;
      default:
         RARCH_ERR("[STATESTREAM] state update chunk token value is invalid\n");
         goto exit;
     }
   }
exit:
   if(state_data)
      free(state_data);
   return ret;
}
