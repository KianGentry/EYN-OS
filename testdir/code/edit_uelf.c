#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Graphical text editor.", "edit [/path/to/file]");

// Limits

#define MAX_LINES      2048
#define MAX_LINE_LEN   256
#define MAX_PATH_LEN   256
#define UNDO_STACK_MAX 64
#define FIND_BUF_MAX   64
#define TAB_WIDTH      4

// Colours -- base palette from OS defaults, app-specific overrides follow.

/* Background */
#define BG_R  GUI_PAL_BG_R
#define BG_G  GUI_PAL_BG_G
#define BG_B  GUI_PAL_BG_B

/* Gutter (line numbers) */
#define GUT_BG_R  36
#define GUT_BG_G  36
#define GUT_BG_B  36

#define GUT_FG_R 120
#define GUT_FG_G 120
#define GUT_FG_B 120

/* Current line gutter highlight */
#define GUT_CUR_R 200
#define GUT_CUR_G 200
#define GUT_CUR_B 100

/* Text */
#define TXT_R GUI_PAL_TEXT_R
#define TXT_G GUI_PAL_TEXT_G
#define TXT_B GUI_PAL_TEXT_B

/* Current line highlight */
#define CUR_LINE_R  GUI_PAL_STATUS_R
#define CUR_LINE_G  GUI_PAL_STATUS_G
#define CUR_LINE_B  44

/* Cursor */
#define CUR_R GUI_PAL_TEXT_R
#define CUR_G GUI_PAL_TEXT_G
#define CUR_B GUI_PAL_TEXT_B

/* Selection */
#define SEL_R  60
#define SEL_G  90
#define SEL_B 150

/* Status bar */
#define STAT_BG_R  GUI_PAL_STATUS_R
#define STAT_BG_G  GUI_PAL_STATUS_G
#define STAT_BG_B  GUI_PAL_STATUS_B

#define STAT_FG_R 180
#define STAT_FG_G 180
#define STAT_FG_B 180

/* Find bar */
#define FIND_BG_R  50
#define FIND_BG_G  50
#define FIND_BG_B  60

/* Match highlight */
#define MATCH_BG_R  80
#define MATCH_BG_G 120
#define MATCH_BG_B  60

/* Scrollbar */
#define SBAR_R  GUI_PAL_BORDER_R
#define SBAR_G  GUI_PAL_BORDER_G
#define SBAR_B  GUI_PAL_BORDER_B

/* Syntax: keywords */
#define KW_R  GUI_PAL_ACCENT_R
#define KW_G  170
#define KW_B  GUI_PAL_ACCENT_B

/* Syntax: strings */
#define STR_R  200
#define STR_G  150
#define STR_B   80

/* Syntax: comments */
#define CMT_R  100
#define CMT_G  140
#define CMT_B  100

/* Syntax: numbers */
#define NUM_R  180
#define NUM_G  140
#define NUM_B  255

/* Syntax: preprocessor */
#define PP_R   180
#define PP_G   120
#define PP_B   200


// Data structures

typedef struct {
    char data[MAX_LINE_LEN];
    int  len;
} line_t;

typedef enum {
    UNDO_INSERT_CHAR,
    UNDO_DELETE_CHAR,
    UNDO_INSERT_LINE,    /* newline created a new line */
    UNDO_DELETE_LINE,    /* backspace merged lines */
} undo_type_t;

typedef struct {
    undo_type_t type;
    int line;
    int col;
    char ch;
    char saved_line[MAX_LINE_LEN]; /* for line merge/split undo */
    int saved_len;
} undo_entry_t;

typedef struct {
    int handle;
    int running;

    /* File */
    char filepath[MAX_PATH_LEN];
    int  has_file;
    int  modified;

    /* Buffer */
    line_t* lines;
    int     line_count;

    /* Cursor */
    int cur_line;
    int cur_col;

    /* Scroll */
    int scroll_y;
    int scroll_x;

    /* Selection (anchor-based; -1 = no selection) */
    int sel_anchor_line;
    int sel_anchor_col;

    /* Viewport metrics (updated each frame from content size + font) */
    int content_w;
    int content_h;
    int char_w;
    int char_h;
    int gutter_w;   /* pixels */
    int visible_lines;
    int visible_cols;

    /* Status message (transient, shows for a few frames then clears) */
    char status_msg[80];
    int  status_timer;

    /* Find mode */
    int  find_active;
    char find_buf[FIND_BUF_MAX];
    int  find_len;
    int  find_match_line;
    int  find_match_col;

    /* Undo / redo */
    undo_entry_t undo_stack[UNDO_STACK_MAX];
    int undo_top;  /* next free slot (0 = empty) */
    undo_entry_t redo_stack[UNDO_STACK_MAX];
    int redo_top;

    /* Mouse state */
    int prev_left_down;

    /* Syntax: is C file */
    int is_c_file;

} editor_t;


// Helpers

static void safe_strcpy(char* dst, int cap, const char* src) {
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}

static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

static void set_status(editor_t* ed, const char* msg) {
    safe_strcpy(ed->status_msg, (int)sizeof(ed->status_msg), msg);
    ed->status_timer = 120; /* ~2 seconds at 60fps / ~4 sec at 30fps */
}

/* Forward declarations for functions used by clipboard_paste before their definition */
static void editor_insert_char(editor_t* ed, char ch);
static void editor_insert_newline(editor_t* ed);
static void update_title(editor_t* ed);
static int delete_selection(editor_t* ed);
static void clear_selection(editor_t* ed);

/* ── Selection helpers ── */

static int has_selection(editor_t* ed) {
    return ed->sel_anchor_line >= 0 &&
           (ed->sel_anchor_line != ed->cur_line || ed->sel_anchor_col != ed->cur_col);
}

/*
 * Return selection bounds in document order (start <= end).
 * Returns 1 if there is a selection, 0 otherwise.
 */
static int get_sel_ordered(editor_t* ed, int* l1, int* c1, int* l2, int* c2) {
    if (!has_selection(ed)) return 0;
    if (ed->sel_anchor_line < ed->cur_line ||
        (ed->sel_anchor_line == ed->cur_line && ed->sel_anchor_col < ed->cur_col)) {
        *l1 = ed->sel_anchor_line; *c1 = ed->sel_anchor_col;
        *l2 = ed->cur_line;        *c2 = ed->cur_col;
    } else {
        *l1 = ed->cur_line;        *c1 = ed->cur_col;
        *l2 = ed->sel_anchor_line; *c2 = ed->sel_anchor_col;
    }
    return 1;
}

/* Clear any active selection */
static void clear_selection(editor_t* ed) {
    ed->sel_anchor_line = -1;
    ed->sel_anchor_col = -1;
}

/* If no selection anchor is set, plant one at the current cursor position */
static void ensure_anchor(editor_t* ed) {
    if (ed->sel_anchor_line < 0) {
        ed->sel_anchor_line = ed->cur_line;
        ed->sel_anchor_col = ed->cur_col;
    }
}

static int is_word_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* Move cursor one word to the left (Ctrl+Left behavior) */
static void word_left(editor_t* ed) {
    if (ed->cur_col > 0) {
        line_t* ln = &ed->lines[ed->cur_line];
        int c = ed->cur_col - 1;
        /* Skip whitespace/non-word chars */
        while (c > 0 && !is_word_char(ln->data[c])) c--;
        /* Skip word chars */
        while (c > 0 && is_word_char(ln->data[c - 1])) c--;
        ed->cur_col = c;
    } else if (ed->cur_line > 0) {
        ed->cur_line--;
        ed->cur_col = ed->lines[ed->cur_line].len;
    }
}

/* Move cursor one word to the right (Ctrl+Right behavior) */
static void word_right(editor_t* ed) {
    line_t* ln = &ed->lines[ed->cur_line];
    if (ed->cur_col < ln->len) {
        int c = ed->cur_col;
        /* Skip word chars */
        while (c < ln->len && is_word_char(ln->data[c])) c++;
        /* Skip whitespace/non-word chars */
        while (c < ln->len && !is_word_char(ln->data[c])) c++;
        ed->cur_col = c;
    } else if (ed->cur_line < ed->line_count - 1) {
        ed->cur_line++;
        ed->cur_col = 0;
    }
}

