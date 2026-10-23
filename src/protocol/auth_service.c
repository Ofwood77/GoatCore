#include "mc_auth.h"
#include "mc_util.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_WORKERS 4
#define AUTH_JOBS 32
#define AUTH_RESPONSE_MAX 32768

typedef enum { AUTH_EMPTY, AUTH_QUEUED, AUTH_ACTIVE, AUTH_READY } auth_job_state_t;
typedef struct {
    auth_job_state_t state;
    uint64_t ticket;
    int64_t completed_ms;
    char name[17];
    char hash[42];
    int result;
    mc_public_profile_t profile;
} auth_job_t;

struct mc_auth_service {
    EVP_PKEY *key;
    uint8_t *public_key;
    size_t public_key_len;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    bool stop;
    pthread_t workers[AUTH_WORKERS];
    int worker_count;
    uint64_t next_ticket;
    auth_job_t *jobs;
    mc_auth_fetch_fn fetch;
    void *fetch_userdata;
};

static int64_t auth_now_ms(void) { return mc_now_us() / 1000; }

typedef struct { char *data; size_t len; } auth_http_buffer_t;

static size_t auth_http_write(char *ptr, size_t size, size_t count, void *userdata) {
    auth_http_buffer_t *buf = userdata;
    if (!size || count > SIZE_MAX / size) return 0;
    size_t len = size * count;
    if (len > AUTH_RESPONSE_MAX - buf->len) return 0;
    memcpy(buf->data + buf->len, ptr, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return len;
}

static int auth_fetch_mojang(const char *name, const char *hash,
                             mc_public_profile_t *out, void *userdata) {
    (void)userdata;
    char url[256];
    int n = snprintf(url, sizeof(url),
        "https://sessionserver.mojang.com/session/minecraft/hasJoined?username=%s&serverId=%s",
        name, hash);
    if (n <= 0 || (size_t)n >= sizeof(url)) return -1;
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    char response[AUTH_RESPONSE_MAX + 1];
    auth_http_buffer_t buffer = {response, 0};
    response[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    /* Some hosts spend ~5 s falling back from a dead primary DNS resolver.
     * This call runs only in an auth worker; keep a bounded, but usable, window. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoatCore/26.1.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, auth_http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        log_error("Mojang hasJoined request failed for %s: %s", name, curl_easy_strerror(rc));
        return -1;
    }
    if (status != 200) {
        log_info("Mojang hasJoined denied %s: HTTP %ld", name, status);
        return -1;
    }
    int parse_result = mc_profile_parse_auth(response, name, out);
    if (parse_result != 0) log_error("Mojang hasJoined returned an invalid profile for %s", name);
    return parse_result;
}

static void *auth_worker(void *argument) {
    mc_auth_service_t *service = argument;
    for (;;) {
        pthread_mutex_lock(&service->lock);
        int slot = -1;
        while (!service->stop && slot < 0) {
            for (int i = 0; i < AUTH_JOBS; i++) {
                if (service->jobs[i].state == AUTH_QUEUED) { slot = i; break; }
            }
            if (slot < 0) pthread_cond_wait(&service->cv, &service->lock);
        }
        if (service->stop) { pthread_mutex_unlock(&service->lock); break; }
        auth_job_t *job = &service->jobs[slot];
        job->state = AUTH_ACTIVE;
        char name[17], hash[42];
        memcpy(name, job->name, sizeof(name));
        memcpy(hash, job->hash, sizeof(hash));
        pthread_mutex_unlock(&service->lock);

        mc_public_profile_t *profile = calloc(1, sizeof(*profile));
        int result = profile ? service->fetch(name, hash, profile, service->fetch_userdata) : -1;

        pthread_mutex_lock(&service->lock);
        if (result == 0) job->profile = *profile;
        job->result = result == 0 ? 1 : -1;
        job->completed_ms = auth_now_ms();
        job->state = AUTH_READY;
        pthread_mutex_unlock(&service->lock);
        free(profile);
    }
    return NULL;
}

static int auth_generate_key(mc_auth_service_t *service) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) return -1;
    int rc = -1;
    if (EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) != 1 ||
        EVP_PKEY_keygen(ctx, &service->key) != 1) goto done;
    int der_len = i2d_PUBKEY(service->key, NULL);
    if (der_len <= 0 || der_len > 4096) goto done;
    service->public_key = malloc((size_t)der_len);
    if (!service->public_key) goto done;
    unsigned char *cursor = service->public_key;
    if (i2d_PUBKEY(service->key, &cursor) != der_len) goto done;
    service->public_key_len = (size_t)der_len;
    rc = 0;
done:
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

mc_auth_service_t *mc_auth_service_create_with_fetch(mc_auth_fetch_fn fetch, void *userdata) {
    if (!fetch || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return NULL;
    mc_auth_service_t *service = calloc(1, sizeof(*service));
    if (!service) { curl_global_cleanup(); return NULL; }
    service->fetch = fetch;
    service->fetch_userdata = userdata;
    service->jobs = calloc(AUTH_JOBS, sizeof(*service->jobs));
    if (!service->jobs || auth_generate_key(service) != 0) goto fail_alloc;
    if (pthread_mutex_init(&service->lock, NULL) != 0) goto fail_alloc;
    if (pthread_cond_init(&service->cv, NULL) != 0) {
        pthread_mutex_destroy(&service->lock);
        goto fail_alloc;
    }
    for (int i = 0; i < AUTH_WORKERS; i++) {
        if (pthread_create(&service->workers[i], NULL, auth_worker, service) != 0) {
            mc_auth_service_destroy(service);
            return NULL;
        }
        service->worker_count++;
    }
    return service;
fail_alloc:
    free(service->jobs);
    free(service->public_key);
    EVP_PKEY_free(service->key);
    free(service);
    curl_global_cleanup();
    return NULL;
}

mc_auth_service_t *mc_auth_service_create(void) {
    return mc_auth_service_create_with_fetch(auth_fetch_mojang, NULL);
}

void mc_auth_service_destroy(mc_auth_service_t *service) {
    if (!service) return;
    pthread_mutex_lock(&service->lock);
    service->stop = true;
    pthread_cond_broadcast(&service->cv);
    pthread_mutex_unlock(&service->lock);
    for (int i = 0; i < service->worker_count; i++) pthread_join(service->workers[i], NULL);
    pthread_cond_destroy(&service->cv);
    pthread_mutex_destroy(&service->lock);
    free(service->jobs);
    free(service->public_key);
    EVP_PKEY_free(service->key);
    free(service);
    curl_global_cleanup();
}

const uint8_t *mc_auth_public_key(mc_auth_service_t *service, size_t *len) {
    if (!service || !len) return NULL;
    *len = service->public_key_len;
    return service->public_key;
}

int mc_auth_decrypt(mc_auth_service_t *service, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_cap, size_t *output_len) {
    if (!service || !input || !input_len || !output || !output_len) return -1;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(service->key, NULL);
    if (!ctx) return -1;
    int rc = -1;
    size_t len = 0;
    if (EVP_PKEY_decrypt_init(ctx) != 1 || EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) != 1 ||
        EVP_PKEY_decrypt(ctx, NULL, &len, input, input_len) != 1 || len > output_cap ||
        EVP_PKEY_decrypt(ctx, output, &len, input, input_len) != 1) goto done;
    *output_len = len;
    rc = 0;
done:
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

int mc_auth_random(uint8_t *output, size_t len) {
    return output && len <= INT32_MAX && RAND_bytes(output, (int)len) == 1 ? 0 : -1;
}

int mc_auth_server_hash(const uint8_t secret[16], const uint8_t *public_key,
                        size_t public_key_len, char out[42]) {
    if (!secret || !public_key || !public_key_len || !out) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    uint8_t digest[20];
    unsigned digest_len = 0;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, secret, 16) == 1 &&
             EVP_DigestUpdate(ctx, public_key, public_key_len) == 1 &&
             EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 && digest_len == 20;
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    bool negative = (digest[0] & 0x80) != 0;
    if (negative) {
        unsigned carry = 1;
        for (int i = 19; i >= 0; i--) {
            unsigned value = (unsigned)(~digest[i] & 0xff) + carry;
            digest[i] = (uint8_t)value;
            carry = value >> 8;
        }
    }
    static const char hex[] = "0123456789abcdef";
    char digits[41];
    for (int i = 0; i < 20; i++) {
        digits[2 * i] = hex[digest[i] >> 4];
        digits[2 * i + 1] = hex[digest[i] & 15];
    }
    digits[40] = '\0';
    size_t first = 0;
    while (first < 39 && digits[first] == '0') first++;
    size_t pos = 0;
    if (negative) out[pos++] = '-';
    memcpy(out + pos, digits + first, 41 - first);
    return 0;
}

uint64_t mc_auth_submit(mc_auth_service_t *service, const char *name, const char *hash) {
    if (!service || !name || !hash || strlen(name) < 3 || strlen(name) > 16 ||
        strlen(hash) < 1 || strlen(hash) > 41) return 0;
    for (const char *p = name; *p; p++) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    }
    for (const char *p = hash; *p; p++) {
        if (p == hash && *p == '-') continue;
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    }
    pthread_mutex_lock(&service->lock);
    int slot = -1;
    int64_t now = auth_now_ms();
    for (int i = 0; i < AUTH_JOBS; i++) {
        if (service->jobs[i].state == AUTH_EMPTY ||
            (service->jobs[i].state == AUTH_READY && now - service->jobs[i].completed_ms > 30000)) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || service->stop) { pthread_mutex_unlock(&service->lock); return 0; }
    auth_job_t *job = &service->jobs[slot];
    memset(job, 0, sizeof(*job));
    job->ticket = ++service->next_ticket;
    if (!job->ticket) job->ticket = ++service->next_ticket;
    snprintf(job->name, sizeof(job->name), "%s", name);
    snprintf(job->hash, sizeof(job->hash), "%s", hash);
    job->state = AUTH_QUEUED;
    pthread_cond_signal(&service->cv);
    uint64_t ticket = job->ticket;
    pthread_mutex_unlock(&service->lock);
    return ticket;
}

int mc_auth_poll(mc_auth_service_t *service, uint64_t ticket, mc_public_profile_t *out) {
    if (!service || !ticket || !out) return -2;
    pthread_mutex_lock(&service->lock);
    for (int i = 0; i < AUTH_JOBS; i++) {
        auth_job_t *job = &service->jobs[i];
        if (job->ticket != ticket || job->state == AUTH_EMPTY) continue;
        if (job->state != AUTH_READY) { pthread_mutex_unlock(&service->lock); return 0; }
        int result = job->result;
        if (result == 1) *out = job->profile;
        job->state = AUTH_EMPTY;
        job->ticket = 0;
        pthread_mutex_unlock(&service->lock);
        return result;
    }
    pthread_mutex_unlock(&service->lock);
    return -2;
}
