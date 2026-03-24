#ifndef MC_UTIL_H
#define MC_UTIL_H

#include <stdint.h>
#include <stddef.h>

void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void hex_dump(const uint8_t *data, size_t len);

uint16_t read_be16(const uint8_t *p);
void write_be16(uint8_t *p, uint16_t v);

int read_file(const char *path, uint8_t **out, size_t *out_len);

#endif /* MC_UTIL_H */
