#include <assert.h>
#include <string.h>
#include <time.h>
#include "../kilo.h"

struct editorConfig E;

void test_editorSetStatusMessage(void) {
    editorSetStatusMessage("test message");
    assert(strcmp(E.statusmsg, "test message") == 0);
    assert(E.statusmsg_time > 0);
}
