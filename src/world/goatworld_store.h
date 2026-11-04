#ifndef MC_GOATWORLD_STORE_H
#define MC_GOATWORLD_STORE_H
#include "mc_chunk.h"
typedef struct mc_goatworld mc_goatworld_t;
int mc_goatworld_open(const char *world, bool bound, mc_goatworld_t **out);
int mc_goatworld_read(mc_goatworld_t *s,int32_t x,int32_t z,mc_chunk_t *out);
int mc_goatworld_write(mc_goatworld_t *s,mc_chunk_t *const *chunks,size_t count);
int mc_goatworld_close(mc_goatworld_t *s);
#endif