/* ── Clipboard ── */

#define CLIPBOARD_MAX  (MAX_LINES * 64)  /* max clipboard size in bytes */
static char g_clipboard[CLIPBOARD_MAX];
static int  g_clipboard_len = 0;

/* Copy current selection to clipboard. Returns number of bytes copied. */
static int clipboard_copy(editor_t* ed) {
    int l1, c1, l2, c2;
    if (!get_sel_ordered(ed, &l1, &c1, &l2, &c2)) return 0;

    int pos = 0;
    for (int li = l1; li <= l2 && pos < CLIPBOARD_MAX - 2; ++li) {
        line_t* ln = &ed->lines[li];
        int from = (li == l1) ? c1 : 0;
        int to   = (li == l2) ? c2 : ln->len;
        for (int ci = from; ci < to && pos < CLIPBOARD_MAX - 2; ++ci)
            g_clipboard[pos++] = ln->data[ci];
        if (li < l2 && pos < CLIPBOARD_MAX - 2)
            g_clipboard[pos++] = '\n';
    }
    g_clipboard[pos] = '\0';
    g_clipboard_len = pos;
    return pos;
}

/*
 * Delete the current selection from the buffer.
 * Returns 1 if text was deleted, 0 if no selection.
 * After deletion, cursor is at the start of the former selection.
 */
static int delete_selection(editor_t* ed) {
    int l1, c1, l2, c2;
    if (!get_sel_ordered(ed, &l1, &c1, &l2, &c2)) return 0;

    if (l1 == l2) {
        /* Single-line selection: shift chars left */
        line_t* ln = &ed->lines[l1];
        int del_count = c2 - c1;
        for (int i = c1; i + del_count <= ln->len; ++i)
            ln->data[i] = ln->data[i + del_count];
        ln->len -= del_count;
        ln->data[ln->len] = '\0';
    } else {
        /* Multi-line selection */
        line_t* first = &ed->lines[l1];
        line_t* last  = &ed->lines[l2];

        /* Keep first line up to c1, append last line from c2 onward */
        int tail_len = last->len - c2;
        if (c1 + tail_len < MAX_LINE_LEN) {
            memcpy(first->data + c1, last->data + c2, (size_t)tail_len);
            first->len = c1 + tail_len;
            first->data[first->len] = '\0';
        } else {
            first->len = c1;
            first->data[first->len] = '\0';
        }

        /* Remove lines l1+1..l2 */
        int remove_count = l2 - l1;
        for (int i = l1 + 1; i + remove_count <= ed->line_count; ++i)
            ed->lines[i] = ed->lines[i + remove_count];
        ed->line_count -= remove_count;
        if (ed->line_count < 1) ed->line_count = 1;
    }

    ed->cur_line = l1;
    ed->cur_col = c1;
    clear_selection(ed);
    ed->modified = 1;
    return 1;
}

/* Paste clipboard at cursor position */
static void clipboard_paste(editor_t* ed) {
    if (g_clipboard_len <= 0) { set_status(ed, "Clipboard empty"); return; }

    /* Delete selection first if active */
    delete_selection(ed);

    for (int i = 0; i < g_clipboard_len; ++i) {
        if (g_clipboard[i] == '\n') {
            editor_insert_newline(ed);
        } else {
            editor_insert_char(ed, g_clipboard[i]);
        }
    }
    update_title(ed);
}

static void update_title(editor_t* ed) {
    char title[MAX_PATH_LEN + 16];
    int i = 0;
    if (ed->modified) {
        title[i++] = '*';
        title[i++] = ' ';
    }
    if (ed->has_file) {
        /* Show just the filename portion */
        const char* name = ed->filepath;
        const char* slash = ed->filepath;
        for (int j = 0; ed->filepath[j]; ++j)
            if (ed->filepath[j] == '/') slash = &ed->filepath[j + 1];
        if (slash[0]) name = slash;
        for (int j = 0; name[j] && i < (int)sizeof(title) - 8; ++j)
            title[i++] = name[j];
    } else {
        const char* unt = "Untitled";
        for (int j = 0; unt[j] && i < (int)sizeof(title) - 8; ++j)
            title[i++] = unt[j];
    }
    const char* suf = " - Edit";
    for (int j = 0; suf[j] && i < (int)sizeof(title) - 1; ++j)
        title[i++] = suf[j];
    title[i] = '\0';
    gui_set_title(ed->handle, title);
}

static int is_c_extension(const char* path) {
    int len = str_len(path);
    if (len >= 2 && path[len-2] == '.' && (path[len-1] == 'c' || path[len-1] == 'h'))
        return 1;
    if (len >= 4 && path[len-4] == '.' && path[len-3] == 'a' && path[len-2] == 's' && path[len-1] == 'm')
        return 1;
    return 0;
}


// Buffer management

static void editor_init_buffer(editor_t* ed) {
    ed->lines = (line_t*)malloc(MAX_LINES * sizeof(line_t));
    if (!ed->lines) {
        /* Fatal: cannot allocate buffer */
        puts("edit: out of memory");
        _exit(1);
    }
    memset(ed->lines, 0, MAX_LINES * sizeof(line_t));
    ed->line_count = 1;
    ed->lines[0].len = 0;
    ed->lines[0].data[0] = '\0';
}

static int editor_load_file(editor_t* ed, const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    ed->line_count = 0;
    int cur_line = 0;
    int cur_col = 0;
    char buf[512];

    for (;;) {
        int n = read(fd, buf, (int)sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            char ch = buf[i];
            if (ch == '\r') continue; /* skip CR */
            if (ch == '\n') {
                if (cur_line < MAX_LINES) {
                    ed->lines[cur_line].data[cur_col] = '\0';
                    ed->lines[cur_line].len = cur_col;
                    cur_line++;
                    cur_col = 0;
                }
                continue;
            }
            if (cur_line < MAX_LINES && cur_col < MAX_LINE_LEN - 1) {
                /* Expand tabs to spaces */
                if (ch == '\t') {
                    int spaces = TAB_WIDTH - (cur_col % TAB_WIDTH);
                    for (int s = 0; s < spaces && cur_col < MAX_LINE_LEN - 1; ++s)
                        ed->lines[cur_line].data[cur_col++] = ' ';
                } else {
                    ed->lines[cur_line].data[cur_col++] = ch;
                }
            }
        }
    }

    /* Handle last line (no trailing newline) */
    if (cur_col > 0 || cur_line == 0) {
        if (cur_line < MAX_LINES) {
            ed->lines[cur_line].data[cur_col] = '\0';
            ed->lines[cur_line].len = cur_col;
            cur_line++;
        }
    }

    ed->line_count = cur_line;
    if (ed->line_count == 0) {
        ed->line_count = 1;
        ed->lines[0].len = 0;
        ed->lines[0].data[0] = '\0';
    }

    close(fd);
    safe_strcpy(ed->filepath, MAX_PATH_LEN, path);
    ed->has_file = 1;
    ed->modified = 0;
    ed->is_c_file = is_c_extension(path);
    return 0;
}

static int editor_save_file(editor_t* ed) {
    if (!ed->has_file) {
        set_status(ed, "No filename -- use Ctrl+S after providing path via args");
        return -1;
    }

    /* Use EYNFS streaming writer for efficient saves */
    int sh = eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_BEGIN, (int)ed->filepath);
    if (sh < 0) {
        set_status(ed, "Save failed: cannot open file for writing");
        return -1;
    }

    for (int i = 0; i < ed->line_count; ++i) {
        if (ed->lines[i].len > 0) {
            eyn_syscall3(EYN_SYSCALL_EYNFS_STREAM_WRITE, sh,
                         ed->lines[i].data, ed->lines[i].len);
        }
        /* Write newline after each line except the very last if it's empty */
        if (i < ed->line_count - 1 || ed->lines[i].len > 0) {
            eyn_syscall3(EYN_SYSCALL_EYNFS_STREAM_WRITE, sh, "\n", 1);
        }
    }

    int rc = eyn_syscall1(EYN_SYSCALL_EYNFS_STREAM_END, sh);
    if (rc < 0) {
        set_status(ed, "Save failed: stream end error");
        return -1;
    }

    ed->modified = 0;
    update_title(ed);
    set_status(ed, "Saved.");
    return 0;
}


