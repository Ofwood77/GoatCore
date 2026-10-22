#include "mc_profile.h"
#include "mc_util.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define PROFILE_QUEUE_MAX 8
#define PROFILE_CACHE_MAX 128
#define PROFILE_HTTP_MAX 32768
#define PROFILE_POSITIVE_TTL_MS (60LL * 60 * 1000)
#define PROFILE_NEGATIVE_TTL_MS (60LL * 1000)

typedef struct {
    char name[17];
    int64_t expires_ms;
    mc_public_profile_t profile;
} profile_cache_entry_t;

struct mc_profile_service {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    bool stop;
    char queue[PROFILE_QUEUE_MAX][17];
    size_t head, len;
    char active_name[17];
    profile_cache_entry_t *cache;
    uint64_t cache_hits, queued, rejected, fetched, found;
    mc_profile_fetch_fn fetch;
    void *fetch_userdata;
};

typedef struct {
    char *data;
    size_t len;
} http_buffer_t;

static int64_t monotonic_ms(void) { return mc_now_us() / 1000; }

static size_t http_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_buffer_t *buf = userdata;
    if (!buf || size == 0 || nmemb > SIZE_MAX / size) return 0;
    size_t len = size * nmemb;
    if (len > PROFILE_HTTP_MAX - buf->len) return 0;
    memcpy(buf->data + buf->len, ptr, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return len;
}

static int http_get(const char *url, char *response, long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    http_buffer_t buf = {response, 0};
    response[0] = '\0';
    *status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoatCore/26.1.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -1;
}

static int fetch_public_profile(const char *name, mc_public_profile_t *out) {
    char url[256];
    char response[PROFILE_HTTP_MAX + 1];
    char official_name[17];
    uint8_t official_uuid[16];
    long status = 0;
    int n = snprintf(url, sizeof(url), "https://api.mojang.com/users/profiles/minecraft/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(url) || http_get(url, response, &status) != 0 || status != 200 ||
        mc_profile_parse_name(response, official_uuid, official_name) != 0 || strcasecmp(name, official_name) != 0) return -1;
    static const char hex[] = "0123456789abcdef";
    char id[33];
    for (int i = 0; i < 16; i++) {
        id[i * 2] = hex[official_uuid[i] >> 4];
        id[i * 2 + 1] = hex[official_uuid[i] & 15];
    }
    id[32] = '\0';
    n = snprintf(url, sizeof(url), "https://sessionserver.mojang.com/session/minecraft/profile/%s?unsigned=false", id);
    if (n <= 0 || (size_t)n >= sizeof(url) || http_get(url, response, &status) != 0 || status != 200 ||
        mc_profile_parse_signed(response, official_uuid, out) != 0 || strcasecmp(name, out->name) != 0) return -1;
    return 0;
}

