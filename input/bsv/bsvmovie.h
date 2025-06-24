#ifndef __BSV_MOVIE__H
#define __BSV_MOVIE__H

#include <sys/types.h>
#include "../input_driver.h"

void bsv_movie_poll(input_driver_state_t *input_st);
int16_t bsv_movie_read_state(input_driver_state_t *input_st,
                             unsigned port, unsigned device,
                             unsigned idx, unsigned id);
void bsv_movie_push_key_event(bsv_movie_t *movie,
                              uint8_t down, uint16_t mod, uint32_t code, uint32_t character);
void bsv_movie_push_input_event(bsv_movie_t *movie,
                                uint8_t port, uint8_t dev, uint8_t idx, uint16_t id, int16_t val);

#endif /* __BSV_MOVIE__H */
