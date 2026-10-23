#include "mc_profile.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

typedef struct { const char *cur, *end; } json_reader_t;
typedef struct { const char *start, *end; char type; } json_span_t;

static void skip_ws(json_reader_t *r) {
    while (r->cur < r->end && (*r->cur == ' ' || *r->cur == '\t' || *r->cur == '\r' || *r->cur == '\n')) r->cur++;
}

static int parse_string(json_reader_t *r) {
    if (r->cur >= r->end || *r->cur++ != '"') return -1;
    while (r->cur < r->end) {
        unsigned char ch = (unsigned char)*r->cur++;
        if (ch == '"') return 0;
        if (ch < 0x20) return -1;
        if (ch != '\\') continue;
        if (r->cur >= r->end) return -1;
        ch = (unsigned char)*r->cur++;
        if (strchr("\"\\/bfnrt", ch)) continue;
        if (ch != 'u' || r->end - r->cur < 4) return -1;
        for (int i = 0; i < 4; i++) {
            if (!isxdigit((unsigned char)r->cur[i])) return -1;
        }
        r->cur += 4;
    }
    return -1;
}

static int parse_value(json_reader_t *r, json_span_t *span, int depth) {
    if (depth > 16) return -1;
    skip_ws(r);
    if (r->cur >= r->end) return -1;
    const char *start = r->cur;
    char type = *r->cur;
    if (type == '"') {
        if (parse_string(r) != 0) return -1;
    } else if (type == '{' || type == '[') {
        char close = type == '{' ? '}' : ']';
        r->cur++;
        skip_ws(r);
        if (r->cur < r->end && *r->cur == close) r->cur++;
        else {
            for (;;) {
                if (type == '{') {
                    if (parse_string(r) != 0) return -1;
                    skip_ws(r);
                    if (r->cur >= r->end || *r->cur++ != ':') return -1;
                }
                if (parse_value(r, NULL, depth + 1) != 0) return -1;
                skip_ws(r);
                if (r->cur >= r->end) return -1;
                if (*r->cur == close) { r->cur++; break; }
                if (*r->cur++ != ',') return -1;
                skip_ws(r);
            }
        }
    } else if (type == 't' || type == 'f' || type == 'n') {
        const char *literal = type == 't' ? "true" : type == 'f' ? "false" : "null";
        size_t len = strlen(literal);
        if ((size_t)(r->end - r->cur) < len || memcmp(r->cur, literal, len) != 0) return -1;
        r->cur += len;
    } else if (type == '-' || (type >= '0' && type <= '9')) {
        /* Unknown numeric fields are only skipped, never interpreted. */
        do { r->cur++; } while (r->cur < r->end &&
            (isdigit((unsigned char)*r->cur) || *r->cur == '-' || *r->cur == '+' ||
             *r->cur == '.' || *r->cur == 'e' || *r->cur == 'E'));
    } else return -1;
    if (span) *span = (json_span_t){start, r->cur, type};
    return 0;
}

static int object_field(json_span_t object, const char *key, json_span_t *out) {
    if (object.type != '{') return -1;
    json_reader_t r = {object.start + 1, object.end - 1};
    while (r.cur < r.end) {
        skip_ws(&r);
        const char *key_start = r.cur;
        if (parse_string(&r) != 0) return -1;
        const char *key_end = r.cur;
        skip_ws(&r);
        if (r.cur >= r.end || *r.cur++ != ':') return -1;
        json_span_t value;
        if (parse_value(&r, &value, 0) != 0) return -1;
        if ((size_t)(key_end - key_start) == strlen(key) + 2 &&
            memcmp(key_start + 1, key, strlen(key)) == 0) {
            *out = value;
            return 0;
        }
        skip_ws(&r);
        if (r.cur < r.end && *r.cur++ != ',') return -1;
    }
    return -1;
}

static int root_object(const char *json, json_span_t *out) {
    if (!json || !out || strlen(json) > 32768) return -1;
    json_reader_t r = {json, json + strlen(json)};
    if (parse_value(&r, out, 0) != 0 || out->type != '{') return -1;
    skip_ws(&r);
    return r.cur == r.end ? 0 : -1;
}

static int copy_string(json_span_t value, char *out, size_t cap) {
    if (value.type != '"' || value.end - value.start < 2) return -1;
    size_t len = (size_t)(value.end - value.start - 2);
    if (len == 0 || len >= cap) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value.start[1 + i];
        if (ch < 0x20 || ch == '\\' || ch == '"') return -1;
    }
    memcpy(out, value.start + 1, len);
    out[len] = '\0';
    return 0;
}

