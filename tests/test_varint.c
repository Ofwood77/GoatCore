#include "mc_protocol.h"
#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t buf[8];
    size_t n = 0;
    int32_t out = 0;

    assert(varint_write(buf, sizeof(buf), 0, &n) == 0);
    assert(n == 1);
    assert(varint_read(buf, n, &out, &n) == 0 && out == 0);

    assert(varint_write(buf, sizeof(buf), 300, &n) == 0);
    assert(varint_read(buf, n, &out, &n) == 0 && out == 300);

    assert(varint_write(buf, sizeof(buf), 2147483647, &n) == 0);
    assert(varint_read(buf, n, &out, &n) == 0 && out == 2147483647);

    return 0;
}
