#include <assert.h>
#include "../kilo.h"

int editorRowHasOpenComment(erow *row);

void test_editorRowHasOpenComment(void) {
    erow row;
    row.hl = NULL;
    row.rsize = 0;
    assert(editorRowHasOpenComment(&row) == 0);

    row.rsize = 10;
    row.hl = malloc(row.rsize);
    memset(row.hl, HL_NORMAL, row.rsize);
    assert(editorRowHasOpenComment(&row) == 0);

    row.hl[row.rsize - 1] = HL_MLCOMMENT;
    row.render = malloc(row.rsize);
    row.render[row.rsize - 2] = ' ';
    row.render[row.rsize - 1] = ' ';
    assert(editorRowHasOpenComment(&row) == 1);

    row.render[row.rsize - 2] = '*';
    row.render[row.rsize - 1] = '/';
    assert(editorRowHasOpenComment(&row) == 0);

    free(row.hl);
    free(row.render);
}
