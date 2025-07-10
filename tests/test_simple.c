#include <assert.h>

int is_separator(int c);

void test_is_separator(void) {
    assert(is_separator(' ') == 1);
    assert(is_separator('a') == 0);
}