static void *profile_worker(void *arg) {
    mc_profile_service_t *service = arg;
    for (;;) {
        char name[17];
        pthread_mutex_lock(&service->lock);
        while (!service->stop && service->len == 0) pthread_cond_wait(&service->cv, &service->lock);
        if (service->stop) { pthread_mutex_unlock(&service->lock); break; }
        memcpy(name, service->queue[service->head], sizeof(name));
        service->head = (service->head + 1) % PROFILE_QUEUE_MAX;
        service->len--;
        memcpy(service->active_name, name, sizeof(name));
        pthread_mutex_unlock(&service->lock);

        mc_public_profile_t *profile = calloc(1, sizeof(*profile));
        bool found = profile && service->fetch(name, profile, service->fetch_userdata) == 0;
        if (profile) profile->found = found;

        pthread_mutex_lock(&service->lock);
        service->fetched++;
        if (found) service->found++;
        size_t slot = PROFILE_CACHE_MAX;
        int64_t oldest = INT64_MAX;
        for (size_t i = 0; i < PROFILE_CACHE_MAX; i++) {
            if (!service->cache[i].name[0] || strcasecmp(service->cache[i].name, name) == 0) { slot = i; break; }
            if (service->cache[i].expires_ms < oldest) { oldest = service->cache[i].expires_ms; slot = i; }
        }
        if (slot < PROFILE_CACHE_MAX) {
            profile_cache_entry_t *entry = &service->cache[slot];
            memcpy(entry->name, name, sizeof(name));
            entry->expires_ms = monotonic_ms() + (found ? PROFILE_POSITIVE_TTL_MS : PROFILE_NEGATIVE_TTL_MS);
            if (profile) entry->profile = *profile;
            else memset(&entry->profile, 0, sizeof(entry->profile));
        }
        service->active_name[0] = '\0';
        pthread_mutex_unlock(&service->lock);
        free(profile);

        /* At most two public lookups per second, including negative names. */
        struct timespec delay = {0, 500000000};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static int default_fetch(const char *name, mc_public_profile_t *out, void *userdata) {
    (void)userdata;
    return fetch_public_profile(name, out);
}

mc_profile_service_t *mc_profile_service_create_with_fetch(mc_profile_fetch_fn fetch, void *userdata) {
    if (!fetch) return NULL;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return NULL;
    mc_profile_service_t *service = calloc(1, sizeof(*service));
    if (!service) { curl_global_cleanup(); return NULL; }
    service->fetch = fetch;
    service->fetch_userdata = userdata;
    service->cache = calloc(PROFILE_CACHE_MAX, sizeof(*service->cache));
    if (!service->cache || pthread_mutex_init(&service->lock, NULL) != 0) {
        free(service->cache); free(service); curl_global_cleanup(); return NULL;
    }
    if (pthread_cond_init(&service->cv, NULL) != 0) {
        pthread_mutex_destroy(&service->lock); free(service->cache); free(service); curl_global_cleanup(); return NULL;
    }
    if (pthread_create(&service->thread, NULL, profile_worker, service) != 0) {
        pthread_cond_destroy(&service->cv); pthread_mutex_destroy(&service->lock);
        free(service->cache); free(service); curl_global_cleanup(); return NULL;
    }
    return service;
}

mc_profile_service_t *mc_profile_service_create(void) {
    return mc_profile_service_create_with_fetch(default_fetch, NULL);
}

int mc_profile_service_lookup(mc_profile_service_t *service, const char *name, mc_public_profile_t *out) {
    if (!service || !name || !out || strlen(name) < 3 || strlen(name) > 16) return -1;
    pthread_mutex_lock(&service->lock);
    int64_t now = monotonic_ms();
    for (size_t i = 0; i < PROFILE_CACHE_MAX; i++) {
        profile_cache_entry_t *entry = &service->cache[i];
        if (entry->name[0] && entry->expires_ms > now && strcasecmp(entry->name, name) == 0) {
            *out = entry->profile;
            service->cache_hits++;
            pthread_mutex_unlock(&service->lock);
            return 1;
        }
    }
    if (strcasecmp(service->active_name, name) == 0) {
        pthread_mutex_unlock(&service->lock);
        return 0;
    }
    for (size_t i = 0; i < service->len; i++) {
        if (strcasecmp(service->queue[(service->head + i) % PROFILE_QUEUE_MAX], name) == 0) {
            pthread_mutex_unlock(&service->lock);
            return 0;
        }
    }
    if (service->len >= PROFILE_QUEUE_MAX) {
        service->rejected++;
        pthread_mutex_unlock(&service->lock);
        return -1;
    }
    size_t tail = (service->head + service->len) % PROFILE_QUEUE_MAX;
    snprintf(service->queue[tail], sizeof(service->queue[tail]), "%s", name);
    service->len++;
    service->queued++;
    pthread_cond_signal(&service->cv);
    pthread_mutex_unlock(&service->lock);
    return 0;
}

void mc_profile_service_destroy(mc_profile_service_t *service) {
    if (!service) return;
    pthread_mutex_lock(&service->lock);
    service->stop = true;
    pthread_cond_signal(&service->cv);
    pthread_mutex_unlock(&service->lock);
    pthread_join(service->thread, NULL);
    log_info("profile cache: hits=%llu queued=%llu rejected=%llu fetched=%llu found=%llu",
             (unsigned long long)service->cache_hits, (unsigned long long)service->queued,
             (unsigned long long)service->rejected, (unsigned long long)service->fetched,
             (unsigned long long)service->found);
    pthread_cond_destroy(&service->cv);
    pthread_mutex_destroy(&service->lock);
    free(service->cache);
    free(service);
    curl_global_cleanup();
}
