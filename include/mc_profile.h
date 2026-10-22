#ifndef MC_PROFILE_H
#define MC_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#define MC_PROFILE_TEXTURE_MAX 8192

typedef struct {
    bool found;
    uint8_t public_uuid[16];
    char name[17];
    char texture_value[MC_PROFILE_TEXTURE_MAX];
    char texture_signature[MC_PROFILE_TEXTURE_MAX];
} mc_public_profile_t;

typedef struct mc_profile_service mc_profile_service_t;
typedef int (*mc_profile_fetch_fn)(const char *name, mc_public_profile_t *out, void *userdata);

/* 1: cached result (possibly negative), 0: lookup pending, -1: unavailable.
 * Never performs network IO on the caller's thread. */
int mc_profile_service_lookup(mc_profile_service_t *service, const char *name, mc_public_profile_t *out);
mc_profile_service_t *mc_profile_service_create(void);
mc_profile_service_t *mc_profile_service_create_with_fetch(mc_profile_fetch_fn fetch, void *userdata);
void mc_profile_service_destroy(mc_profile_service_t *service);

/* Strict parsers for the two public Mojang responses; useful with mock JSON. */
int mc_profile_parse_name(const char *json, uint8_t uuid[16], char name[17]);
int mc_profile_parse_signed(const char *json, const uint8_t expected_uuid[16], mc_public_profile_t *out);

#endif
