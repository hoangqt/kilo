/* Kilo -- A very simple editor in less than 1-kilo lines of code (as counted
 *         by "cloc"). Does not depend on libcurses, directly emits VT100
 *         escapes on the terminal.
 *
 * -----------------------------------------------------------------------
 *
 * Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define KILO_VERSION "0.0.3"

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8     /* Search match. */
#define HL_UNDERLINE 9 /* Word under cursor match. */

#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

#define TAB_SIZE 4

/* Undo operation types */
enum undo_type {
  UNDO_DELETE_LINE,
  UNDO_DELETE_CHAR,
  UNDO_INSERT_CHAR,
  UNDO_INSERT_LINE
};

/* Undo operation structure */
typedef struct undo_op {
  enum undo_type type;
  int row;              /* Row where operation occurred */
  int col;              /* Column where operation occurred */
  char *data;           /* Data for the operation (deleted text, etc.) */
  int data_len;         /* Length of data */
  struct undo_op *next; /* Next operation in the stack */
} undo_op;

#define MAX_UNDO_STACK 100

struct editorSyntax {
  char **filematch;
  char **keywords;
  char singleline_comment_start[2];
  char multiline_comment_start[3];
  char multiline_comment_end[3];
  int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
  int idx;           /* Row index in the file, zero-based. */
  int size;          /* Size of the row, excluding the null term. */
  int rsize;         /* Size of the rendered row. */
  char *chars;       /* Row content. */
  char *render;      /* Row content "rendered" for screen (for TABs). */
  unsigned char *hl; /* Syntax highlight type for each character in render.*/
  int hl_oc;         /* Row had open comment at end in last syntax highlight
                        check. */
} erow;

typedef struct hlcolor {
  int r, g, b;
} hlcolor;

struct editorConfig {
  int cx, cy;     /* Cursor x and y position in characters */
  int rowoff;     /* Offset of row displayed. */
  int coloff;     /* Offset of column displayed. */
  int screenrows; /* Number of rows that we can show */
  int screencols; /* Number of cols that we can show */
  int numrows;    /* Number of rows */
  int rawmode;    /* Is terminal raw mode enabled? */
  erow *row;      /* Rows */
  int dirty;      /* File modified but not saved. */
  char *filename; /* Currently open filename */
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax; /* Current syntax highlight, or NULL. */
  char lineno_buf[16];         /* Buffer for line number input */
  int lineno_len;              /* Length of line number buffer */
  int d_pressed;               /* Track if 'd' was pressed for dd command */
  time_t d_press_time;         /* Time when 'd' was pressed */
  undo_op *undo_stack;         /* Stack of undo operations */
  int undo_count;              /* Number of operations in undo stack */
};

static struct editorConfig E;

enum KEY_ACTION {
  KEY_NULL = 0,    /* NULL */
  CTRL_C = 3,      /* Ctrl-c */
  CTRL_D = 4,      /* Ctrl-d */
  CTRL_F = 6,      /* Ctrl-f */
  CTRL_H = 8,      /* Ctrl-h */
  TAB = 9,         /* Tab */
  CTRL_L = 12,     /* Ctrl+l */
  ENTER = 13,      /* Enter */
  CTRL_Q = 17,     /* Ctrl-q */
  CTRL_S = 19,     /* Ctrl-s */
  CTRL_U = 21,     /* Ctrl-u */
  ESC = 27,        /* Escape */
  BACKSPACE = 127, /* Backspace */
  CTRL_G = 7,      /* Ctrl-g */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
  UNDO_KEY /* ESC+u for undo */
};

void editorSetStatusMessage(const char *fmt, ...);

/* Word highlighting function declarations */
void editorHighlightWordUnderCursor(void);
int editorGetWordAtCursor(char *word, int *start_pos, int *end_pos);

/* Undo function declarations */
void pushUndoOp(enum undo_type type, int row, int col, char *data,
                int data_len);
void executeUndo(void);
void clearUndoStack(void);

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

/* C / C++ */
char *C_HL_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
char *C_HL_keywords[] = {
    /* C Keywords */
    "auto", "break", "case", "continue", "default", "do", "else", "enum",
    "extern", "for", "goto", "if", "register", "return", "sizeof", "static",
    "struct", "switch", "typedef", "union", "volatile", "while", "NULL",

    /* C++ Keywords */
    "alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "class",
    "compl", "constexpr", "const_cast", "deltype", "delete", "dynamic_cast",
    "explicit", "export", "false", "friend", "inline", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
    "private", "protected", "public", "reinterpret_cast", "static_assert",
    "static_cast", "template", "this", "thread_local", "throw", "true", "try",
    "typeid", "typename", "virtual", "xor", "xor_eq",

    /* C types */
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "auto|", "const|", "bool|", NULL};

/* Here we define an array of syntax highlights by extensions, keywords,
 * comments delimiters and flags. */
