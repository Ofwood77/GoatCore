#include "mc_auth.h"

mc_auth_service_t *mc_auth_service_create(void) { return NULL; }
mc_auth_service_t *mc_auth_service_create_with_fetch(mc_auth_fetch_fn fetch, void *userdata) {
    (void)fetch; (void)userdata; return NULL;
}
void mc_auth_service_destroy(mc_auth_service_t *service) { (void)service; }
const uint8_t *mc_auth_public_key(mc_auth_service_t *service, size_t *len) {
    (void)service; if (len) *len = 0; return NULL;
}
int mc_auth_decrypt(mc_auth_service_t *service, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_cap, size_t *output_len) {
    (void)service; (void)input; (void)input_len; (void)output; (void)output_cap; (void)output_len;
    return -1;
}
int mc_auth_random(uint8_t *output, size_t len) { (void)output; (void)len; return -1; }
int mc_auth_server_hash(const uint8_t secret[16], const uint8_t *public_key,
                        size_t public_key_len, char out[42]) {
    (void)secret; (void)public_key; (void)public_key_len; (void)out; return -1;
}
uint64_t mc_auth_submit(mc_auth_service_t *service, const char *name, const char *server_hash) {
    (void)service; (void)name; (void)server_hash; return 0;
}
int mc_auth_poll(mc_auth_service_t *service, uint64_t ticket, mc_public_profile_t *out) {
    (void)service; (void)ticket; (void)out; return -2;
}