// Undo / redo management

static void undo_push(editor_t* ed, undo_type_t type, int line, int col, char ch,
                       const char* saved, int saved_len) {
    if (ed->undo_top >= UNDO_STACK_MAX) {
        /* Shift stack down by half to make room (lose oldest entries) */
        int keep = UNDO_STACK_MAX / 2;
        memmove(&ed->undo_stack[0], &ed->undo_stack[UNDO_STACK_MAX - keep],
                (size_t)keep * sizeof(undo_entry_t));
        ed->undo_top = keep;
    }
    undo_entry_t* e = &ed->undo_stack[ed->undo_top++];
    e->type = type;
    e->line = line;
    e->col = col;
    e->ch = ch;
    if (saved && saved_len > 0) {
        int copy_len = saved_len < MAX_LINE_LEN ? saved_len : MAX_LINE_LEN - 1;
        memcpy(e->saved_line, saved, (size_t)copy_len);
        e->saved_line[copy_len] = '\0';
        e->saved_len = copy_len;
    } else {
        e->saved_line[0] = '\0';
        e->saved_len = 0;
    }

    /* Clear redo stack on new edit */
    ed->redo_top = 0;
}

static void redo_push(editor_t* ed, const undo_entry_t* entry) {
    if (ed->redo_top >= UNDO_STACK_MAX) return;
    ed->redo_stack[ed->redo_top++] = *entry;
}


// Editing operations

static void editor_insert_char(editor_t* ed, char ch) {
    /* If text is selected, delete it first */
    delete_selection(ed);

    line_t* ln = &ed->lines[ed->cur_line];
    if (ln->len >= MAX_LINE_LEN - 1) return;

    /* Shift right */
    for (int i = ln->len; i > ed->cur_col; --i)
        ln->data[i] = ln->data[i - 1];
    ln->data[ed->cur_col] = ch;
    ln->len++;
    ln->data[ln->len] = '\0';

    undo_push(ed, UNDO_INSERT_CHAR, ed->cur_line, ed->cur_col, ch, NULL, 0);
    ed->cur_col++;
    ed->modified = 1;
}

static void editor_insert_newline(editor_t* ed) {
    /* If text is selected, delete it first */
    delete_selection(ed);

    if (ed->line_count >= MAX_LINES) return;

    line_t* cur = &ed->lines[ed->cur_line];

    /* Save current line for undo */
    undo_push(ed, UNDO_INSERT_LINE, ed->cur_line, ed->cur_col, '\n',
              cur->data, cur->len);

    /* Create new line from text after cursor */
    int tail_len = cur->len - ed->cur_col;

    /* Shift all lines below down by 1 */
    for (int i = ed->line_count; i > ed->cur_line + 1; --i)
        ed->lines[i] = ed->lines[i - 1];

    line_t* newline = &ed->lines[ed->cur_line + 1];
    if (tail_len > 0) {
        memcpy(newline->data, cur->data + ed->cur_col, (size_t)tail_len);
    }
    newline->data[tail_len] = '\0';
    newline->len = tail_len;

    /* Truncate current line */
    cur->data[ed->cur_col] = '\0';
    cur->len = ed->cur_col;

    ed->line_count++;
    ed->cur_line++;
    ed->cur_col = 0;
    ed->modified = 1;
}

static void editor_delete_char_backward(editor_t* ed) {
    /* If text is selected, delete selection instead of single char */
    if (delete_selection(ed)) return;

    if (ed->cur_col > 0) {
        line_t* ln = &ed->lines[ed->cur_line];
        char deleted = ln->data[ed->cur_col - 1];
        undo_push(ed, UNDO_DELETE_CHAR, ed->cur_line, ed->cur_col - 1, deleted, NULL, 0);

        for (int i = ed->cur_col - 1; i < ln->len - 1; ++i)
            ln->data[i] = ln->data[i + 1];
        ln->len--;
        ln->data[ln->len] = '\0';
        ed->cur_col--;
        ed->modified = 1;
    } else if (ed->cur_line > 0) {
        /* Merge with previous line */
        line_t* prev = &ed->lines[ed->cur_line - 1];
        line_t* cur = &ed->lines[ed->cur_line];

        undo_push(ed, UNDO_DELETE_LINE, ed->cur_line, 0, '\n',
                  prev->data, prev->len);

        int prev_len = prev->len;
        if (prev_len + cur->len < MAX_LINE_LEN) {
            memcpy(prev->data + prev_len, cur->data, (size_t)cur->len);
            prev->len = prev_len + cur->len;
            prev->data[prev->len] = '\0';
        }

        /* Shift lines up */
        for (int i = ed->cur_line; i < ed->line_count - 1; ++i)
            ed->lines[i] = ed->lines[i + 1];
        ed->line_count--;

        ed->cur_line--;
        ed->cur_col = prev_len;
        ed->modified = 1;
    }
}

static void editor_delete_char_forward(editor_t* ed) {
    /* If text is selected, delete selection instead of single char */
    if (delete_selection(ed)) return;

    line_t* ln = &ed->lines[ed->cur_line];
    if (ed->cur_col < ln->len) {
        char deleted = ln->data[ed->cur_col];
        undo_push(ed, UNDO_DELETE_CHAR, ed->cur_line, ed->cur_col, deleted, NULL, 0);

        for (int i = ed->cur_col; i < ln->len - 1; ++i)
            ln->data[i] = ln->data[i + 1];
        ln->len--;
        ln->data[ln->len] = '\0';
        ed->modified = 1;
    } else if (ed->cur_line < ed->line_count - 1) {
        /* Merge with next line */
        line_t* cur = &ed->lines[ed->cur_line];
        line_t* next = &ed->lines[ed->cur_line + 1];

        undo_push(ed, UNDO_DELETE_LINE, ed->cur_line + 1, 0, '\n',
                  cur->data, cur->len);

        int cur_len = cur->len;
        if (cur_len + next->len < MAX_LINE_LEN) {
            memcpy(cur->data + cur_len, next->data, (size_t)next->len);
            cur->len = cur_len + next->len;
            cur->data[cur->len] = '\0';
        }

        for (int i = ed->cur_line + 1; i < ed->line_count - 1; ++i)
            ed->lines[i] = ed->lines[i + 1];
        ed->line_count--;
        ed->modified = 1;
    }
}

