#include <stdio.h>

void test_is_separator(void);
void test_editorSyntaxToColor(void);
void test_editorRowHasOpenComment(void);
void test_editorUpdateRow_tab_expansion(void);
void test_editorSetStatusMessage(void);

int main(void) {
    printf("Running tests...\n");
    test_is_separator();
    test_editorSyntaxToColor();
    test_editorRowHasOpenComment();
    test_editorUpdateRow_tab_expansion();
    test_editorSetStatusMessage();
    printf("All tests passed.\n");
    return 0;
}

