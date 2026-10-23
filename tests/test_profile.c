#include "mc_profile.h"
#include "mc_protocol.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_int fetch_calls;

static int fake_fetch(const char *name, mc_public_profile_t *out, void *userdata) {
    (void)userdata;
    atomic_fetch_add(&fetch_calls, 1);
    if (strcmp(name, "MissingUser") == 0) return -1;
    memset(out, 0, sizeof(*out));
    out->found = true;
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->texture_value, sizeof(out->texture_value), "QUJDRA==");
    snprintf(out->texture_signature, sizeof(out->texture_signature), "RUZHSA==");
    return 0;
}

static mc_public_profile_t wait_result(mc_profile_service_t *service, const char *name) {
    mc_public_profile_t result = {0};
    for (int i = 0; i < 300; i++) {
        int rc = mc_profile_service_lookup(service, name, &result);
        if (rc == 1) return result;
        assert(rc == 0);
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    assert(!"profile lookup did not complete");
    return result;
}

int main(void) {
    const char *name_json = "{\"id\":\"069a79f444e94726a5befca90e38aaf5\",\"name\":\"Notch\"}";
    const char *signed_json = "{\"id\":\"069a79f444e94726a5befca90e38aaf5\",\"name\":\"Notch\","
        "\"extra\":{\"escaped\":\"a\\\\b\"},\"properties\":[{\"name\":\"textures\","
        "\"value\":\"QUJDRA==\",\"signature\":\"RUZHSA==\"}]}";
    uint8_t uuid[16];
    char name[17];
    mc_public_profile_t profile = {0};
    assert(mc_profile_parse_name(name_json, uuid, name) == 0);
    assert(strcmp(name, "Notch") == 0);
    assert(mc_profile_parse_signed(signed_json, uuid, &profile) == 0);
    assert(profile.found && strcmp(profile.texture_value, "QUJDRA==") == 0);
    assert(mc_profile_parse_auth(signed_json, "notch", &profile) == 0);
    assert(mc_profile_parse_auth(name_json, "Notch", &profile) == 0 && !profile.found);
    assert(mc_profile_parse_auth(name_json, "SomebodyElse", &profile) == -1);
    assert(mc_profile_parse_name("{\"id\":\"bogus\",\"name\":\"Notch\"}", uuid, name) == -1);
    assert(mc_profile_parse_signed("{\"id\":\"069a79f444e94726a5befca90e38aaf5\","
        "\"name\":\"Notch\",\"properties\":[{\"name\":\"textures\",\"value\":\"AAAA\"}]}", uuid, &profile) == -1);
    assert(mc_profile_parse_signed("{broken", uuid, &profile) == -1);

    mc_conn_t conn = {0};
    conn.skin_value = "QUJDRA==";
    conn.skin_signature = "RUZHSA==";
    assert(proto_profile_properties_capacity(&conn) >= 29);
    uint8_t encoded[64] = {0};
    size_t encoded_len = 0;
    assert(proto_profile_write_properties(&conn, encoded, sizeof(encoded), &encoded_len) == 0);
    assert(encoded_len == 29);
    assert(encoded[0] == 1 && encoded[1] == 8);
    assert(memcmp(encoded + 2, "textures", 8) == 0);
    assert(encoded[10] == 8 && memcmp(encoded + 11, "QUJDRA==", 8) == 0);
    assert(encoded[19] == 1 && encoded[20] == 8);
    assert(memcmp(encoded + 21, "RUZHSA==", 8) == 0);

    atomic_init(&fetch_calls, 0);
    mc_profile_service_t *service = mc_profile_service_create_with_fetch(fake_fetch, NULL);
    assert(service);
    profile = wait_result(service, "TestUser");
    assert(profile.found && strcmp(profile.name, "TestUser") == 0);
    assert(mc_profile_service_lookup(service, "testuser", &profile) == 1);
    assert(atomic_load(&fetch_calls) == 1); /* case-insensitive cache hit */
    profile = wait_result(service, "MissingUser");
    assert(!profile.found);
    assert(mc_profile_service_lookup(service, "MissingUser", &profile) == 1);
    assert(atomic_load(&fetch_calls) == 2); /* negative cache hit */
    mc_profile_service_destroy(service);
    puts("profile: signed JSON parser, invalid responses and async cache ok");
    return 0;
}
