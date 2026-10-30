#ifndef MC_UTIL_H
#define MC_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void hex_dump(const uint8_t *data, size_t len);

uint16_t read_be16(const uint8_t *p);
void write_be16(uint8_t *p, uint16_t v);

int read_file(const char *path, uint8_t **out, size_t *out_len);

int64_t mc_now_us(void);
bool mc_perf_enabled(void);
int64_t mc_perf_slow_us(void);
uint64_t mc_perf_summary_ticks(void);

/* Optional MC_SAVE_TRACE_PATH diagnostics; disabled spans read no clocks. */
typedef struct { int64_t wall_us, cpu_us; } mc_save_clock_t;
mc_save_clock_t mc_save_profile_begin(void);
void mc_save_profile_end(const char *phase, int32_t cx, int32_t cz,
                         mc_save_clock_t start, size_t bytes, int result);

#endif /* MC_UTIL_H */
