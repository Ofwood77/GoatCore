#ifndef MC_MEMORY_H
#define MC_MEMORY_H
#include <stdint.h>
#define MC_MEMORY_DEFAULT_MIB UINT64_C(4096)
#define MC_MEMORY_RESERVE_BYTES (UINT64_C(512) * 1024 * 1024)
int mc_memory_parse_mib(const char *text, uint64_t *mib);
/* Conservative process address-space ceiling, not a preallocation or RSS
 * measurement. External cgroups remain the authority for charged RAM/cache. */
int mc_memory_plan(uint64_t requested_mib, uint64_t available, uint64_t cgroup_headroom, uint64_t *bytes);
int mc_memory_apply(uint64_t requested_mib);
#endif
