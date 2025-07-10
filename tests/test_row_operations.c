#include <assert.h>
#include <string.h>
#include "../kilo.h"

void editorUpdateRow(erow *row);

struct editorConfig E;

void test_editorUpdateRow_tab_expansion(void) {
    erow row;
    row.chars = "\t";
    row.size = 1;
    row.hl = NULL;
    row.render = NULL;
    E.syntax = NULL;

    editorUpdateRow(&row);

    assert(row.rsize == TAB_SIZE);
    assert(strncmp(row.render, "    ", TAB_SIZE) == 0);

    free(row.render);
    free(row.hl);
}

