#include "../kilo.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

// Forward declarations for functions under test
void editorForwardDelChar(void);
void initEditor(void);
void editorDelRow(int at);
void editorInsertRow(int at, char *s, size_t len);

// Test functions
void test_del_key_middle_of_line() {
    initEditor();
    editorInsertRow(0, "hello world", 11);
    E.cx = 6; // Cursor on 'w'
    E.cy = 0;

    editorForwardDelChar();

    assert(E.numrows == 1);
    assert(strcmp(E.row[0].chars, "hello orld") == 0);
    assert(E.cx == 6); // Cursor should not move
}

void test_del_key_end_of_line_merge() {
    initEditor();
    editorInsertRow(0, "hello", 5);
    editorInsertRow(1, "world", 5);

    E.cx = 5; // Cursor at the end of "hello"
    E.cy = 0;

    editorForwardDelChar();

    assert(E.numrows == 1);
    assert(strcmp(E.row[0].chars, "helloworld") == 0);
    assert(E.cx == 5);
}

void test_del_key_at_end_of_file() {
    initEditor();
    editorInsertRow(0, "hello", 5);
    E.cx = 5; // Cursor at the end of "hello"
    E.cy = 0;

    editorForwardDelChar();

    assert(E.numrows == 1);
    assert(strcmp(E.row[0].chars, "hello") == 0); // Nothing should change
    assert(E.cx == 5);
}