static void editor_undo(editor_t* ed) {
    if (ed->undo_top <= 0) { set_status(ed, "Nothing to undo"); return; }
    undo_entry_t* e = &ed->undo_stack[--ed->undo_top];
    redo_push(ed, e);

    switch (e->type) {
    case UNDO_INSERT_CHAR:
        /* Was an insert -- delete it */
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        {
            line_t* ln = &ed->lines[e->line];
            if (e->col < ln->len) {
                for (int i = e->col; i < ln->len - 1; ++i)
                    ln->data[i] = ln->data[i + 1];
                ln->len--;
                ln->data[ln->len] = '\0';
            }
        }
        break;
    case UNDO_DELETE_CHAR:
        /* Was a delete -- re-insert it */
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        {
            line_t* ln = &ed->lines[e->line];
            if (ln->len < MAX_LINE_LEN - 1) {
                for (int i = ln->len; i > e->col; --i)
                    ln->data[i] = ln->data[i - 1];
                ln->data[e->col] = e->ch;
                ln->len++;
                ln->data[ln->len] = '\0';
                ed->cur_col = e->col + 1;
            }
        }
        break;
    case UNDO_INSERT_LINE:
        /* Was a newline insert -- merge the lines back */
        if (e->line + 1 < ed->line_count) {
            /* Restore original line content */
            memcpy(ed->lines[e->line].data, e->saved_line, (size_t)e->saved_len);
            ed->lines[e->line].data[e->saved_len] = '\0';
            ed->lines[e->line].len = e->saved_len;
            /* Remove the line that was created */
            for (int i = e->line + 1; i < ed->line_count - 1; ++i)
                ed->lines[i] = ed->lines[i + 1];
            ed->line_count--;
        }
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        break;
    case UNDO_DELETE_LINE:
        /* Was a line merge -- split them back */
        if (ed->line_count < MAX_LINES) {
            /* Restore the line before merge */
            memcpy(ed->lines[e->line - 1].data, e->saved_line, (size_t)e->saved_len);
            ed->lines[e->line - 1].data[e->saved_len] = '\0';
            ed->lines[e->line - 1].len = e->saved_len;

            /* Re-create the merged line: what was after the cursor */
            line_t* merged = &ed->lines[e->line - 1];
            int tail_start = e->saved_len;
            int cur_total = merged->len; /* current merged length may be wrong; recalculate */
            /* Actually, merged->len was just set to saved_len above.
             * The content after saved_len in the current (already merged) line
             * needs to be the new line. But we already overwrote it.
             * For simplicity, re-insert an empty line (data loss on complex undo chains).
             * TODO: improve by saving both lines. */
            (void)tail_start;
            (void)cur_total;
            for (int i = ed->line_count; i > e->line; --i)
                ed->lines[i] = ed->lines[i - 1];
            ed->lines[e->line].data[0] = '\0';
            ed->lines[e->line].len = 0;
            ed->line_count++;
        }
        ed->cur_line = e->line;
        ed->cur_col = 0;
        break;
    }
    ed->modified = 1;
}

static void editor_redo(editor_t* ed) {
    if (ed->redo_top <= 0) { set_status(ed, "Nothing to redo"); return; }
    undo_entry_t* e = &ed->redo_stack[--ed->redo_top];

    /* Re-apply the operation */
    switch (e->type) {
    case UNDO_INSERT_CHAR:
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        {
            line_t* ln = &ed->lines[e->line];
            if (ln->len < MAX_LINE_LEN - 1) {
                for (int i = ln->len; i > e->col; --i)
                    ln->data[i] = ln->data[i - 1];
                ln->data[e->col] = e->ch;
                ln->len++;
                ln->data[ln->len] = '\0';
                ed->cur_col = e->col + 1;
            }
        }
        break;
    case UNDO_DELETE_CHAR:
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        {
            line_t* ln = &ed->lines[e->line];
            if (e->col < ln->len) {
                for (int i = e->col; i < ln->len - 1; ++i)
                    ln->data[i] = ln->data[i + 1];
                ln->len--;
                ln->data[ln->len] = '\0';
            }
        }
        break;
    case UNDO_INSERT_LINE:
        ed->cur_line = e->line;
        ed->cur_col = e->col;
        editor_insert_newline(ed);
        /* Remove the duplicate undo entry that insert_newline pushed */
        if (ed->undo_top > 0) ed->undo_top--;
        break;
    case UNDO_DELETE_LINE:
        ed->cur_line = e->line;
        ed->cur_col = 0;
        editor_delete_char_backward(ed);
        if (ed->undo_top > 0) ed->undo_top--;
        break;
    }
    ed->modified = 1;
}


// Cursor movement

static void clamp_cursor(editor_t* ed) {
    if (ed->cur_line < 0) ed->cur_line = 0;
    if (ed->cur_line >= ed->line_count) ed->cur_line = ed->line_count - 1;
    if (ed->cur_col < 0) ed->cur_col = 0;
    if (ed->cur_col > ed->lines[ed->cur_line].len)
        ed->cur_col = ed->lines[ed->cur_line].len;
}

static void ensure_cursor_visible(editor_t* ed) {
    if (ed->visible_lines <= 0) return;
    if (ed->cur_line < ed->scroll_y) ed->scroll_y = ed->cur_line;
    if (ed->cur_line >= ed->scroll_y + ed->visible_lines)
        ed->scroll_y = ed->cur_line - ed->visible_lines + 1;
    if (ed->scroll_y < 0) ed->scroll_y = 0;

    if (ed->visible_cols <= 0) return;
    if (ed->cur_col < ed->scroll_x) ed->scroll_x = ed->cur_col;
    if (ed->cur_col >= ed->scroll_x + ed->visible_cols)
        ed->scroll_x = ed->cur_col - ed->visible_cols + 1;
    if (ed->scroll_x < 0) ed->scroll_x = 0;
}


// Find

static void find_next(editor_t* ed) {
    if (ed->find_len <= 0) return;
    /* Search from cursor position forward */
    int start_line = ed->cur_line;
    int start_col = ed->cur_col + 1;

    for (int pass = 0; pass < 2; ++pass) {
        int from_line = (pass == 0) ? start_line : 0;
        int to_line = (pass == 0) ? ed->line_count : start_line + 1;

        for (int li = from_line; li < to_line; ++li) {
            int from_col = (pass == 0 && li == start_line) ? start_col : 0;
            line_t* ln = &ed->lines[li];
            for (int ci = from_col; ci <= ln->len - ed->find_len; ++ci) {
                int match = 1;
                for (int fi = 0; fi < ed->find_len; ++fi) {
                    if (ln->data[ci + fi] != ed->find_buf[fi]) { match = 0; break; }
                }
                if (match) {
                    ed->cur_line = li;
                    ed->cur_col = ci;
                    ed->find_match_line = li;
                    ed->find_match_col = ci;
                    ensure_cursor_visible(ed);
                    return;
                }
            }
        }
    }
    ed->find_match_line = -1;
    ed->find_match_col = -1;
    set_status(ed, "Not found");
}


// Basic C syntax highlighting

static const char* c_keywords[] = {
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if",
    "inline","int","long","register","return","short","signed","sizeof",
    "static","struct","switch","typedef","union","unsigned","void","volatile",
    "while","uint8","uint16","uint32","int8","int16","int32","string",
    "NULL","TRUE","FALSE","true","false",
    "#include","#define","#ifdef","#ifndef","#endif","#else","#if","#pragma",
    "#undef","#error",
    NULL
};

static int is_keyword_at(const char* text, int pos, int len) {
    for (int k = 0; c_keywords[k]; ++k) {
        int klen = str_len(c_keywords[k]);
        if (pos + klen > len) continue;
        int match = 1;
        for (int j = 0; j < klen; ++j) {
            if (text[pos + j] != c_keywords[k][j]) { match = 0; break; }
        }
        if (!match) continue;
        /* Check word boundary */
        if (pos > 0) {
            char prev = text[pos - 1];
            if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                prev == '_' || (prev >= '0' && prev <= '9'))
                continue;
        }
        if (pos + klen < len) {
            char next = text[pos + klen];
            if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                next == '_' || (next >= '0' && next <= '9'))
                continue;
        }
        return klen;
    }
    return 0;
}

/*
 * Get syntax colour for a character at position 'col' in a line.
 * Returns the colour via r/g/b pointers.
 * Simple state machine: preprocessor, comment, string, keyword, number, default.
 */
typedef struct { int r, g, b; } syn_colour_t;