struct editorSyntax HLDB[] = {{/* C / C++ */
                               C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
                               HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS}};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ======================= Low level terminal handling ====================== */

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
  /* Don't even check the return value as it's too late. */
  if (E.rawmode) {
    tcsetattr(fd, TCSAFLUSH, &orig_termios);
    E.rawmode = 0;
  }
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) { disableRawMode(STDIN_FILENO); }

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
  struct termios raw;

  if (E.rawmode)
    return 0; /* Already enabled. */
  if (!isatty(STDIN_FILENO))
    goto fatal;
  atexit(editorAtExit);
  if (tcgetattr(fd, &orig_termios) == -1)
    goto fatal;

  raw = orig_termios; /* modify the original mode */
  /* input modes: no break, no CR to NL, no parity check, no strip char,
   * no start/stop output control. */
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* output modes - disable post processing */
  raw.c_oflag &= ~(OPOST);
  /* control modes - set 8 bit chars */
  raw.c_cflag |= (CS8);
  /* local modes - choing off, canonical off, no extended functions,
   * no signal chars (^Z,^C) */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  /* control chars - set return condition: min number of bytes and timer. */
  raw.c_cc[VMIN] = 0;  /* Return each byte, or zero for timeout. */
  raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    goto fatal;
  E.rawmode = 1;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
  int nread;
  char c, seq[3];
  while ((nread = read(fd, &c, 1)) == 0)
    ;
  if (nread == -1)
    exit(1);

  while (1) {
    switch (c) {
    case ESC: /* escape sequence */
      /* If this is just an ESC, we'll timeout here. */
      if (read(fd, seq, 1) == 0)
        return ESC;

      /* Check for single character commands first */
      if (seq[0] == 'u') {
        return UNDO_KEY;
      }

      /* For multi-character sequences, read second character */
      if (read(fd, seq + 1, 1) == 0)
        return ESC;

      /* ESC [ sequences. */
      if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
          /* Extended escape, read additional byte. */
          if (read(fd, seq + 2, 1) == 0)
            return ESC;
          if (seq[2] == '~') {
            switch (seq[1]) {
            case '1':
              return HOME_KEY;
            case '3':
              return DEL_KEY;
            case '4':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            case '7':
              return HOME_KEY;
            case '8':
              return END_KEY;
            }
          }
        } else {
          switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
          }
        }
      }

      /* ESC O sequences. */
      else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
      break;
    default:
      return c;
    }
  }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  /* Report cursor location */
  if (write(ofd, "\x1b[6n", 4) != 4)
    return -1;

  /* Read the response: ESC [ rows ; cols R */
  while (i < sizeof(buf) - 1) {
    if (read(ifd, buf + i, 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  /* Parse it. */
  if (buf[0] != ESC || buf[1] != '[')
    return -1;
  if (sscanf(buf + 2, "%9d;%9d", rows, cols) != 2)
    return -1; // NOLINT(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    /* ioctl() failed. Try to query the terminal itself. */
    int orig_row, orig_col, retval;

    /* Get the initial position so we can restore it later. */
    retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
    if (retval == -1)
      goto failed;

    /* Go to right/bottom margin and get position. */
    if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12)
      goto failed;
    retval = getCursorPosition(ifd, ofd, rows, cols);
    if (retval == -1)
      goto failed;

    /* Restore position. */
    char seq[32];
    snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
    if (write(ofd, seq, strlen(seq)) == -1) {
      /* Can't recover... */
    }
    return 0;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

failed:
  return -1;
}

/* ====================== Syntax highlight color scheme  ==================== */

int is_separator(int c) {
  return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editorRowHasOpenComment(erow *row) {
  if (row->hl && row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT &&
      (row->rsize < 2 || (row->render[row->rsize - 2] != '*' ||
                          row->render[row->rsize - 1] != '/')))
    return 1;
  return 0;
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editorUpdateSyntax(erow *row) {
  if (row->rsize == 0) {
    free(row->hl);
    row->hl = NULL;
  } else {
    row->hl = realloc(row->hl, row->rsize);
  }
  memset(row->hl, HL_NORMAL, row->rsize);

  if (E.syntax == NULL)
    return; /* No syntax, everything is HL_NORMAL. */

  int i, prev_sep, in_string, in_comment;
  char *p;
  char **keywords = E.syntax->keywords;
  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  /* Point to the first non-space char. */
  p = row->render;
  i = 0; /* Current char offset */
  while (*p && isspace(*p)) {
    p++;
    i++;
  }
  prev_sep = 1;   /* Tell the parser if 'i' points to start of word. */
  in_string = 0;  /* Are we inside "" or '' ? */
  in_comment = 0; /* Are we inside multi-line comment? */

  /* If the previous line has an open comment, this line starts
   * with an open comment state. */
  if (row->idx > 0 && editorRowHasOpenComment(&E.row[row->idx - 1]))
    in_comment = 1;

  while (*p) { // NOLINT(clang-analyzer-core.uninitialized.Branch)
    /* Handle // comments. */
    if (prev_sep && *p == scs[0] && *(p + 1) == scs[1]) {
      /* From here to end is a comment */
      memset(row->hl + i, HL_COMMENT, row->size - i);
      return;
    }

    /* Handle multi line comments. */
    if (in_comment) {
      row->hl[i] = HL_MLCOMMENT;
      if (*p == mce[0] && *(p + 1) == mce[1]) {
        row->hl[i + 1] = HL_MLCOMMENT;
        p += 2;
        i += 2;
        in_comment = 0;
        prev_sep = 1;
        continue;
      } else {
        prev_sep = 0;
        p++;
        i++;
        continue;
      }
    } else if (*p == mcs[0] && *(p + 1) == mcs[1]) {
      row->hl[i] = HL_MLCOMMENT;
      row->hl[i + 1] = HL_MLCOMMENT;
      p += 2;
      i += 2;
      in_comment = 1;
      prev_sep = 0;
      continue;
    }

    /* Handle "" and '' */
    if (in_string) {
      row->hl[i] = HL_STRING;
      if (*p == '\\') {
        row->hl[i + 1] = HL_STRING;
        p += 2;
        i += 2;
        prev_sep = 0;
        continue;
      }
      if (*p == in_string)
        in_string = 0;
      p++;
      i++;
      continue;
    } else {
      if (*p == '"' || *p == '\'') {
        in_string = *p;
        row->hl[i] = HL_STRING;
        p++;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    /* Handle non printable chars. */
    if (!isprint(*p)) {
      row->hl[i] = HL_NONPRINT;
      p++;
      i++;
      prev_sep = 0;
      continue;
    }

    /* Handle numbers */
    if ((isdigit(*p) && (prev_sep || row->hl[i - 1] == HL_NUMBER)) ||
        (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER)) {
      row->hl[i] = HL_NUMBER;
      p++;
      i++;
      prev_sep = 0;
      continue;
    }

    /* Handle keywords and lib calls */
    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen - 1] == '|';
        if (kw2)
          klen--;

        if (!memcmp(p, keywords[j], klen) && is_separator(*(p + klen))) {
          /* Keyword */
          memset(row->hl + i, kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          p += klen;
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue; /* We had a keyword match */
      }
    }

    /* Not special chars */
    prev_sep = is_separator(*p);
    p++;
    i++;
  }

  /* Propagate syntax change to the next row if the open commen
   * state changed. This may recursively affect all the following rows
   * in the file. */
  int oc = editorRowHasOpenComment(row);
  if (row->hl_oc != oc && row->idx + 1 < E.numrows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
  row->hl_oc = oc;
}

/* Maps syntax highlight token types to terminal colors. */
int editorSyntaxToColor(int hl) {
  switch (hl) {
  case HL_COMMENT:
  case HL_MLCOMMENT:
    return 36; /* cyan */
  case HL_KEYWORD1:
    return 33; /* yellow */
  case HL_KEYWORD2:
    return 32; /* green */
  case HL_STRING:
    return 35; /* magenta */
  case HL_NUMBER:
    return 31; /* red */
  case HL_MATCH:
    return 34; /* blu */
  case HL_UNDERLINE:
    return 37; /* white with underline */
  default:
    return 37; /* white */
  }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax. */
void editorSelectSyntaxHighlight(char *filename) {
  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = HLDB + j;
    unsigned int i = 0;
    while (s->filematch[i]) {
      char *p;
      int patlen = strlen(s->filematch[i]);
      if ((p = strstr(filename, s->filematch[i])) != NULL) {
        if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
          E.syntax = s;
          return;
        }
      }
      i++;
    }
  }
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(erow *row) {
  unsigned int tabs = 0, nonprint = 0;
  int j, idx;

  /* Create a version of the row we can directly print on the screen,
   * respecting tabs, substituting non printable characters with '?'. */
  free(row->render);
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == TAB)
      tabs++;

  unsigned long long allocsize =
      (unsigned long long)row->size + tabs * 8 + nonprint * 9 + 1;
  if (allocsize > UINT32_MAX) {
    printf("Some line of the edited file is too long for kilo\n");
    exit(1);
  }

  row->render = malloc(row->size + tabs * 8 + nonprint * 9 + 1);
  idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == TAB) {
      row->render[idx++] = ' ';
      while ((idx + 1) % 8 != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->rsize = idx;
  row->render[idx] = '\0';

  /* Update the syntax highlighting attributes of the row. */
  editorUpdateSyntax(row);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
  if (at > E.numrows)
    return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  if (at != E.numrows) {
    memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
      E.row[j].idx++;
  }
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len + 1);
  E.row[at].hl = NULL;
  E.row[at].hl_oc = 0;
  E.row[at].render = NULL;
  E.row[at].rsize = 0;
  E.row[at].idx = at;
  editorUpdateRow(E.row + at);
  E.numrows++;
  E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
  erow *row;

  if (at >= E.numrows)
    return;
  row = E.row + at;
  editorFreeRow(row);
  memmove(E.row + at, E.row + at + 1, sizeof(E.row[0]) * (E.numrows - at - 1));
  for (int j = at; j < E.numrows - 1; j++)
    E.row[j].idx++;
  E.numrows--;
  E.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(int *buflen) {
  char *buf = NULL, *p;
  int totlen = 0;
  int j;

  /* Compute count of bytes */
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1; /* +1 is for "\n" at end of every row */
  *buflen = totlen;
  totlen++; /* Also make space for nulterm */

  p = buf = malloc(totlen);
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  *p = '\0';
  return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(erow *row, int at, int c) {
  if (row == NULL)
    return;

  if (at > row->size) {
    /* Pad the string with spaces if the insert location is outside the
     * current length by more than a single character. */
    int padlen = at - row->size;
    /* In the next line +2 means: new char and null term. */
    row->chars = realloc(row->chars, row->size + padlen + 2);
    memset(row->chars + row->size, ' ', padlen);
    row->chars[row->size + padlen + 1] = '\0';
    row->size += padlen + 1;
  } else {
    /* If we are in the middle of the string just make space for 1 new
     * char plus the (already existing) null term. */
    row->chars = realloc(row->chars, row->size + 2);
    memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
    row->size++;
  }
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(row->chars + row->size, s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(erow *row, int at) {
  if (row->size <= at)
    return;
  memmove(row->chars + at, row->chars + at + 1, row->size - at);
  editorUpdateRow(row);
  row->size--;
  E.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  /* If the row where the cursor is currently located does not exist in our
   * logical representaion of the file, add enough empty rows as needed. */
  if (!row) {
    while (E.numrows <= filerow)
      editorInsertRow(E.numrows, "", 0);
  }
  row = &E.row[filerow];
  /* Save insert operation for undo */
  pushUndoOp(UNDO_INSERT_CHAR, filerow, filecol, NULL, 0);
  editorRowInsertChar(row, filecol, c);
  if (E.cx == E.screencols - 1)
    E.coloff++;
  else
    E.cx++;
  E.dirty++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  if (!row) {
    if (filerow == E.numrows) {
      editorInsertRow(filerow, "", 0);
      goto fixcursor;
    }
    return;
  }
  /* If the cursor is over the current line size, we want to conceptually
   * think it's just over the last character. */
  if (filecol >= row->size)
    filecol = row->size;
  if (filecol == 0) {
    editorInsertRow(filerow, "", 0);
  } else {
    /* We are in the middle of a line. Split it between two rows. */
    editorInsertRow(filerow + 1, row->chars + filecol, row->size - filecol);
    row = &E.row[filerow];
    row->chars[filecol] = '\0';
    row->size = filecol;
    editorUpdateRow(row);
  }
fixcursor:
  if (E.cy == E.screenrows - 1) {
    E.rowoff++;
  } else {
    E.cy++;
  }
  E.cx = 0;
  E.coloff = 0;
}

/* Delete the char at the current prompt position. */
void editorDelChar(void) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  if (!row || (filecol == 0 && filerow == 0))
    return;
  if (filecol == 0) {
    /* Handle the case of column 0, we need to move the current line
     * on the right of the previous one. */
    filecol = E.row[filerow - 1].size;
    editorRowAppendString(&E.row[filerow - 1], row->chars, row->size);
    editorDelRow(filerow);
    row = NULL;
    if (E.cy == 0)
      E.rowoff--;
    else
      E.cy--;
    E.cx = filecol;
    if (E.cx >= E.screencols) {
      int shift = (E.screencols - E.cx) + 1;
      E.cx -= shift;
      E.coloff += shift;
    }
  } else {
    /* Save character for undo before deleting */
    if (filecol > 0 && filecol <= row->size) {
      char deleted_char = row->chars[filecol - 1];
      pushUndoOp(UNDO_DELETE_CHAR, filerow, filecol - 1, &deleted_char, 1);
    }
    editorRowDelChar(row, filecol - 1);
    if (E.cx == 0 && E.coloff)
      E.coloff--;
    else
      E.cx--;
  }
  if (row)
    editorUpdateRow(row);
  E.dirty++;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
  FILE *fp;

  E.dirty = 0;
  free(E.filename);
  size_t fnlen = strlen(filename) + 1;
  E.filename = malloc(fnlen);
  memcpy(E.filename, filename, fnlen);

  fp = fopen(filename, "r");
  if (!fp) {
    if (errno != ENOENT) {
      perror("Opening file");
      exit(1);
    }
    return 1;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      line[--linelen] = '\0';
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
  return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd == -1)
    goto writeerr;

  /* Use truncate + a single write(2) call in order to make saving
   * a bit safer, under the limits of what we can do in a small editor. */
  if (ftruncate(fd, len) == -1)
    goto writeerr;
  if (write(fd, buf, len) != len)
    goto writeerr;

  close(fd);
  free(buf);
  E.dirty = 0;
  editorSetStatusMessage("%d bytes written on disk", len);
  return 0;

writeerr:
  free(buf);
  if (fd != -1)
    close(fd);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
  return 1;
}

/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT                                                              \
  { NULL, 0 }

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
  int y;
  erow *r;
  char buf[32];
  struct abuf ab = ABUF_INIT;

  /* Highlight word under cursor */
  editorHighlightWordUnderCursor();

  abAppend(&ab, "\x1b[?25l", 6); /* Hide cursor. */
  abAppend(&ab, "\x1b[H", 3);    /* Go home. */
  int lineno_width = 1;
  if (E.numrows > 0) {
    int max_lineno = E.numrows;
    lineno_width = 0;
    while (max_lineno > 0) {
      lineno_width++;
      max_lineno /= 10;
    }
  }
  lineno_width += 2; // 1 space padding after number
  for (y = 0; y < E.screenrows; y++) {
    int filerow = E.rowoff + y;

    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen =
            snprintf(welcome, sizeof(welcome),
                     "Kilo editor -- verison %s\x1b[0K\r\n", KILO_VERSION);
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(&ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(&ab, " ", 1);
        abAppend(&ab, welcome, welcomelen);
      } else {
        abAppend(&ab, "~\x1b[0K\r\n", 7);
      }
      continue;
    }

    r = &E.row[filerow];
    int len = r->rsize - E.coloff;
    int current_color = -1;
    // Print line number
    char lnbuf[16];
    int lnbuflen =
        snprintf(lnbuf, sizeof(lnbuf), "%*d ", lineno_width - 1, filerow + 1);
    abAppend(&ab, "\x1b[90m", 5); // dark gray color for line numbers
    abAppend(&ab, lnbuf, lnbuflen);
    abAppend(&ab, "\x1b[39m", 5); // reset color
    // Highlight active line
    /* if (y == E.cy) abAppend(&ab, "\x1b[7m", 4); // reverse video */
    if (len > 0) {
      // Do not truncate at colorcolumn, always show up to screencols
      if (len > E.screencols - lineno_width)
        len = E.screencols - lineno_width;
      char *c = r->render + E.coloff;
      unsigned char *hl = r->hl + E.coloff;
      int j;
      int screen_col =
          lineno_width; // screen column index (starts after line number)
      int source_col = E.coloff; // actual column in source text

      for (j = 0; j < len; j++, screen_col++, source_col++) {
        // Check if we should draw an indent guide
        int should_draw_indent_guide = 0;
        if (c[j] == ' ' && source_col % TAB_SIZE == 0 && source_col > 0) {
          // Check if this is part of leading whitespace
          int is_leading_space = 1;
          for (int k = 0; k < source_col; k++) {
            if (k + E.coloff < r->rsize && r->render[k + E.coloff] != ' ') {
              is_leading_space = 0;
              break;
            }
          }
          should_draw_indent_guide = is_leading_space;
        }

        // Draw text (even if over the bar)
        int is_colorcol = (screen_col == 80);
        if (is_colorcol)
          abAppend(&ab, "\x1b[7m", 4); // reverse video for colorcolumn

        if (should_draw_indent_guide) {
          // Draw indent guide with dim color
          abAppend(&ab, "\x1b[2m", 4); // dim
          abAppend(&ab, "|", 1);
          abAppend(&ab, "\x1b[0m", 4); // reset
        } else if (hl[j] == HL_NONPRINT) {
          char sym;
          abAppend(&ab, "\x1b[7m", 4);
          if (c[j] <= 26)
            sym = '@' + c[j];
          else
            sym = '?';
          abAppend(&ab, &sym, 1);
          abAppend(&ab, "\x1b[0m", 4);
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(&ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(&ab, c + j, 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            char buf[16];
            int clen;
            if (hl[j] == HL_UNDERLINE) {
              /* Add underline formatting */
              clen = snprintf(buf, sizeof(buf), "\x1b[%d;4m", color);
            } else {
              clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            }
            current_color = color;
            abAppend(&ab, buf, clen);
          } else if (hl[j] == HL_UNDERLINE) {
            /* Add underline to existing color */
            abAppend(&ab, "\x1b[4m", 4);
          }
          abAppend(&ab, c + j, 1);
          if (hl[j] == HL_UNDERLINE) {
            /* Reset underline */
            abAppend(&ab, "\x1b[24m", 5);
          }
        }
        if (is_colorcol)
          abAppend(&ab, "\x1b[0m", 4); // reset after colorcolumn
      }
      // If line is shorter than colorcolumn, still draw the bar
      if (screen_col <= 80 && (E.screencols - lineno_width) >= 80) {
        int pad = 80 - screen_col;
        while (pad-- > 0) {
          abAppend(&ab, " ", 1);
          screen_col++;
        }
        // Draw a vertical bar at column 80 as background highlight
        abAppend(&ab, "\x1b[7m \x1b[0m", 9);
      }
    } else {
      // If empty line, still draw colorcolumn and indent guides if visible
      int screen_col = lineno_width;
      int source_col = E.coloff;

      // For empty lines, draw indent guides based on surrounding context
      if (r->size == 0 && filerow > 0 && filerow < E.numrows - 1) {
        // Look for indent level from previous or next non-empty line
        int indent_level = 0;
        for (int search_row = filerow - 1; search_row >= 0; search_row--) {
          if (E.row[search_row].size > 0) {
            // Count leading spaces in this line
            for (int i = 0; i < E.row[search_row].size; i++) {
              if (E.row[search_row].chars[i] == ' ') {
                indent_level++;
              } else {
                break;
              }
            }
            break;
          }
        }

        // Draw indent guides for empty line
        while (source_col < indent_level && screen_col < E.screencols) {
          if (source_col % TAB_SIZE == 0 && source_col > 0) {
            abAppend(&ab, "\x1b[2m", 4); // dim
            abAppend(&ab, "|", 1);
            abAppend(&ab, "\x1b[0m", 4); // reset
          } else {
            abAppend(&ab, " ", 1);
          }
          source_col++;
          screen_col++;
        }
      }

      // Fill remaining space and draw colorcolumn if visible
      if ((E.screencols - lineno_width) >= 80) {
        int pad = 80 - screen_col;
        while (pad-- > 0) {
          abAppend(&ab, " ", 1);
          screen_col++;
        }
        abAppend(&ab, "\x1b[7m \x1b[0m", 9);
      }
    }
    abAppend(&ab, "\x1b[39m", 5);
    if (y == E.cy)
      abAppend(&ab, "\x1b[0m", 4); // reset after active line
    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\r\n", 2);
  }

  /* Create a two rows status. First row: */
  abAppend(&ab, "\x1b[0K", 4);
  abAppend(&ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  snprintf(rstatus, sizeof(rstatus), "%d:%d", E.rowoff + E.cy + 1,
           E.coloff + E.cx + 1);
  if (len > E.screencols)
    len = E.screencols;
  abAppend(&ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == (int)strlen(rstatus)) {
      abAppend(&ab, rstatus, strlen(rstatus));
      break;
    } else {
      abAppend(&ab, " ", 1);
      len++;
    }
  }
  abAppend(&ab, "\x1b[0m\r\n", 6);

  /* Second row depends on E.statusmsg and the status message update time. */
  abAppend(&ab, "\x1b[0K", 4);
  int msglen = strlen(E.statusmsg);
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(&ab, E.statusmsg, msglen <= E.screencols ? msglen : E.screencols);

  /* Put cursor at its current position. Note that the horizontal position
   * at which the cursor is displayed may be different compared to 'E.cx'
   * because of TABs. */
  int j;
  int cx = lineno_width + 1; // account for line number margin
  int filerow = E.rowoff + E.cy;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  if (row) {
    for (j = E.coloff; j < (E.cx + E.coloff); j++) {
      if (j < row->size && row->chars[j] == TAB)
        cx += 7 - ((cx) % 8);
      cx++;
    }
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, cx);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6); /* Show cursor. */
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* =============================== Word highlighting ======================= */

/* Get the word under the cursor. Returns 1 if a word is found, 0 otherwise. */
int editorGetWordAtCursor(char *word, int *start_pos, int *end_pos) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;

  if (filerow >= E.numrows)
    return 0;

  erow *row = &E.row[filerow];
  if (filecol >= row->size)
    return 0;

  /* Check if cursor is on a word character */
  if (!isalnum(row->chars[filecol]) && row->chars[filecol] != '_')
    return 0;

  /* Find start of word */
  int start = filecol;
  while (start > 0 &&
         (isalnum(row->chars[start - 1]) || row->chars[start - 1] == '_')) {
    start--;
  }

  /* Find end of word */
  int end = filecol;
  while (end < row->size &&
         (isalnum(row->chars[end]) || row->chars[end] == '_')) {
    end++;
  }

  /* Extract word */
  int word_len = end - start;
  if (word_len > 255)
    word_len = 255; /* Limit word length */

  strncpy(word, row->chars + start, word_len);
  word[word_len] = '\0';

  *start_pos = start;
  *end_pos = end;

  return word_len > 0 ? 1 : 0;
}

/* Highlight all occurrences of the word under cursor */
void editorHighlightWordUnderCursor(void) {
  char word[256];
  int start_pos, end_pos;

  /* Clear existing underline highlights */
  for (int i = 0; i < E.numrows; i++) {
    erow *row = &E.row[i];
    if (row->hl) {
      for (int j = 0; j < row->rsize; j++) {
        if (row->hl[j] == HL_UNDERLINE) {
          row->hl[j] = HL_NORMAL;
        }
      }
    }
  }

  /* Get word under cursor */
  if (!editorGetWordAtCursor(word, &start_pos, &end_pos)) {
    return; /* No word under cursor */
  }

  /* Highlight all matching words in all rows */
  for (int i = 0; i < E.numrows; i++) {
    erow *row = &E.row[i];
    if (!row->render)
      continue;

    char *match = row->render;
    while ((match = strstr(match, word)) != NULL) {
      /* Check if this is a whole word match */
      int match_start = match - row->render;
      int match_end = match_start + strlen(word);

      /* Check boundaries */
      int is_word_start =
          (match_start == 0 || (!isalnum(row->render[match_start - 1]) &&
                                row->render[match_start - 1] != '_'));
      int is_word_end =
          (match_end >= row->rsize ||
           (!isalnum(row->render[match_end]) && row->render[match_end] != '_'));

      if (is_word_start && is_word_end && row->hl) {
        /* Highlight this match */
        for (int j = 0; j < (int)strlen(word) && match_start + j < row->rsize;
             j++) {
          /* Only highlight if it's not already a search match or special syntax
           */
          if (row->hl[match_start + j] != HL_MATCH &&
              row->hl[match_start + j] != HL_COMMENT &&
              row->hl[match_start + j] != HL_MLCOMMENT &&
              row->hl[match_start + j] != HL_STRING) {
            row->hl[match_start + j] = HL_UNDERLINE;
          }
        }
      }

      match++; /* Move to next position */
    }
  }
}

/* =============================== Undo functionality ====================== */

/* Push an operation onto the undo stack */
void pushUndoOp(enum undo_type type, int row, int col, char *data,
                int data_len) {
  /* Limit undo stack size */
  if (E.undo_count >= MAX_UNDO_STACK) {
    /* Remove oldest operation */
    undo_op *oldest = E.undo_stack;
    while (oldest && oldest->next && oldest->next->next) {
      oldest = oldest->next;
    }
    if (oldest && oldest->next) {
      undo_op *to_remove = oldest->next;
      oldest->next = NULL;
      free(to_remove->data);
      free(to_remove);
      E.undo_count--;
    }
  }

  /* Create new undo operation */
  undo_op *op = malloc(sizeof(undo_op));
  op->type = type;
  op->row = row;
  op->col = col;
  op->data_len = data_len;
  if (data && data_len > 0) {
    op->data = malloc(data_len + 1);
    memcpy(op->data, data, data_len);
    op->data[data_len] = '\0';
  } else {
    op->data = NULL;
  }

  /* Push to front of stack */
  op->next = E.undo_stack;
  E.undo_stack = op;
  E.undo_count++;
}

/* Pop and execute an undo operation */
void executeUndo(void) {
  if (!E.undo_stack) {
    editorSetStatusMessage("Nothing to undo");
    return;
  }

  undo_op *op = E.undo_stack;
  E.undo_stack = op->next;
  E.undo_count--;

  switch (op->type) {
  case UNDO_DELETE_LINE:
    /* Restore deleted line */
    if (op->data) {
      editorInsertRow(op->row, op->data, op->data_len);
      /* Move cursor to the restored line */
      E.cy = 0;
      E.cx = 0;
      E.rowoff = op->row;
      E.coloff = 0;
      if (op->row < E.screenrows) {
        E.cy = op->row;
        E.rowoff = 0;
      }
      editorSetStatusMessage("Line restored");
    }
    break;
  case UNDO_DELETE_CHAR:
    /* Restore deleted character */
    if (op->data && op->row < E.numrows) {
      erow *row = &E.row[op->row];
      editorRowInsertChar(row, op->col, op->data[0]);
      /* Move cursor to after the restored character */
      E.cy = op->row - E.rowoff;
      E.cx = op->col + 1;
      if (E.cy < 0) {
        E.rowoff += E.cy;
        E.cy = 0;
      } else if (E.cy >= E.screenrows) {
        E.rowoff += E.cy - E.screenrows + 1;
        E.cy = E.screenrows - 1;
      }
      editorSetStatusMessage("Character restored");
    }
    break;
  case UNDO_INSERT_CHAR:
    /* Remove inserted character */
    if (op->row < E.numrows) {
      erow *row = &E.row[op->row];
      if (op->col < row->size) {
        editorRowDelChar(row, op->col);
        /* Move cursor to the deletion point */
        E.cy = op->row - E.rowoff;
        E.cx = op->col;
        editorSetStatusMessage("Character insertion undone");
      }
    }
    break;
  case UNDO_INSERT_LINE:
    /* Remove inserted line */
    if (op->row < E.numrows) {
      editorDelRow(op->row);
      /* Adjust cursor position */
      if (op->row <= E.rowoff + E.cy) {
        if (E.cy > 0) {
          E.cy--;
        } else if (E.rowoff > 0) {
          E.rowoff--;
        }
      }
      editorSetStatusMessage("Line insertion undone");
    }
    break;
  }

  /* Clean up */
  free(op->data);
  free(op);
}

/* Clear all undo operations */
void clearUndoStack(void) {
  while (E.undo_stack) {
    undo_op *op = E.undo_stack;
    E.undo_stack = op->next;
    free(op->data);
    free(op);
  }
  E.undo_count = 0;
}

/* =============================== Find mode ================================ */

#define KILO_QUERY_LEN 256

/* Delete the current line where the cursor is located */
void editorDeleteCurrentLine(void) {
  int filerow = E.rowoff + E.cy;

  /* If no rows exist, nothing to delete */
  if (E.numrows == 0)
    return;

  /* If cursor is beyond the file, nothing to delete */
  if (filerow >= E.numrows)
    return;

  /* Save the line content for undo */
  erow *row = &E.row[filerow];
  pushUndoOp(UNDO_DELETE_LINE, filerow, 0, row->chars, row->size);

  /* Delete the row */
  editorDelRow(filerow);

  /* Adjust cursor position after deletion */
  if (E.numrows == 0) {
    /* File is now empty, reset cursor */
    E.cy = 0;
    E.cx = 0;
    E.rowoff = 0;
    E.coloff = 0;
  } else if (filerow >= E.numrows) {
    /* Cursor was on last line, move up */
    if (E.cy > 0) {
      E.cy--;
    } else if (E.rowoff > 0) {
      E.rowoff--;
    }
  }

  /* Reset cursor to beginning of line */
  E.cx = 0;
  E.coloff = 0;
}

/* Go to the specified line number */
void editorGoToLine(int line) {
  if (line <= 0)
    line = 1;
  if (line > E.numrows)
    line = E.numrows;

  /* Convert to 0-based indexing */
  line--;

  /* Set cursor to beginning of the target line */
  E.cy = 0;
  E.cx = 0;
  E.rowoff = line;
  E.coloff = 0;

  /* If the line is visible on screen, adjust cy instead of rowoff */
  if (line < E.screenrows) {
    E.cy = line;
    E.rowoff = 0;
  }
}

void editorGoTo(int fd);

void editorGoTo(int fd) {
  char query[KILO_QUERY_LEN + 1] = {0};
  int qlen = 0;
  int saved_cx = E.cx, saved_cy = E.cy;
  int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

  while (1) {
    editorSetStatusMessage("Go to line: %s (Use ESC/Enter)", query);
    editorRefreshScreen();

    int c = editorReadKey(fd);
    if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
      if (qlen != 0) {
        query[--qlen] = '\0';
        if (qlen > 0) {
          editorGoToLine(atoi(query));
        } else {
          E.cx = saved_cx;
          E.cy = saved_cy;
          E.coloff = saved_coloff;
          E.rowoff = saved_rowoff;
        }
      }
    } else if (c == ESC) {
      E.cx = saved_cx;
      E.cy = saved_cy;
      E.coloff = saved_coloff;
      E.rowoff = saved_rowoff;
      editorSetStatusMessage("");
      return;
    } else if (c == ENTER) {
      editorSetStatusMessage("");
      return;
    } else if (isdigit(c)) {
      if (qlen < KILO_QUERY_LEN) {
        query[qlen++] = c;
        query[qlen] = '\0';
        editorGoToLine(atoi(query));
      }
    }
  }
}

void editorFind(int fd) {
  char query[KILO_QUERY_LEN + 1] = {0};
  int qlen = 0;
  int last_match = -1;    /* Last line where a match was found. -1 for none. */
  int find_next = 0;      /* if 1 search next, if -1 search prev. */
  int saved_hl_line = -1; /* No saved HL */
  char *saved_hl = NULL;

#define FIND_RESTORE_HL                                                        \
  do {                                                                         \
    if (saved_hl) {                                                            \
      memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);   \
      free(saved_hl);                                                          \
      saved_hl = NULL;                                                         \
    }                                                                          \
  } while (0)

  /* Save the cursor position in order to restore it later. */
  int saved_cx = E.cx, saved_cy = E.cy;
  int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

  while (1) {
    editorSetStatusMessage("Search: %s (Use ESC/Arrows/Enter)", query);
    editorRefreshScreen();

    int c = editorReadKey(fd);
    if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
      if (qlen != 0)
        query[--qlen] = '\0';
      last_match = -1;
    } else if (c == ESC || c == ENTER) {
      if (c == ESC) {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
      }
      FIND_RESTORE_HL;
      editorSetStatusMessage("");
      return;
    } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
      find_next = 1;
    } else if (c == ARROW_LEFT || c == ARROW_UP) {
      find_next = -1;
    } else if (isprint(c)) {
      if (qlen < KILO_QUERY_LEN) {
        query[qlen++] = c;
        query[qlen] = '\0';
        last_match = -1;
      }
    }

    /* Search occurrence. */
    if (last_match == -1)
      find_next = 1;
    if (find_next) {
      char *match = NULL;
      int match_offset = 0;
      int i, current = last_match;

      for (i = 0; i < E.numrows; i++) {
        current += find_next;
        if (current == -1)
          current = E.numrows - 1;
        else if (current == E.numrows)
          current = 0;
        match = strstr(E.row[current].render, query);
        if (match) {
          match_offset = match - E.row[current].render;
          break;
        }
      }
      find_next = 0;

      /* Highlight */
      FIND_RESTORE_HL;

      if (match) {
        erow *row = &E.row[current];
        last_match = current;
        if (row->hl) {
          saved_hl_line = current;
          saved_hl = malloc(row->rsize);
          memcpy(saved_hl, row->hl, row->rsize);
          memset(row->hl + match_offset, HL_MATCH, qlen);
        }
        E.cy = 0;
        E.cx = match_offset;
        E.rowoff = current;
        E.coloff = 0;
        /* Scroll horizontally as needed. */
        if (E.cx > E.screencols) {
          int diff = E.cx - E.screencols;
          E.cx -= diff;
          E.coloff += diff;
        }
      }
    }
  }
}

/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
  int filerow = E.rowoff + E.cy;
  int filecol = E.coloff + E.cx;
  int rowlen;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx == 0) {
      if (E.coloff) {
        E.coloff--;
      } else {
        if (filerow > 0) {
          E.cy--;
          E.cx = E.row[filerow - 1].size;
          if (E.cx > E.screencols - 1) {
            E.coloff = E.cx - E.screencols + 1;
            E.cx = E.screencols - 1;
          }
        }
      }
    } else {
      E.cx -= 1;
    }
    break;
  case ARROW_RIGHT:
    if (row && filecol < row->size) {
      if (E.cx == E.screencols - 1) {
        E.coloff++;
      } else {
        E.cx += 1;
      }
    } else if (row && filecol == row->size) {
      E.cx = 0;
      E.coloff = 0;
      if (E.cy == E.screenrows - 1) {
        E.rowoff++;
      } else {
        E.cy += 1;
      }
    }
    break;
  case ARROW_UP:
    if (E.cy == 0) {
      if (E.rowoff)
        E.rowoff--;
    } else {
      E.cy -= 1;
    }
    break;
  case ARROW_DOWN:
    if (filerow < E.numrows) {
      if (E.cy == E.screenrows - 1) {
        E.rowoff++;
      } else {
        E.cy += 1;
      }
    }
    break;
  }
  /* Fix cx if the current line has not enough chars. */
  filerow = E.rowoff + E.cy;
  filecol = E.coloff + E.cx;
  row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  rowlen = row ? row->size : 0;
  if (filecol > rowlen) {
    E.cx -= filecol - rowlen;
    if (E.cx < 0) {
      E.coloff += E.cx;
      E.cx = 0;
    }
  }
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define KILO_QUIT_TIMES 3
void editorProcessKeypress(int fd) {
  /* When the file is modified, requires Ctrl-q to be pressed N times
   * before actually quitting. */
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey(fd);
  switch (c) {
  case ENTER: /* Enter */
    E.d_pressed = 0;
    editorInsertNewline();
    break;
  case CTRL_C: /* Ctrl-c */
    /* We ignore ctrl-c, it can't be so simple to lose the changes
     * to the edited file. */
    break;
  case CTRL_Q: /* Ctrl-q */
    /* Quit if the file was already saved. */
    if (E.dirty && quit_times) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    exit(0);
    break;
  case CTRL_S:
    editorSave();
    break;
  case CTRL_F:
    editorFind(fd);
    break;
  case BACKSPACE: /* Backspace */
  case CTRL_H:    /* Ctrl-h */
  case DEL_KEY:
    editorDelChar();
    break;
  case CTRL_U: /* Ctrl-u for page up */
  case PAGE_UP:
  case PAGE_DOWN:
  case CTRL_D: /* Ctrl-d for page down */
    if ((c == PAGE_UP || c == CTRL_U) && E.cy != 0)
      E.cy = 0;
    else if ((c == PAGE_DOWN || c == CTRL_D) && E.cy != E.screenrows - 1)
      E.cy = E.screenrows - 1;
    {
      int times = E.screenrows;
      while (times--)
        editorMoveCursor((c == PAGE_UP || c == CTRL_U) ? ARROW_UP : ARROW_DOWN);
    }
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    E.d_pressed = 0; /* Reset d state */
    editorMoveCursor(c);
    break;
  case CTRL_L: /* ctrl+l, clear screen */
    /* Just refresht the line as side effect. */
    break;
  case TAB:
    /* Insert 4 spaces instead of a tab character */
    for (int i = 0; i < TAB_SIZE; i++)
      editorInsertChar(' ');
    break;
  case ESC:
    /* Nothing to do for ESC in this mode. */
    break;
  case END_KEY: {
    int filerow = E.rowoff + E.cy;
    if (filerow < E.numrows) {
      erow *row = &E.row[filerow];
      int end = row->rsize; // use rendered size for cursor position
      // If end is before current coloff, reset coloff/cx
      if (end < E.coloff) {
        E.coloff = 0;
        E.cx = 0;
      } else if (end >= E.coloff + E.screencols) {
        E.coloff = end - E.screencols + 1;
        E.cx = E.screencols - 1;
      } else {
        E.cx = end - E.coloff;
      }
    }
    break;
  }
  case HOME_KEY: {
    int filerow = E.rowoff + E.cy;
    if (filerow < E.numrows) {
      erow *row = &E.row[filerow];
      int first_nonspace = 0;
      while (first_nonspace < row->size && (row->chars[first_nonspace] == ' ' ||
                                            row->chars[first_nonspace] == '\t'))
        first_nonspace++;
      if (first_nonspace >= E.coloff + E.screencols) {
        E.coloff = first_nonspace - E.screencols + 1;
        E.cx = E.screencols - 1;
      } else if (first_nonspace < E.coloff) {
        E.coloff = first_nonspace;
        E.cx = 0;
      } else {
        E.cx = first_nonspace - E.coloff;
      }
    } else {
      E.coloff = 0;
      E.cx = 0;
    }
    break;
  }
  case CTRL_G:
    editorGoTo(fd);
    break;
  case UNDO_KEY:
    E.d_pressed = 0;
    executeUndo();
    break;
  case 'd':
    if (E.d_pressed && (time(NULL) - E.d_press_time) <= 1) {
      /* Second 'd' pressed within 1 second - delete the 'd' we just inserted
       * and delete the line */
      editorDelChar(); /* Remove the 'd' we just inserted */
      editorDeleteCurrentLine();
      editorSetStatusMessage("Line deleted");
      E.d_pressed = 0;
    } else {
      /* First 'd' or timeout - insert 'd' character immediately */
      editorInsertChar('d');
      E.d_pressed = 1;
      E.d_press_time = time(NULL);
    }
    break;
  default:
    /* Reset d_pressed if any other key is pressed */
    E.d_pressed = 0;
    editorInsertChar(c);
    break;
  }

  quit_times = KILO_QUIT_TIMES; /* Reset it to the original value. */
}

int editorFileWasModified(void) { return E.dirty; }

void updateWindowSize(void) {
  if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows,
                    &E.screencols) == -1) {
    perror("Unable to query the screen for size (columns / rows)");
    exit(1);
  }
  E.screenrows -= 2; /* Get room for status bar. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
  updateWindowSize();
  if (E.cy > E.screenrows)
    E.cy = E.screenrows - 1;
  if (E.cx > E.screencols)
    E.cx = E.screencols - 1;
  editorRefreshScreen();
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.syntax = NULL;
  E.lineno_len = 0;
  E.d_pressed = 0;
  E.undo_stack = NULL;
  E.undo_count = 0;
  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fputs("Usage: kilo <filename>\n", stderr);
    exit(1);
  }

  initEditor();
  editorSelectSyntaxHighlight(argv[1]);
  editorOpen(argv[1]);
  enableRawMode(STDIN_FILENO);
  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find "
                         "| Ctrl-G = go to line");
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress(STDIN_FILENO);
  }
  return 0;
}
