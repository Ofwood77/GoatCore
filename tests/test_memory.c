#include "mc_memory.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void) {
    uint64_t profiles[] = {4096,8192,16384,32768,65536,102400,204800};
    uint64_t mib, bytes;
    for (unsigned i=0;i<sizeof(profiles)/sizeof(*profiles);i++) {
        char text[32]; snprintf(text,sizeof(text),"%llu",(unsigned long long)profiles[i]);
        assert(!mc_memory_parse_mib(text,&mib) && mib==profiles[i]);
        assert(!mc_memory_plan(mib,UINT64_C(256)*1024*1048576,UINT64_MAX,&bytes));
        assert(bytes==mib*1048576);
    }
    const char *bad[]={"", "-4096", "+4096", "4095", "4G", "4096x", "18446744073709551616", "18446744073709551615"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(mc_memory_parse_mib(bad[i],&mib)==-1);
    assert(!mc_memory_plan(102400,UINT64_C(16)*1024*1048576,UINT64_MAX,&bytes));
    assert(bytes==UINT64_C(16)*1024*1048576-MC_MEMORY_RESERVE_BYTES);
    assert(!mc_memory_plan(4096,UINT64_MAX,UINT64_C(4)*1024*1048576,&bytes));
    assert(bytes==UINT64_C(4)*1024*1048576-MC_MEMORY_RESERVE_BYTES);
    assert(mc_memory_plan(4096,UINT64_MAX,MC_MEMORY_RESERVE_BYTES,&bytes)==-1 && errno==ENOMEM);
    assert(mc_memory_plan(UINT64_MAX,UINT64_MAX,UINT64_MAX,&bytes)==-1);
    puts("memory policy: profiles 4/8/16/32/64/100/200 GiB, overflow, cgroup clamps and pressure refusal passed; no allocation");
    return 0;
}