static syn_colour_t syntax_colour(const char* text, int len, int col) {
    syn_colour_t c = { TXT_R, TXT_G, TXT_B }; /* default */

    /* Preprocessor line */
    if (len > 0 && text[0] == '#') {
        c.r = PP_R; c.g = PP_G; c.b = PP_B;
        return c;
    }

    /* Walk from start to detect strings/comments up to col */
    int in_string = 0;  /* 0=none, 1=double-quote, 2=single-quote */
    int in_line_comment = 0;

    for (int i = 0; i <= col && i < len; ++i) {
        if (in_line_comment) break;

        if (!in_string && i + 1 < len && text[i] == '/' && text[i+1] == '/') {
            in_line_comment = 1;
            break;
        }

        if (!in_string) {
            if (text[i] == '"') { in_string = 1; continue; }
            if (text[i] == '\'') { in_string = 2; continue; }
        } else if (in_string == 1 && text[i] == '"' && (i == 0 || text[i-1] != '\\')) {
            if (i < col) { in_string = 0; continue; }
        } else if (in_string == 2 && text[i] == '\'' && (i == 0 || text[i-1] != '\\')) {
            if (i < col) { in_string = 0; continue; }
        }
    }

    if (in_line_comment) {
        c.r = CMT_R; c.g = CMT_G; c.b = CMT_B;
        return c;
    }

    if (in_string) {
        c.r = STR_R; c.g = STR_G; c.b = STR_B;
        return c;
    }

    /* Number */
    char ch = text[col];
    if (ch >= '0' && ch <= '9') {
        /* Check it's not part of an identifier */
        if (col == 0 || !((text[col-1] >= 'a' && text[col-1] <= 'z') ||
                           (text[col-1] >= 'A' && text[col-1] <= 'Z') ||
                           text[col-1] == '_')) {
            c.r = NUM_R; c.g = NUM_G; c.b = NUM_B;
            return c;
        }
    }

    /* Keyword */
    int klen = is_keyword_at(text, col, len);
    if (klen > 0) {
        c.r = KW_R; c.g = KW_G; c.b = KW_B;
        return c;
    }
    /* Check if we're in the middle of a keyword */
    for (int start = maxi(0, col - 30); start < col; ++start) {
        klen = is_keyword_at(text, start, len);
        if (klen > 0 && start + klen > col) {
            c.r = KW_R; c.g = KW_G; c.b = KW_B;
            return c;
        }
    }

    return c;
}


// Drawing

static void int_to_str(char* buf, int cap, int val) {
    if (val < 0) { buf[0] = '-'; buf[1] = '\0'; return; }
    char tmp[12];
    int n = 0;
    if (val == 0) { tmp[n++] = '0'; }
    else { while (val > 0 && n < 11) { tmp[n++] = (char)('0' + val % 10); val /= 10; } }
    int i = 0;
    for (int j = n - 1; j >= 0 && i < cap - 1; --j) buf[i++] = tmp[j];
    buf[i] = '\0';
}

static int count_digits(int n) {
    if (n < 10) return 1;
    if (n < 100) return 2;
    if (n < 1000) return 3;
    if (n < 10000) return 4;
    return 5;
}

