#include <assert.h>
#include "../kilo.h"

int editorSyntaxToColor(int hl);

void test_editorSyntaxToColor(void) {
    assert(editorSyntaxToColor(HL_NORMAL) == 37);
    assert(editorSyntaxToColor(HL_COMMENT) == 36);
    assert(editorSyntaxToColor(HL_KEYWORD1) == 33);
    assert(editorSyntaxToColor(HL_STRING) == 35);
    assert(editorSyntaxToColor(HL_NUMBER) == 31);
    assert(editorSyntaxToColor(HL_MATCH) == 34);
}