static int parse_uuid_hex(const char *hex, uint8_t out[16]) {
    if (strlen(hex) != 32) return -1;
    for (int i = 0; i < 16; i++) {
        int hi = isxdigit((unsigned char)hex[2 * i]) ?
            (isdigit((unsigned char)hex[2 * i]) ? hex[2 * i] - '0' : tolower((unsigned char)hex[2 * i]) - 'a' + 10) : -1;
        int lo = isxdigit((unsigned char)hex[2 * i + 1]) ?
            (isdigit((unsigned char)hex[2 * i + 1]) ? hex[2 * i + 1] - '0' : tolower((unsigned char)hex[2 * i + 1]) - 'a' + 10) : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int valid_base64(const char *s) {
    size_t len = strlen(s);
    if (!len || (len & 3)) return -1;
    size_t padding = 0;
    if (s[len - 1] == '=') padding++;
    if (len > 1 && s[len - 2] == '=') padding++;
    for (size_t i = 0; i < len - padding; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && ch != '+' && ch != '/') return -1;
    }
    for (size_t i = len - padding; i < len; i++) if (s[i] != '=') return -1;
    return 0;
}

static int parse_identity(json_span_t root, uint8_t uuid[16], char name[17]) {
    json_span_t id_span, name_span;
    char id[33];
    if (object_field(root, "id", &id_span) != 0 || object_field(root, "name", &name_span) != 0 ||
        copy_string(id_span, id, sizeof(id)) != 0 || parse_uuid_hex(id, uuid) != 0 ||
        copy_string(name_span, name, 17) != 0) return -1;
    size_t len = strlen(name);
    if (len < 3 || len > 16) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isalnum(ch) && ch != '_') return -1;
    }
    return 0;
}

int mc_profile_parse_name(const char *json, uint8_t uuid[16], char name[17]) {
    json_span_t root;
    return root_object(json, &root) == 0 ? parse_identity(root, uuid, name) : -1;
}

static int parse_properties(json_span_t root, mc_public_profile_t *result, bool required) {
    json_span_t properties;
    if (object_field(root, "properties", &properties) != 0) return required ? -1 : 0;
    if (properties.type != '[') return -1;
    json_reader_t r = {properties.start + 1, properties.end - 1};
    int count = 0;
    while (r.cur < r.end) {
        json_span_t property, name_span, value_span, signature_span;
        char property_name[32];
        if (++count > 16 || parse_value(&r, &property, 0) != 0 || property.type != '{' ||
            object_field(property, "name", &name_span) != 0 ||
            copy_string(name_span, property_name, sizeof(property_name)) != 0) return -1;
        if (strcmp(property_name, "textures") == 0) {
            if (result->found) return -1;
            if (object_field(property, "value", &value_span) != 0 ||
                object_field(property, "signature", &signature_span) != 0 ||
                copy_string(value_span, result->texture_value, sizeof(result->texture_value)) != 0 ||
                copy_string(signature_span, result->texture_signature, sizeof(result->texture_signature)) != 0 ||
                valid_base64(result->texture_value) != 0 || valid_base64(result->texture_signature) != 0) return -1;
            result->found = true;
        }
        skip_ws(&r);
        if (r.cur < r.end && *r.cur++ != ',') return -1;
    }
    return required && !result->found ? -1 : 0;
}

int mc_profile_parse_signed(const char *json, const uint8_t expected_uuid[16], mc_public_profile_t *out) {
    if (!expected_uuid || !out) return -1;
    json_span_t root;
    mc_public_profile_t result = {0};
    if (root_object(json, &root) != 0 || parse_identity(root, result.public_uuid, result.name) != 0 ||
        memcmp(result.public_uuid, expected_uuid, 16) != 0 || parse_properties(root, &result, true) != 0) return -1;
    *out = result;
    return 0;
}

int mc_profile_parse_auth(const char *json, const char *expected_name, mc_public_profile_t *out) {
    if (!expected_name || !out) return -1;
    json_span_t root;
    mc_public_profile_t result = {0};
    if (root_object(json, &root) != 0 || parse_identity(root, result.public_uuid, result.name) != 0 ||
        strcasecmp(expected_name, result.name) != 0 || parse_properties(root, &result, false) != 0) return -1;
    *out = result;
    return 0;
}