static void draw_editor(editor_t* ed) {
    gui_size_t sz;
    sz.w = 0; sz.h = 0;
    gui_get_content_size(ed->handle, &sz);
    if (sz.w <= 0) sz.w = 480;
    if (sz.h <= 0) sz.h = 360;
    ed->content_w = sz.w;
    ed->content_h = sz.h;

    /* Query font metrics */
    gui_font_metrics_t fm;
    if (gui_get_font_metrics(ed->handle, &fm) < 0 || fm.char_w <= 0 || fm.char_h <= 0) {
        fm.char_w = 8;
        fm.char_h = 8;
    }
    ed->char_w = fm.char_w;
    ed->char_h = fm.char_h;

    /* Compute layout */
    int status_h = fm.char_h + 6;
    int find_bar_h = ed->find_active ? (fm.char_h + 6) : 0;
    int text_area_h = sz.h - status_h - find_bar_h;
    if (text_area_h < fm.char_h) text_area_h = fm.char_h;

    int gutter_digits = count_digits(ed->line_count);
    if (gutter_digits < 3) gutter_digits = 3;
    ed->gutter_w = (gutter_digits + 1) * fm.char_w + 4; /* +1 for padding */

    ed->visible_lines = text_area_h / fm.char_h;
    ed->visible_cols = (sz.w - ed->gutter_w - 12) / fm.char_w; /* 12px for scrollbar */
    if (ed->visible_cols < 1) ed->visible_cols = 1;

    ensure_cursor_visible(ed);

    gui_begin(ed->handle);

    /* Background */
    gui_rgb_t bg = { BG_R, BG_G, BG_B, 0 };
    gui_clear(ed->handle, &bg);

    /* Gutter background */
    gui_rect_t gutter_bg = {
        .x = 0, .y = 0, .w = ed->gutter_w, .h = text_area_h,
        .r = GUT_BG_R, .g = GUT_BG_G, .b = GUT_BG_B, ._pad = 0
    };
    gui_fill_rect(ed->handle, &gutter_bg);

    /* Current line highlight */
    if (ed->cur_line >= ed->scroll_y && ed->cur_line < ed->scroll_y + ed->visible_lines) {
        int cy = (ed->cur_line - ed->scroll_y) * fm.char_h;
        gui_rect_t cur_hl = {
            .x = ed->gutter_w, .y = cy, .w = sz.w - ed->gutter_w, .h = fm.char_h,
            .r = CUR_LINE_R, .g = CUR_LINE_G, .b = CUR_LINE_B, ._pad = 0
        };
        gui_fill_rect(ed->handle, &cur_hl);
    }

    /* Draw lines */
    for (int vi = 0; vi < ed->visible_lines; ++vi) {
        int li = ed->scroll_y + vi;
        if (li >= ed->line_count) break;

        int py = vi * fm.char_h;

        /* Line number */
        char lnum[8];
        int_to_str(lnum, (int)sizeof(lnum), li + 1);
        int lnum_len = str_len(lnum);
        int lnum_x = ed->gutter_w - (lnum_len + 1) * fm.char_w;
        int is_cur = (li == ed->cur_line) ? 1 : 0;
        gui_text_t lnt = {
            .x = lnum_x, .y = py,
            .r = is_cur ? GUT_CUR_R : GUT_FG_R,
            .g = is_cur ? GUT_CUR_G : GUT_FG_G,
            .b = is_cur ? GUT_CUR_B : GUT_FG_B,
            ._pad = 0,
            .text = lnum
        };
        gui_draw_text(ed->handle, &lnt);

        /* Find match highlight */
        if (ed->find_active && ed->find_match_line == li && ed->find_match_col >= 0) {
            int mx = ed->gutter_w + (ed->find_match_col - ed->scroll_x) * fm.char_w;
            int mw = ed->find_len * fm.char_w;
            if (mx < sz.w && mx + mw > ed->gutter_w) {
                gui_rect_t mhl = {
                    .x = mx, .y = py, .w = mw, .h = fm.char_h,
                    .r = MATCH_BG_R, .g = MATCH_BG_G, .b = MATCH_BG_B, ._pad = 0
                };
                gui_fill_rect(ed->handle, &mhl);
            }
        }

        /* Selection highlight */
        {
            int sl1, sc1, sl2, sc2;
            if (get_sel_ordered(ed, &sl1, &sc1, &sl2, &sc2)) {
                if (li >= sl1 && li <= sl2) {
                    int sel_from = (li == sl1) ? sc1 : 0;
                    int sel_to   = (li == sl2) ? sc2 : ed->lines[li].len;
                    /* Extend to end-of-line char width for multi-line visual */
                    if (li < sl2 && sel_to == ed->lines[li].len) sel_to++;
                    int sx = ed->gutter_w + (sel_from - ed->scroll_x) * fm.char_w;
                    int sw = (sel_to - sel_from) * fm.char_w;
                    if (sw > 0 && sx < sz.w && sx + sw > ed->gutter_w) {
                        if (sx < ed->gutter_w) { sw -= (ed->gutter_w - sx); sx = ed->gutter_w; }
                        gui_rect_t selr = {
                            .x = sx, .y = py, .w = sw, .h = fm.char_h,
                            .r = SEL_R, .g = SEL_G, .b = SEL_B, ._pad = 0
                        };
                        gui_fill_rect(ed->handle, &selr);
                    }
                }
            }
        }

        /* Text content -- draw visible portion using gui_draw_text in chunks */
        line_t* ln = &ed->lines[li];
        int draw_from = ed->scroll_x;
        if (draw_from < 0) draw_from = 0;
        if (draw_from >= ln->len) continue;

        int draw_count = mini(ln->len - draw_from, ed->visible_cols + 1);
        if (draw_count <= 0) continue;

        if (ed->is_c_file) {
            /* Syntax-coloured rendering: batch same-colour chars into text runs */
            int chunk_start = draw_from;
            syn_colour_t prev_colour = syntax_colour(ln->data, ln->len, draw_from);

            for (int ci = draw_from + 1; ci <= draw_from + draw_count; ++ci) {
                syn_colour_t cur_colour = { TXT_R, TXT_G, TXT_B };
                if (ci < draw_from + draw_count)
                    cur_colour = syntax_colour(ln->data, ln->len, ci);

                int flush = (ci >= draw_from + draw_count) ||
                            (cur_colour.r != prev_colour.r || cur_colour.g != prev_colour.g || cur_colour.b != prev_colour.b);

                if (flush) {
                    int run_len = ci - chunk_start;
                    if (run_len > 0 && run_len < 63) {
                        char run_buf[64];
                        memcpy(run_buf, ln->data + chunk_start, (size_t)run_len);
                        run_buf[run_len] = '\0';
                        int tx = ed->gutter_w + (chunk_start - ed->scroll_x) * fm.char_w;
                        gui_text_t tt = {
                            .x = tx, .y = py,
                            .r = (unsigned char)prev_colour.r,
                            .g = (unsigned char)prev_colour.g,
                            .b = (unsigned char)prev_colour.b,
                            ._pad = 0,
                            .text = run_buf
                        };
                        gui_draw_text(ed->handle, &tt);
                    } else if (run_len >= 63) {
                        /* Very long run -- split into 63-char chunks */
                        int offset = 0;
                        while (offset < run_len) {
                            int seg = mini(run_len - offset, 63);
                            char seg_buf[64];
                            memcpy(seg_buf, ln->data + chunk_start + offset, (size_t)seg);
                            seg_buf[seg] = '\0';
                            int tx = ed->gutter_w + (chunk_start + offset - ed->scroll_x) * fm.char_w;
                            gui_text_t tt = {
                                .x = tx, .y = py,
                                .r = (unsigned char)prev_colour.r,
                                .g = (unsigned char)prev_colour.g,
                                .b = (unsigned char)prev_colour.b,
                                ._pad = 0,
                                .text = seg_buf
                            };
                            gui_draw_text(ed->handle, &tt);
                            offset += seg;
                        }
                    }
                    chunk_start = ci;
                    prev_colour = cur_colour;
                }
            }
        } else {
            /* Plain text: draw in 63-char chunks */
            int offset = 0;
            while (offset < draw_count) {
                int seg = mini(draw_count - offset, 63);
                char seg_buf[64];
                memcpy(seg_buf, ln->data + draw_from + offset, (size_t)seg);
                seg_buf[seg] = '\0';
                int tx = ed->gutter_w + offset * fm.char_w;
                gui_text_t tt = {
                    .x = tx, .y = py,
                    .r = TXT_R, .g = TXT_G, .b = TXT_B, ._pad = 0,
                    .text = seg_buf
                };
                gui_draw_text(ed->handle, &tt);
                offset += seg;
            }
        }
    }

    /* Cursor (vertical bar) */
    if (ed->cur_line >= ed->scroll_y && ed->cur_line < ed->scroll_y + ed->visible_lines) {
        int cx = ed->gutter_w + (ed->cur_col - ed->scroll_x) * fm.char_w;
        int cy = (ed->cur_line - ed->scroll_y) * fm.char_h;
        gui_rect_t cursor = {
            .x = cx, .y = cy, .w = 2, .h = fm.char_h,
            .r = CUR_R, .g = CUR_G, .b = CUR_B, ._pad = 0
        };
        gui_fill_rect(ed->handle, &cursor);
    }

    /* Scrollbar */
    if (ed->line_count > ed->visible_lines && ed->visible_lines > 0) {
        int sbar_x = sz.w - 6;
        int sbar_total_h = text_area_h;
        int sbar_h = (ed->visible_lines * sbar_total_h) / ed->line_count;
        if (sbar_h < 10) sbar_h = 10;
        int sbar_y = (ed->scroll_y * sbar_total_h) / ed->line_count;
        gui_rect_t sb = {
            .x = sbar_x, .y = sbar_y, .w = 4, .h = sbar_h,
            .r = SBAR_R, .g = SBAR_G, .b = SBAR_B, ._pad = 0
        };
        gui_fill_rect(ed->handle, &sb);
    }

    /* Gutter/text separator line */
    gui_line_t sep = {
        .x1 = ed->gutter_w - 1, .y1 = 0,
        .x2 = ed->gutter_w - 1, .y2 = text_area_h - 1,
        .r = 50, .g = 50, .b = 55, ._pad = 0
    };
    gui_draw_line(ed->handle, &sep);

    /* Find bar */
    if (ed->find_active) {
        int find_y = text_area_h;
        gui_rect_t find_bg_rect = {
            .x = 0, .y = find_y, .w = sz.w, .h = find_bar_h,
            .r = FIND_BG_R, .g = FIND_BG_G, .b = FIND_BG_B, ._pad = 0
        };
        gui_fill_rect(ed->handle, &find_bg_rect);

        char find_label[FIND_BUF_MAX + 16];
        safe_strcpy(find_label, (int)sizeof(find_label), "Find: ");
        int fl = str_len(find_label);
        for (int i = 0; i < ed->find_len && fl < (int)sizeof(find_label) - 2; ++i)
            find_label[fl++] = ed->find_buf[i];
        find_label[fl++] = '_'; /* cursor */
        find_label[fl] = '\0';

        gui_text_t ft = {
            .x = 8, .y = find_y + 3,
            .r = 220, .g = 220, .b = 220, ._pad = 0,
            .text = find_label
        };
        gui_draw_text(ed->handle, &ft);
    }

    /* Status bar */
    int stat_y = sz.h - status_h;
    gui_rect_t stat_bg = {
        .x = 0, .y = stat_y, .w = sz.w, .h = status_h,
        .r = STAT_BG_R, .g = STAT_BG_G, .b = STAT_BG_B, ._pad = 0
    };
    gui_fill_rect(ed->handle, &stat_bg);

    /* Left: file info */
    char stat_left[120];
    {
        int si = 0;
        stat_left[0] = '\0';

        if (ed->status_timer > 0) {
            /* Show transient message */
            for (int i = 0; ed->status_msg[i] && si < (int)sizeof(stat_left) - 1; ++i)
                stat_left[si++] = ed->status_msg[i];
        } else {
            /* Show filename and modified state */
            const char* fn = ed->has_file ? ed->filepath : "Untitled";
            for (int i = 0; fn[i] && si < 60; ++i)
                stat_left[si++] = fn[i];
            if (ed->modified) {
                const char* mod = " [Modified]";
                for (int i = 0; mod[i] && si < (int)sizeof(stat_left) - 1; ++i)
                    stat_left[si++] = mod[i];
            }
        }
        stat_left[si] = '\0';
    }

    gui_text_t stl = {
        .x = 8, .y = stat_y + 3,
        .r = STAT_FG_R, .g = STAT_FG_G, .b = STAT_FG_B, ._pad = 0,
        .text = stat_left
    };
    gui_draw_text(ed->handle, &stl);

    /* Right: line:col and line count */
    char stat_right[40];
    {
        char lbuf[8], cbuf[8], tcbuf[8];
        int_to_str(lbuf, 8, ed->cur_line + 1);
        int_to_str(cbuf, 8, ed->cur_col + 1);
        int_to_str(tcbuf, 8, ed->line_count);
        int si = 0;
        const char* prefix = "Ln ";
        for (int i = 0; prefix[i]; ++i) stat_right[si++] = prefix[i];
        for (int i = 0; lbuf[i]; ++i) stat_right[si++] = lbuf[i];
        stat_right[si++] = ':';
        for (int i = 0; cbuf[i]; ++i) stat_right[si++] = cbuf[i];
        const char* mid = " / ";
        for (int i = 0; mid[i]; ++i) stat_right[si++] = mid[i];
        for (int i = 0; tcbuf[i]; ++i) stat_right[si++] = tcbuf[i];
        const char* suf = " lines";
        for (int i = 0; suf[i] && si < (int)sizeof(stat_right) - 1; ++i)
            stat_right[si++] = suf[i];
        stat_right[si] = '\0';
    }

    int rlen = str_len(stat_right);
    gui_text_t str = {
        .x = sz.w - (rlen + 1) * fm.char_w, .y = stat_y + 3,
        .r = STAT_FG_R, .g = STAT_FG_G, .b = STAT_FG_B, ._pad = 0,
        .text = stat_right
    };
    gui_draw_text(ed->handle, &str);

    gui_present(ed->handle);

    if (ed->status_timer > 0) ed->status_timer--;
}


