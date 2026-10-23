#ifndef MC_AUTH_H
#define MC_AUTH_H

#include <stddef.h>
#include <stdint.h>
#include "mc_profile.h"

typedef struct mc_auth_service mc_auth_service_t;
typedef int (*mc_auth_fetch_fn)(const char *name, const char *server_hash,
                                 mc_public_profile_t *out, void *userdata);

mc_auth_service_t *mc_auth_service_create(void);
mc_auth_service_t *mc_auth_service_create_with_fetch(mc_auth_fetch_fn fetch, void *userdata);
void mc_auth_service_destroy(mc_auth_service_t *service);
const uint8_t *mc_auth_public_key(mc_auth_service_t *service, size_t *len);
int mc_auth_decrypt(mc_auth_service_t *service, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_cap, size_t *output_len);
int mc_auth_random(uint8_t *output, size_t len);
int mc_auth_server_hash(const uint8_t secret[16], const uint8_t *public_key,
                        size_t public_key_len, char out[42]);
/* A zero ticket means saturation. Poll returns 0 pending, 1 authenticated,
 * -1 denied/invalid/unavailable, -2 unknown or expired ticket. */
uint64_t mc_auth_submit(mc_auth_service_t *service, const char *name, const char *server_hash);
int mc_auth_poll(mc_auth_service_t *service, uint64_t ticket, mc_public_profile_t *out);

#endif