// Event handling

static void handle_find_key(editor_t* ed, int key) {
    unsigned ch = (unsigned)key & 0xFFu;

    /* Escape exits find mode */
    if (ch == 27u) {
        ed->find_active = 0;
        ed->find_match_line = -1;
        ed->find_match_col = -1;
        return;
    }

    /* Enter / F3 = find next */
    if (ch == '\n' || ch == '\r' || key == GUI_KEY_F3) {
        find_next(ed);
        return;
    }

    /* Backspace */
    if (ch == '\b' || ch == 127u) {
        if (ed->find_len > 0) {
            ed->find_len--;
            ed->find_buf[ed->find_len] = '\0';
            /* Re-search from beginning */
            ed->cur_line = 0;
            ed->cur_col = 0;
            if (ed->find_len > 0) find_next(ed);
        }
        return;
    }

    /* Printable character */
    if (ch >= 32u && ch < 127u && ed->find_len < FIND_BUF_MAX - 1) {
        ed->find_buf[ed->find_len++] = (char)ch;
        ed->find_buf[ed->find_len] = '\0';
        /* Incremental search */
        int saved_line = ed->cur_line;
        int saved_col = ed->cur_col;
        ed->cur_line = 0;
        ed->cur_col = 0;
        find_next(ed);
        if (ed->find_match_line < 0) {
            ed->cur_line = saved_line;
            ed->cur_col = saved_col;
        }
        return;
    }
}

static void handle_key(editor_t* ed, int key) {
    /* Find mode intercepts all keys */
    if (ed->find_active) {
        handle_find_key(ed, key);
        return;
    }

    unsigned ch = (unsigned)key & 0xFFu;

    /* ── Ctrl combos ── */

    /* Ctrl+S -- save */
    if (key == GUI_KEY_CTRL_S) {
        editor_save_file(ed);
        return;
    }

    /* Ctrl+Q -- quit */
    if (key == GUI_KEY_CTRL_Q) {
        if (ed->modified) {
            set_status(ed, "Unsaved changes! Ctrl+Q again to force quit.");
            /* Second Ctrl+Q will quit because status_timer is set */
            if (ed->status_timer > 100) { /* already warned */
                ed->running = 0;
            }
        } else {
            ed->running = 0;
        }
        return;
    }

    /* Ctrl+F -- find */
    if (key == GUI_KEY_CTRL_F) {
        ed->find_active = 1;
        ed->find_len = 0;
        ed->find_buf[0] = '\0';
        ed->find_match_line = -1;
        ed->find_match_col = -1;
        return;
    }

    /* Ctrl+G -- go to line (simple: treat next typed number as line number) */
    if (key == GUI_KEY_CTRL_G) {
        set_status(ed, "Go to line: type line number then Enter");
        /* We'll handle this by temporarily using find mode with a special flag */
        /* For simplicity, just go to middle of file for now */
        /* TODO: implement line number input dialog */
        return;
    }

    /* Ctrl+Z -- undo */
    if (key == GUI_KEY_CTRL_Z) {
        editor_undo(ed);
        return;
    }

    /* Ctrl+Y -- redo */
    if (key == GUI_KEY_CTRL_Y) {
        editor_redo(ed);
        return;
    }

    /* Ctrl+N -- new file */
    if (key == GUI_KEY_CTRL_N) {
        if (ed->modified) {
            set_status(ed, "Unsaved changes! Save first (Ctrl+S) or Ctrl+N again.");
            if (ed->status_timer > 100) {
                /* Already warned -- proceed */
                ed->line_count = 1;
                ed->lines[0].len = 0;
                ed->lines[0].data[0] = '\0';
                ed->cur_line = 0;
                ed->cur_col = 0;
                ed->scroll_y = 0;
                ed->scroll_x = 0;
                ed->modified = 0;
                ed->has_file = 0;
                ed->filepath[0] = '\0';
                ed->undo_top = 0;
                ed->redo_top = 0;
                update_title(ed);
                set_status(ed, "New file.");
            }
        } else {
            ed->line_count = 1;
            ed->lines[0].len = 0;
            ed->lines[0].data[0] = '\0';
            ed->cur_line = 0;
            ed->cur_col = 0;
            ed->scroll_y = 0;
            ed->scroll_x = 0;
            ed->modified = 0;
            ed->has_file = 0;
            ed->filepath[0] = '\0';
            ed->undo_top = 0;
            ed->redo_top = 0;
            update_title(ed);
            set_status(ed, "New file.");
        }
        return;
    }

    /* Ctrl+A -- select all (moves cursor to end) */
    if (key == GUI_KEY_CTRL_A) {
        ed->sel_anchor_line = 0;
        ed->sel_anchor_col = 0;
        ed->cur_line = ed->line_count - 1;
        ed->cur_col = ed->lines[ed->cur_line].len;
        return;
    }

    /* Ctrl+C -- copy selection */
    if (key == GUI_KEY_CTRL_C) {
        if (has_selection(ed)) {
            int n = clipboard_copy(ed);
            if (n > 0) set_status(ed, "Copied.");
        }
        return;
    }

    /* Ctrl+X -- cut selection */
    if (key == GUI_KEY_CTRL_X) {
        if (has_selection(ed)) {
            clipboard_copy(ed);
            delete_selection(ed);
            update_title(ed);
            set_status(ed, "Cut.");
        }
        return;
    }

    /* Ctrl+V -- paste */
    if (key == GUI_KEY_CTRL_V) {
        clipboard_paste(ed);
        return;
    }

    /* F3 -- find next */
    if (key == GUI_KEY_F3) {
        if (ed->find_len > 0) find_next(ed);
        else { ed->find_active = 1; ed->find_len = 0; ed->find_buf[0] = '\0'; }
        return;
    }

    /* ── Navigation ── */

    /* Strip modifier flags, keeping only the key identity bits (0x0FFF) and the 0x1000
     * navigation-range marker.  GUI_KEY_SHIFT is 0x3000, but navigation keys already
     * occupy 0x1xxx, so bit 12 (0x1000) is part of the key identity, NOT a Shift
     * indicator.  The true "Shift held" discriminator is bit 13 (0x2000) only. */
    int base = key & 0x1FFF;
    int shifted = (key & 0x2000) != 0;   /* bit 13: Shift held with nav key */
    int ctrl = (key & GUI_KEY_CTRL) != 0;

    if (base == 0x1001) { /* Up */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        if (ed->cur_line > 0) ed->cur_line--;
        clamp_cursor(ed);
        return;
    }
    if (base == 0x1002) { /* Down */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        if (ed->cur_line < ed->line_count - 1) ed->cur_line++;
        clamp_cursor(ed);
        return;
    }
    if (base == 0x1003) { /* Left */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        if (ctrl) {
            word_left(ed);
        } else {
            if (ed->cur_col > 0) {
                ed->cur_col--;
            } else if (ed->cur_line > 0) {
                ed->cur_line--;
                ed->cur_col = ed->lines[ed->cur_line].len;
            }
        }
        return;
    }
    if (base == 0x1004) { /* Right */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        if (ctrl) {
            word_right(ed);
        } else {
            if (ed->cur_col < ed->lines[ed->cur_line].len) {
                ed->cur_col++;
            } else if (ed->cur_line < ed->line_count - 1) {
                ed->cur_line++;
                ed->cur_col = 0;
            }
        }
        return;
    }

    /* Home / End */
    if (base == 0x1006) { /* Home */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        ed->cur_col = 0;
        return;
    }
    if (base == 0x1007) { /* End */
        if (shifted) ensure_anchor(ed); else clear_selection(ed);
        ed->cur_col = ed->lines[ed->cur_line].len;
        return;
    }

    /* PgUp / PgDn */
    if (key == GUI_KEY_PGUP) {
        clear_selection(ed);
        ed->cur_line -= ed->visible_lines;
        if (ed->cur_line < 0) ed->cur_line = 0;
        clamp_cursor(ed);
        return;
    }
    if (key == GUI_KEY_PGDN) {
        clear_selection(ed);
        ed->cur_line += ed->visible_lines;
        if (ed->cur_line >= ed->line_count) ed->cur_line = ed->line_count - 1;
        clamp_cursor(ed);
        return;
    }

    /* Delete */
    if (key == GUI_KEY_DELETE) {
        editor_delete_char_forward(ed);
        clear_selection(ed);
        update_title(ed);
        return;
    }

    /* ── Editing ── */

    /* Escape */
    if (ch == 27u) {
        ed->sel_anchor_line = -1;
        ed->sel_anchor_col = -1;
        return;
    }

    /* Backspace */
    if (ch == '\b' || ch == 127u) {
        editor_delete_char_backward(ed);
        update_title(ed);
        return;
    }

    /* Enter: insert newline then auto-indent to match previous line */
    if (ch == '\n' || ch == '\r') {
        /* Capture leading indent of current line before inserting newline */
        line_t* prev_ln = &ed->lines[ed->cur_line];
        int indent_depth = 0;
        for (int _i = 0; _i < prev_ln->len && prev_ln->data[_i] == ' '; ++_i)
            indent_depth++;
        /* If the line (up to cursor) ends with '{', add one extra TAB_WIDTH */
        int extra_indent = 0;
        if (ed->is_c_file && ed->cur_col > 0 && prev_ln->data[ed->cur_col - 1] == '{')
            extra_indent = TAB_WIDTH;
        editor_insert_newline(ed);
        /* Insert indentation on the new line */
        for (int _i = 0; _i < indent_depth + extra_indent; ++_i)
            editor_insert_char(ed, ' ');
        update_title(ed);
        return;
    }

    /* Tab */
    if (ch == '\t') {
        int spaces = TAB_WIDTH - (ed->cur_col % TAB_WIDTH);
        for (int i = 0; i < spaces; ++i)
            editor_insert_char(ed, ' ');
        update_title(ed);
        return;
    }

    /* Printable characters */
    if (ch >= 32u && ch < 127u) {
        editor_insert_char(ed, (char)ch);
        update_title(ed);
        return;
    }
}

static void handle_mouse(editor_t* ed, gui_event_t* ev) {
    /* Mouse wheel -- scroll */
    if (ev->d > 0) {
        ed->scroll_y -= 3;
        if (ed->scroll_y < 0) ed->scroll_y = 0;
    } else if (ev->d < 0) {
        ed->scroll_y += 3;
        int max_scroll = ed->line_count - ed->visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (ed->scroll_y > max_scroll) ed->scroll_y = max_scroll;
    }

    /* Click to position cursor; drag to extend selection */
    int left_down = (ev->c & 0x1) != 0;
    int press_edge = left_down && !ed->prev_left_down;
    ed->prev_left_down = left_down;

    if (left_down && ed->char_w > 0 && ed->char_h > 0) {
        int mx = ev->a;
        int my = ev->b;

        if (mx > ed->gutter_w) {
            int col = (mx - ed->gutter_w) / ed->char_w + ed->scroll_x;
            int line = my / ed->char_h + ed->scroll_y;
            if (line < 0) line = 0;
            if (line >= ed->line_count) line = ed->line_count - 1;

            if (press_edge) {
                /* New click: move cursor and clear selection */
                ed->cur_line = line;
                ed->cur_col = col;
                clamp_cursor(ed);
                clear_selection(ed);
            } else {
                /*
                 * Drag: plant anchor at previous cursor position on first drag
                 * tick, then update cursor to follow mouse.
                 */
                ensure_anchor(ed);
                ed->cur_line = line;
                ed->cur_col = col;
                clamp_cursor(ed);
            }
        }
    }
}

static void handle_close(editor_t* ed) {
    if (ed->modified) {
        set_status(ed, "Unsaved changes! Ctrl+S to save, Ctrl+Q to quit.");
    } else {
        ed->running = 0;
    }
}


// Main

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: edit [filepath]");
        puts("Ctrl+S save | Ctrl+Q quit | Ctrl+F find | Ctrl+Z undo | Ctrl+Y redo");
        puts("Ctrl+A select all | Ctrl+C copy | Ctrl+X cut | Ctrl+V paste");
        puts("Shift+Arrows select | Ctrl+Arrows word jump | Home/End | PgUp/PgDn");
        return 0;
    }

    editor_t ed;
    memset(&ed, 0, sizeof(ed));
    ed.running = 1;
    ed.sel_anchor_line = -1;
    ed.sel_anchor_col = -1;
    ed.find_match_line = -1;
    ed.find_match_col = -1;

    editor_init_buffer(&ed);

    if (argc >= 2 && argv[1] && argv[1][0]) {
        if (editor_load_file(&ed, argv[1]) < 0) {
            /* File doesn't exist -- create new with that name */
            safe_strcpy(ed.filepath, MAX_PATH_LEN, argv[1]);
            ed.has_file = 1;
            ed.is_c_file = is_c_extension(argv[1]);
            set_status(&ed, "New file.");
        }
    }

    ed.handle = gui_attach(ed.has_file ? ed.filepath : "Untitled - Edit",
                            "Ctrl+S save | Ctrl+C/X/V copy/cut/paste | Ctrl+F find");
    if (ed.handle < 0) {
        puts("edit: gui_attach failed");
        free(ed.lines);
        return 1;
    }

    update_title(&ed);
    int need_redraw = 1;

    while (ed.running) {
        gui_event_t ev;
        while (gui_poll_event(ed.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                handle_key(&ed, ev.a);
                if (ed.running) need_redraw = 1;
            } else if (ev.type == GUI_EVENT_MOUSE) {
                int old_scroll_y = ed.scroll_y;
                int old_cur_line = ed.cur_line;
                int old_cur_col = ed.cur_col;
                int old_anchor_line = ed.sel_anchor_line;
                int old_anchor_col = ed.sel_anchor_col;
                handle_mouse(&ed, &ev);

                if (ed.scroll_y != old_scroll_y ||
                    ed.cur_line != old_cur_line ||
                    ed.cur_col != old_cur_col ||
                    ed.sel_anchor_line != old_anchor_line ||
                    ed.sel_anchor_col != old_anchor_col) {
                    need_redraw = 1;
                }
            } else if (ev.type == GUI_EVENT_CLOSE) {
                int old_running = ed.running;
                int old_status_timer = ed.status_timer;
                char old_status_msg[80];
                safe_strcpy(old_status_msg, (int)sizeof(old_status_msg), ed.status_msg);
                handle_close(&ed);
                if ((old_running && !ed.running) ||
                    ed.status_timer != old_status_timer ||
                    strcmp(old_status_msg, ed.status_msg) != 0) {
                    need_redraw = 1;
                }
            }
        }

        if (need_redraw || ed.status_timer > 0) {
            draw_editor(&ed);
            need_redraw = 0;
            if (ed.status_timer > 0) {
                usleep(33000);
            }
        } else {
            usleep(8000);
        }
    }

    free(ed.lines);
    return 0;
}
