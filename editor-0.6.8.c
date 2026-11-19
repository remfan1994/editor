/*
 * editor 0.6.8 - a small, single-file terminal text editor.
 * Build: cc -std=c99 -O2 -Wall -Wextra -pedantic editor.c -o editor
 * Run:   ./editor [FILE]
 *
 * Arrow keys, Home, End, Page Up/Down, Backspace, Delete, Enter and Tab
 * edit the text. Esc opens Command>; Enter executes; a second Esc on
 * an empty command prompt asks for confirmation before quitting.
 * Commands: write [FILE], find [TEXT], next/n, prev/p, replace, replaceall,
 *           undo [N], redo [N], reverse, history,
 *           menu, cli, about, manual,
 *           help, list, quit, quit!. Startup --menu uses an arrow-key menu for Esc.
 * About/manual are read-only pages; q or Esc returns to editing.
 *
 * Uses libc and Unix terminal interfaces, without curses or terminfo.
 * One file, horizontal scrolling, up to 8 MiB. Optional user key bindings.
 * Existing bytes and line endings are preserved; Enter uses the first
 * newline style in the file. New files use LF. NUL bytes are refused.
 * UTF-8 movement is by code point. Zero-width/nonprinting characters
 * are shown as escapes; complex Unicode shaping is outside this scope.
 * Undo groups consecutive typing; a paste is one change.
 *
 * Writes use a private temporary file beside the destination, then an
 * atomic replacement. Existing permissions and ownership are retained;
 * set-ID bits are cleared. Outside writes require explicit confirmation
 * before replacement. Multiple hard links remain refused. Symlinks resolve
 * to their targets when opening a file.
 * "write FILE" prompts before overwriting an existing other file.
 * Undo history is saved privately per file after successful writes,
 * and restored on reopening when the saved document matches exactly.
 */
/* OpenBSD hides SIGWINCH under X/Open mode unless BSD interfaces are enabled. */
#if defined(__OpenBSD__) && !defined(_BSD_SOURCE)
#define _BSD_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define LIMIT ((size_t)8 * 1024 * 1024)
#define COMMAND_SIZE 4096
#define VERSION "0.6.8"
#define COMMAND_HISTORY_MAX 32
#define JOURNAL_LIMIT ((size_t)128 * 1024 * 1024)
#define JOURNAL_CHANGE_LIMIT 100000
#define MAX_BINDINGS 64
#define KEY_ALT_BASE 1000
#define KEY_FUNCTION_BASE 2000
#define KEY_CTRL_ARROW_BASE 3000
#define KEY_ALT_ARROW_BASE 4000

static const char about_text[] =
    "editor " VERSION "\n"
    "A small, single-file terminal text editor.\n\n"
    "design by remfan1994\n"
    "published by ThatIAm.vip\n\n"
    "One file, menu or commands, undo/redo.\n"
    "Uses the system C library and Unix terminal interfaces.\n"
    "No curses, terminfo, readline, or pager.\n"
    "For VT/xterm-compatible terminals; file limit: 8 MiB.\n\n"
    "BLOODGUILT CURSE\n"
    "Cows aren't killers, and beef is the bloodshed of the innocent.  "
    "www.bloodguiltcurse.net has the Bible study\n\n"
    "Use the manual command or editor --manual for instructions.";

/* A plain exit notice, no timed delay, key prompt, network access or heap
 * allocation. Restored terminal wrapping makes the full text available. */
static const char exit_notice[] =
    "\nBLOODGUILT CURSE\n"
    "Cows aren't killers, and beef is the bloodshed of the innocent.  "
    "www.bloodguiltcurse.net has the Bible study\n"
    "editor " VERSION " | design by remfan1994 | published by ThatIAm.vip\n";

static const char manual_text[] =
    "EDITOR MANUAL\n"
    "editor 0.6.8\n"
    "\n"
    "RUN: editor [--menu] [--mouse] [--movement|--edit] [--no-keys] [FILE|-]\n"
    "\n"
    "EDIT MODE (default): Type text. Arrows, Home/End and PgUp/PgDn move.\n"
    "  Backspace/Delete erase. Enter inserts newline; Tab inserts tab.\n"
    "  Esc opens Command>; a second Esc asks Really quit? (Y/n).\n"
    "  movement enters navigation-only control; edit or Enter resumes typing.\n"
    "  MOVEMENT: arrows/hjkl, Ctrl-P/N/B/F, p/n/b/f, 0/$ or a/e,\n"
    "    g/G document ends, y/v pages, w/@ next word, Home/End, PgUp/PgDn.\n"
    "    Movement never edits; Esc opens controls.\n"
    "\n"
    "MENU: arrows/jk move; Enter/l selects; Esc/h goes back.\n"
    "  Number+Enter selects. Mouse is optional.\n"
    "\n"
    "COMMANDS: list explains commands by task; list WORD filters them.\n"
    "  help starts here; help NAME gives examples and synonyms.\n"
    "  Esc/q returns to Command>; Enter on a blank prompt resumes editing.\n"
    "  Unknown commands retain your draft; suggestions never run.\n"
    "  write [FILE]       Save; write FILE changes active filename on success.\n"
    "  read [FILE]        Open file (save edits before switching).\n"
    "  ls [DIRECTORY]     Browse folders; Tab completes paths after commands.\n"
    "  copy/cut [LINE|A-B] Copy or cut lines into the selected clipboard.\n"
    "  paste [SLOT]       Paste current clipboard, or named slot (e.g. paste 2).\n"
    "  clipboard          Edit selected clipboard (aliases: clip, cb, cboard).\n"
    "  clipboard 2        Select persistent clipboard slot 2 (copy uses it).\n"
    "  cliphistory [on|off]  List or toggle optional saved clipboards.\n"
    "  write -            Send text to redirected stdout.\n"
    "  dline [N]          Delete current or numbered line.\n"
    "  deleol              Delete cursor through end of line (keep newline).\n"
    "  deleof              Delete cursor through end of document.\n"
    "  backbof             Delete start of document through cursor.\n"
    "  dline A-B          Delete inclusive range.\n"
    "  N dline            Delete N lines starting at cursor line.\n"
    "  undo [N], redo [N] Move through undo history, default 1.\n"
    "  reverse [INDEX]    Browse undo history; Esc returns to editing.\n"
    "    Left/h/</- undo; Right/l/>/+ redo (one step per repeat).\n"
    "  find TEXT          Literal search from cursor, wrapping at end.\n"
    "    Bare find prompts; spaces in Find> are preserved exactly.\n"
    "  next/n, prev/p    Next or previous match of last search.\n"
    "  replace, replaceall  Literal replacement; prompts or OLD => NEW.\n"
    "  goto [LINE [COLUMN]] 1-based navigation; top/bottom to endpoints.\n"
    "  up/down/left/right [N]  Move by N steps (default one).\n"
    "  movement / edit    Navigate only / resume ordinary typing.\n"
    "  Config: ~/.config/editor/config or $XDG_CONFIG_HOME/editor/config.\n"
    "    startup=movement or startup=edit (default); --movement/--edit overrides.\n"
    "  menu / cli         Change what Esc opens.\n"
    "  mouse [on|off]     Optional mouse handling.\n"
    "  signals [on|off|toggle]  Optional signal handling (default OFF).\n"
    "  bind KEY COMMAND   User-assigned hotkey; none by default.\n"
    "  quit / quit!       Normal exit / discard changes.\n"
    "\n"
    "SIGNALS: Ctrl-C/Z remain terminal controls (including remapping).\n"
    "  Interrupt exits, LOSING UNSAVED TEXT; stop suspends (fg resumes).\n"
    "  Config signal_controls=on makes interrupt cancel instead (default off).\n"
    "  Ordinary quit prints a notice; quick/discard/signal exits skip it.\n"
    "\n"
    "COMMAND INPUT: Tab completes a command name or alias.\n"
    "  Up/Down recalls 32 unique recent commands.\n"
    "  Tab completes paths after read/write/ls or in Filename>.\n"
    "\n"
    "PIPES: grep pattern file | editor\n"
    "  Use write - to send raw bytes to next pipeline stage.\n"
    "\n"
    "SAVING: Staged replacement; multiple hard links refused.\n"
    "  Persistent undo is private; unsaved edits are not recoverable.\n"
    "  Clipboard editor keeps original file and undo chain in memory.\n"
    "  write saves the clipboard and stays; clipboard saves and returns.\n"

    "  Clips: $XDG_STATE_HOME/editor/ or ~/.local/state/editor/.\n"
    "  clipboard.txt / clipboard-N.txt; private plaintext.\n"
    "  History defaults off (64 entries). Viewer: Esc/q closes; arrows/hjkl move.\n"
    "\n"
    "ABOUT\n"
    "  design by remfan1994\n"
    "  published by ThatIAm.vip\n"
    "  BLOODGUILT CURSE\n"
    "  Cows aren't killers, and beef is the bloodshed of the innocent.\n"
    "  www.bloodguiltcurse.net has the Bible study\n";
typedef struct { char *s; size_t n, cap; } Buffer;
typedef struct Change Change;
struct Change {
    size_t at, before, after, old_n, new_n, new_cap;
    size_t ordinal; /* 1-based history position; constant-time undo depth. */
    char *old_s, *new_s;
    Change *prev, *next;
};
static Buffer text, saved;
static size_t cursor, top, left, goal;
/* A bounded, literal search pattern.  No regex or additional libraries.
 * search_mark is a display-only match; edits invalidate the mark. */
static char search_pattern[COMMAND_SIZE];
static size_t search_length;
static size_t search_mark = SIZE_MAX;
static char replace_search[COMMAND_SIZE];
static bool replace_everywhere;
static bool goal_set, group, command_mode, filename_mode, menu_preferred, menu_open, running = true;
/* Navigation is modal, not a set of global hotkeys: ordinary edit mode
 * keeps printable characters available for literal text entry. */
static bool movement_mode;
static bool startup_config_warning;
/* OFF leaves interrupt/stop to terminal job control. ON opts into the
 * editor's cancel-on-interrupt behavior, never a keyboard binding. */
static volatile sig_atomic_t signal_controls;
static bool ordinary_quit;
static size_t menu_selection, menu_first;
/* A compact, two-level menu exposes the full command set on small screens.
 * Action arguments use the existing line-input engine, not extra hotkeys. */
static int menu_page;
static bool menu_argument_mode;
static const char *menu_argument_command, *menu_argument_label;
static bool menu_discard_pending;
static char command[COMMAND_SIZE], message[256];
static size_t command_n, command_cursor;
static char *command_history[COMMAND_HISTORY_MAX];
static size_t command_history_n;
static int history_choice = -1;
static char command_draft[COMMAND_SIZE];
static char *loaded_journal_path;
/* Track the exact journal read/written by this document, not just its path.
 * A different editor can save history without changing the document bytes. */
static struct stat loaded_journal_stat;
static bool loaded_journal_stat_valid;
static bool journal_ignored;
static bool journal_dirty;
static bool journal_rebased, journal_branched;
static char *filename;
static bool existed;
static struct stat original;
static char newline[2] = {'\n', 0};
static size_t newline_n = 1;
static Change *history, *current, *history_tail;
/* Only active inside the dedicated, non-editing undo browser. */
static bool reverse_mode;
enum { PROMPT_NONE, PROMPT_QUIT, PROMPT_OVERWRITE,
       PROMPT_MENU_WRITE, PROMPT_TARGET_OVERWRITE };
static int prompt;
static Buffer conflict_snapshot;
static struct stat conflict_stat;
/* Used only while confirming overwrite of another named destination. */
static char *pending_write_path;
static size_t reverse_position, reverse_total;
/* Editing clipboard.txt is a temporary one-level document context.  Save the
 * original document by moving ownership of its buffers, history and journal
 * into this structure.  No copy of an 8 MiB file is needed, and dirty edits
 * are preserved until returning from the clipboard. */
typedef struct {
    Buffer text, saved;
    size_t cursor, top, left, goal;
    char search_pattern[COMMAND_SIZE];
    size_t search_length, search_mark;
    bool goal_set, group, existed;
    char *filename, *loaded_journal_path;
    struct stat original, loaded_journal_stat;
    bool loaded_journal_stat_valid;
    char newline[2];
    size_t newline_n;
    Change *history, *current, *history_tail;
    bool journal_ignored, journal_dirty, journal_rebased, journal_branched;
} DocumentContext;
static DocumentContext parent_document;
static bool editing_clipboard;
/* Empty string means original clipboard.txt. Named slots are strict ASCII
 * identifiers; they never become path separators or shell expressions. */
#define CLIP_SLOT_MAX 32
static char selected_clipboard[CLIP_SLOT_MAX+1];
/* A clipboard toggle may request save-and-return. A plain write still only
 * saves; failed clipboard saves must keep the sub-editor and its buffer. */
static bool clipboard_leave_after_save;

static struct termios initial_termios;
static bool terminal_active;
static int rows = 24, cols = 80;
static char **previous_rows;
static int previous_count;
static bool drawing_failed;
static volatile sig_atomic_t resized, interrupted, suspended, terminated;
static unsigned char input[4096];
static size_t input_at, input_n;
static int discard_sequence, discard_last;
static int64_t discard_until;
/* After a signal cancels a bracketed paste, its remaining bytes are still
 * paste data, never commands. Resume draining after interrupt/suspend. */
static bool paste_discard;
static size_t paste_end_matched;
static const char *viewer_text, *viewer_title;
static size_t viewer_top;

/* Terminal UI is always separate from stdin/stdout data streams. */
static int tty_in = STDIN_FILENO, tty_out = STDOUT_FILENO, tty_extra = -1;
static bool stream_input, mouse_enabled, keys_disabled, clip_history_enabled, filename_read_mode;
static int mouse_x, mouse_y, mouse_button;
static bool mouse_pressed;
/* Help, command lists and completion share one bounded, reusable page.
 * They never own or copy the document and never execute the examples. */
static char command_page[16384];
static char directory_page[131072], clipboard_page[32768];
#define CLIP_HISTORY_MAX 64
static const char help_topics[] =
    "HELP - start here\n"
    "\n"
    "Type a command at Command>, then press Enter.\n"
    "For example, write saves; find asks what to search for.\n"
    "Tab finishes a name. Up/Down recalls your commands.\n"
    "Esc leaves a nonempty command without running it.\n"
    "\n"
    "list\n"
    "  Commands explained, grouped by the task you want to do.\n"
    "list save\n"
    "  Find commands by a word, topic, or synonym. Try list clipboard.\n"
    "help copy\n"
    "  How to use one command, with examples and synonyms.\n"
    "manual\n"
    "  Complete instructions, settings and limitations.\n"
    "about\n"
    "  Version, credits and the Bloodguilt Curse notice.\n"
    "\n"
    "In examples: FILE means your filename; [ ] means optional.\n"
    "Do not type the brackets. Example: write notes.txt\n"
    "\n"
    "j/k or Up/Down scroll; h/l or PgUp/PgDn turn pages.\n"
    "Esc/q returns to Command>. Enter on an empty prompt resumes editing.\n"
    "Esc at an empty prompt still asks Really quit?\n"
    "Reading help does not run commands or change your document.";
static int menu_number = -1;
typedef struct { int code; char name[24], action[256]; } Binding;
static Binding binds[MAX_BINDINGS];
static size_t bind_count;
static char binds_page[16384];

static bool reserve(Buffer *b, size_t n)
{
    size_t cap;
    char *s;
    /* A capacity is usable only while its allocation is still owned. Reject
     * an inconsistent descriptor rather than returning a false success. */
    if (b->cap && !b->s) { errno = EINVAL; return false; }
    if (n <= b->cap) return true;
    cap = b->cap ? b->cap : 128;
    while (cap < n) {
        if (cap > SIZE_MAX / 2) { cap = n; break; }
        cap *= 2;
    }
    s = realloc(b->s, cap);
    if (!s) return false;
    b->s = s; b->cap = cap;
    return true;
}

static bool copy_buffer(Buffer *b, const char *s, size_t n)
{
    if (n == SIZE_MAX || (n && !s)) { errno = EINVAL; return false; }
    if (!reserve(b, n + 1)) return false;
    if (n) memcpy(b->s, s, n);
    b->s[n] = 0; b->n = n;
    return true;
}

static void tell(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof message, format, ap);
    va_end(ap);
}

static bool changed(void)
{
    return text.n != saved.n || (text.n && memcmp(text.s, saved.s, text.n));
}

/* An undrained output pipe must not defeat termination/suspension. All
 * callers retain their editing buffers; staged saves are discarded on error. */
static int write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t k;
        if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
        k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { if (!k) errno = EIO; return -1; }
        s += k; n -= (size_t)k;
    }
    return 0;
}

static void terminal_restore(void)
{
    static const char end[] = "\033[?1000l\033[?1006l\033[?2004l\033[0m\033[?25h\033[?1049l";
    int flags;
    if (!terminal_active) return;
    /* Best-effort, nonblocking screen cleanup even when a signal cancelled
     * normal writes. Always restore termios, regardless of output errors. */
    flags = fcntl(tty_out, F_GETFL);
    if (flags >= 0) (void)fcntl(tty_out, F_SETFL, flags | O_NONBLOCK);
    (void)write(tty_out, end, sizeof end - 1);
    if (flags >= 0) (void)fcntl(tty_out, F_SETFL, flags);
    /* A second signal must not leave the shell with our raw input flags. */
    while (tcsetattr(tty_in, TCSANOW, &initial_termios) < 0 && errno == EINTR) {}
    terminal_active = false;
}

static void die(const char *s)
{
    terminal_restore();
    fprintf(stderr, "editor: %s\n", s);
    exit(1);
}

static void add(Buffer *b, const char *s, size_t n)
{
    if (drawing_failed) return;
    if (b->n == SIZE_MAX || n > SIZE_MAX - b->n - 1 ||
        !reserve(b, b->n + n + 1)) {
        drawing_failed = true;
        return;
    }
    if (n) memcpy(b->s + b->n, s, n);
    b->n += n; b->s[b->n] = 0;
}

static void output(Buffer *b, const char *s) { add(b, s, strlen(s)); }

/* Decode one UTF-8 code point. Invalid bytes remain individual units. */
static size_t decode(const char *s, size_t n, size_t p, uint32_t *cp)
{
    unsigned char c;
    uint32_t u, minimum;
    size_t k, count;
    if (p >= n) { *cp = 0; return 0; }
    c = (unsigned char)s[p];
    if (c < 128) { *cp = c; return 1; }
    if (c >= 0xc2 && c <= 0xdf) { count = 2; u = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { count = 3; u = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 4; u = c & 7; minimum = 0x10000; }
    else { *cp = 0xffffffffu; return 1; }
    if (count > n - p) { *cp = 0xffffffffu; return 1; }
    for (k = 1; k < count; k++) {
        c = (unsigned char)s[p+k];
        if ((c & 0xc0) != 0x80) { *cp = 0xffffffffu; return 1; }
        u = (u << 6) | (c & 63);
    }
    if (u < minimum || u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff)) {
        *cp = 0xffffffffu; return 1;
    }
    *cp = u; return count;
}

static size_t next_pos(const char *s, size_t n, size_t p)
{
    uint32_t cp;
    if (p >= n) return n;
    if (s[p] == '\r' && p + 1 < n && s[p+1] == '\n') return p + 2;
    return p + decode(s, n, p, &cp);
}

static size_t prev_pos(const char *s, size_t n, size_t p)
{
    size_t q, limit;
    uint32_t cp;
    if (!p) return 0;
    if (p >= 2 && s[p-2] == '\r' && s[p-1] == '\n') return p - 2;
    q = p - 1; limit = p > 4 ? p - 4 : 0;
    while (q > limit && ((unsigned char)s[q] & 0xc0) == 0x80) q--;
    if (q + decode(s, n, q, &cp) == p && cp != 0xffffffffu) return q;
    return p - 1;
}

static size_t line_start(size_t p)
{
    while (p && text.s[p-1] != '\n') p--;
    return p;
}

static size_t line_end(size_t p)
{
    while (p < text.n && text.s[p] != '\n') p++;
    if (p < text.n && p && text.s[p-1] == '\r') p--;
    return p;
}

static size_t next_line(size_t p)
{
    while (p < text.n && text.s[p] != '\n') p++;
    return p < text.n ? p + 1 : text.n;
}

/* Convert a unit to printable bytes and its terminal-cell width. */
static size_t representation(const char *s, size_t n, size_t p, size_t column,
                             char out[32], int *width, bool *wide)
{
    uint32_t cp;
    size_t count = decode(s, n, p, &cp);
    int w;
    *wide = false;
    if (cp == '\t') {
        *width = 8 - (int)(column % 8);
        memset(out, ' ', (size_t)*width); out[*width] = 0;
    } else if (cp < 32 || cp == 127) {
        out[0] = '^'; out[1] = cp == 127 ? '?' : (char)(cp + 64);
        out[2] = 0; *width = 2;
    } else if (cp == 0xffffffffu) {
        snprintf(out, 32, "\\x%02X", (unsigned char)s[p]); *width = 4;
    } else if (cp < 128) {
        out[0] = (char)cp; out[1] = 0; *width = 1;
    } else if ((w = wcwidth((wchar_t)cp)) > 0) {
        memcpy(out, s + p, count); out[count] = 0;
        *width = w; *wide = true;
    } else {
        snprintf(out, 32, "\\u{%lX}", (unsigned long)cp);
        *width = (int)strlen(out);
    }
    return count;
}

static size_t column_at(size_t start, size_t p)
{
    size_t column = 0;
    char rep[32]; int width; bool wide;
    while (start < p) {
        start += representation(text.s, p, start, column, rep, &width, &wide);
        column += (size_t)width;
    }
    return column;
}

static size_t position_at(size_t start, size_t column)
{
    size_t end = line_end(start), x = 0, count;
    char rep[32]; int width; bool wide;
    while (start < end) {
        count = representation(text.s, end, start, x, rep, &width, &wide);
        if (x + (size_t)width > column) break;
        x += (size_t)width; start += count;
    }
    return start;
}

/* Search only the candidate start positions in [first, limit). A forward
 * search stops at its first match; a backward search retains the last one.
 * KMP still handles repetitive/overlapping text in linear time. Restricting
 * the range avoids rescanning the whole prefix on every "next" command. */
static bool search_range(size_t first, size_t limit, bool forward,
                         const size_t *failure, size_t *match)
{
    size_t i, j=0, end;
    *match=SIZE_MAX;
    if (first>=limit) return true;
    end=limit+search_length-1; /* limit <= text.n-search_length+1 */
    for (i=first; i<end; i++) {
        if (!(i & 8191u) && (terminated || suspended || interrupted)) return false;
        while (j && text.s[i]!=search_pattern[j]) j=failure[j-1];
        if (text.s[i]==search_pattern[j]) j++;
        if (j==search_length) {
            *match=i+1-search_length;
            if (forward) return true;
            j=failure[j-1];
        }
    }
    return true;
}
static bool find_match(bool forward, bool include_current)
{
    size_t failure[COMMAND_SIZE], i, j, selected=SIZE_MAX;
    size_t boundary, limit, line_no, from;
    bool wrapped=false, ok;
    if (terminated || suspended || interrupted) goto cancelled;
    if (!search_length) { tell("No search text. Use find TEXT."); return false; }
    if (search_length>=COMMAND_SIZE) {
        tell("Invalid search length; nothing moved."); return false;
    }
    if (search_length > text.n) {
        search_mark=SIZE_MAX;
        tell("No match for the current search."); return false;
    }
    failure[0]=0;
    for (i=1,j=0; i<search_length; i++) {
        while (j && search_pattern[i]!=search_pattern[j]) j=failure[j-1];
        if (search_pattern[i]==search_pattern[j]) j++;
        failure[i]=j;
    }
    limit=text.n-search_length+1;
    boundary=cursor<text.n ? cursor : text.n;
    if (forward && !include_current) boundary++;
    if (boundary>limit) boundary=limit;
    ok=forward ? search_range(boundary,limit,true,failure,&selected) :
                 search_range(0,boundary,false,failure,&selected);
    if (!ok) goto cancelled;
    if (selected==SIZE_MAX) {
        wrapped=true;
        ok=forward ? search_range(0,boundary,true,failure,&selected) :
                     search_range(boundary,limit,false,failure,&selected);
        if (!ok) goto cancelled;
    }
    if (terminated || suspended || interrupted) goto cancelled;
    if (selected==SIZE_MAX) {
        search_mark=SIZE_MAX;
        tell("No match for the current search."); return false;
    }
    /* Compute the message before committing cursor/mark, so cancellation
     * also leaves navigation unchanged while counting a long prefix. */
    for (i=0,line_no=1; i<selected; i++) {
        if (!(i & 8191u) && (terminated || suspended || interrupted)) goto cancelled;
        if (text.s[i]=='\n') line_no++;
    }
    from=line_start(selected);
    i=column_at(from,selected)+1;
    if (terminated || suspended || interrupted) goto cancelled;
    cursor=selected; search_mark=selected; goal_set=false; group=false;
    tell("Match at %zu:%zu%s.",line_no,i,wrapped ? " (wrapped)" : "");
    return true;
cancelled:
    tell("Search interrupted; cursor unchanged.");
    return false;
}

/* The Find> prompt is text, not another command to parse. In particular,
 * leading/trailing spaces and a whitespace-only search are meaningful. */
static void start_search(const char *pattern)
{
    size_t length = strlen(pattern);
    if (!length || length >= sizeof search_pattern) {
        tell("Search text must be 1-%d bytes.", COMMAND_SIZE - 1); return;
    }
    memcpy(search_pattern, pattern, length + 1);
    search_length = length; search_mark = SIZE_MAX;
    (void)find_match(true, true);
}

/* Replacement uses the same edit transactions as normal typing.  In the all
 * case, build one bounded result and commit it in a single undo step. */
static bool edit(size_t at, size_t old_n, const char *s, size_t n, bool typing);
static bool replace_append(Buffer *b, const char *data, size_t n)
{
    if (n > LIMIT - b->n || !reserve(b,b->n+n+1)) return false;
    if (n) memcpy(b->s+b->n,data,n);
    b->n+=n; b->s[b->n]=0;
    return true;
}
static void run_replacement(const char *needle, const char *value, bool all)
{
    size_t n=strlen(needle), m=strlen(value), i, j, last=0, changes=0;
    size_t *failure;
    Buffer result={0};
    if (terminated || suspended || interrupted) {
        tell("Replacement interrupted; text unchanged."); return;
    }
    if (!n || n>=COMMAND_SIZE) { tell("Search text must be 1-%d bytes.",COMMAND_SIZE-1); return; }
    if (m>=COMMAND_SIZE) { tell("Replacement text too long."); return; }
    if (!all) {
        memcpy(search_pattern,needle,n+1); search_length=n;
        if (!find_match(true,true)) return;
        if (n==m && !memcmp(text.s+cursor,value,n)) {
            tell("Match already has that text; nothing changed."); return;
        }
        if (terminated || suspended || interrupted) {
            tell("Replacement interrupted; text unchanged."); return;
        }
        if (edit(cursor,n,value,m,false)) tell("Replaced one match.");
        return;
    }
    if (n>text.n) { tell("No matches to replace."); return; }
    failure=malloc(n*sizeof *failure);
    if (!failure) { tell("Not enough memory; text unchanged."); return; }
    failure[0]=0;
    for (i=1,j=0;i<n;i++) {
        while (j && needle[i]!=needle[j]) j=failure[j-1];
        if (needle[i]==needle[j]) j++;
        failure[i]=j;
    }
    for (i=0,j=0;i<text.n;i++) {
        if (!(i & 8191u) && (terminated || suspended || interrupted)) goto cancelled;
        while (j && text.s[i]!=needle[j]) j=failure[j-1];
        if (text.s[i]==needle[j]) j++;
        if (j==n) {
            size_t at=i+1-n;
            if (!replace_append(&result,text.s+last,at-last) ||
                !replace_append(&result,value,m)) goto too_large;
            last=i+1; changes++;
            j=0; /* all replacements are non-overlapping */
        }
    }
    if (!changes) { tell("No matches to replace."); goto done; }
    if (!replace_append(&result,text.s+last,text.n-last)) goto too_large;
    if (result.n==text.n && !memcmp(result.s,text.s,text.n)) {
        tell("%zu matches already have that text.",changes); goto done;
    }
    if (terminated || suspended || interrupted) goto cancelled;
    if (edit(0,text.n,result.s,result.n,false)) {
        memcpy(search_pattern,needle,n+1); search_length=n;
        tell("Replaced %zu matches as one undo step.",changes);
    }
    goto done;
too_large:
    tell("Replacement exceeds file limit or available memory; text unchanged.");
    goto done;
cancelled:
    tell("Replacement interrupted; text unchanged.");
done:
    free(failure); free(result.s);
}

static void free_changes(Change *p)
{
    while (p) {
        Change *next = p->next;
        free(p->old_s); free(p->new_s); free(p); p = next;
    }
}

static bool replace_bytes(size_t at, size_t old_n, const char *s, size_t n)
{
    size_t length;
    if (at > text.n || old_n > text.n - at || n > LIMIT - (text.n - old_n))
        return false;
    length = text.n - old_n + n;
    if (!reserve(&text, length + 1)) return false;
    memmove(text.s + at + n, text.s + at + old_n, text.n - at - old_n);
    if (n) memcpy(text.s + at, s, n);
    text.n = length; text.s[length] = 0;
    return true;
}

static bool edit(size_t at, size_t old_n, const char *s, size_t n, bool typing)
{
    Change *p;
    char *joined;
    /* Guard before subtracting lengths. Invalid edit requests must never
     * mutate history, even if a future caller miscalculates an offset. */
    if (at > text.n || old_n > text.n - at || (n && !s)) {
        tell("Invalid edit position; no changes made."); return false;
    }
    if (!old_n && !n) return true;
    if (n > LIMIT - (text.n - old_n)) { tell("File limit: 8 MiB."); return false; }
    if (!reserve(&text, text.n - old_n + n + 1)) goto memory_error;
    /* Consecutive insertion is one undo transaction. Navigation ends it. */
    if (typing && group && current && !current->next && !old_n &&
        !current->old_n && at == current->at + current->new_n) {
        /* Grow geometrically rather than reallocating on every keystroke. */
        if (n > SIZE_MAX - current->new_n) goto memory_error;
        if (current->new_cap < current->new_n + n) {
            size_t need = current->new_n + n;
            size_t cap = current->new_cap ? current->new_cap : 64;
            while (cap < need) {
                if (cap > SIZE_MAX / 2) { cap = need; break; }
                cap *= 2;
            }
            joined = realloc(current->new_s, cap);
            if (!joined) goto memory_error;
            current->new_s = joined; current->new_cap = cap;
        }
        memcpy(current->new_s + current->new_n, s, n);
        current->new_n += n;
        (void)replace_bytes(at, 0, s, n);
        cursor = at + n; current->after = cursor;
    } else {
        p = calloc(1, sizeof *p);
        if (!p) goto memory_error;
        if (old_n && !(p->old_s = malloc(old_n))) { free(p); goto memory_error; }
        if (n && !(p->new_s = malloc(n))) { free(p->old_s); free(p); goto memory_error; }
        if (old_n) memcpy(p->old_s, text.s + at, old_n);
        if (n) memcpy(p->new_s, s, n);
        p->at = at; p->old_n = old_n; p->new_n = n; p->new_cap = n;
        p->before = cursor; p->after = at + n; p->prev = current;
        p->ordinal = current ? current->ordinal + 1 : 1;
        /* Keep previously saved alternate futures reachable as stale
         * journals; branching must not silently erase an older path. */
        if (current) {
            journal_branched |= current->next != NULL;
            free_changes(current->next); current->next = p;
        } else {
            journal_branched |= history != NULL;
            free_changes(history); history = p;
        }
        current = history_tail = p;
        (void)replace_bytes(at, old_n, s, n); cursor = p->after;
    }
    journal_dirty = true;
    search_mark = SIZE_MAX;
    group = typing; goal_set = false; message[0] = 0;
    top = line_start(top > text.n ? text.n : top);
    return true;
memory_error:
    tell("Not enough memory. Text unchanged."); return false;
}

/* One validated, in-memory history step. Never writes a file or journal.
 * A false return leaves the document and history pointer unchanged. */
static bool history_step(bool redo)
{
    Change *p = redo ? (current ? current->next : history) : current;
    group = false; goal_set = false;
    if (!p) return false;
    {
        size_t expected_n = redo ? p->old_n : p->new_n;
        size_t inserted_n = redo ? p->new_n : p->old_n;
        size_t target_cursor = redo ? p->after : p->before;
        const char *expected = redo ? p->old_s : p->new_s;
        const char *inserted = redo ? p->new_s : p->old_s;
        if (p->at > text.n || expected_n > text.n - p->at ||
            inserted_n > LIMIT - (text.n - expected_n) ||
            target_cursor > text.n - expected_n + inserted_n ||
            (inserted_n && memchr(inserted, 0, inserted_n)) ||
            (expected_n && memcmp(text.s + p->at, expected, expected_n))) {
            tell("Undo journal is inconsistent with document. Action refused.");
            return false;
        }
    }
    if (!replace_bytes(p->at, redo ? p->old_n : p->new_n,
                       redo ? p->new_s : p->old_s, redo ? p->new_n : p->old_n)) {
        tell("Not enough memory. Text unchanged.");
        return false;
    }
    cursor = redo ? p->after : p->before;
    search_mark = SIZE_MAX;
    current = redo ? p : p->prev;
    journal_dirty = true;
    top = line_start(top > text.n ? text.n : top);
    return true;
}

static size_t undo_depth(void);
static size_t redo_depth(void);
/* One command can move many steps without allocating additional history.
 * Large requests stop at the earliest/latest available transaction. */
static size_t history_move(bool redo, size_t requested, bool quiet)
{
    size_t available = reverse_mode ? (redo ? reverse_total - reverse_position : reverse_position) :
                                  (redo ? redo_depth() : undo_depth());
    size_t done = 0, steps = requested < available ? requested : available;
    for (; done < steps; done++) {
        if (terminated || suspended || interrupted) break;
        if (!history_step(redo)) break;
        if (reverse_mode) {
            if (redo) reverse_position++;
            else reverse_position--;
        }
    }
    if (!quiet) {
        if (done) tell("%zu step%s %s. Undo: %zu, redo: %zu.", done,
                       done == 1 ? "" : "s", redo ? "redone" : "undone",
                       undo_depth(), redo_depth());
        else if (!steps) tell(redo ? "Nothing to redo." : "Nothing to undo.");
    }
    return done;
}
static void reverse_open(void)
{
    group = false;
    reverse_position = undo_depth();
    reverse_total = reverse_position + redo_depth();
    reverse_mode = true;
    command_mode = false;
    menu_open = false;
    message[0] = 0;
}
/* Resolve existing links, or the parent of a not-yet-existing file. */
static char *resolve_path(const char *name)
{
    char *expanded = NULL, *path, *copy, *slash, *parent, *result;
    const char *base, *input = name, *home = getenv("HOME");
    size_t n;
    if (name[0] == '~' && (name[1] == '/' || !name[1])) {
        if (!home || *home != '/') { errno = EINVAL; return NULL; }
        n = strlen(home) + strlen(name + 1) + 1;
        expanded = malloc(n);
        if (!expanded) return NULL;
        snprintf(expanded, n, "%s%s", home, name + 1);
        input = expanded;
    }
    path = realpath(input, NULL);
    if (path || errno != ENOENT) { free(expanded); return path; }
    copy = strdup(input);
    free(expanded);
    if (!copy) return NULL;
    slash = strrchr(copy, '/');
    base = slash ? slash + 1 : copy;
    if (!*base) { free(copy); errno = EINVAL; return NULL; }
    if (slash) {
        *slash = 0;
        parent = realpath(*copy ? copy : "/", NULL);
    } else parent = realpath(".", NULL);
    if (!parent) { free(copy); return NULL; }
    n = strlen(parent) + strlen(base) + 2;
    result = malloc(n);
    if (result) snprintf(result, n, "%s%s%s", parent,
                         !strcmp(parent, "/") ? "" : "/", base);
    free(copy); free(parent); return result;
}

static bool read_stream(int fd, Buffer *b)
{
    char chunk[8192]; ssize_t n;
    for (;;) {
        if (terminated || suspended || interrupted) { errno = EINTR; return false; }
        n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (!n) break;
        if ((size_t)n > LIMIT - b->n) { errno = EFBIG; return false; }
        if (!reserve(b, b->n + (size_t)n + 1)) { errno = ENOMEM; return false; }
        memcpy(b->s + b->n, chunk, (size_t)n); b->n += (size_t)n;
    }
    if (!reserve(b, b->n + 1)) { errno = ENOMEM; return false; }
    b->s[b->n] = 0;
    return true;
}

static bool same_disk_version(const struct stat *a, const struct stat *b);
static bool read_file(const char *path, Buffer *b, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    struct stat after;
    char chunk[8192]; ssize_t n;
    bool ok = false;
    if (fd < 0) return false;
    if (fstat(fd, st) < 0) goto done;
    if (!S_ISREG(st->st_mode)) { errno = EINVAL; goto done; }
    if (st->st_size < 0 || (uint64_t)st->st_size > LIMIT) {
        errno = EFBIG; goto done;
    }
    b->n = 0;
    for (;;) {
        if (terminated || suspended || interrupted) { errno = EINTR; goto done; }
        n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        if ((size_t)n > LIMIT - b->n) { errno = EFBIG; goto done; }
        if (!reserve(b, b->n + (size_t)n + 1)) { errno = ENOMEM; goto done; }
        memcpy(b->s + b->n, chunk, (size_t)n); b->n += (size_t)n;
    }
    if (!reserve(b, b->n + 1)) { errno = ENOMEM; goto done; }
    b->s[b->n] = 0;
    /* A disk read straddling an outside write is not a trustworthy
     * version to show or overwrite. Reject and let the user retry. */
    if (fstat(fd, &after) < 0) goto done;
    if (!same_disk_version(st, &after)) { errno = EAGAIN; goto done; }
    ok = true;
done:
    { int error = errno; if (close(fd) < 0 && ok) return false; errno = error; }
    return ok;
}

/* Check both identity and change times, not merely file contents. An
 * external writer that replaces a file with identical bytes still counts. */
static bool same_disk_version(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_nlink == b->st_nlink &&
           a->st_mode == b->st_mode &&
           a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static bool same_bytes(const Buffer *a, const Buffer *b)
{
    return a->n == b->n && (!a->n || !memcmp(a->s, b->s, a->n));
}
static bool disk_matches(const Buffer *seen, const struct stat *seen_stat,
                         const Buffer *now, const struct stat *now_stat)
{
    return same_disk_version(seen_stat, now_stat) && same_bytes(seen, now);
}
static void clear_prompt(void)
{
    prompt = PROMPT_NONE;
    free(conflict_snapshot.s);
    memset(&conflict_snapshot, 0, sizeof conflict_snapshot);
    free(pending_write_path);
    pending_write_path = NULL;
}
/* A confirmation is tied to one observed version of one target. A changed
 * target triggers a NEW confirmation instead of reusing an old yes. */
static void ask_overwrite(Buffer *disk, const struct stat *st,
                          const char *target, bool other_file)
{
    char *copy = other_file ? strdup(target) : NULL;
    if (other_file && !copy) {
        tell("Cannot request overwrite: out of memory.");
        return;
    }
    clear_prompt();
    pending_write_path = copy;
    conflict_snapshot = *disk;
    memset(disk, 0, sizeof *disk);
    conflict_stat = *st;
    prompt = other_file ? PROMPT_TARGET_OVERWRITE : PROMPT_OVERWRITE;
    tell(other_file ? "Overwrite existing file? Y/n" :
                      "File written since read. Overwrite y/n?");
}
/* One content transition, represented as a compact single undo step.
 * The caller owns the returned node until it is linked into history. */
static Change *difference(const Buffer *from, const Buffer *to,
                          size_t before, size_t after)
{
    size_t prefix = 0, suffix = 0, common = from->n < to->n ? from->n : to->n;
    Change *p;
    while (prefix < common && from->s[prefix] == to->s[prefix]) prefix++;
    while (suffix < common - prefix &&
           from->s[from->n-1-suffix] == to->s[to->n-1-suffix]) suffix++;
    p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->at = prefix;
    p->old_n = from->n - prefix - suffix;
    p->new_n = to->n - prefix - suffix;
    p->before = before < from->n ? before : from->n;
    p->after = after < to->n ? after : to->n;
    if (p->old_n) p->old_s = malloc(p->old_n);
    if (p->new_n) p->new_s = malloc(p->new_n);
    if ((p->old_n && !p->old_s) || (p->new_n && !p->new_s)) {
        free_changes(p);
        return NULL;
    }
    if (p->old_n) memcpy(p->old_s, from->s + prefix, p->old_n);
    if (p->new_n) memcpy(p->new_s, to->s + prefix, p->new_n);
    return p;
}
/* After an explicitly approved overwrite, the undo chain goes from the
 * editor's version to the external version and back to the editor version.
 * Undo once restores the overwritten external contents; undo again resumes
 * the older edit chain. Both transitions were allocated before the write. */
static void remember_overwrite(Change *to_outside, Change *to_editor)
{
    if (!to_outside || !to_editor) return;
    if (current) {
        journal_branched |= current->next != NULL;
        free_changes(current->next);
        current->next = to_outside;
    } else {
        journal_branched |= history != NULL;
        free_changes(history);
        history = to_outside;
    }
    to_outside->prev = current;
    to_outside->ordinal = current ? current->ordinal + 1 : 1;
    to_outside->next = to_editor;
    to_editor->prev = to_outside;
    to_editor->ordinal = to_outside->ordinal + 1;
    current = history_tail = to_editor;
    journal_dirty = true;
    group = false;
}

static int sync_fd(int fd)
{
    int r;
    do {
        if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
        r = fsync(fd);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* Persistent undo uses one atomically replaced private journal per canonical
 * pathname. It is a cache, not part of the edited file. No journal is written
 * for unsaved drafts, and failure must never prevent a document from saving. */
static int read_exact(int fd, void *out, size_t n)
{
    char *p = out;
    while (n) {
        ssize_t k;
        if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
        k = read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int put_number(int fd, uint64_t v)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++) { b[i] = (unsigned char)v; v >>= 8; }
    return write_all(fd, (const char *)b, sizeof b);
}
static int get_number(int fd, uint64_t *v)
{
    unsigned char b[8]; int i;
    if (read_exact(fd, b, sizeof b) < 0) return -1;
    *v = 0;
    for (i = 7; i >= 0; i--) *v = (*v << 8) | b[i];
    return 0;
}
/* Version 2 journal integrity: FNV-1a over the complete serialized journal
 * except its final 8-byte checksum. This detects accidental corruption and
 * torn/truncated data; it is not authentication or protection from tampering.
 * Version 1 journals are still readable for compatibility. */
#define JOURNAL_HASH_INITIAL UINT64_C(14695981039346656037)
static uint64_t journal_hash(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) { h ^= *p++; h *= UINT64_C(1099511628211); }
    return h;
}
/* Keep the on-disk format unchanged, but batch tiny transaction fields.
 * A long history otherwise needs several read/write syscalls per edit.
 * Fixed buffers add bounded working space, never another history copy. */
typedef struct {
    int fd;
    size_t used;
    uint64_t hash;
    unsigned char bytes[8192];
} JournalOutput;
typedef struct {
    int fd;
    size_t at, used;
    unsigned char bytes[8192];
} JournalInput;
static int journal_flush(JournalOutput *out)
{
    if (write_all(out->fd, (const char *)out->bytes, out->used) < 0) return -1;
    out->used = 0;
    return 0;
}
static int journal_write(JournalOutput *out, const void *data, size_t n)
{
    const unsigned char *p = data;
    out->hash = journal_hash(out->hash, data, n);
    while (n) {
        size_t take = sizeof out->bytes - out->used;
        if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
        if (take > n) take = n;
        memcpy(out->bytes + out->used, p, take);
        out->used += take; p += take; n -= take;
        if (out->used == sizeof out->bytes && journal_flush(out) < 0) return -1;
    }
    return 0;
}
static int journal_number(JournalOutput *out, uint64_t v)
{
    unsigned char b[8]; int i;
    for (i = 0; i < 8; i++) { b[i] = (unsigned char)v; v >>= 8; }
    return journal_write(out, b, sizeof b);
}
static int journal_read(JournalInput *in, void *data, size_t n)
{
    unsigned char *p = data;
    while (n) {
        size_t take;
        if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
        if (in->at == in->used) {
            ssize_t got = read(in->fd, in->bytes, sizeof in->bytes);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) { if (!got) errno = EINVAL; return -1; }
            in->at = 0; in->used = (size_t)got;
        }
        take = in->used - in->at;
        if (take > n) take = n;
        memcpy(p, in->bytes + in->at, take);
        in->at += take; p += take; n -= take;
    }
    return 0;
}
static int journal_read_number(JournalInput *in, uint64_t *v)
{
    unsigned char b[8]; int i;
    if (journal_read(in, b, sizeof b) < 0) return -1;
    *v = 0;
    for (i = 7; i >= 0; i--) *v = (*v << 8) | b[i];
    return 0;
}
/* fd is positioned at the end of the 8-byte magic when called; verify the
 * file without allocating another copy, then reset to the first header. */
static bool journal_checksum_ok(int fd, off_t size)
{
    unsigned char block[8192];
    uint64_t hash = JOURNAL_HASH_INITIAL, stored;
    off_t remain;
    if (size < 16 || lseek(fd, 0, SEEK_SET) < 0) return false;
    remain = size - 8;
    while (remain > 0) {
        size_t take = remain < (off_t)sizeof block ? (size_t)remain : sizeof block;
        if (read_exact(fd, block, take) < 0) return false;
        hash = journal_hash(hash, block, take);
        remain -= (off_t)take;
    }
    if (get_number(fd, &stored) < 0 || stored != hash) return false;
    return lseek(fd, 8, SEEK_SET) == 8;
}
static uint64_t path_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (*s) { h ^= (unsigned char)*s++; h *= UINT64_C(1099511628211); }
    return h;
}
/* Only use directories, never symbolic links, for state storage. */
static bool private_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return false;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode)) return false;
    return true;
}
static void open_viewer(const char *title, const char *contents);
typedef struct {
    const char *name, *synonyms, *syntax, *description;
    unsigned section;
    const char *summary, *examples;
} CommandHelp;
static const char *const command_sections[] = {
    "Files and saving", "Finding and replacing", "Deleting text",
    "Copy, cut and clipboards", "Undo and earlier versions", "Moving around",
    "Controls and settings", "Instructions", "Leaving editor"
};
#define COMMAND_SECTION_COUNT (sizeof command_sections / sizeof command_sections[0])
static const CommandHelp command_info[] = {
    {"write", "save, w", "write [FILE|-]",
     "Save or prompt for a name; write FILE switches the active file after saving. Confirm before replacing an existing other file. write - sends the document to redirected stdout.",
     0, "Save the active file, or save under a new name.",
     "write\n"
     "  Save this file; an unnamed document asks for a filename.\n"
     "write notes.txt\n"
     "  Save as notes.txt and make it the active file.\n"
     "write -\n"
     "  Send exact document bytes to redirected stdout."},
    {"read", "open, editfile", "read [FILE]",
     "Open another file, refusing to discard unsaved changes. Tab completes paths; Tab on a directory lists its entries.",
     0, "Open another file after saving your current work.",
     "read\n"
     "  Ask which file to open; Tab completes filenames.\n"
     "read notes.txt\n"
     "  Open notes.txt. Save unsaved edits before changing files."},
    {"ls", "dir, files, directory", "ls [DIRECTORY]",
     "Browse directory entries in a scrollable viewer. Defaults to the current directory.",
     0, "Look at the files in a directory without leaving editor.",
     "ls\n"
     "  List the current directory.\n"
     "ls ~/Documents/\n"
     "  List Documents in your home directory."},
    {"clipboard", "clip, cb, cboard, editclip", "clipboard [SLOT|default|list]",
     "With no argument, toggle editing the selected clipboard. clipboard 2 selects named slot 2; clipboard default selects clipboard.txt; clipboard list shows slots. Copy/cut go to the selected slot, but paste 2 reads slot 2 directly. Slot selection persists. The slot name selected is reserved for internal state.",
     3, "Edit the clipboard, return to the document, or select a slot.",
     "clipboard\n"
     "  Open the selected clipboard for editing.\n"
     "write\n"
     "  Save it and stay in the clipboard.\n"
     "clipboard\n"
     "  Save it and return to your original document.\n"
     "clipboard 2\n"
     "  Select slot 2; this does not mean document line 2.\n"
     "clipboard list\n"
     "  Show saved clipboard slots."},
    {"copy", "yank, copyline", "copy [LINE|FIRST-LAST]",
     "Copy complete lines to the selected clipboard without changing the document. A number after copy is a LINE number, not a clipboard slot.",
     3, "Copy lines without removing them.",
     "copy\n"
     "  Copy the current line to the selected clipboard.\n"
     "copy 5\n"
     "  Copy document line 5, not clipboard 5.\n"
     "copy 10-15\n"
     "  Copy lines 10 through 15.\n"
     "clipboard 2\n"
     "  Select clipboard 2 for subsequent copy or cut commands."},
    {"cut", "cutline, movelines", "cut [LINE|FIRST-LAST]",
     "Copy complete lines to the selected clipboard, then delete them in one undoable edit. Numeric arguments are line numbers, not clipboard slots.",
     3, "Move lines to the clipboard and remove them here.",
     "cut\n"
     "  Cut the current line into the selected clipboard.\n"
     "cut 2\n"
     "  Cut document line 2, not into clipboard 2.\n"
     "cut 6-9\n"
     "  Cut lines 6 through 9. undo restores the removed text."},
    {"paste", "put, insertclip", "paste [SLOT|default]",
     "Insert lines from the selected clipboard, or paste 2 from slot 2, without changing the selected slot. Each paste is one undoable edit.",
     3, "Insert clipboard contents before the current line.",
     "paste\n"
     "  Insert from the selected clipboard.\n"
     "paste 2\n"
     "  Insert from clipboard 2; the selected slot stays unchanged.\n"
     "undo\n"
     "  Remove this paste."},
    {"cliphistory", "clipboard-history, clips", "cliphistory [on|off]",
     "Browse saved prior contents across clipboard slots; enable/disable persistent history (off by default, max 64 snapshots).",
     3, "Browse saved clipboard contents or enable their history.",
     "cliphistory on\n"
     "  Keep up to 64 older clipboard snapshots.\n"
     "cliphistory\n"
     "  View available snapshots.\n"
     "cliphistory off\n"
     "  Stop collecting snapshots; already saved ones remain."},
    {"clipuse", "restoreclip, clipboard-use", "clipuse [NUMBER]",
     "Restore a numbered saved clipboard entry (1 is newest), archiving the previous clipboard when history is enabled.",
     3, "Restore a numbered snapshot from clipboard history.",
     "cliphistory\n"
     "  See the available snapshot numbers first.\n"
     "clipuse 1\n"
     "  Restore the newest snapshot to the selected clipboard.\n"
     "clipuse\n"
     "  Ask for a snapshot number."},
    {"dline", "delline, delete-line, delete, remove-line", "dline [LINE|FIRST-LAST]",
     "Delete a line or inclusive range. Also: COUNT dline deletes COUNT lines from the cursor. One undo step.",
     2, "Delete the current line or a range of numbered lines.",
     "dline\n"
     "  Delete the current line.\n"
     "dline 30-43\n"
     "  Delete lines 30 through 43, including both ends.\n"
     "13 dline\n"
     "  Delete 13 lines starting at the cursor.\n"
     "undo\n"
     "  Restore the deleted text."},
    {"deleol", "deltoeol, delete-to-eol, delete-eol, delendline", "deleol",
     "Delete text from cursor through end of this line, preserving LF/CRLF line separator. One undo step.",
     2, "Delete from the cursor to the end of this line.",
     "deleol\n"
     "  Remove the rest of this line, keeping its line break.\n"
     "undo\n"
     "  Restore it."},
    {"deleof", "deltoeof, delete-to-eof, delete-to-end, delenddoc", "deleof",
     "Delete text from cursor through end of document. One undo step.",
     2, "Delete from the cursor to the end of the document.",
     "deleof\n"
     "  Remove everything from the cursor onward.\n"
     "undo\n"
     "  Restore it in one step."},
    {"backbof", "backtobof, backspace-to-start, backspace-to-bof, backstart", "backbof",
     "Delete text from beginning of document through cursor. One undo step.",
     2, "Delete from the start of the document up to the cursor.",
     "backbof\n"
     "  Remove everything before the cursor.\n"
     "undo\n"
     "  Restore it in one step."},
    {"undo", "u", "undo [COUNT]",
     "Undo COUNT transactions (default 1; stops at earliest available).",
     4, "Reverse one or more changes.",
     "undo\n"
     "  Reverse the last change.\n"
     "undo 5\n"
     "  Reverse up to five changes; stop at the oldest."},
    {"redo", "r", "redo [COUNT]",
     "Redo COUNT transactions (default 1; stops at latest available).",
     4, "Reapply changes you have undone.",
     "redo\n"
     "  Reapply one undone change.\n"
     "redo 5\n"
     "  Reapply up to five changes; stop at the newest."},
    {"history", "undos", "history",
     "Display undo and redo counts.",
     4, "See how many undo and redo steps are available.",
     "history\n"
     "  Report the available undo and redo counts.\n"
     "reverse\n"
     "  Browse those states while viewing the document."},
    {"reverse", "logbrowser, undomode, undo-mode, browse-undo, browse-history", "reverse [INDEX]",
     "Browse the undo chain with Left/h/</- and Right/l/>/+ (one step per repeated key); INDEX jumps to an exact state (0 is oldest). Esc exits without saving.",
     4, "Browse earlier and later versions of your text.",
     "reverse\n"
     "  Enter the undo browser. - goes back; + goes forward.\n"
     "reverse 0\n"
     "  Go to the oldest available state.\n"
     "reverse 12\n"
     "  Go to history state 12, if it exists.\n"
     "Esc leaves the browser. write saves the displayed version."},
    {"find", "search, locate", "find [TEXT]",
     "Search for literal, case-sensitive text from the current cursor (inclusive), wrapping at the end. Without TEXT, open a Find> prompt that preserves every space, including a search made only of spaces. This does not change the document.",
     1, "Find text in the document.",
     "find\n"
     "  Ask what to search for.\n"
     "find server_name\n"
     "  Find literal server_name. Then use next or prev."},
    {"next", "n, findnext, nextmatch, find-next, nextfind", "next",
     "Find the next occurrence of remembered search text after the cursor, wrapping at the end.",
     1, "Go to the next search match.",
     "find port\n"
     "  Start a search for port.\n"
     "next\n"
     "  Move to its next match. n does the same thing."},
    {"prev", "p, previous, findprev, prevmatch, previousmatch, find-prev, previousfind", "prev",
     "Find the previous occurrence of remembered search text before the cursor, wrapping at the beginning.",
     1, "Go to the previous search match.",
     "prev\n"
     "  Move back to the previous match of your last search.\n"
     "p\n"
     "  Same as prev."},
    {"replace", "substitute, change-text", "replace [OLD => NEW]",
     "Replace the first match from cursor, wrapping. With no arguments prompt for search and replacement. One undo step.",
     1, "Find and replace one occurrence.",
     "replace\n"
     "  Ask for search text, then replacement text.\n"
     "replace old => new\n"
     "  Replace one occurrence of old with new.\n"
     "undo\n"
     "  Reverse that replacement. Use prompts for an empty replacement."},
    {"replaceall", "replace-all, substituteall", "replaceall [OLD => NEW]",
     "Replace all non-overlapping literal matches in a single undoable operation. Prompts when omitted.",
     1, "Replace every occurrence in one undoable change.",
     "replaceall\n"
     "  Ask for search text and replacement text.\n"
     "replaceall old => new\n"
     "  Replace all non-overlapping occurrences of old.\n"
     "undo\n"
     "  Reverse the whole replacement."},
    {"goto", "go, line, jump", "goto [LINE [COLUMN]]",
     "Move to a 1-based line and optional visual column.",
     5, "Move to a numbered line and optional column.",
     "goto\n"
     "  Ask for a line number.\n"
     "goto 50\n"
     "  Move to line 50.\n"
     "goto 50 12\n"
     "  Move to line 50, column 12. Both start at 1."},
    {"top", "first, start", "top",
     "Move to the beginning of the document.",
     5, "Go to the start of the document.",
     "top\n"
     "  Move to the first character."},
    {"bottom", "last, end", "bottom",
     "Move to the end of the document.",
     5, "Go to the end of the document.",
     "bottom\n"
     "  Move after the last character."},
    {"left", "backward", "left [COUNT]",
     "Move left by COUNT characters (default one).",
     5, "Move left by one or more characters.",
     "left\n"
     "  Move left once.\n"
     "left 8\n"
     "  Move left up to eight characters."},
    {"right", "forward", "right [COUNT]",
     "Move right by COUNT characters (default one).",
     5, "Move right by one or more characters.",
     "right\n"
     "  Move right once.\n"
     "right 8\n"
     "  Move right up to eight characters."},
    {"up", "moveup, line-up, lineup", "up [COUNT]",
     "Move up by COUNT displayed lines (default one).",
     5, "Move up by one or more lines.",
     "up\n"
     "  Move up one line.\n"
     "up 8\n"
     "  Move up eight lines, stopping at the beginning."},
    {"down", "movedown, line-down, linedown", "down [COUNT]",
     "Move down by COUNT displayed lines (default one).",
     5, "Move down by one or more lines.",
     "down\n"
     "  Move down one line.\n"
     "down 8\n"
     "  Move down eight lines, stopping at the end."},
    {"pageup", "pgup", "pageup",
     "Move up one screen.",
     5, "Move up one screenful.",
     "pageup\n"
     "  Move up a page without modifying the document."},
    {"pagedown", "pgdown", "pagedown",
     "Move down one screen.",
     5, "Move down one screenful.",
     "pagedown\n"
     "  Move down a page without modifying the document."},
    {"movement", "move, navigation", "movement",
     "Navigate without typing: arrows/hjkl, p/n/b/f, Ctrl-P/N/B/F, 0/$, a/e, g/G, y/v and w/@. Enter or the edit command resumes text entry. Set startup=movement in editor/config for future launches, or use --movement.",
     6, "Navigate without accidentally typing into the document.",
     "movement\n"
     "  Use arrows or hjkl to navigate. Enter resumes typing.\n"
     "For future launches, put startup=movement in editor/config.\n"
     "From the shell: editor --movement notes.txt"},
    {"edit", "editmode, insertmode, typing", "edit",
     "Resume ordinary text entry. The movement command enables navigation-only control. Set startup=edit in editor/config for future launches, or use --edit. No user hotkeys are installed.",
     6, "Return to ordinary typing.",
     "edit\n"
     "  Leave movement mode and resume text entry.\n"
     "For future launches, put startup=edit in editor/config."},
    {"menu", "options", "menu",
     "Make Esc open the numbered/arrow menu.",
     6, "Use the arrow-key and numbered menu for commands.",
     "menu\n"
     "  Open the menu; Esc will open it on later use too.\n"
     "cli\n"
     "  Make Esc open the command prompt again."},
    {"cli", "command", "cli",
     "Make Esc open Command>.",
     6, "Use typed commands instead of the menu.",
     "cli\n"
     "  Make Esc open Command>.\n"
     "menu\n"
     "  Switch back to menu-based control."},
    {"mouse", "pointer", "mouse [on|off|toggle]",
     "Enable or disable optional mouse input.",
     6, "Turn optional mouse control on or off.",
     "mouse on\n"
     "  Enable mouse control.\n"
     "mouse off\n"
     "  Leave the mouse to the terminal.\n"
     "mouse\n"
     "  Toggle the current setting."},
    {"signals", "signal-controls", "signals [on|off|toggle]",
     "Report or change optional editor signal handling (off by default). Off: terminal interrupt exits; terminal stop suspends. On: interrupt cancels input instead of exiting; stop still suspends, cancelling a pending confirmation. No key is bound or terminal signal character changed. Save signal_controls=on/off in editor/config for future launches.",
     6, "Choose terminal interrupt behavior; no hotkeys are assigned.",
     "signals\n"
     "  Report the setting. OFF is the default.\n"
     "signals on\n"
     "  Interrupt cancels input instead of exiting.\n"
     "signals off\n"
     "  Interrupt exits without saving. Stop still suspends.\n"
     "Save signal_controls=on or off in editor/config for future launches."},
    {"bind", "key", "bind KEY COMMAND",
     "Create a user-defined hotkey for a built-in command.",
     6, "Optionally assign your own key to a command.",
     "bind F2 write\n"
     "  Assign F2 to write and save your keymap.\n"
     "unbind F2\n"
     "  Remove that assignment. No assignments are installed by default.\n"
     "Ctrl+C and Ctrl+Z belong to terminal signal handling, not bindings."},
    {"unbind", "unkey", "unbind KEY",
     "Remove one user-defined hotkey.",
     6, "Remove a key assignment you made.",
     "unbind F2\n"
     "  Remove your F2 assignment.\n"
     "bindings\n"
     "  See any remaining assignments."},
    {"bindings", "keys, keymap", "bindings",
     "Display user-defined key assignments.",
     6, "Show your own optional key assignments.",
     "bindings\n"
     "  List your assignments. An empty list is normal."},
    {"suspend", "pause", "suspend",
     "Suspend editor and restore the terminal; resume with fg.",
     6, "Return temporarily to your shell, keeping editor in memory.",
     "suspend\n"
     "  Suspend the editor.\n"
     "At the shell, type fg to resume. Unsaved edits stay in memory."},
    {"about", "info", "about",
     "Display version, credits, and warning.",
     7, "Show version, credits and the Bloodguilt Curse notice.",
     "about\n"
     "  Display program information; Esc or q returns."},
    {"manual", "man", "manual",
     "Read the complete manual.",
     7, "Read complete instructions and configuration details.",
     "manual\n"
     "  Read the full manual. j/k scroll; h/l turn pages.\n"
     "Esc or q closes the viewer."},
    {"help", "?", "help [COMMAND|TOPIC]",
     "Start with practical command guidance, or see examples, usage and synonyms for one command. A topic searches the command guide.",
     7, "Get started or learn how to use a command.",
     "help\n"
     "  Read the short starting guide.\n"
     "help cut\n"
     "  See examples and synonyms for cutting lines.\n"
     "help save\n"
     "  See write help through its save synonym."},
    {"list", "commands, cmdlist", "list [TOPIC]",
     "Browse commands by task, or filter by words in their names, synonyms, descriptions or category. Nothing in the list runs by itself.",
     7, "Find commands by task, with a short explanation of each.",
     "list\n"
     "  Browse all commands grouped by purpose.\n"
     "list clipboard\n"
     "  Narrow the list to clipboard-related tasks.\n"
     "list save\n"
     "  Search names, synonyms, descriptions and topics."},
    {"quit", "exit, q", "quit",
     "Leave if there are no unsaved changes, then print the Bloodguilt Curse notice to the terminal without pausing. Forced and confirmed quick exits skip the notice.",
     8, "Leave after your work is saved.",
     "write\n"
     "  Save your work first.\n"
     "quit\n"
     "  Leave; unsaved changes prevent quitting.\n"
     "The ordinary quit prints the Bloodguilt Curse notice without pausing."},
    {"quit!", "exit!, q!", "quit!",
     "Discard unsaved changes and leave.",
     8, "Discard unsaved changes and leave immediately.",
     "quit!\n"
     "  Discard unsaved document and clipboard edits and exit.\n"
     "There is no confirmation. Use write then quit to keep your work."},
};
#define COMMAND_INFO_COUNT (sizeof command_info / sizeof command_info[0])
static const CommandHelp *command_lookup(const char *name)
{
    size_t i;
    for (i=0; i < COMMAND_INFO_COUNT; i++) {
        const char *p = command_info[i].synonyms;
        size_t n = strlen(name);
        if (!strcasecmp(name,command_info[i].name)) return &command_info[i];
        while (*p) {
            const char *begin, *end;
            while (*p == ' ' || *p == ',') p++;
            begin = p;
            while (*p && *p != ',') p++;
            end = p;
            while (end > begin && end[-1] == ' ') end--;
            if ((size_t)(end-begin)==n && !strncasecmp(name,begin,n)) return &command_info[i];
        }
    }
    return NULL;
}
/* Build read-only guidance without heap allocations. The current registry
 * fits comfortably; future growth is explicitly marked rather than silently
 * clipping a command or writing beyond the page. */
static void guide_append(size_t *used, const char *format, ...)
{
    va_list ap;
    int n;
    size_t remain;
    if (*used >= sizeof command_page - 1) return;
    remain = sizeof command_page - *used;
    va_start(ap, format);
    n = vsnprintf(command_page + *used, remain, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= remain) {
        static const char marker[] = "\n(Use list TOPIC to narrow this list.)\n";
        size_t at = sizeof command_page - sizeof marker;
        memcpy(command_page + at, marker, sizeof marker);
        *used = sizeof command_page - 1;
    } else *used += (size_t)n;
}
static void command_set(const char *s);
/* Unlike a manual page launched from the document, help/list are part of
 * command entry. Closing them resumes Command>, not a detour through edit
 * mode. Error/completion pages keep the user's draft instead of erasing it. */
static void guide_open(const char *title, const char *contents, bool keep_draft)
{
    if (!keep_draft) {
        filename_mode = filename_read_mode = menu_argument_mode = false;
        menu_argument_command = menu_argument_label = NULL;
        history_choice = -1;
        command_set("");
    }
    menu_open = false;
    command_mode = true;
    open_viewer(title, contents);
}
/* A message in the ordinary document footer is hidden while Command> is
 * active. Reuse the guidance viewer for input failures, keeping the exact
 * draft and any filename/search parameter context for a retry. */
static void command_notice(const char *title)
{
    size_t used = 0;
    guide_append(&used, "%s\n\nEsc/q returns to your command.\n", message);
    guide_open(title, command_page, true);
}
static bool contains_word(const char *haystack, const char *word, size_t n)
{
    const char *p;
    if (!n) return true;
    for (p = haystack; *p; p++)
        if (!strncasecmp(p, word, n)) return true;
    return false;
}
/* All words must match; this is literal filtering, not a shell or regex. */
static bool guide_matches(const CommandHelp *info, const char *query)
{
    while (*query) {
        size_t n;
        while (*query == ' ' || *query == '\t') query++;
        n = strcspn(query, " \t");
        if (!n) break;
        if (!contains_word(info->name,query,n) &&
            !contains_word(info->synonyms,query,n) &&
            !contains_word(info->syntax,query,n) &&
            !contains_word(info->summary,query,n) &&
            !contains_word(info->description,query,n) &&
            !contains_word(command_sections[info->section],query,n)) return false;
        query += n;
    }
    return true;
}
static void command_overview(const char *query, bool keep_draft)
{
    size_t used = 0, i, section, count = 0;
    guide_append(&used,"COMMANDS BY TASK\n\n");
    if (*query) guide_append(&used,"Matching: %s\n\n",query);
    guide_append(&used,"Type the command, then Enter. [ ] means optional.\n"
                       "help NAME gives examples; list WORD narrows this list.\n");
    for (section = 0; section < COMMAND_SECTION_COUNT; section++) {
        bool heading = false;
        for (i = 0; i < COMMAND_INFO_COUNT; i++) {
            const CommandHelp *info = command_info + i;
            if (info->section != section || !guide_matches(info,query)) continue;
            if (!heading) { guide_append(&used,"\n%s\n",command_sections[section]); heading=true; }
            guide_append(&used,"  %s\n    %s\n",info->syntax,info->summary);
            count++;
        }
    }
    if (!count) guide_append(&used,"\nNo matching commands. Try a shorter word,\n"
                                  "or list without a topic to see everything.\n");
    guide_append(&used,"\n%zu command%s. Esc/q returns to Command>.\n",count,count==1?"":"s");
    guide_open("Commands by task",command_page,keep_draft);
}
static void command_details(const CommandHelp *info, const char *problem,
                            bool keep_draft)
{
    size_t used = 0;
    if (problem) guide_append(&used,"%s\n\nNothing ran. Esc/q returns to your command to correct it.\n\n",problem);
    guide_append(&used,"%s - %s\n\nEXAMPLES\n%s\n\nDETAILS\n%s\n\n"
                       "FORM\n%s\n[ ] means optional; do not type the brackets.\n\n"
                       "SYNONYMS\n%s\n\nEsc/q returns to Command>. Nothing here runs itself.\n",
                 info->name, info->summary, info->examples, info->description,
                 info->syntax, *info->synonyms ? info->synonyms : "(none)");
    guide_open(problem ? "Check this command" : "How to use this command",command_page,keep_draft);
}
static void command_help(const char *name)
{
    char topic[COMMAND_SIZE];
    const CommandHelp *info;
    size_t n = strlen(name);
    if (n >= sizeof topic) n = sizeof topic - 1;
    while (n && (name[n-1] == ' ' || name[n-1] == '\t')) n--;
    memcpy(topic,name,n); topic[n]=0;
    info=command_lookup(topic);
    if (!info) { command_overview(topic,false); return; }
    command_details(info,NULL,false);
}
static void command_usage(const CommandHelp *info, const char *format, ...)
{
    char reason[256];
    va_list ap;
    va_start(ap,format);
    (void)vsnprintf(reason,sizeof reason,format,ap);
    va_end(ap);
    command_details(info,reason,true);
    tell("%s",reason);
}
/* Typo suggestions are suggestions only. Bound the work to short command
 * words; document contents, filenames and replacement values are not guessed. */
static unsigned command_distance(const char *a, const char *b)
{
    unsigned row[65], next_row[65];
    size_t i,j,n=strlen(a),m=strlen(b);
    if (n>64 || m>64 || n+2<m || m+2<n) return 3;
    for(j=0;j<=m;j++) row[j]=(unsigned)j;
    for(i=1;i<=n;i++) {
        next_row[0]=(unsigned)i;
        for(j=1;j<=m;j++) {
            unsigned cost=tolower((unsigned char)a[i-1])==tolower((unsigned char)b[j-1]) ? 0u:1u;
            unsigned best=row[j]+1;
            if(next_row[j-1]+1<best) best=next_row[j-1]+1;
            if(row[j-1]+cost<best) best=row[j-1]+cost;
            next_row[j]=best;
        }
        memcpy(row,next_row,(m+1)*sizeof *row);
    }
    return row[m];
}
static void command_unknown(const char *word)
{
    size_t i, used=0, n=strlen(word), matches=0;
    guide_append(&used,"Command not found: %s\n\nNothing ran. Esc/q returns to your command\n"
                       "so you can correct it without retyping.\n\n",word);
    for(i=0;i<COMMAND_INFO_COUNT;i++) {
        const CommandHelp *info=command_info+i;
        if (!strncasecmp(info->name,word,n) ||
            (n>=3 && command_distance(word,info->name)<=(n>4?2u:1u))) {
            guide_append(&used,"  %s\n    %s\n",info->name,info->summary);
            matches++;
        }
    }
    if (!matches) guide_append(&used,"No close command name found.\n");
    guide_append(&used,"\nTry list to browse tasks, list WORD to search,\n"
                       "or help NAME for examples. Tab completes names.\n");
    guide_open("Command not found",command_page,true);
}
static void request_argument(const char *name, const char *question)
{
    menu_argument_mode = true;
    menu_argument_command = name;
    menu_argument_label = question;
    command_mode = true;
    command_set("");
    history_choice = -1;
}

/* All hotkeys are user-supplied. There is deliberately no default map.
 * Printable typing, Enter, Esc, arrows, and deletion remain core text input,
 * not command shortcuts. Some Ctrl combinations are indistinguishable from
 * these editing keys and are therefore not bindable. */
static int hotkey_code(const char *key, char canonical[24])
{
    char name[48] = {0}; size_t n = strlen(key), i;
    const char *rest;
    int modifier = 0, code = -1, direction = 0;
    if (!n || n >= sizeof name) return -1;
    for (i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c == '-') c = '+';
        name[i] = (char)tolower(c);
    }
    if (!strncmp(name, "ctrl+", 5)) { modifier = 1; rest = name + 5; }
    else if (!strncmp(name, "alt+", 4)) { modifier = 2; rest = name + 4; }
    else rest = name;
    if (!strcmp(rest, "up")) direction = 1;
    else if (!strcmp(rest, "down")) direction = 2;
    else if (!strcmp(rest, "left")) direction = 3;
    else if (!strcmp(rest, "right")) direction = 4;
    if (direction) {
        if (!modifier) return -1;
        code = (modifier == 1 ? KEY_CTRL_ARROW_BASE : KEY_ALT_ARROW_BASE) + direction;
        (void)snprintf(canonical, 24, "%s+%s", modifier == 1 ? "Ctrl" : "Alt",
                       direction == 1 ? "up" : direction == 2 ? "down" :
                       direction == 3 ? "left" : "right");
    } else if (!modifier && name[0] == 'f' && isdigit((unsigned char)name[1])) {
        char *end;
        long fn;
        errno = 0;
        fn = strtol(name + 1, &end, 10);
        if (errno || *end || fn < 1 || fn > 12) return -1;
        code = KEY_FUNCTION_BASE + (int)fn;
        (void)snprintf(canonical, 24, "F%ld", fn);
    } else if (modifier && rest[0] && !rest[1] && rest[0] >= 'a' && rest[0] <= 'z') {
        if (modifier == 1) {
            code = rest[0] - 'a' + 1;
            /* Ctrl-C/Z are reserved for terminal signaling, not commands.
             * Reject old saved bindings too, even when ISIG is disabled. */
            if (code == 3 || code == 8 || code == 9 || code == 10 ||
                code == 13 || code == 26) return -1;
        } else code = KEY_ALT_BASE + rest[0];
        (void)snprintf(canonical, 24, "%s+%c", modifier == 1 ? "Ctrl" : "Alt",
                       toupper((unsigned char)rest[0]));
    }
    return code;
}

static bool allowed_key_command(const char *command_text)
{
    char word[32]; size_t n = 0;
    const CommandHelp *info;
    while (*command_text == ' ' || *command_text == '\t') command_text++;
    /* User-defined bindings may invoke the count-first dline grammar too. */
    if (*command_text >= '0' && *command_text <= '9') {
        char *end; unsigned long long number;
        const char *p;
        errno = 0;
        number = strtoull(command_text, &end, 10);
        if (errno || !number || number > SIZE_MAX || end == command_text ||
            (*end != ' ' && *end != '\t')) return false;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        while (p[n] && p[n] != ' ' && p[n] != '\t' && n < sizeof word-1) {
            word[n] = p[n]; n++;
        }
        word[n] = 0;
        p += n;
        while (*p == ' ' || *p == '\t') p++;
        info = command_lookup(word);
        return info && !strcmp(info->name, "dline") && !*p;
    }
    while (command_text[n] && command_text[n] != ' ' &&
           command_text[n] != '\t' && n < sizeof word-1) {
        word[n] = command_text[n]; n++;
    }
    word[n] = 0; info = command_lookup(word);
    /* No bind/unbind from inside another binding. */
    return info && strcmp(info->name,"bind") && strcmp(info->name,"unbind");
}

static int binding_index(int code)
{
    size_t i;
    for (i = 0; i < bind_count; i++) if (binds[i].code == code) return (int)i;
    return -1;
}
static const char *binding_action(int code)
{
    int i = binding_index(code);
    return i < 0 ? NULL : binds[i].action;
}
static bool binding_assign(const char *name, const char *action)
{
    char normal[24]; int code = hotkey_code(name, normal), i;
    if (code < 0 || !allowed_key_command(action) || strlen(action) >= sizeof binds[0].action)
        return false;
    i = binding_index(code);
    if (i < 0) {
        if (bind_count == MAX_BINDINGS) return false;
        i = (int)bind_count++;
    }
    binds[i].code = code;
    strcpy(binds[i].name, normal);
    strcpy(binds[i].action, action);
    return true;
}

static char *config_path(const char *leaf, bool create)
{
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *work, *result, *q; struct stat st;
    size_t n;
    if (!base || *base != '/') {
        if (!home || *home != '/') return NULL;
        n = strlen(home) + sizeof "/.config/editor";
        work = malloc(n);
        if (!work) return NULL;
        snprintf(work, n, "%s/.config/editor", home);
    } else {
        n = strlen(base) + sizeof "/editor";
        work = malloc(n);
        if (!work) return NULL;
        snprintf(work, n, "%s/editor", base);
    }
    if (create) {
        for (q = work + 1; *q; q++) if (*q == '/') {
            *q = 0;
            if (!private_directory(work)) { free(work); return NULL; }
            *q = '/';
        }
        if (!private_directory(work)) { free(work); return NULL; }
    }
    if (lstat(work, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077)) { free(work); return NULL; }
    result = malloc(strlen(work) + strlen(leaf) + 2);
    if (result) sprintf(result, "%s/%s", work, leaf);
    free(work); return result;
}

static char *keymap_path(bool create)
{
    return config_path("keys.conf", create);
}

/* Bounded, inert configuration text. Refuse special files, links, NUL bytes,
 * writable-by-others files and files changing during the read. Never execute
 * shell text. 0 means absent, 1 loaded, -1 invalid/unavailable. */
static int read_config(const char *leaf, Buffer *b)
{
    char *path = config_path(leaf, false), chunk[1024];
    struct stat before, after;
    int fd = -1, result = -1;
    ssize_t n;
    if (!path) return 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    free(path);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_uid != geteuid() || (before.st_mode & 022) ||
        before.st_size < 0 || before.st_size > 32768) goto done;
    for (;;) {
        if (terminated || interrupted || suspended) goto done;
        n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        if ((size_t)n > 32768 - b->n || memchr(chunk, 0, (size_t)n) ||
            !reserve(b, b->n + (size_t)n + 1)) goto done;
        memcpy(b->s + b->n, chunk, (size_t)n); b->n += (size_t)n;
    }
    if (!reserve(b, b->n + 1) || fstat(fd, &after) < 0 ||
        !same_disk_version(&before, &after)) goto done;
    b->s[b->n] = 0;
    result = 1;
done:
    if (close(fd) < 0) result = -1;
    return result;
}

static void load_startup_config(void)
{
    Buffer b = {0};
    int result = read_config("config", &b);
    char *line = b.s;
    if (result < 0) startup_config_warning = true;
    while (result == 1 && line && *line) {
        char *next = strchr(line, '\n'), *end, *value;
        if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        end = strchr(line, '#');
        if (end) *end = 0;
        end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
        if (*line) {
            value = strchr(line, '=');
            if (!value) startup_config_warning = true;
            else {
                *value++ = 0;
                end = line + strlen(line);
                while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
                while (*value == ' ' || *value == '\t') value++;
                if (!strcmp(line, "startup")) {
                    if (!strcmp(value, "movement")) movement_mode = true;
                    else if (!strcmp(value, "edit")) movement_mode = false;
                    else startup_config_warning = true;
                } else if (!strcmp(line, "signal_controls")) {
                    if (!strcmp(value, "on")) signal_controls = 1;
                    else if (!strcmp(value, "off")) signal_controls = 0;
                    else startup_config_warning = true;
                } else startup_config_warning = true;
            }
        }
        line = next;
    }
    free(b.s);
}

static void load_bindings(void)
{
    Buffer b = {0};
    char *line;
    if (keys_disabled) return;
    if (read_config("keys.conf", &b) != 1) { free(b.s); return; }
    line = b.s;
    while (line && *line) {
        char *next = strchr(line, '\n'), *key, *action, *end;
        if (next) *next++ = 0;
        end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
        if (strlen(line) < 512 && !strncmp(line, "bind ", 5)) {
            key = line + 5;
            while (*key == ' ') key++;
            action = key;
            while (*action && *action != ' ' && *action != '\t') action++;
            if (*action) {
                *action++ = 0;
                while (*action == ' ' || *action == '\t') action++;
                if (*action) (void)binding_assign(key, action);
            }
        }
        line = next;
    }
    free(b.s);
}

static bool save_bindings(void)
{
    char *path = keymap_path(true), *tmp;
    size_t i;
    int fd = -1, dir_fd;
    struct stat st;
    bool ok = false;
    if (!path) return false;
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
            goto done;
    } else if (errno != ENOENT) goto done;
    tmp = malloc(strlen(path) + 16);
    if (!tmp) goto done;
    sprintf(tmp, "%s.XXXXXX", path);
    fd = mkstemp(tmp);
    if (fd < 0) goto temp_done;
    if (fchmod(fd, 0600) < 0 ||
        write_all(fd, "# editor user-defined keys; no built-in hotkeys\n",
                  strlen("# editor user-defined keys; no built-in hotkeys\n")) < 0) goto temp_done;
    for (i = 0; i < bind_count; i++) {
        char line[320];
        int len = snprintf(line, sizeof line, "bind %s %s\n", binds[i].name, binds[i].action);
        if (len < 0 || (size_t)len >= sizeof line || write_all(fd, line, (size_t)len) < 0)
            goto temp_done;
    }
    /* Do not lose the descriptor if synchronization fails before close. */
    if (sync_fd(fd) < 0) goto temp_done;
    if (close(fd) < 0) { fd = -1; goto temp_done; }
    fd = -1;
    if (rename(tmp, path) < 0) goto temp_done;
    { char *slash = strrchr(path, '/');
      if (slash) {
          *slash = 0; dir_fd = open(path, O_RDONLY);
          if (dir_fd >= 0) { (void)sync_fd(dir_fd); close(dir_fd); }
          *slash = '/';
      }
    }
    ok = true;
temp_done:
    if (fd >= 0) close(fd);
    if (!ok) unlink(tmp);
    free(tmp);
done:
    free(path);
    return ok;
}

static void open_viewer(const char *title, const char *contents);
static void show_bindings(void)
{
    size_t i, used = 0;
    int n;
    n = snprintf(binds_page, sizeof binds_page,
                 "USER HOTKEYS (none assigned by default)\n\n"
                 "bind KEY COMMAND   Assign and save\n"
                 "unbind KEY         Remove and save\n"
                 "bindings           Display assignments\n\n"
                 "Supports Ctrl+A-Z except C/H/I/J/M/Z, Alt+A-Z, F1-F12,\n"
                 "Ctrl/Alt+Up/Down/Left/Right (if terminal supports them).\n"
                 "Ctrl-C/Z belong to the terminal, never this keymap.\n"
                 "Hotkeys operate in the document, not while entering commands.\n"
                 "Settings: ~/.config/editor/keys.conf or XDG_CONFIG_HOME.\n\n");
    if (n < 0 || (size_t)n >= sizeof binds_page) return;
    used = (size_t)n;
    if (!bind_count) snprintf(binds_page + used, sizeof binds_page - used, "(no bindings)\n");
    for (i = 0; i < bind_count; i++) {
        n = snprintf(binds_page + used, sizeof binds_page - used, "%s -> %s\n",
                     binds[i].name, binds[i].action);
        if (n < 0 || (size_t)n >= sizeof binds_page - used) break;
        used += (size_t)n;
    }
    open_viewer("Bindings", binds_page);
}

static char *journal_path(const char *path)
{
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char *root, *work, *result, *p;
    size_t n;
    if (!path) return NULL;
    if (!base || *base != '/') {
        if (!home || *home != '/') return NULL;
        n = strlen(home) + sizeof "/.local/state";
        root = malloc(n);
        if (!root) return NULL;
        snprintf(root, n, "%s/.local/state", home);
    } else {
        root = strdup(base);
        if (!root) return NULL;
    }
    n = strlen(root) + sizeof "/editor/history";
    work = malloc(n);
    if (!work) { free(root); return NULL; }
    snprintf(work, n, "%s/editor/history", root);
    /* Create directory components with owner-only permissions as needed. */
    for (p = work + 1; *p; p++) if (*p == '/') {
        *p = 0;
        if (!private_directory(work)) { free(root); free(work); return NULL; }
        *p = '/';
    }
    if (!private_directory(work)) { free(root); free(work); return NULL; }
    {
        struct stat st;
        if (lstat(work, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 077)) {
            free(root); free(work); return NULL;
        }
    }
    n = strlen(work) + 1 + 16 + sizeof ".undo";
    result = malloc(n);
    if (result) snprintf(result, n, "%s/%016" PRIx64 ".undo", work, path_hash(path));
    free(root); free(work); return result;
}
static size_t undo_depth(void)
{
    return current ? current->ordinal : 0;
}
static size_t redo_depth(void)
{
    return history_tail ? history_tail->ordinal - undo_depth() : 0;
}
static void journal_load(void)
{
    char *path = journal_path(filename), *stored_path = NULL;
    Buffer state = {0};
    Change *head = NULL, *tail = NULL, *node = NULL;
    struct stat st, after;
    uint64_t path_n, snapshot_n, count, position, x[5];
    size_t i, bytes = 0;
    JournalInput in = {0};
    int fd = -1;
    bool ok = false, checked = false;
    loaded_journal_stat_valid = false;
    if (!path) return;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) goto done;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size < 0 || (uint64_t)st.st_size > JOURNAL_LIMIT)
        goto done;
    {
        char magic[8];
        if (read_exact(fd, magic, 8) < 0) goto done;
        checked = !memcmp(magic, "EDUNDO02", 8);
        if (!checked && memcmp(magic, "EDUNDO01", 8)) goto done;
        if (checked && !journal_checksum_ok(fd, st.st_size)) goto done;
    }
    in.fd = fd;
    if (journal_read_number(&in, &path_n) < 0 || path_n > 16384 ||
        journal_read_number(&in, &snapshot_n) < 0 || snapshot_n > LIMIT ||
        journal_read_number(&in, &count) < 0 || count > JOURNAL_CHANGE_LIMIT ||
        journal_read_number(&in, &position) < 0 || (position != UINT64_MAX && position >= count)) goto done;
    if (path_n != strlen(filename)) goto done;
    stored_path = malloc((size_t)path_n + 1);
    if (!stored_path || journal_read(&in, stored_path, (size_t)path_n) < 0) goto done;
    stored_path[path_n] = 0;
    if (strcmp(stored_path, filename)) goto done;
    if (!reserve(&state, (size_t)snapshot_n + 1) ||
        journal_read(&in, state.s, (size_t)snapshot_n) < 0) goto done;
    state.n = (size_t)snapshot_n; state.s[state.n] = 0;
    /* A private, structurally valid journal may precede an outside edit.
     * Its saved snapshot is the exact old side of one synthetic undo step. */
    if (memchr(state.s, 0, state.n)) goto done;
    bytes = (size_t)path_n + (size_t)snapshot_n + 40;
    for (i = 0; i < count; i++) {
        int j;
        node = calloc(1, sizeof *node);
        if (!node) goto done;
        for (j = 0; j < 5; j++) if (journal_read_number(&in, &x[j]) < 0 || x[j] > LIMIT) goto done;
        node->at = (size_t)x[0]; node->before = (size_t)x[1];
        node->after = (size_t)x[2]; node->old_n = (size_t)x[3];
        node->new_n = (size_t)x[4];
        if (node->at > LIMIT - node->old_n || node->at > LIMIT - node->new_n ||
            bytes > JOURNAL_LIMIT - 40 ||
            node->old_n > JOURNAL_LIMIT - bytes - 40 ||
            node->new_n > JOURNAL_LIMIT - bytes - node->old_n - 40) goto done;
        bytes += 40 + node->old_n + node->new_n;
        if (node->old_n && !(node->old_s = malloc(node->old_n))) goto done;
        if (node->new_n && !(node->new_s = malloc(node->new_n))) goto done;
        if ((node->old_n && journal_read(&in, node->old_s, node->old_n) < 0) ||
            (node->new_n && journal_read(&in, node->new_s, node->new_n) < 0)) goto done;
        if ((node->old_n && memchr(node->old_s, 0, node->old_n)) ||
            (node->new_n && memchr(node->new_s, 0, node->new_n))) goto done;
        node->prev = tail;
        node->ordinal = tail ? tail->ordinal + 1 : 1;
        if (tail) tail->next = node;
        else head = node;
        tail = node; node = NULL;
    }
    if (checked) { uint64_t trailer; if (journal_read_number(&in, &trailer) < 0) goto done; }
    /* Reject trailing bytes, including bytes already prefetched. */
    if (in.at != in.used) goto done;
    { char extra; if (read(fd, &extra, 1) != 0) goto done; }
    if (fstat(fd, &after) < 0 || !same_disk_version(&st, &after)) goto done;
    {
        bool outside = state.n != saved.n ||
                       (state.n && memcmp(state.s, saved.s, state.n));
        Change *place = position == UINT64_MAX ? NULL : head;
        Change *external = NULL;
        if (place) for (i = 0; i < position; i++) place = place->next;
        if (outside) {
            /* No existing file to compare with means a removed/recreated path,
             * not a safely identified external content change. */
            if (!existed) goto done;
            /* Trim common prefix and suffix, preserving one undo operation
             * without copying two entire files for a tiny outside edit. */
            {
                size_t prefix = 0, suffix = 0;
                size_t common = state.n < saved.n ? state.n : saved.n;
                while (prefix < common && state.s[prefix] == saved.s[prefix]) prefix++;
                while (suffix < common - prefix &&
                       state.s[state.n - 1 - suffix] == saved.s[saved.n - 1 - suffix]) suffix++;
                external = calloc(1, sizeof *external);
                if (!external) goto done;
                external->at = prefix;
                external->old_n = state.n - prefix - suffix;
                external->new_n = saved.n - prefix - suffix;
                if (external->old_n) external->old_s = malloc(external->old_n);
                if (external->new_n) external->new_s = malloc(external->new_n);
                if ((external->old_n && !external->old_s) ||
                    (external->new_n && !external->new_s)) {
                    free(external->old_s); free(external->new_s); free(external);
                    goto done;
                }
                if (external->old_n)
                    memcpy(external->old_s, state.s + prefix, external->old_n);
                if (external->new_n)
                    memcpy(external->new_s, saved.s + prefix, external->new_n);
                external->before = prefix;
                external->after = prefix + external->new_n;
            }
            /* The file's prior redo tail is a different branch. Archive the
             * old journal on the next successful save if it is truncated. */
            journal_branched = place ? place->next != NULL : head != NULL;
            if (place) { free_changes(place->next); place->next = external; }
            else { free_changes(head); head = external; }
            external->prev = place;
            external->ordinal = place ? place->ordinal + 1 : 1;
            tail = external;
            place = external;
            journal_rebased = true;
        }
        /* Check all reconstructed lengths and cursor offsets before making
         * a journal live. A damaged journal must not leave an impossible
         * history state waiting to crash during later navigation. These are
         * O(transaction count) checks, without replaying large file buffers. */
        {
            Change *q;
            size_t length = journal_rebased ? saved.n : state.n, oldest_length;
            for (q = place; q; q = q->prev) {
                size_t previous;
                if (q->at > length || q->new_n > length - q->at ||
                    q->old_n > LIMIT - (length - q->new_n) ||
                    q->after > length) goto done;
                previous = length - q->new_n + q->old_n;
                if (q->before > previous) goto done;
                length = previous;
            }
            oldest_length = length;
            for (q = head; q; q = q->next) {
                size_t following;
                if (q->at > length || q->old_n > length - q->at ||
                    q->new_n > LIMIT - (length - q->old_n) ||
                    q->before > length) goto done;
                following = length - q->old_n + q->new_n;
                if (q->after > following) goto done;
                length = following;
                if (q == place && length != (journal_rebased ? saved.n : state.n)) goto done;
            }
            if (!place && oldest_length != (journal_rebased ? saved.n : state.n)) goto done;
        }
        history = head; head = NULL;
        current = place;
        history_tail = tail;
    }
    loaded_journal_path = strdup(path);
    loaded_journal_stat = after;
    loaded_journal_stat_valid = loaded_journal_path != NULL;
    /* A subsequent ordinary write upgrades a legacy unchecked journal. */
    journal_dirty = journal_rebased || !checked;
    ok = true;
done:
    if (fd >= 0) close(fd);
    free(node ? node->old_s : NULL); free(node ? node->new_s : NULL); free(node);
    free_changes(head); free(stored_path); free(state.s); free(path);
    if (!ok && fd >= 0) {
        journal_ignored = true;
        journal_rebased = journal_branched = false;
    }
}
static bool journal_save(void)
{
    char *path = journal_path(filename), *tmp = NULL, *dir = NULL, *slash;
    char *archived = NULL;
    bool archived_linked = false, archive_old = false;
    Change *p;
    size_t count = 0, bytes = 0, path_n;
    uint64_t current_position = UINT64_MAX;
    JournalOutput out = {0};
    int fd = -1, dir_fd = -1, old_fd = -1;
    struct stat st, written, check;
    bool ok = false, old_present = false;
    if (!path) return false;
    path_n = strlen(filename);
    if (path_n > 16384 || text.n > LIMIT) goto done;
    for (p = history; p; p = p->next) {
        if (p == current) current_position = (uint64_t)count;
        if (++count > JOURNAL_CHANGE_LIMIT || bytes > JOURNAL_LIMIT - 40 ||
            p->old_n > JOURNAL_LIMIT - bytes - 40 ||
            p->new_n > JOURNAL_LIMIT - bytes - p->old_n - 40) goto done;
        bytes += 40 + p->old_n + p->new_n;
    }
    if (bytes > JOURNAL_LIMIT - text.n - path_n - 48) goto done;
    /* Preserve an old, incompatible journal rather than destroying past work. */
    old_fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (old_fd >= 0) {
        if (fstat(old_fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 077)) goto done;
        /* Leave the current journal untouched until the replacement is ready.
         * A failed write must not strand it under a stale-only name. */
        old_present = true;
        archive_old = journal_branched || journal_rebased ||
                      !loaded_journal_path || strcmp(loaded_journal_path, path) ||
                      !loaded_journal_stat_valid ||
                      !same_disk_version(&loaded_journal_stat, &st);
    } else if (errno != ENOENT) goto done;
    if (old_fd >= 0) { close(old_fd); old_fd = -1; }
    tmp = malloc(strlen(path) + 24);
    if (!tmp) goto done;
    snprintf(tmp, strlen(path) + 24, "%s.XXXXXX", path);
    fd = mkstemp(tmp);
    if (fd < 0) goto done;
    out.fd = fd; out.hash = JOURNAL_HASH_INITIAL;
    if (fchmod(fd, 0600) < 0 || journal_write(&out, "EDUNDO02", 8) < 0 ||
        journal_number(&out, (uint64_t)path_n) < 0 ||
        journal_number(&out, (uint64_t)text.n) < 0 ||
        journal_number(&out, (uint64_t)count) < 0 ||
        journal_number(&out, current_position) < 0 ||
        journal_write(&out, filename, path_n) < 0 ||
        journal_write(&out, text.s, text.n) < 0) goto done;
    for (p = history; p; p = p->next) {
        if (journal_number(&out, p->at) < 0 ||
            journal_number(&out, p->before) < 0 ||
            journal_number(&out, p->after) < 0 ||
            journal_number(&out, p->old_n) < 0 ||
            journal_number(&out, p->new_n) < 0 ||
            (p->old_n && journal_write(&out, p->old_s, p->old_n) < 0) ||
            (p->new_n && journal_write(&out, p->new_s, p->new_n) < 0)) goto done;
    }
    if (journal_flush(&out) < 0 || put_number(fd, out.hash) < 0 || sync_fd(fd) < 0 ||
        fstat(fd, &written) < 0) goto done;
    if (close(fd) < 0) { fd = -1; goto done; }
    fd = -1;
    /* Another process may have saved while this journal was being staged.
     * Do not overwrite that intervening version; retain the dirty history
     * in memory so a later write can retry and archive it. */
    if (old_present) {
        if (lstat(path, &check) < 0 || !same_disk_version(&st, &check)) {
            errno = EAGAIN; goto done;
        }
    }
    if (archive_old) {
        archived = malloc(strlen(path) + 80);
        if (!archived) goto done;
        /* Several branches can occur in one second in a single process.
         * A unique suffix keeps all old branches without blocking writes. */
        {
            unsigned attempt;
            for (attempt = 0; attempt < 10000; attempt++) {
                snprintf(archived, strlen(path) + 80, "%s.stale.%ld.%ld.%u", path,
                         (long)time(NULL), (long)getpid(), attempt);
                if (link(path, archived) == 0) { archived_linked = true; break; }
                if (errno != EEXIST) goto done;
            }
            if (!archived_linked) { errno = EEXIST; goto done; }
        }
    }
    if (old_present) {
        if (rename(tmp, path) < 0) goto done;
    } else {
        /* Do not replace a journal created after the initial absence check. */
        if (link(tmp, path) < 0) goto done;
        (void)unlink(tmp);
    }
    /* Successful replacement leaves the old journal reachable through stale. */
    archived_linked = false;
    free(loaded_journal_path);
    loaded_journal_path = strdup(path);
    loaded_journal_stat_valid = loaded_journal_path && lstat(path, &check) == 0 &&
        check.st_dev == written.st_dev && check.st_ino == written.st_ino &&
        check.st_size == written.st_size &&
        check.st_mtim.tv_sec == written.st_mtim.tv_sec &&
        check.st_mtim.tv_nsec == written.st_mtim.tv_nsec;
    if (loaded_journal_stat_valid) loaded_journal_stat = check;
    slash = strrchr(path, '/');
    if (slash) {
        dir = strdup(path);
        if (dir) {
            dir[slash - path] = 0;
            dir_fd = open(dir, O_RDONLY);
            if (dir_fd >= 0) (void)sync_fd(dir_fd);
        }
    }
    journal_dirty = false;
    journal_branched = false;
    journal_rebased = false;
    ok = true;
done:
    if (dir_fd >= 0) close(dir_fd);
    if (old_fd >= 0) close(old_fd);
    if (fd >= 0) close(fd);
    if (!ok && tmp) (void)unlink(tmp);
    if (archived_linked) (void)unlink(archived);
    free(archived); free(tmp); free(dir); free(path);
    return ok;
}

static void save_to_stdout(void)
{
    if (isatty(STDOUT_FILENO)) {
        tell("write - requires redirected stdout, not the screen.");
        return;
    }
    if (write_all(STDOUT_FILENO, text.s ? text.s : "", text.n) < 0) {
        tell("Pipe write failed: %s. Buffer retained.", strerror(errno));
        return;
    }
    /* A stream-only document has successfully been emitted; a named file has
     * NOT been saved to disk and must remain marked as modified. */
    if (!filename && !copy_buffer(&saved, text.s, text.n)) {
        tell("Output sent; not enough memory to mark saved.");
        return;
    }
    tell("Text sent to stdout (%zu bytes).", text.n);
}

/* Approved overwrites are valid for one exact observed disk version,
 * never blanket permission to overwrite future changes. */
static char *state_path(const char *leaf, bool create);
static char *clipboard_path(bool create);
static bool clipboard_archive(const Buffer *before);
static void clipboard_return(void);
static void save_file(const char *name, bool approved)
{
    char *path = NULL, *temporary = NULL, *slash;
    Buffer snapshot = {0}, disk = {0}, again = {0};
    Change *to_outside = NULL, *to_editor = NULL;
    struct stat st, check, written;
    bool present, same, other_file, committed = false, outside = false;
    bool clipboard_written = false, warn_history = false, warn_archive = false;
    bool warn_dirsync = false;
    int fd = -1, permission_fd = -1, dir_fd = -1, error;
    mode_t mode, mask;
    if (name && !strcmp(name, "-")) { save_to_stdout(); return; }
    if (!name || !*name) {
        if (!filename) { tell("Filename required. Enter write FILE, or choose Write in the menu."); return; }
        name = filename;
    }
    path = resolve_path(name);
    if (!path) goto failure;
    /* Clipboard is a temporary sub-editor. Never let a copy-name write
     * silently change which file quit will return from; write is for the
     * clipboard.txt itself. Exit first to save another document. */
    if (editing_clipboard && (!filename || strcmp(path, filename))) {
        tell("Clipboard mode: write saves clipboard.txt; other paths refused.");
        goto done;
    }
    same = filename && !strcmp(filename, path);
    other_file = !same || !existed;
    present = lstat(path, &st) == 0;
    if (!present && errno != ENOENT) goto failure;
    if (present) {
        if (!S_ISREG(st.st_mode)) {
            tell("Destination is not a regular file; write refused.");
            goto done;
        }
        if (!read_file(path, &disk, &st)) goto failure;
        if (disk.n && memchr(disk.s, 0, disk.n)) {
            tell("Outside file contains NUL bytes. Write refused; use write FILE.");
            goto done;
        }
        if (st.st_nlink != 1) {
            tell("File has multiple hard links. Use write FILE for a new path."); goto done;
        }
        if (editing_clipboard &&
            (st.st_uid != geteuid() || (st.st_mode & 077))) {
            tell("Clipboard file is not private; save refused."); goto done;
        }
        outside = other_file || !disk_matches(&saved, &original, &disk, &st);
        /* clipboard.txt is a private scratch buffer, not a normal edited
         * document. Auto-save may replace a newer clipboard without asking.
         * Ordinary files keep the full overwrite-confirmation behavior. */
        if (outside && !editing_clipboard &&
            (!approved || !disk_matches(&conflict_snapshot, &conflict_stat, &disk, &st))) {
            ask_overwrite(&disk, &st, path, other_file); goto done;
        }
        if (!outside && !changed()) {
            if (journal_dirty && history && !journal_save())
                tell("Unchanged; undo history could not be persisted.");
            else {
                tell("Already saved (unchanged).");
                clipboard_written = editing_clipboard;
            }
            goto done;
        }
        /* Allocate the external-content recovery steps before committing.
         * If insufficient memory, leave the outside writer's data untouched. */
        if (outside && !same_bytes(&disk, &text)) {
            size_t remote_cursor = cursor < disk.n ? cursor : disk.n;
            to_outside = difference(&text, &disk, cursor, remote_cursor);
            if (to_outside) to_editor = difference(&disk, &text, remote_cursor, cursor);
            if (!to_outside || !to_editor) {
                tell("Cannot preserve external changes in undo. Write cancelled."); goto done;
            }
        }
        permission_fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        if (permission_fd < 0) goto failure;
        if (fstat(permission_fd, &check) < 0) goto failure;
        if (!same_disk_version(&check, &st)) {
            tell("File changed during write preparation. Try write again."); goto done;
        }
        mode = editing_clipboard ? 0600 : st.st_mode & 0777;
    } else {
        if (same && existed && !editing_clipboard) {
            tell("File was removed on disk. Use write FILE for a new path."); goto done;
        }
        mask = umask(0); umask(mask);
        mode = editing_clipboard ? 0600 : 0666 & ~mask;
    }
    if (!copy_buffer(&snapshot, text.s, text.n)) { errno = ENOMEM; goto failure; }
    temporary = malloc(strlen(path) + 32);
    if (!temporary) goto failure;
    strcpy(temporary, path);
    slash = strrchr(temporary, '/');
    strcpy(slash + 1, ".editor.XXXXXX");
    fd = mkstemp(temporary);
    if (fd < 0) goto failure;
    if (fstat(fd, &written) < 0) goto failure;
    if (present && (written.st_uid != st.st_uid || written.st_gid != st.st_gid) &&
        fchown(fd, st.st_uid, st.st_gid) < 0) goto failure;
    if (fchmod(fd, mode) < 0 || write_all(fd, text.s, text.n) < 0 || sync_fd(fd) < 0)
        goto failure;
    if (fstat(fd, &written) < 0) goto failure;
    if (close(fd) < 0) { fd = -1; goto failure; }
    fd = -1;
    if (present) {
        /* Re-read after preparing the temporary file. A later outside write
         * requires a new approval, never the old one. */
        if (!read_file(path, &again, &check)) goto failure;
        if (!disk_matches(&disk, &st, &again, &check)) {
            if (editing_clipboard)
                tell("Clipboard changed during save; retry clipboard to return.");
            else if (check.st_nlink == 1)
                ask_overwrite(&again, &check, path, other_file);
            else tell("File now has multiple hard links. Write cancelled.");
            goto done;
        }
        if (rename(temporary, path) < 0) goto failure;
    } else {
        /* link() creates the name without replacing a concurrently created file. */
        if (link(temporary, path) < 0) goto failure;
        (void)unlink(temporary);
    }
    committed = true;
    free(filename); filename = path; path = NULL;
    free(saved.s); saved = snapshot; memset(&snapshot, 0, sizeof snapshot);
    /* rename may update the new inode's ctime. Snapshot metadata after
     * replacement, otherwise our own next write would look external. */
    original = written;
    if (lstat(filename, &check) == 0 && check.st_dev == written.st_dev &&
        check.st_ino == written.st_ino) original = check;
    existed = true;
    if (to_outside && to_editor) {
        remember_overwrite(to_outside, to_editor);
        to_outside = to_editor = NULL;
    }
    *slash = 0;
    dir_fd = open(*temporary ? temporary : "/", O_RDONLY);
    if (dir_fd < 0 || sync_fd(dir_fd) < 0) {
        warn_dirsync = true;
        tell("Written; directory synchronization was unavailable.");
    } else tell("Written.");
    if (!journal_save()) {
        warn_history = true;
        tell("Written; undo history could not be persisted.");
    }
    if (clip_history_enabled && present && !same_bytes(&disk,&text)) {
        char *clip_path=clipboard_path(false);
        if (clip_path) {
            if (!strcmp(clip_path,filename) && !clipboard_archive(&disk)) {
                warn_archive = true;
                tell("Written; clipboard history save/cleanup incomplete.");
            }
            free(clip_path);
        }
    }
    clipboard_written = editing_clipboard;
    goto done;
failure:
    error = errno;
    tell("Write failed: %s. Buffer retained.", strerror(error));
done:
    if (fd >= 0) (void)close(fd);
    if (permission_fd >= 0) (void)close(permission_fd);
    if (dir_fd >= 0) (void)close(dir_fd);
    if (temporary && !committed) (void)unlink(temporary);
    free(path); free(temporary); free(snapshot.s); free(disk.s); free(again.s);
    free_changes(to_outside); free_changes(to_editor);
    /* An ordinary write only saves. A clipboard toggle also returns to the
     * parent, but ONLY after a successful clipboard save. No confirmation. */
    if (editing_clipboard && clipboard_leave_after_save && prompt == PROMPT_NONE) {
        clipboard_leave_after_save = false;
        if (clipboard_written && !terminated) {
            clipboard_return();
            if (warn_history || warn_archive || warn_dirsync)
                tell("Clipboard saved; returned (history/archive/sync warning).");
            else tell("Clipboard saved; returned to %s.", filename ? filename : "[new file]");
        } else if (!message[0]) {
            tell("Clipboard save incomplete; still editing clipboard.txt.");
        } /* Keep the specific save failure visible when one was reported. */
    }
}

static void reset_screen(void)
{
    int i;
    for (i = 0; i < previous_count; i++) free(previous_rows[i]);
    free(previous_rows); previous_rows = NULL; previous_count = 0;
}

static void mouse_tracking(bool enable)
{
    static const char on[] = "\033[?1000h\033[?1006h";
    static const char off[] = "\033[?1000l\033[?1006l";
    if (terminal_active)
        (void)write_all(tty_out, enable ? on : off,
                        enable ? sizeof on - 1 : sizeof off - 1);
}

static void terminal_start(void)
{
    struct termios raw = initial_termios;
    int result, error;
    static const char begin[] = "\033[?1049h\033[?2004h\033[2J";
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ICRNL | INLCR | IGNCR | INPCK | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag = (raw.c_cflag & (tcflag_t)~(CSIZE | PARENB)) | CS8;
    /* Keep the caller's ISIG, VINTR, VQUIT and VSUSP settings unchanged.
     * The terminal, not an editor key binding, generates interrupt/stop.
     * Deliberately disabled or remapped terminal controls stay that way. */
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    terminal_active = true; /* Cleanup owns even a partially applied change. */
    do {
        result = tcsetattr(tty_in, TCSANOW, &raw);
    } while (result < 0 && errno == EINTR &&
             !(terminated || suspended || interrupted));
    if (result < 0) {
        error = errno;
        terminal_restore();
        if (error == EINTR && (terminated || suspended || interrupted)) return;
        die(strerror(error));
    }
    reset_screen(); resized = 1;
    if (write_all(tty_out, begin, sizeof begin - 1) < 0) {
        /* A signal during startup/resume is serviced by the main loop;
         * do not turn a harmless second suspend into a fatal I/O error. */
        if (errno == EINTR && (terminated || suspended || interrupted)) {
            terminal_restore();
            return; /* main retries full setup after servicing the signal */
        }
        die(strerror(errno));
    }
    mouse_tracking(mouse_enabled);
    if (terminated || suspended || interrupted) terminal_restore();
}

static void signal_handler(int sig)
{
    if (sig == SIGWINCH) resized = 1;
    else if (sig == SIGINT) {
        if (signal_controls) interrupted = 1;
        else terminated = SIGINT;
    }
    else if (sig == SIGTSTP) suspended = 1;
    else if (sig == SIGCONT) resized = 1;
    else terminated = sig;
}

static void install_handler(int sig, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handler; sigemptyset(&action.sa_mask);
    if (sigaction(sig, &action, NULL) < 0) die(strerror(errno));
}

static void dimensions(void)
{
    struct winsize ws;
    if (ioctl(tty_out, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row ? ws.ws_row : 24;
        cols = ws.ws_col ? ws.ws_col : 80;
    }
    if (rows > 1000) rows = 1000;
    if (cols > 4096) cols = 4096;
    reset_screen(); resized = 0;
}

static void visible(Buffer *b, const char *s, size_t n, size_t from, size_t width)
{
    size_t p = 0, x = 0, count, a, z, k;
    int cells; bool wide; char rep[32];
    b->n = 0;
    if (b->s) b->s[0] = 0;
    while (p < n && x < from + width) {
        count = representation(s, n, p, x, rep, &cells, &wide);
        a = x > from ? x : from;
        z = x + (size_t)cells < from + width ? x + (size_t)cells : from + width;
        if (z > a) {
            if (wide && a == x && z == x + (size_t)cells) output(b, rep);
            else if (wide) { for (k = a; k < z; k++) output(b, " "); }
            else add(b, rep + (a - x), z - a);
        }
        x += (size_t)cells; p += count;
    }
}

static void visible_match(Buffer *b, const char *s, size_t n, size_t from,
                          size_t width, size_t match_from, size_t match_to)
{
    size_t p=0,x=0,count,a,z,k;
    int cells; bool wide, active=false, within;
    char rep[32];
    b->n=0;
    if (b->s) b->s[0]=0;
    while (p<n && x<from+width) {
        count=representation(s,n,p,x,rep,&cells,&wide);
        a=x>from?x:from;
        z=x+(size_t)cells<from+width?x+(size_t)cells:from+width;
        within=p<match_to && p+count>match_from;
        if (z>a) {
            if (within!=active) {
                output(b,within ? "\033[7m" : "\033[0m");
                active=within;
            }
            if (wide && a==x && z==x+(size_t)cells) output(b,rep);
            else if (wide) { for (k=a;k<z;k++) output(b," "); }
            else add(b,rep+(a-x),z-a);
        }
        x+=(size_t)cells; p+=count;
    }
    if (active) output(b,"\033[0m");
}

static void draw_row(Buffer *out, int row, Buffer *line, bool reverse)
{
    Buffer row_text = {0};
    char address[48];
    output(&row_text, reverse ? "\033[7m" : "\033[0m");
    if (line->n) add(&row_text, line->s, line->n);
    output(&row_text, "\033[0m\033[K");
    if (drawing_failed) { free(row_text.s); return; }
    if (!previous_rows[row] || strcmp(previous_rows[row], row_text.s)) {
        snprintf(address, sizeof address, "\033[%d;1H", row + 1);
        output(out, address); add(out, row_text.s, row_text.n);
        free(previous_rows[row]); previous_rows[row] = row_text.s;
    } else free(row_text.s);
}

/* Wrap read-only pages in terminal columns. Besides the ASCII manual,
 * these pages contain pathnames and command arguments with UTF-8 or tabs.
 * Never cut a code point, or skip bytes just because their display is wide. */
static size_t viewer_row(size_t start, size_t width, size_t *end)
{
    size_t p = start, columns = 0, split = start, count;
    char rep[32]; int cells; bool wide;
    if (!width) width = 1;
    while (viewer_text[p] && viewer_text[p] != '\n') {
        count = representation(viewer_text + p, strnlen(viewer_text + p, 4),
                               0, columns, rep, &cells, &wide);
        if ((size_t)cells > width - columns) break;
        columns += (size_t)cells;
        p += count;
        if (viewer_text[p - 1] == ' ' || viewer_text[p - 1] == '\t') split = p;
        if (columns == width) break;
    }
    if (viewer_text[p] && viewer_text[p] != '\n' && split > start) p = split;
    /* Viewers are hidden on undersized terminals, but scrolling must still
     * make progress at widths smaller than a single escaped character. */
    if (p == start && viewer_text[p] && viewer_text[p] != '\n') {
        uint32_t cp;
        p += decode(viewer_text + p, strnlen(viewer_text + p, 4), 0, &cp);
    }
    *end = p;
    return viewer_text[p] == '\n' ? p + 1 : p;
}

static size_t viewer_rows(size_t width)
{
    size_t p = 0, end, count = 0;
    while (viewer_text[p]) { p = viewer_row(p, width, &end); count++; }
    return count ? count : 1;
}

static void draw_viewer(Buffer *out, Buffer *line)
{
    size_t p = 0, end, next, row = 0, width = (size_t)(cols - 1);
    size_t total = viewer_rows(width), body = (size_t)(rows - 2);
    size_t maximum = total > body ? total - body : 0;
    int screen_row = 1;
    char heading[96], footer[160];
    if (viewer_top > maximum) viewer_top = maximum;
    snprintf(heading, sizeof heading, "%s | editor " VERSION, viewer_title);
    visible(line, heading, strlen(heading), 0, width);
    draw_row(out, 0, line, true);
    while (viewer_text[p] && screen_row < rows - 1) {
        next = viewer_row(p, width, &end);
        if (row++ >= viewer_top) {
            visible(line, viewer_text + p, end - p, 0, width);
            draw_row(out, screen_row++, line, false);
        }
        p = next;
    }
    while (screen_row < rows - 1) {
        line->n = 0; draw_row(out, screen_row++, line, false);
    }
    snprintf(footer, sizeof footer,
             "Esc/q back | %zu/%zu | j/k rows h/l pages",
             viewer_top + 1, total);
    visible(line, footer, strlen(footer), 0, width);
    draw_row(out, rows - 1, line, true);
}

/* The root stays short; secondary pages contain every operation.
 * Each row is either an executable command, a parameter prompt, or a subpage.
 * The data table is also the source of menu numbering and mouse hit testing. */
enum { MENU_ROOT, MENU_FILE, MENU_EDIT, MENU_MOVE, MENU_HISTORY, MENU_CONTROL, MENU_HELP, MENU_EXIT };
enum { MENU_COMMAND, MENU_SUBPAGE, MENU_WRITE, MENU_PARAMETER,
       MENU_MOUSE_TOGGLE, MENU_ESC_TOGGLE, MENU_QUIT_FORCE };
typedef struct {
    const char *label;
    int action;
    const char *command;
    const char *question;
    int child;
} MenuItem;
#define ITEM(label,cmd) { label, MENU_COMMAND, cmd, NULL, 0 }
#define GROUP(label,page) { label, MENU_SUBPAGE, NULL, NULL, page }
#define ASK(label,cmd,prompt) { label, MENU_PARAMETER, cmd, prompt, 0 }
#define SPECIAL(label,type) { label, type, NULL, NULL, 0 }
static const MenuItem menu_root_items[] = {
    SPECIAL("Write", MENU_WRITE), GROUP("Files >", MENU_FILE), GROUP("Edit >", MENU_EDIT),
    GROUP("Move >", MENU_MOVE), GROUP("History >", MENU_HISTORY),
    GROUP("Controls >", MENU_CONTROL), GROUP("Help >", MENU_HELP),
    ITEM("Command line", "@cli-prompt"), GROUP("Exit >", MENU_EXIT)
};
static const MenuItem menu_file_items[] = {
    ASK("Read file...", "read", "Open file> "),
    ASK("Browse folder...", "ls", "Folder> "),
    ITEM("Edit clipboard", "clipboard"),
    ASK("Select clipboard...", "clipboard", "Slot/default> "),
    ITEM("List clipboards", "clipboard list"),
    ITEM("Clipboard history", "cliphistory"),
    ITEM("History on", "cliphistory on"),
    ITEM("History off", "cliphistory off"),
    ASK("Restore clip...", "clipuse", "Clip entry #> ")
};
/* The clipboard has a shorter file menu; hide entries that cannot operate
 * on its temporary buffer, and make discard explicitly available. */
static const MenuItem menu_clipboard_items[] = {
    ITEM("Return to document", "clipboard"),
    ITEM("Write clipboard", "write"),
    ASK("Switch clipboard...", "clipboard", "Slot/default> "),
    ITEM("List clipboards", "clipboard list"),
    ASK("Browse folder...", "ls", "Folder> "),
    ITEM("Clipboard history", "cliphistory"),
    ITEM("History on", "cliphistory on"),
    ITEM("History off", "cliphistory off")
};
static const MenuItem menu_edit_items[] = {
    ITEM("Undo", "undo"), ITEM("Redo", "redo"),
    ITEM("Delete to line end", "deleol"),
    ITEM("Delete to file end", "deleof"),
    ITEM("Backspace to file start", "backbof"),
    ASK("Replace...", "replace", "Find text> "),
    ASK("Replace all...", "replaceall", "Find text> "),
    ASK("Undo N...", "undo", "Undo count> "),
    ASK("Redo N...", "redo", "Redo count> "),
    ITEM("Delete line", "dline"),
    ITEM("Copy line", "copy"), ITEM("Cut line", "cut"),
    ITEM("Paste lines", "paste"),
    ASK("Paste slot...", "paste", "Clipboard slot> "),
    ASK("Copy range...", "copy", "Line/range> "),
    ASK("Cut range...", "cut", "Line/range> "),
    ASK("Delete range...", "dline", "Line/range> "),
    ASK("Delete N...", "@count-dline", "Delete count> ")
};
static const MenuItem menu_move_items[] = {
    ASK("Find text...", "find", "Find> "),
    ITEM("Next match", "next"), ITEM("Previous match", "prev"),
    ASK("Go to line...", "goto", "Line/col> "),
    ITEM("Top", "top"), ITEM("Bottom", "bottom"),
    ASK("Left N...", "left", "Left count> "),
    ASK("Right N...", "right", "Right count> "),
    ASK("Up N...", "up", "Up count> "),
    ASK("Down N...", "down", "Down count> "),
    ITEM("Page up", "pageup"), ITEM("Page down", "pagedown")
};
static const MenuItem menu_history_items[] = {
    ITEM("Reverse", "reverse"),
    ASK("Reverse index...", "reverse", "Register #> "),
    ITEM("History counts", "history")
};
static const MenuItem menu_control_items[] = {
    ITEM("Movement", "movement"), ITEM("Edit", "edit"),
    SPECIAL("Mouse on/off", MENU_MOUSE_TOGGLE),
    ITEM("Signal handling", "signals toggle"),
    ITEM("Key bindings", "bindings"),
    ASK("Bind key...", "bind", "Key command> "),
    ASK("Unbind key...", "unbind", "Key name> "),
    SPECIAL("Esc: CLI/menu", MENU_ESC_TOGGLE),
    ITEM("Write to pipe", "write -"),
    ITEM("Suspend", "suspend")
};
static const MenuItem menu_help_items[] = {
    ITEM("Getting started", "help"), ITEM("Commands by task", "list"),
    ASK("Find a command...", "list", "Task or word> "),
    ASK("Explain a command...", "help", "Command or topic> "),
    ITEM("Manual", "manual"), ITEM("About", "about")
};
static const MenuItem menu_exit_items[] = {
    ITEM("Quit", "quit"), SPECIAL("Discard & quit", MENU_QUIT_FORCE)
};
#undef ITEM
#undef GROUP
#undef ASK
#undef SPECIAL
static const MenuItem *menu_entries(size_t *count, const char **title)
{
    const MenuItem *entry;
    switch (menu_page) {
    case MENU_FILE:
        if (editing_clipboard) {
            entry = menu_clipboard_items;
            *count = sizeof menu_clipboard_items / sizeof *entry;
            *title="Clipboard";
        } else {
            entry = menu_file_items;
            *count = sizeof menu_file_items / sizeof *entry;
            *title="Files";
        }
        break;
    case MENU_EDIT: entry = menu_edit_items; *count = sizeof menu_edit_items / sizeof *entry; *title="Edit"; break;
    case MENU_MOVE: entry = menu_move_items; *count = sizeof menu_move_items / sizeof *entry; *title="Move"; break;
    case MENU_HISTORY: entry = menu_history_items; *count = sizeof menu_history_items / sizeof *entry; *title="History"; break;
    case MENU_CONTROL: entry = menu_control_items; *count = sizeof menu_control_items / sizeof *entry; *title="Controls"; break;
    case MENU_HELP: entry = menu_help_items; *count = sizeof menu_help_items / sizeof *entry; *title="Help"; break;
    case MENU_EXIT: entry = menu_exit_items; *count = sizeof menu_exit_items / sizeof *entry; *title="Exit"; break;
    default: entry = menu_root_items; *count = sizeof menu_root_items / sizeof *entry; *title="Menu"; break;
    }
    return entry;
}
static void menu_enter(int page)
{
    /* A mouse click can enter from a filename/find/replace prompt. Its
     * pending action must not survive and consume a later CLI command. */
    command_mode = filename_mode = filename_read_mode = false;
    menu_argument_mode = false;
    menu_argument_command = menu_argument_label = NULL;
    command[0] = 0; command_n = command_cursor = 0;
    history_choice = -1; group = false;
    menu_page = page;
    menu_selection = menu_first = 0;
    menu_number = -1; menu_discard_pending = false;
    menu_open = true;
}
static void menu_back(void)
{
    menu_number = -1; menu_discard_pending = false;
    if (menu_page == MENU_ROOT) menu_open = false;
    else {
        int previous_page=menu_page;
        size_t i;
        menu_enter(MENU_ROOT);
        for (i=0; i<sizeof menu_root_items/sizeof menu_root_items[0]; i++)
            if (menu_root_items[i].action==MENU_SUBPAGE &&
                menu_root_items[i].child==previous_page) {
                menu_selection=i; break;
            }
    }
}
static void draw_menu(Buffer *out, Buffer *line, int body)
{
    size_t count, capacity = body > 1 ? (size_t)body - 1 : 1, i;
    const char *title;
    const MenuItem *entries=menu_entries(&count, &title);
    int row=1;
    char label[160], item[160];
    if (menu_selection >= count) menu_selection = count - 1;
    if (menu_selection < menu_first) menu_first = menu_selection;
    if (menu_selection >= menu_first + capacity) menu_first = menu_selection - capacity + 1;
    if (menu_discard_pending)
        snprintf(label,sizeof label,cols<27 ? "Enter again" : "Discard changes? Enter again to confirm");
    else if (cols < 27)
        snprintf(label,sizeof label,"%s %zu/%zu",title,menu_selection+1,count);
    else
        snprintf(label,sizeof label,"%s %zu/%zu | j/k or arrows; h/Esc back", title,menu_selection+1,count);
    visible(line,label,strlen(label),0,(size_t)(cols - 1));
    draw_row(out,0,line,true);
    for (i=menu_first; i<count && i<menu_first+capacity && row<body; i++, row++) {
        const char *name=entries[i].label;
        if (entries[i].command && !strcmp(entries[i].command, "signals toggle"))
            name = cols < 27 ? (signal_controls ? "Sig ON" : "Sig OFF") :
                   (signal_controls ? "Signal handling: ON" : "Signal handling: OFF");
        snprintf(item,sizeof item,"%c%zu %s",i==menu_selection ? '>' : ' ',i+1,name);
        visible(line,item,strlen(item),0,(size_t)(cols - 1));
        draw_row(out,row,line,i==menu_selection);
    }
    while (row<body) { line->n=0; draw_row(out,row++,line,false); }
}

static void draw(void)
{
    Buffer out = {0}, line = {0}, status = {0};
    size_t start, p, end, col, line_number = 1, command_column = 0;
    size_t width, name_width;
    int r, cursor_row = 0, body;
    bool dirty;
    const char *name;
    char info[160], address[64];
    drawing_failed = false;
    if (resized) dimensions();
    body = rows > 2 ? rows - 2 : 1;
    if (!previous_rows) {
        previous_rows = calloc((size_t)rows, sizeof *previous_rows);
        if (!previous_rows) { drawing_failed = true; goto finish; }
        previous_count = rows;
    }
    output(&out, "\033[?25l");
    if (rows < 3 || cols < 12) {
        visible(&line, "Window too small", 16, 0, (size_t)(cols > 1 ? cols - 1 : 0));
        draw_row(&out, 0, &line, false);
        output(&out, "\033[1;1H\033[?25h");
        goto finish;
    }
    if (viewer_text) { draw_viewer(&out, &line); goto finish; }
    if (menu_open) {
        const char *hint;
        /* Draw the overlay directly. Repainting the document underneath every
         * menu keystroke doubles output and can flicker over slow terminals. */
        draw_menu(&out,&line,body<=1 ? 2 : body);
        width=(size_t)(cols-1);
        if (body<=1) {
            visible(&line,"j/k l h",7,0,width);
            draw_row(&out,rows-1,&line,false);
            output(&out,"\033[?25l");
            goto finish;
        }
        dirty=changed();
        name=filename ? strrchr(filename,'/')+1 : "[new file]";
        visible(&line,name,strlen(name),0,width > (dirty ? 1u : 0u) ?
                width - (dirty ? 1u : 0u) : 0);
        if (dirty) output(&line,"*");
        draw_row(&out,body,&line,true);
        if (menu_discard_pending) hint="Enter confirm, Esc back";
        else if (menu_number>=0) {
            snprintf(info,sizeof info,"Choice %d: Enter",menu_number);
            hint=info;
        } else if (editing_clipboard) hint="Write saves | clipboard returns";
        else hint=cols<27 ? "j/k l h" : "j/k move | l/Enter select | h/Esc back";
        visible(&line,hint,strlen(hint),0,width);
        draw_row(&out,body+1,&line,false);
        output(&out,"\033[?25l");
        goto finish;
    }
    start = line_start(cursor);
    if (top > start) top = start;
    p = top;
    for (r = 0; r < body && p < start; r++) p = next_line(p);
    if (r >= body) {
        top = start;
        for (r = 1; r < body && top; r++) top = line_start(top - 1);
    }
    col = column_at(start, cursor);
    if (col < left) left = col;
    if (col >= left + (size_t)(cols - 1)) left = col - (size_t)(cols - 2);
    p = top;
    for (r = 0; r < body; r++) {
        if (p == start) cursor_row = r;
        end = line_end(p);
        if (search_mark!=SIZE_MAX && search_length && search_mark>=p &&
            search_mark<end && search_length<=text.n-search_mark &&
            !memcmp(text.s+search_mark,search_pattern,search_length))
            visible_match(&line,text.s+p,end-p,left,(size_t)(cols-1),
                          search_mark-p,search_mark-p+search_length);
        else visible(&line,text.s ? text.s+p : "",end-p,left,(size_t)(cols-1));
        draw_row(&out, r, &line, false);
        if (end < text.n) p = next_line(p);
        else {
            r++;
            for (; r < body; r++) { line.n = 0; draw_row(&out, r, &line, false); }
            break;
        }
    }
    for (p = 0; p < cursor; p++) if (text.s[p] == '\n') line_number++;
    dirty = changed(); width = (size_t)(cols - 1);
    name = filename ? strrchr(filename, '/') + 1 : "[new file]";
    snprintf(info, sizeof info, "%zu:%zu | ", line_number, col + 1);
    /* Reserve room for the filename and the unsaved marker even in a
     * narrow terminal. Avoid unsigned underflow when line numbers grow. */
    visible(&status, info, strlen(info), 0, width > (dirty ? 2u : 1u) ?
            width - (dirty ? 2u : 1u) : 0);
    name_width = width - status.n - (dirty ? 1u : 0u);
    visible(&line, name, strlen(name), 0, name_width);
    add(&status, line.s, line.n);
    if (dirty) output(&status, "*");
    visible(&line, status.s, status.n, 0, (size_t)(cols - 1));
    draw_row(&out, body, &line, true);
    status.n = 0;
    if (prompt != PROMPT_NONE) {
        const char *question = prompt == PROMPT_OVERWRITE ?
            "File written since read. Overwrite y/n?" :
            (prompt == PROMPT_MENU_WRITE || prompt == PROMPT_TARGET_OVERWRITE) ?
            "Overwrite existing file? Y/n" : "Really quit? (Y/n)";
        visible(&line, question, strlen(question), 0, (size_t)(cols - 1));
        draw_row(&out, body + 1, &line, true);
    } else if (command_mode) {
        const char *prefix = filename_mode ? "Filename> " :
            menu_argument_mode && menu_argument_label ? menu_argument_label : "Command> ";
        size_t prefix_n = strlen(prefix);
        output(&status, prefix); add(&status, command, command_n);
        p = 0;
        while (p < command_cursor) {
            int w; bool wide; char rep[32];
            p += representation(command, command_cursor, p, command_column + prefix_n, rep, &w, &wide);
            command_column += (size_t)w;
        }
        command_column += prefix_n;
        p = command_column >= (size_t)(cols - 1) ? command_column - (size_t)(cols - 2) : 0;
        visible(&line, status.s, status.n, p, (size_t)(cols - 1));
        draw_row(&out, body + 1, &line, false);
        cursor_row = body + 1; col = command_column - p;
    } else {
        if (reverse_mode) {
            char location[256];
            if (cols < 35)
                snprintf(location, sizeof location, "R %zu/%zu -/+", reverse_position, reverse_total);
            else
                snprintf(location, sizeof location,
                         "REVERSE %zu/%zu | undo %zu redo %zu | - back, + forward, Esc exit",
                         reverse_position, reverse_total, reverse_position, reverse_total - reverse_position);
            visible(&line, location, strlen(location), 0, (size_t)(cols - 1));
        } else if (menu_open && menu_number >= 0) {
            char choice_prompt[80];
            snprintf(choice_prompt, sizeof choice_prompt, "Menu number: %d (Enter selects)", menu_number);
            visible(&line, choice_prompt, strlen(choice_prompt), 0, (size_t)(cols - 1));
        } else if (editing_clipboard) {
            char foot[512];
            const char *prior = parent_document.filename ?
                strrchr(parent_document.filename,'/')+1 : "[new file]";
            if (message[0])
                snprintf(foot,sizeof foot,"%s%s",movement_mode?"MOVE | ":"",message);
            else snprintf(foot,sizeof foot,"%sCMD clipboard: return to %s | write: save",
                          movement_mode?"MOVE | ":"",prior);
            visible(&line,foot,strlen(foot),0,(size_t)(cols-1));
        } else if (movement_mode) {
            char foot[256];
            const char *hint=cols<24 ? "MOVE: Enter edit" : "MOVE | Enter: edit | Esc: commands";
            if (message[0]) snprintf(foot,sizeof foot,"M | %.245s",message);
            else snprintf(foot,sizeof foot,"%s",hint);
            visible(&line,foot,strlen(foot),0,(size_t)(cols-1));
        } else visible(&line, message, strlen(message), 0, (size_t)(cols - 1));
        draw_row(&out, body + 1, &line, false); col -= left;
    }
    if (menu_open) { draw_menu(&out, &line, body); output(&out, "\033[?25l"); }
    else {
        snprintf(address, sizeof address, "\033[%d;%zuH\033[?25h", cursor_row + 1, col + 1);
        output(&out, address);
    }
finish:
    if (drawing_failed) {
        static const char low_memory[] =
            "\033[0m\033[H\033[KLow memory; text retained.\033[?25h";
        /* Drawing is expendable; unsaved text is not. Release the expendable
         * screen cache, keep all document/history ownership intact, and let
         * the next input/resize retry. No allocation is needed for this hint. */
        reset_screen();
        free(out.s); free(line.s); free(status.s);
        tell("Low memory: screen refresh failed; text retained.");
        if (write_all(tty_out, low_memory, sizeof low_memory - 1) < 0 &&
            !(errno == EINTR && (terminated || suspended || interrupted)))
            terminated = SIGTERM;
        return;
    }
    if (write_all(tty_out, out.s, out.n) < 0) {
        /* draw_row populated the cache before sending the frame. A partial
         * or cancelled write means those rows may never have reached the
         * terminal, so the next successful frame must repaint everything. */
        int error = errno;
        reset_screen(); resized = 1;
        if (!(error == EINTR && (terminated || suspended || interrupted)))
            terminated = SIGTERM;
    }
    free(out.s); free(line.s); free(status.s);
}

enum { KEY_NONE = 256, KEY_ESCAPE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
       KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_DELETE, KEY_PASTE,
       KEY_TOP, KEY_BOTTOM, KEY_EOF, KEY_MOUSE };

static int byte(int timeout)
{
    struct pollfd fd;
    ssize_t n; int r;
    if (terminated || suspended || interrupted) return -2;
    if (input_at < input_n) return input[input_at++];
    fd.fd = tty_in; fd.events = POLLIN; fd.revents = 0;
    r = poll(&fd, 1, timeout);
    if (r < 0) return errno == EINTR ? -2 : -1;
    if (!r) return -2;
    n = read(tty_in, input, sizeof input);
    if (n < 0) return errno == EINTR || errno == EAGAIN ? -2 : -1;
    if (!n) return -1;
    input_n = (size_t)n; input_at = 1; return input[0];
}

static int64_t milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int sequence_byte(int64_t deadline)
{
    int64_t remaining = deadline - milliseconds();
    if (remaining <= 0) return -2;
    return byte(remaining < 80 ? (int)remaining : 80);
}

/* Terminal coordinates are untrusted decimal text. scanf's unsigned
 * conversions can wrap oversized numbers into an ordinary click. */
static bool key_number(const char **textp, unsigned maximum, unsigned *value)
{
    const char *p = *textp;
    unsigned n = 0;
    if (*p < '0' || *p > '9') return false;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (n > maximum / 10 ||
            (n == maximum / 10 && digit > maximum % 10)) return false;
        n = n * 10 + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    *textp = p; *value = n;
    return true;
}
static void paste(void);
static int key(void)
{
    int c, first, last = 0, k;
    char sequence[64]; size_t n = 0;
    int64_t deadline;
    if (paste_discard) { paste(); return KEY_NONE; }
    c = byte(250);
    if (c == -1) return KEY_EOF;
    if (c < 0) return KEY_NONE;
    /* A truncated OSC/DCS/CSI sequence must not swallow keystrokes forever.
     * After a quiet interval, the next key is handled normally. */
    if (discard_sequence && milliseconds() > discard_until) discard_sequence = 0;
    if (discard_sequence) {
        discard_until = milliseconds() + 1200;
        if (discard_sequence == 1) {
            if (c >= 0x40 && c <= 0x7e) discard_sequence = 0;
            if (c == 27) { discard_sequence = 0; return KEY_ESCAPE; }
        } else {
            if (c == 7 || (discard_last == 27 && c == '\\')) discard_sequence = 0;
            discard_last = c;
        }
        return KEY_NONE;
    }
    if (c != 27) return c;
    first = byte(80);
    if (first < 0) return KEY_ESCAPE;
    deadline = milliseconds() + 250;
    if (first == ']' || first == 'P' || first == '^' || first == '_') {
        for (k = 0; k < 4096; k++) {
            c = sequence_byte(deadline);
            if (c < 0) { discard_sequence = 2; discard_last = last;
                         discard_until = milliseconds() + 1200; return KEY_NONE; }
            if (c == 7 || (last == 27 && c == '\\')) return KEY_NONE;
            last = c;
        }
        discard_sequence = 2; discard_last = last;
        discard_until = milliseconds() + 1200;
        return KEY_NONE;
    }
    if (first != '[' && first != 'O') {
        /* Distinguish Alt+letter ONLY if the user explicitly assigned it.
         * Otherwise retain the old rapid Esc followed by command text. */
        if (!command_mode && !menu_open && !viewer_text &&
            first >= 'a' && first <= 'z' &&
            binding_action(KEY_ALT_BASE + first)) return KEY_ALT_BASE + first;
        input_at--;
        return KEY_ESCAPE;
    }
    for (k = 0; k < 256; k++) {
        c = sequence_byte(deadline);
        if (c < 0) { discard_sequence = 1;
                     discard_until = milliseconds() + 1200; return KEY_NONE; }
        if (n < sizeof sequence - 1) sequence[n++] = (char)c;
        if (c >= 0x40 && c <= 0x7e) {
            if (c == '[' && n == 1) continue; /* Linux console function keys. */
            break;
        }
    }
    sequence[n] = 0;
    if (k == 256) { discard_sequence = 1;
                    discard_until = milliseconds() + 1200; return KEY_NONE; }
    if (n == sizeof sequence - 1 || sequence[0] == '[') return KEY_NONE;
    if (first == '[' && sequence[0] == '<' && (c == 'M' || c == 'm')) {
        unsigned b, x, y;
        const char *p = sequence + 1;
        if (key_number(&p, 255, &b) && *p++ == ';' &&
            key_number(&p, 9999, &x) && *p++ == ';' &&
            key_number(&p, 9999, &y) && *p == c && !p[1] && x && y) {
            mouse_x = (int)x; mouse_y = (int)y; mouse_button = (int)b;
            mouse_pressed = c == 'M';
            return mouse_enabled ? KEY_MOUSE : KEY_NONE;
        }
        return KEY_NONE;
    }
    if (first == 'O') {
        if (c == 'P') return KEY_FUNCTION_BASE + 1;
        if (c == 'Q') return KEY_FUNCTION_BASE + 2;
        if (c == 'R') return KEY_FUNCTION_BASE + 3;
        if (c == 'S') return KEY_FUNCTION_BASE + 4;
    }
    if (first == '[' && strchr("ABCD", c) && !strncmp(sequence, "1;", 2)) {
        int direction = c == 'A' ? 1 : c == 'B' ? 2 : c == 'C' ? 4 : 3;
        if (!strcmp(sequence, c == 'A' ? "1;5A" : c == 'B' ? "1;5B" : c == 'C' ? "1;5C" : "1;5D"))
            return KEY_CTRL_ARROW_BASE + direction;
        if (!strcmp(sequence, c == 'A' ? "1;3A" : c == 'B' ? "1;3B" : c == 'C' ? "1;3C" : "1;3D"))
            return KEY_ALT_ARROW_BASE + direction;
    }
    if (c == 'A') return KEY_UP;
    if (c == 'B') return KEY_DOWN;
    if (c == 'C') return KEY_RIGHT;
    if (c == 'D') return KEY_LEFT;
    if (c == 'H') return !strcmp(sequence, "1;5H") ? KEY_TOP : KEY_HOME;
    if (c == 'F') return !strcmp(sequence, "1;5F") ? KEY_BOTTOM : KEY_END;
    if (!strcmp(sequence, "1~") || !strcmp(sequence, "7~")) return KEY_HOME;
    if (!strcmp(sequence, "4~") || !strcmp(sequence, "8~")) return KEY_END;
    if (!strcmp(sequence, "3~")) return KEY_DELETE;
    if (!strcmp(sequence, "5~")) return KEY_PAGEUP;
    if (!strcmp(sequence, "6~")) return KEY_PAGEDOWN;
    if (!strcmp(sequence, "200~")) return KEY_PASTE;
    if (first == '[' && c == '~') {
        const int nums[] = {11,12,13,14,15,17,18,19,20,21,23,24};
        char *end; long num = strtol(sequence, &end, 10);
        int j;
        if (!strcmp(end,"~")) for (j = 0; j < 12; j++)
            if (num == nums[j]) return KEY_FUNCTION_BASE + j + 1;
    }
    return KEY_NONE;
}

static void open_viewer(const char *title, const char *contents)
{
    viewer_title = title; viewer_text = contents; viewer_top = 0;
    message[0] = 0;
}

static void viewer_key(int c)
{
    size_t width = (size_t)(cols > 1 ? cols - 1 : 1);
    size_t page = (size_t)(rows > 2 ? rows - 2 : 1);
    size_t total = viewer_rows(width), maximum = total > page ? total - page : 0;
    if (viewer_top > maximum) viewer_top = maximum;
    if (c == KEY_ESCAPE || c == 'q') viewer_text = viewer_title = NULL;
    else if ((c == KEY_UP || c == 'k') && viewer_top) viewer_top--;
    else if ((c == KEY_DOWN || c == 'j') && viewer_top < maximum) viewer_top++;
    else if (c == KEY_PAGEUP || c == KEY_LEFT || c == 'h')
        viewer_top = viewer_top > page ? viewer_top - page : 0;
    else if (c == KEY_PAGEDOWN || c == KEY_RIGHT || c == 'l')
        viewer_top = page > maximum - viewer_top ? maximum : viewer_top + page;
    else if (c == KEY_HOME || c == KEY_TOP) viewer_top = 0;
    else if (c == KEY_END || c == KEY_BOTTOM) viewer_top = maximum;
}

/* The clipboard and its optional bounded history are ordinary plaintext
 * files. They live in the private editor state directory and are never
 * implemented as shell commands or terminal escape-sequence clipboard APIs. */
static char *state_path(const char *leaf, bool create)
{
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char *dir, *p, *result;
    size_t n;
    if (!base || *base != '/') {
        if (!home || *home != '/') return NULL;
        n = strlen(home) + sizeof "/.local/state/editor";
        dir = malloc(n);
        if (!dir) return NULL;
        snprintf(dir,n,"%s/.local/state/editor",home);
    } else {
        n = strlen(base) + sizeof "/editor";
        dir = malloc(n);
        if (!dir) return NULL;
        snprintf(dir,n,"%s/editor",base);
    }
    if (create) {
        for (p = dir + 1; *p; p++) if (*p == '/') {
            *p = 0;
            if (!private_directory(dir)) { free(dir); return NULL; }
            *p = '/';
        }
        if (!private_directory(dir)) { free(dir); return NULL; }
    }
    {
        struct stat st;
        if (lstat(dir,&st) < 0 || !S_ISDIR(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 077)) {
            free(dir); return NULL;
        }
    }
    n = strlen(dir) + strlen(leaf) + 2;
    result = malloc(n);
    if (result) snprintf(result,n,"%s/%s",dir,leaf);
    free(dir);
    return result;
}
static char *expand_home_path(const char *path)
{
    const char *home = getenv("HOME");
    char *result;
    size_t n;
    if (path[0] != '~' || (path[1] && path[1] != '/')) return strdup(path);
    if (!home || *home != '/') { errno = EINVAL; return NULL; }
    n = strlen(home) + strlen(path+1) + 1;
    result = malloc(n);
    if (result) snprintf(result,n,"%s%s",home,path+1);
    return result;
}
static bool private_write(const char *path, const char *data, size_t n, bool no_replace)
{
    char *tmp = NULL, *dir = NULL, *slash;
    int fd = -1, dfd = -1;
    bool ok = false;
    tmp = malloc(strlen(path)+16);
    if (!tmp) goto done;
    snprintf(tmp,strlen(path)+16,"%s.XXXXXX",path);
    fd = mkstemp(tmp);
    if (fd < 0) goto done;
    if (fchmod(fd,0600) < 0 || write_all(fd,data,n)<0 || sync_fd(fd)<0) goto done;
    if (close(fd)<0) { fd=-1; goto done; }
    fd=-1;
    if (no_replace) {
        if (link(tmp,path)<0) goto done;
        (void)unlink(tmp);
    } else if (rename(tmp,path)<0) goto done;
    ok=true;
    dir = strdup(path);
    if (dir && (slash = strrchr(dir,'/'))) {
        *slash=0;
        dfd=open(dir,O_RDONLY);
        if (dfd>=0) (void)sync_fd(dfd);
    }
done:
    if (dfd>=0) close(dfd);
    if (fd>=0) close(fd);
    if (!ok && tmp) (void)unlink(tmp);
    free(dir); free(tmp);
    return ok;
}
static bool clipboard_history_setting(bool on)
{
    char *path=state_path("clipboard-history.enabled",true);
    bool ok;
    if (!path) return false;
    ok=private_write(path,on ? "on\n" : "off\n",on ? 3 : 4,false);
    free(path);
    if (ok) clip_history_enabled=on;
    return ok;
}
static void clipboard_history_load_setting(void)
{
    char *path=state_path("clipboard-history.enabled",false);
    Buffer b={0}; struct stat st;
    if (path && read_file(path,&b,&st) && b.n==3 && !memcmp(b.s,"on\n",3))
        clip_history_enabled=true;
    free(b.s); free(path);
}
/* A slot name is a literal, short ASCII identifier. No separators,
 * whitespace or dot names are accepted; the special default is not a slot. */
static bool clipboard_slot_valid(const char *slot)
{
    size_t i, n=strlen(slot);
    if (!n || n>CLIP_SLOT_MAX || !strcmp(slot,"default") || !strcmp(slot,"list") ||
        !strcmp(slot,"selected")) /* clipboard-selected.txt is selector metadata. */
        return false;
    for (i=0;i<n;i++) {
        unsigned char c=(unsigned char)slot[i];
        if (!((c>='0' && c<='9') || (c>='A' && c<='Z') ||
              (c>='a' && c<='z') || c=='-' || c=='_')) return false;
    }
    return true;
}
static char *clipboard_path_for(const char *slot, bool create)
{
    char name[CLIP_SLOT_MAX+20];
    if (!slot || !*slot) return state_path("clipboard.txt",create);
    if (!clipboard_slot_valid(slot)) { errno=EINVAL; return NULL; }
    snprintf(name,sizeof name,"clipboard-%s.txt",slot);
    return state_path(name,create);
}
static char *clipboard_path(bool create)
{
    return clipboard_path_for(selected_clipboard,create);
}
/* Changing the selector is intentionally one small atomic state write, not
 * a copy/move of any clipboard's contents. */
static bool clipboard_choose(const char *slot)
{
    char *path=state_path("clipboard-selected.txt",true);
    bool ok;
    if (!path) return false;
    ok=private_write(path,slot,strlen(slot),false);
    free(path);
    if (ok) strcpy(selected_clipboard,slot);
    return ok;
}
static void clipboard_load_selection(void)
{
    char *path=state_path("clipboard-selected.txt",false);
    struct stat st;
    Buffer b={0};
    if (!path) return;
    if (lstat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        !(st.st_mode & 077) && read_file(path,&b,&st) && b.n<=CLIP_SLOT_MAX) {
        if (b.n==0) selected_clipboard[0]=0;
        else if (b.s && !(memchr(b.s,0,b.n))) {
            b.s[b.n]=0;
            if (clipboard_slot_valid(b.s)) strcpy(selected_clipboard,b.s);
        }
    }
    free(b.s); free(path);
}
/* Return 1 if the selected/named clipboard exists, 0 if absent, -1 on error. */
static int clipboard_read_named(const char *slot, Buffer *out)
{
    char *path=clipboard_path_for(slot,true);
    struct stat st;
    int result;
    if (!path) { errno=EACCES; return -1; }
    if (lstat(path,&st)<0) {
        result=errno==ENOENT ? 0 : -1;
        free(path); return result;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077)) {
        free(path); errno=EACCES; return -1;
    }
    {
        bool read_ok=read_file(path,out,&st);
        if (!read_ok || (out->n && memchr(out->s,0,out->n))) {
            /* A rejected/partly read clipboard may own up to LIMIT bytes.
             * Free even when the read fails after allocating megabytes. */
            int saved_error = read_ok ? EINVAL : errno;
            free(out->s);
            memset(out,0,sizeof *out);
            free(path);
            errno=saved_error;
            return -1;
        }
    }
    free(path); return 1;
}
static int clipboard_read(Buffer *out)
{
    return clipboard_read_named(selected_clipboard,out);
}
/* List existing private clipboard files without following symlinks.  The
 * selected slot is always shown, even when it has never been written. */
static int directory_next(DIR *dir, struct dirent **entry);
static void clipboard_list(void)
{
    char *sample=state_path("clipboard.txt",true), *slash;
    DIR *d; struct dirent *entry;
    size_t used=0,shown=0;
    int n, scan, error;
    bool truncated=false;
    if (!sample) { tell("Clipboard storage unavailable."); return; }
    slash=strrchr(sample,'/');
    if (!slash) { free(sample);return; }
    *slash=0;
    d=opendir(sample);
    if (!d) { tell("Clipboard listing unavailable."); free(sample); return; }
    n=snprintf(clipboard_page,sizeof clipboard_page,
       "CLIPBOARD SLOTS\nSelected: %s\n\n"
       "clipboard NAME selects a slot; clipboard opens its editor.\n"
       "paste NAME reads a slot without selecting it.\n\n",
       *selected_clipboard ? selected_clipboard : "default");
    if (n<0 || (size_t)n>=sizeof clipboard_page) goto done_list;
    used=(size_t)n;
    while ((scan=directory_next(d,&entry))>0) {
        const char *name=entry->d_name; const char *slot;
        size_t len=strlen(name);
        char path[4096];struct stat st;
        bool is_default=!strcmp(name,"clipboard.txt");
        if (!is_default) {
            if (len<14 || strncmp(name,"clipboard-",10) ||
                strcmp(name+len-4,".txt")) continue;
            if (len-14>CLIP_SLOT_MAX) continue;
            {
                char candidate[CLIP_SLOT_MAX+1];
                memcpy(candidate,name+10,len-14); candidate[len-14]=0;
                if (!clipboard_slot_valid(candidate)) continue;
            }
            slot=name+10;
        } else slot=name;
        (void)slot;
        {
            size_t dirlen=strlen(sample);
            if (dirlen+len+2>=sizeof path) continue;
            memcpy(path,sample,dirlen);
            path[dirlen]='/';
            memcpy(path+dirlen+1,name,len+1);
        }
        if (lstat(path,&st)!=0 || !S_ISREG(st.st_mode) ||
            st.st_uid!=geteuid() || (st.st_mode & 077)) continue;
        n=snprintf(clipboard_page+used,sizeof clipboard_page-used,
             "%s %s (%lld bytes)\n",
             is_default ? "default" : "slot",
             name, (long long)st.st_size);
        if (n<0 || (size_t)n>=sizeof clipboard_page-used) {
            clipboard_page[used]=0; truncated=true; break;
        }
        used+=(size_t)n;
        shown++;
    }
    if (scan<0) goto list_failed;
    if (truncated && used+24<sizeof clipboard_page)
        strcpy(clipboard_page+used,"(listing truncated)\n");
    if (!shown && used+25<sizeof clipboard_page) {
        memcpy(clipboard_page+used,"(no stored clipboards)\n",23);
        used+=23; clipboard_page[used]=0;
    }
done_list:
    scan=closedir(d);d=NULL;
    if (scan<0) goto list_failed;
    if (terminated || suspended || interrupted) { errno=EINTR; goto list_failed; }
    free(sample);
    open_viewer("Clipboard slots",clipboard_page);
    return;
list_failed:
    error=errno;
    if (d) (void)closedir(d);
    free(sample);
    tell("Clipboard listing failed: %s. No partial list shown.",strerror(error));
}

typedef struct { char name[64]; } ClipEntry;
/* Only private regular snapshots with the canonical name participate in
 * numbering. Return -1 for a real metadata error, not an incomplete result. */
static int clip_history_entry(int fd, const char *name)
{
    size_t i;
    struct stat st;
    if (strlen(name)!=29 || strncmp(name,"clip-",5) || strcmp(name+25,".txt")) return 0;
    for (i=5;i<25;i++) if (name[i]<'0' || name[i]>'9') return 0;
    if (fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)<0)
        return errno==ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode & 077);
}
/* SIZE_MAX is failure, never a usable partial list. Retention cleanup starts
 * only after a complete successful scan; a read error cannot renumber the
 * history and make clipuse restore the wrong item. Storage stays bounded to
 * the newest CLIP_HISTORY_MAX names, independent of directory size. */
static size_t clip_history_entries(ClipEntry items[CLIP_HISTORY_MAX], bool prune)
{
    char *dir=state_path("clipboard-history",false);
    DIR *d=NULL;
    struct dirent *ent;
    struct stat before, after;
    size_t count=0, seen=0;
    int result=0, error, fd;
    if (!dir) return errno==ENOENT ? 0 : SIZE_MAX;
    if (lstat(dir,&before)<0) {
        error=errno;free(dir);errno=error;
        return error==ENOENT ? 0 : SIZE_MAX;
    }
    if (!S_ISDIR(before.st_mode) || before.st_uid!=geteuid() || (before.st_mode & 077)) {
        errno=EACCES;goto failure;
    }
    d=opendir(dir);
    if (!d) goto failure;
    fd=dirfd(d);
    if (fd<0 || fstat(fd,&after)<0) goto failure;
    if (!same_disk_version(&before,&after)) {errno=EAGAIN;goto failure;}
    while ((result=directory_next(d,&ent))>0) {
        size_t i,j;
        int valid=clip_history_entry(fd,ent->d_name);
        if (valid<0) goto failure;
        if (!valid) continue;
        seen++;
        for (i=0;i<count;i++) if (strcmp(ent->d_name,items[i].name)>0) break;
        if (i>=CLIP_HISTORY_MAX) continue;
        if (count<CLIP_HISTORY_MAX) count++;
        for (j=count-1;j>i;j--) items[j]=items[j-1];
        strcpy(items[i].name,ent->d_name);
    }
    if (result<0 || fstat(fd,&after)<0) goto failure;
    if (!same_disk_version(&before,&after)) {errno=EAGAIN;goto failure;}
    result=closedir(d);d=NULL;
    if (result<0) goto failure;
    if (terminated || suspended || interrupted) {errno=EINTR;goto failure;}
    if (prune && count==CLIP_HISTORY_MAX && seen>count) {
        /* A second pass removes only entries proved older than all retained
         * names. Failed enumeration in the first pass never deletes anything. */
        d=opendir(dir);
        if (!d) goto failure;
        fd=dirfd(d);
        if (fd<0 || fstat(fd,&after)<0) goto failure;
        if (!same_disk_version(&before,&after)) {errno=EAGAIN;goto failure;}
        while ((result=directory_next(d,&ent))>0) {
            int valid=clip_history_entry(fd,ent->d_name);
            if (valid<0) goto failure;
            if (valid && strcmp(ent->d_name,items[count-1].name)<0 &&
                unlinkat(fd,ent->d_name,0)<0 && errno!=ENOENT) goto failure;
        }
        if (result<0) goto failure;
        result=closedir(d);d=NULL;
        if (result<0) goto failure;
        if (terminated || suspended || interrupted) {errno=EINTR;goto failure;}
    }
    free(dir);return count;
failure:
    error=errno;
    if (d) (void)closedir(d);
    free(dir);errno=error;
    return SIZE_MAX;
}
static bool clipboard_archive(const Buffer *before)
{
    char *dir=NULL, *path=NULL;
    ClipEntry current[CLIP_HISTORY_MAX];
    struct timespec now;
    unsigned long long stamp;
    int tries;
    bool ok=false;
    if (!clip_history_enabled || !before->n) return true;
    dir=state_path("clipboard-history",true);
    if (!dir) return false;
    if (!private_directory(dir)) goto done;
    if (clock_gettime(CLOCK_REALTIME,&now)<0) goto done;
    stamp=(unsigned long long)now.tv_sec*1000000000ull+(unsigned long long)now.tv_nsec;
    path=malloc(strlen(dir)+36);
    if (!path) goto done;
    for (tries=0;tries<100;tries++,stamp++) {
        snprintf(path,strlen(dir)+36,"%s/clip-%020llu.txt",dir,stamp);
        if (private_write(path,before->s,before->n,true)) { ok=true; break; }
        if (errno!=EEXIST) break;
    }
    if (ok && clip_history_entries(current,true)==SIZE_MAX) ok=false;
done:
    free(path);free(dir);return ok;
}
/* New clipboard bytes become visible atomically. History failure does not
 * undo a successful clipboard replacement; callers are informed. */
enum { CLIP_FAILED, CLIP_SAVED, CLIP_SAVED_NO_HISTORY };
static int clipboard_set(const char *s, size_t n)
{
    Buffer before={0};
    char *path;
    int got=clipboard_read(&before);
    bool ok, archive_ok;
    if (got<0) { tell("Clipboard read failed: %s.",strerror(errno)); return CLIP_FAILED; }
    if (got && before.n==n && (!n || !memcmp(before.s,s,n))) {
        free(before.s); tell("Clipboard unchanged."); return CLIP_SAVED;
    }
    path=clipboard_path(true);
    if (!path) { free(before.s); tell("Clipboard directory unavailable."); return CLIP_FAILED; }
    ok=private_write(path,s,n,false);
    if (!ok) tell("Clipboard write failed: %s.",strerror(errno));
    archive_ok=ok && clipboard_archive(&before);
    if (ok && !archive_ok) tell("Clipboard updated; history save/cleanup incomplete.");
    free(path);free(before.s);
    return !ok ? CLIP_FAILED : archive_ok ? CLIP_SAVED : CLIP_SAVED_NO_HISTORY;
}
/* A history listing needs a short preview, not up to 64 complete 8 MiB
 * documents. Read only the displayed prefix; clipuse still validates the
 * entire selected entry before changing any clipboard. */
static bool clipboard_preview(const char *path, char prefix[64],
                              size_t *got, size_t *total)
{
    struct stat before, after;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    bool ok = false;
    size_t want;
    *got = *total = 0;
    if (fd < 0) return false;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_uid != geteuid() || (before.st_mode & 077) ||
        before.st_size < 0 || (uint64_t)before.st_size > LIMIT) goto done;
    *total = (size_t)before.st_size;
    want = *total < 64 ? *total : 64;
    while (*got < want) {
        ssize_t n;
        if (terminated || interrupted || suspended) goto done;
        n = read(fd, prefix + *got, want - *got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        *got += (size_t)n;
    }
    if (fstat(fd, &after) < 0 || !same_disk_version(&before, &after)) goto done;
    ok = true;
done:
    if (close(fd) < 0) ok = false;
    return ok;
}
static void clip_history_list(void)
{
    ClipEntry entries[CLIP_HISTORY_MAX];
    size_t i,count=clip_history_entries(entries,false),used=0;
    char *dir=NULL, *path=NULL;
    size_t capacity=0;
    int n, error;
    if (count==SIZE_MAX) goto failed;
    if (count) {
        dir=state_path("clipboard-history",false);
        if (!dir) goto failed;
        capacity=strlen(dir)+sizeof entries[0].name+2;
        path=malloc(capacity);
        if (!path) goto failed;
    }
    n=snprintf(clipboard_page,sizeof clipboard_page,
         "CLIPBOARD HISTORY (%s)\nSaved previous clips from any slot; 1 newest.\n"
         "clipuse N restores into the selected slot. Max %d entries.\n\n",
         clip_history_enabled ? "ON" : "OFF", CLIP_HISTORY_MAX);
    if (n<0 || (size_t)n>=sizeof clipboard_page) {errno=EOVERFLOW;goto failed;}
    used=(size_t)n;
    for (i=0;i<count;i++) {
        char prefix[64];
        size_t got,total,j,k;
        bool available;
        if (terminated || suspended || interrupted) {errno=EINTR;goto failed;}
        (void)snprintf(path,capacity,"%s/%s",dir,entries[i].name);
        available=clipboard_preview(path,prefix,&got,&total);
        if (terminated || suspended || interrupted) {errno=EINTR;goto failed;}
        /* Keep every confirmed index visible even if its content cannot be
         * previewed. Restoration still reads and validates the whole file. */
        if (!available) {
            n=snprintf(clipboard_page+used,sizeof clipboard_page-used,
                       "%zu  (preview unavailable)\n",i+1);
            if (n<0 || (size_t)n>=sizeof clipboard_page-used) {errno=EOVERFLOW;goto failed;}
            used+=(size_t)n;
            continue;
        }
        n=snprintf(clipboard_page+used,sizeof clipboard_page-used,
                   "%zu  (%zu bytes)  ",i+1,total);
        if (n<0 || (size_t)n>=sizeof clipboard_page-used) {errno=EOVERFLOW;goto failed;}
        used+=(size_t)n;
        k=0;
        for (j=0;j<got && k<64 && used+4<sizeof clipboard_page;j++,k++) {
            unsigned char ch=(unsigned char)prefix[j];
            if (ch=='\n' || ch=='\r') {clipboard_page[used++]=' ';break;}
            clipboard_page[used++]=(ch>=32 && ch<127) ? (char)ch : '?';
        }
        if (j<total && used+4<sizeof clipboard_page) {memcpy(clipboard_page+used,"...",3);used+=3;}
        clipboard_page[used++]='\n';clipboard_page[used]=0;
    }
    if (!count && used+32<sizeof clipboard_page)
        strcpy(clipboard_page+used,"(no saved clipboard entries)\n");
    free(path);free(dir);
    open_viewer("Clipboard history",clipboard_page);
    return;
failed:
    error=errno;free(path);free(dir);
    tell("Clipboard history listing failed: %s. No partial list shown.",strerror(error));
}
static void clip_history_use(size_t number)
{
    ClipEntry entries[CLIP_HISTORY_MAX];
    size_t count=clip_history_entries(entries,false);
    Buffer b={0}; struct stat st;
    char *dir, *path;
    if (count==SIZE_MAX) {
        tell("Clipboard history scan failed: %s. Clipboard unchanged.",strerror(errno));return;
    }
    if (!number || number>count) { tell("Clip entry must be 1-%zu.",count);return; }
    dir=state_path("clipboard-history",false);
    if (!dir) { tell("Clipboard history directory unavailable.");return; }
    path=malloc(strlen(dir)+strlen(entries[number-1].name)+2);
    if (!path) { free(dir);tell("Out of memory.");return; }
    sprintf(path,"%s/%s",dir,entries[number-1].name);
    if (!read_file(path,&b,&st))
        tell("Cannot read history entry: %s.",strerror(errno));
    else if (st.st_uid!=geteuid() || (st.st_mode & 077))
        tell("Cannot restore a nonprivate history entry.");
    else if (b.n && memchr(b.s,0,b.n))
        tell("Cannot restore history entry containing NUL bytes.");
    else {
        int result = clipboard_set(b.s,b.n);
        if (result) tell("Clipboard restored from entry %zu.%s",number,
                         result == CLIP_SAVED_NO_HISTORY ? " Clipboard history save/cleanup incomplete." : "");
    }
    free(dir);free(path);free(b.s);
}
/* Move the active document (including its persistent undo state) without
 * copying its bytes. The only active buffer is owned by the editor globals. */
static void document_capture(DocumentContext *d)
{
    memset(d,0,sizeof *d);
    d->text=text; d->saved=saved;
    d->cursor=cursor; d->top=top; d->left=left; d->goal=goal;
    memcpy(d->search_pattern,search_pattern,sizeof search_pattern);
    d->search_length=search_length; d->search_mark=search_mark;
    d->goal_set=goal_set; d->group=group; d->existed=existed;
    d->filename=filename; d->loaded_journal_path=loaded_journal_path;
    d->original=original;
    d->loaded_journal_stat=loaded_journal_stat;
    d->loaded_journal_stat_valid=loaded_journal_stat_valid;
    memcpy(d->newline,newline,sizeof newline); d->newline_n=newline_n;
    d->history=history; d->current=current; d->history_tail=history_tail;
    d->journal_ignored=journal_ignored; d->journal_dirty=journal_dirty;
    d->journal_rebased=journal_rebased; d->journal_branched=journal_branched;
    memset(&text,0,sizeof text); memset(&saved,0,sizeof saved);
    filename=loaded_journal_path=NULL;
    loaded_journal_stat_valid=false;
    history=current=history_tail=NULL;
    cursor=top=left=goal=0;
    search_pattern[0]=0; search_length=0; search_mark=SIZE_MAX;
    goal_set=group=existed=false;
    memset(&original,0,sizeof original);
    newline[0]='\n'; newline[1]=0; newline_n=1;
    journal_ignored=journal_dirty=journal_rebased=journal_branched=false;
}
static void document_restore(DocumentContext *d)
{
    free_changes(history); free(text.s); free(saved.s);
    free(filename); free(loaded_journal_path);
    text=d->text; saved=d->saved;
    cursor=d->cursor; top=d->top; left=d->left; goal=d->goal;
    memcpy(search_pattern,d->search_pattern,sizeof search_pattern);
    search_length=d->search_length; search_mark=d->search_mark;
    goal_set=d->goal_set; group=d->group; existed=d->existed;
    filename=d->filename; loaded_journal_path=d->loaded_journal_path;
    original=d->original;
    loaded_journal_stat=d->loaded_journal_stat;
    loaded_journal_stat_valid=d->loaded_journal_stat_valid;
    memcpy(newline,d->newline,sizeof newline); newline_n=d->newline_n;
    history=d->history; current=d->current; history_tail=d->history_tail;
    journal_ignored=d->journal_ignored; journal_dirty=d->journal_dirty;
    journal_rebased=d->journal_rebased; journal_branched=d->journal_branched;
    memset(d,0,sizeof *d);
}
/* Return from temporary clipboard editing without touching the parent.
 * The clipboard command toggles contexts, and write does not. quit and quit!
 * remain global editor-exit commands. */
static bool parent_unsaved(void)
{
    if (!editing_clipboard) return false;
    return parent_document.text.n != parent_document.saved.n ||
        (parent_document.text.n &&
         memcmp(parent_document.text.s, parent_document.saved.s,
                parent_document.text.n)) ||
        (parent_document.filename && parent_document.journal_dirty &&
         parent_document.history);
}
static void clipboard_return(void)
{
    if (!editing_clipboard) return;
    document_restore(&parent_document);
    editing_clipboard=false;
    clipboard_leave_after_save=false;
    reverse_mode=false; command_mode=false; menu_open=false;
    menu_argument_mode=false; group=false;
    clear_prompt();
    reset_screen(); resized=1;
    tell("Returned to %s.",filename ? filename : "[new file]");
}
/* Switching documents is transactional: failures keep the original file,
 * undo chain and cursor. A dirty buffer must be written first. */
static void read_document(const char *name)
{
    char *expanded=NULL,*path=NULL;
    Buffer fresh={0},fresh_saved={0};
    struct stat st;
    bool present=false;
    if (changed() || (filename && journal_dirty && history && !journal_save())) {
        tell("Unsaved work/history. Write before reading another file."); return;
    }
    expanded=expand_home_path(name);
    if (!expanded || !(path=resolve_path(expanded))) goto failure;
    if (lstat(path,&st)==0) {
        if (S_ISDIR(st.st_mode)) { /* Directory navigation is handled below. */
            tell("Directory given; use ls DIRECTORY to browse.");goto done;
        }
        if (!read_file(path,&fresh,&st)) goto failure;
        present=true;
    } else if (errno!=ENOENT) goto failure;
    if (fresh.n && memchr(fresh.s,0,fresh.n)) { tell("Binary file refused (NUL byte).");goto done; }
    if (fresh.n > LIMIT) { errno=EFBIG; goto failure; }
    if (!copy_buffer(&fresh_saved,fresh.s ? fresh.s : "",fresh.n) ||
        !reserve(&fresh,fresh.n+1)) { errno=ENOMEM;goto failure; }
    fresh.s[fresh.n]=0;
    free_changes(history); history=current=history_tail=NULL;
    free(text.s);text=fresh;memset(&fresh,0,sizeof fresh);
    free(saved.s);saved=fresh_saved;memset(&fresh_saved,0,sizeof fresh_saved);
    free(filename);filename=path;path=NULL;
    existed=present; if (present) original=st; else memset(&original,0,sizeof original);
    free(loaded_journal_path);loaded_journal_path=NULL;
    loaded_journal_stat_valid=false;
    journal_ignored=journal_dirty=journal_rebased=journal_branched=false;
    cursor=top=left=goal=0;goal_set=group=false;
    search_pattern[0]=0; search_length=0; search_mark=SIZE_MAX;
    newline[0]='\n';newline[1]=0;newline_n=1;
    if (filename) journal_load();
    {
        size_t i;
        for (i=0;i<text.n;i++) if (text.s[i]=='\n') {
            if (i && text.s[i-1]=='\r') {
                newline[0]='\r';newline[1]='\n';newline_n=2;
            }
            break;
        }
    }
    if (journal_ignored) tell("File opened; undo journal unavailable.");
    else if (journal_rebased) tell("File opened; outside edits are one undo.");
    else tell("File opened.");
    goto done;
failure:
    tell("Read failed: %s. Current file retained.",strerror(errno));
done:
    free(expanded);free(path);free(fresh.s);free(fresh_saved.s);
}
static void clipboard_open(void)
{
    char *path;
    if (editing_clipboard) {
        if (changed()) {
            /* No question: clipboard.txt is just a scratch file. Retain this
             * buffer and the original document on any write failure. */
            clipboard_leave_after_save = true;
            save_file(NULL, false);
        } else {
            /* No content to save; try to persist undo history if needed, but
             * don't trap the user in a clipboard with no unsaved text. */
            bool history_ok = true;
            if (journal_dirty && history) history_ok = journal_save();
            clipboard_return();
            if (!history_ok) tell("Returned; clipboard undo journal not saved.");
        }
        return;
    }
    path=clipboard_path(true);
    if (!path) { tell("Clipboard state directory unavailable."); return; }
    /* The clipboard belongs in the private state directory, never a
     * symlink to some other file or an existing public-readable file. */
    {
        struct stat st;
        if (lstat(path, &st)==0) {
            if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                (st.st_mode & 077)) {
                tell("Clipboard file is not private (regular, owned, mode 0600 required).");
                free(path); return;
            }
        } else if (errno!=ENOENT) {
            tell("Cannot inspect clipboard.txt: %s.",strerror(errno));
            free(path); return;
        }
    }
    /* Opening the active clipboard file itself needs no parent context. */
    if (filename && !strcmp(filename,path)) {
        tell("Already editing this clipboard."); free(path); return;
    }
    document_capture(&parent_document);
    read_document(path);
    if (!filename || strcmp(filename,path)) {
        /* A read error leaves a new empty context, not the user's old file. */
        document_restore(&parent_document);
        tell("Clipboard open failed; original document retained.");
    } else {
        editing_clipboard=true;
        reverse_mode=false; command_mode=false; menu_open=false;
        reset_screen(); resized=1;
        tell("Editing clipboard %s; write saves, clipboard returns.",
             *selected_clipboard ? selected_clipboard : "default");
    }
    free(path);
}
/* End-of-directory and an unsuccessful scan must be distinguished. A
 * partially scanned directory is never evidence of a unique completion. */
static int directory_next(DIR *dir, struct dirent **entry)
{
    if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
    errno = 0;
    *entry = readdir(dir);
    if (terminated || suspended || interrupted) { errno = EINTR; return -1; }
    return *entry ? 1 : errno ? -1 : 0;
}
static int filename_compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void directory_list(const char *provided)
{
    char *expanded = NULL;
    const char *target = provided && *provided ? provided : ".";
    DIR *d = NULL;
    struct dirent *ent;
    char **names = NULL;
    size_t n = 0, used = 0, i, limit = 1024;
    bool more = false;
    int len, result, error;
    expanded = expand_home_path(target);
    if (!expanded || !(d = opendir(expanded))) goto fail;
    names = calloc(limit, sizeof *names);
    if (!names) goto fail;
    while ((result = directory_next(d, &ent)) > 0) {
        char *entry;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (n == limit) { more = true; break; }
        entry = strdup(ent->d_name);
        if (!entry) goto fail;
        names[n++] = entry;
    }
    if (result < 0) goto fail;
    qsort(names, n, sizeof *names, filename_compare);
    len = snprintf(directory_page, sizeof directory_page,
        "DIRECTORY: %s\n%zu entries%s. Esc/q returns.\n\n",
        target, n, more ? "+ (listing truncated)" : "");
    if (len < 0 || (size_t)len >= sizeof directory_page) { errno = EOVERFLOW; goto fail; }
    used = (size_t)len;
    for (i = 0; i < n; i++) {
        struct stat st;
        bool is_dir;
        size_t j;
        if (terminated || suspended || interrupted) { errno = EINTR; goto fail; }
        /* Relative lookup uses the directory already open, not a new heap
         * pathname for every entry. Broken links still appear without '/'. */
        is_dir = fstatat(dirfd(d), names[i], &st, 0) == 0 && S_ISDIR(st.st_mode);
        for (j = 0; names[i][j] && used + 48 < sizeof directory_page; j++) {
            unsigned char ch = (unsigned char)names[i][j];
            if (ch == '\n' || ch == '\r') {
                directory_page[used++] = '\\';
                directory_page[used++] = ch == '\n' ? 'n' : 'r';
            } else if (ch < 32 || ch == 127) directory_page[used++] = '?';
            else directory_page[used++] = (char)ch;
        }
        if (names[i][j]) { more = true; break; }
        if (is_dir) directory_page[used++] = '/';
        directory_page[used++] = '\n';
    }
    directory_page[used] = 0;
    if (more) (void)snprintf(directory_page + used, sizeof directory_page - used,
                            "\n(Listing truncated.)\n");
    result = closedir(d); d = NULL;
    if (result < 0) goto fail;
    if (terminated || suspended || interrupted) { errno = EINTR; goto fail; }
    open_viewer("Files", directory_page);
    goto done;
fail:
    error = errno;
    tell("Directory listing failed: %s. No partial list shown.", strerror(error));
    if (command_mode) command_notice("Directory listing");
done:
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    if (d) (void)closedir(d);
    free(expanded);
}
static void complete_command(void);
static void path_complete(void)
{
    size_t start = 0, prefix_n, nmatch = 0, common = 0, append, extra;
    char *entered = NULL, *expanded = NULL, *dir = NULL, *slash;
    char *first = NULL, *full = NULL;
    const char *prefix;
    DIR *d = NULL;
    struct dirent *ent;
    bool directory = false;
    int result, error;
    if (command_n >= sizeof command || command[command_n] != 0 ||
        command_cursor != command_n) return;
    if (!filename_mode && !(menu_argument_mode && menu_argument_command &&
          (!strcmp(menu_argument_command, "read") || !strcmp(menu_argument_command, "ls") ||
           !strcmp(menu_argument_command, "write")))) {
        const CommandHelp *cmd;
        char verb[64]; size_t len;
        while (command[start] == ' ' || command[start] == '\t') start++;
        len = strcspn(command + start, " \t");
        if (!len || len >= sizeof verb || !command[start + len]) { complete_command(); return; }
        memcpy(verb, command + start, len); verb[len] = 0;
        cmd = command_lookup(verb);
        if (!cmd || (strcmp(cmd->name, "read") && strcmp(cmd->name, "write") &&
                     strcmp(cmd->name, "ls"))) { complete_command(); return; }
        start += len;
        while (command[start] == ' ' || command[start] == '\t') start++;
    }
    entered = strdup(command + start);
    if (!entered) goto fail;
    if (!*entered) { directory_list("."); goto done; }
    expanded = expand_home_path(entered);
    if (!expanded) goto fail;
    slash = strrchr(expanded, '/');
    prefix = slash ? slash + 1 : expanded;
    prefix_n = strlen(prefix);
    dir = slash ? strndup(expanded, (size_t)(slash - expanded) + 1) : strdup(".");
    if (!dir) goto fail;
    if (!prefix_n) { directory_list(expanded); goto done; }
    d = opendir(dir);
    if (!d) goto fail;
    while ((result = directory_next(d, &ent)) > 0) {
        if (strncmp(ent->d_name, prefix, prefix_n)) continue;
        if (!nmatch) {
            first = strdup(ent->d_name);
            if (!first) goto fail;
            common = strlen(first);
        } else {
            size_t j = 0;
            while (j < common && ent->d_name[j] && first[j] == ent->d_name[j]) j++;
            common = j;
        }
        nmatch++;
    }
    if (result < 0) goto fail;
    if (!nmatch) {
        tell("No matching filename; command unchanged.");
        command_notice("Filename completion"); goto done;
    }
    if (nmatch == 1) {
        struct stat st;
        /* Inspect before changing the draft, so any failure leaves the
         * exact original command available for another attempt. */
        if (fstatat(dirfd(d), first, &st, 0) < 0) goto fail;
        directory = S_ISDIR(st.st_mode);
        if (directory) {
            full = malloc(strlen(dir) + strlen(first) + 2);
            if (!full) goto fail;
            sprintf(full, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", first);
        }
    }
    result = closedir(d); d = NULL;
    if (result < 0) goto fail;
    if (terminated || suspended || interrupted) { errno = EINTR; goto fail; }
    /* A bytewise common prefix can stop inside a valid UTF-8 character.
     * Keep the full first name to identify that boundary. Invalid filename
     * bytes remain individual units; already typed bytes are never erased. */
    {
        size_t p = 0, length = strlen(first);
        uint32_t cp;
        while (p < common) {
            size_t next = p + decode(first, length, p, &cp);
            if (next > common) { common = p; break; }
            p = next;
        }
    }
    append = common > prefix_n ? common - prefix_n : 0;
    extra = directory ? 1 : 0;
    if (append + extra > sizeof command - 1 - command_n) {
        tell("Completion is too long; command unchanged.");
        command_notice("Filename completion"); goto done;
    }
    /* Commit only after a complete successful scan, all allocations, and
     * the space check. Never append '/' to an uncompleted directory name. */
    if (append) memcpy(command + command_n, first + prefix_n, append);
    command_n += append;
    if (extra) command[command_n++] = '/';
    command[command_n] = 0; command_cursor = command_n;
    if (directory) directory_list(full);
    else if (nmatch > 1) directory_list(dir);
    goto done;
fail:
    error = errno;
    tell("Cannot complete path: %s. Command unchanged.", strerror(error));
    command_notice("Filename completion");
done:
    if (d) (void)closedir(d);
    free(entered); free(expanded); free(dir); free(first); free(full);
}

static void paste(void)
{
    static const char end[] = "\033[201~";
    Buffer b = {0};
    bool invalid = command_mode ?
        (command_n >= COMMAND_SIZE || command_cursor > command_n || command[command_n] != 0) :
        (text.n > LIMIT || cursor > text.n);
    bool rejected = invalid, draining = paste_discard;
    bool size_rejected = false;
    size_t limit = invalid ? 0 : command_mode ? COMMAND_SIZE - 1 - command_n : LIMIT - text.n;
    bool ignored = draining || prompt != PROMPT_NONE || viewer_text != NULL ||
        menu_open || reverse_mode || (movement_mode && !command_mode);
    size_t matched = draining ? paste_end_matched : 0;
    int c, idle = 0;
    paste_discard = false; paste_end_matched = 0;
    group = false;
    for (;;) {
        c = byte(250);
        if (terminated || c == -1) break;
        if (suspended || interrupted) {
            paste_discard = true; paste_end_matched = matched;
            break;
        }
        if (c < 0) {
            if (++idle >= 20) { tell("Paste timed out; text unchanged."); break; }
            if (resized) draw();
            continue;
        }
        idle = 0;
        if (c == end[matched]) {
            matched++;
            if (matched == sizeof end - 1) {
                if (ignored) {
                    if (draining) { tell("Cancelled paste discarded; document unchanged."); resized=1; }
                    free(b.s); return;
                }
                if (rejected) {
                    if (size_rejected && command_mode)
                        tell("Paste exceeds command space; draft unchanged.");
                    else tell("Paste refused: NUL byte, memory, or size limit.");
                    if (command_mode) command_notice("Paste not inserted");
                } else if (command_mode) {
                    if (b.n && (memchr(b.s, '\n', b.n) || memchr(b.s, '\r', b.n) || memchr(b.s, 27, b.n))) {
                        tell("Paste one command line at a time; draft unchanged.");
                        command_notice("Paste not inserted");
                    }
                    else if (b.n < COMMAND_SIZE - command_n) {
                        memmove(command + command_cursor + b.n, command + command_cursor,
                                command_n - command_cursor + 1);
                        if (b.n) memcpy(command + command_cursor, b.s, b.n);
                        command_cursor += b.n; command_n += b.n;
                    }
                } else (void)edit(cursor, 0, b.s, b.n, false);
                free(b.s); return;
            }
            continue;
        }
        if (matched && !rejected && !ignored) {
            if (matched > limit - b.n) rejected = size_rejected = true;
            else if (!reserve(&b, b.n + matched + 1)) rejected = true;
            else { memcpy(b.s + b.n, end, matched); b.n += matched; }
        }
        matched = 0;
        if (c == 27) { matched = 1; continue; }
        if (!c && !ignored) rejected = true;
        if (!rejected && !ignored) {
            if (b.n >= limit) rejected = size_rejected = true;
            else if (!reserve(&b, b.n + 2)) rejected = true;
            else { b.s[b.n++] = (char)c; b.s[b.n] = 0; }
        }
    }
    free(b.s);
}

static void reverse_key(int c)
{
    size_t amount = 0;
    bool redo = false;
    if (c == KEY_ESCAPE || c == 'q') {
        reverse_mode = false;
        tell("History position %zu/%zu; write to save this version.",
             reverse_position, reverse_total);
        return;
    }
    /* One history entry per event, including terminal key autorepeat.
     * The number keys have no action in Reverse mode; use + and - only.
     * Home and End retain direct oldest/newest navigation. */
    if (c == KEY_LEFT || c == 'h' || c == '<' || c == '-') amount = 1;
    else if (c == KEY_RIGHT || c == 'l' || c == '>' || c == '+') { amount = 1; redo = true; }
    else if (c == KEY_HOME || c == KEY_TOP) amount = reverse_position;
    else if (c == KEY_END || c == KEY_BOTTOM) { redo = true; amount = reverse_total - reverse_position; }
    if (amount) (void)history_move(redo, amount, true);
}

static void command_set(const char *s)
{
    size_t n = strlen(s);
    if (n >= COMMAND_SIZE) n = COMMAND_SIZE - 1;
    memcpy(command, s, n); command[n] = 0;
    command_n = command_cursor = n;
}
/* Tab completes command names using the same table as the parser. The first
 * word is completable; the argument of 'help' and the verb in 'N dline'
 * are also commands. Filenames, arbitrary arguments, document tabs and user
 * key bindings remain entirely untouched. Ambiguities open a read-only
 * viewer and leave the draft intact. */
static void complete_command(void)
{
    size_t start=0, end=0, typed, i, matches=0, word_end, tail_n;
    char first[64], replacement[64];
    const char *token, *begin, *finish;
    bool found, prefer_canonical=false;
    size_t used;
    while (command[start] == ' ' || command[start] == '\t') start++;
    word_end=start;
    while (command[word_end] && command[word_end]!=' ' && command[word_end]!='\t') word_end++;
    if (command_cursor > word_end) {
        size_t n=word_end-start;
        bool number=n>0;
        if (!n || n >= sizeof first) return;
        memcpy(first,command+start,n); first[n]=0;
        for (i=0;i<n;i++) if (first[i]<'0' || first[i]>'9') number=false;
        {
            const CommandHelp *context=command_lookup(first);
            if ((!context || (strcmp(context->name,"help") && strcmp(context->name,"list"))) && !number) return;
        }
        start=word_end;
        while (command[start]==' ' || command[start]=='\t') start++;
    }
    end=start;
    while (command[end] && command[end]!=' ' && command[end]!='\t') end++;
    if (command_cursor == start && start == command_n) {
        command_overview("",true); return;
    }
    if (command_cursor <= start || command_cursor > end) return;
    typed=command_cursor-start;
    used=0;
    guide_append(&used,"More than one command matches.\n"
                       "Esc/q returns to your unchanged command.\n"
                       "Type more letters and press Tab again.\n\n");
    replacement[0]=0;
    /* Prefer matching full command names over aliases. Otherwise an alias
     * such as "line" could block completion of the distinct "list" name. */
    for (i=0;i<COMMAND_INFO_COUNT;i++)
        if (!strncasecmp(command_info[i].name,command+start,typed))
            prefer_canonical=true;
    for (i=0;i<COMMAND_INFO_COUNT;i++) {
        found=false; token=NULL;
        if (!strncasecmp(command_info[i].name,command+start,typed)) {
            token=command_info[i].name;
            found=true;
        } else if (!prefer_canonical) {
            const char *p=command_info[i].synonyms;
            while (*p) {
                size_t n;
                while (*p==' ' || *p==',') p++;
                begin=p;
                while (*p && *p!=',') p++;
                finish=p;
                while (finish>begin && finish[-1]==' ') finish--;
                n=(size_t)(finish-begin);
                if (n>=typed && !strncasecmp(begin,command+start,typed)) {
                    /* Alias completion retains the alias the user began. */
                    if (n<sizeof replacement) {
                        if (!found) {
                            char alias[64];
                            memcpy(alias,begin,n); alias[n]=0;
                            /* Save the candidate only if this is the sole
                             * matching canonical operation so far. */
                            if (matches==0) strcpy(replacement,alias);
                            found=true;
                        }
                    }
                    break;
                }
            }
        }
        if (found) {
            if (!matches && token) {
                snprintf(replacement,sizeof replacement,"%s",token);
            }
            matches++;
            guide_append(&used,"  %s\n    %s\n",command_info[i].name,command_info[i].summary);
        }
    }
    if (matches==1) {
        size_t len=strlen(replacement);
        tail_n=command_n-end;
        if (command_n-(end-start)+len >= COMMAND_SIZE) return;
        memmove(command+start+len,command+end,tail_n+1);
        memcpy(command+start,replacement,len);
        command_n=command_n-(end-start)+len;
        command_cursor=start+len;
        history_choice=-1;
    } else if (matches>1) open_viewer("Choose a command",command_page);
    else {
        used=0;
        guide_append(&used,"No command starts with that text.\n"
                           "Correct the name, or use list WORD to find a task.\n");
        open_viewer("No completion",command_page);
    }
}

/* Filename entry reuses the existing command-line editor: cursor movement,
 * Backspace, Delete and bracketed paste are already supported. This isn't a
 * new terminal input engine or a default hotkey. */
static void ask_filename(bool for_read)
{
    clear_prompt();
    filename_read_mode = for_read;
    filename_mode = true;
    command_mode = true;
    command_set("");
    history_choice = -1;
    group = false;
    tell(for_read ? "Open filename, or Esc to cancel." : "Filename: enter a destination, or Esc to cancel.");
}
static void request_menu_write(void)
{
    struct stat st;
    /* Clipboard Write saves without leaving or asking for confirmation. */
    if (editing_clipboard) { save_file(NULL, false); return; }
    if (!filename) { ask_filename(false); return; }
    if (lstat(filename, &st) == 0) {
        /* A menu Write always offers a choice of overwrite or another name.
         * A later write performs its own independent disk/version checks. */
        clear_prompt();
        prompt = PROMPT_MENU_WRITE;
        tell("Overwrite existing file? Y/n");
    } else if (errno == ENOENT) save_file(NULL, false);
    else tell("Cannot inspect destination: %s", strerror(errno));
}
static void command_remember(const char *s)
{
    size_t i;
    char *copy;
    if (!*s) return;
    for (i = 0; i < command_history_n; i++) if (!strcmp(command_history[i], s)) {
        copy = command_history[i];
        memmove(command_history + 1, command_history, i * sizeof *command_history);
        command_history[0] = copy; return;
    }
    copy = strdup(s);
    if (!copy) return;
    if (command_history_n == COMMAND_HISTORY_MAX) free(command_history[--command_history_n]);
    memmove(command_history + 1, command_history, command_history_n * sizeof *command_history);
    command_history[0] = copy; command_history_n++;
}
static void command_browse(int direction)
{
    if (direction < 0) {
        if (!command_history_n) return;
        if (history_choice < 0) {
            memcpy(command_draft, command, command_n + 1);
            history_choice = 0;
        } else if (history_choice + 1 < (int)command_history_n) history_choice++;
        command_set(command_history[history_choice]);
    } else {
        if (history_choice < 0) return;
        if (history_choice > 0) history_choice--;
        else { history_choice = -1; command_set(command_draft); return; }
        command_set(command_history[history_choice]);
    }
}
/* Line deletion is always one undoable transaction, including ranges.
 * The empty row following a final newline is addressable as the last line.
 * Removing the final row also removes its preceding line separator; this
 * handles both LF and CRLF without leaving an unintended trailing blank row.
 */
static size_t line_count(void)
{
    size_t i, count = 1;
    for (i = 0; i < text.n; i++) if (text.s[i] == '\n') count++;
    return count;
}
static size_t numbered_line(size_t number)
{
    size_t i, line = 1;
    /* A single pass also detects an out-of-range line number. */
    if (!number) return SIZE_MAX;
    if (number == 1) return 0;
    for (i = 0; i < text.n; i++) if (text.s[i] == '\n' && ++line == number) return i+1;
    return SIZE_MAX;
}
static size_t current_line_number(void)
{
    size_t i, line = 1;
    for (i = 0; i < cursor; i++) if (text.s[i] == '\n') line++;
    return line;
}
static void delete_line_range(size_t first, size_t last)
{
    size_t from, end, total = line_count();
    if (!text.n) { tell("File is already empty."); return; }
    if (!first || first > last || last > total) {
        tell("Line range must be within 1-%zu; nothing deleted.", total); return;
    }
    from = numbered_line(first);
    end = last == total ? text.n : numbered_line(last+1);
    if (from == SIZE_MAX || end == SIZE_MAX || from > end) {
        tell("Invalid line range; nothing deleted."); return;
    }
    if (last == total && from > 0) {
        /* Include preceding LF (and CR if present) for the final line. */
        from--;
        if (from && text.s[from-1] == '\r') from--;
    }
    if (end > from) (void)edit(from, end-from, NULL, 0, false);
    else tell("No line to delete.");
}
static bool positive_number(const char *s, const char **after, size_t *out)
{
    char *end; unsigned long long number;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    errno = 0; number = strtoull(s, &end, 10);
    if (errno || end == s || number == 0 || number > SIZE_MAX) return false;
    *out = (size_t)number; *after = end;
    return true;
}
static bool blank_tail(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return !*s;
}
static void vertical(int direction, int count);
static bool history_suppress;

static void execute(void);
static void menu_run(void)
{
    size_t count;
    const char *title;
    const MenuItem *entries=menu_entries(&count,&title), *chosen;
    const char *cmd;
    (void)title;
    if (menu_selection >= count) return;
    chosen=entries+menu_selection;
    if (chosen->action == MENU_SUBPAGE) { menu_enter(chosen->child); return; }
    if (chosen->action == MENU_QUIT_FORCE &&
        (changed() || (filename && journal_dirty && history) || parent_unsaved()) &&
        !menu_discard_pending) {
        menu_discard_pending = true;
        tell("Unsaved changes. Press Enter again to discard; Esc returns.");
        return;
    }
    menu_open=false; menu_number=-1; menu_discard_pending=false; group=false;
    if (chosen->action == MENU_WRITE) { request_menu_write(); return; }
    if (chosen->action == MENU_PARAMETER) {
        menu_argument_mode=true;
        menu_argument_command=chosen->command;
        menu_argument_label=chosen->question;
        command_mode=true; command_set(""); history_choice=-1;
        return;
    }
    if (chosen->action == MENU_MOUSE_TOGGLE) {
        mouse_enabled=!mouse_enabled; mouse_tracking(mouse_enabled);
        tell("Mouse %s.",mouse_enabled ? "enabled" : "disabled"); return;
    }
    if (chosen->action == MENU_ESC_TOGGLE) {
        menu_preferred=!menu_preferred;
        tell("Esc now opens %s.",menu_preferred ? "the menu" : "Command>"); return;
    }
    if (chosen->action == MENU_QUIT_FORCE) {
        running=false;
        return;
    }
    cmd=chosen->command;
    if (!cmd) return;
    if (!strcmp(cmd,"@cli-prompt")) { command_mode=true; command_set(""); return; }
    command_set(cmd);
    history_suppress=true; execute(); history_suppress=false;
}
static void menu_key(int c)
{
    size_t count;
    const char *title;
    (void)menu_entries(&count,&title);
    if (c == KEY_ESCAPE || c == KEY_LEFT || c == 'h' || c == 'q') {
        menu_back(); return;
    }
    if (c >= '0' && c <= '9') {
        int d=c-'0';
        if (menu_number < 0) menu_number=d;
        else if (menu_number < 100) menu_number=menu_number * 10 + d;
        if (menu_number >= 1 && (size_t)menu_number <= count)
            menu_selection=(size_t)menu_number-1;
        menu_discard_pending=false; return;
    }
    if (c == 127 || c == 8) {
        if (menu_number > 0) menu_number /= 10;
        else menu_number = -1;
        menu_discard_pending=false; return;
    }
    if (c != '\r' && c != '\n') { menu_discard_pending=false; menu_number=-1; }
    if (c == KEY_UP || c == 'k' || c == KEY_PAGEUP) {
        size_t step=(c == KEY_UP || c == 'k') ? 1 : (size_t)(rows > 4 ? rows-4 : 1);
        menu_selection=step > menu_selection ? 0 : menu_selection-step;
    } else if (c == KEY_DOWN || c == 'j' || c == KEY_PAGEDOWN) {
        size_t step=(c == KEY_DOWN || c == 'j') ? 1 : (size_t)(rows > 4 ? rows-4 : 1);
        menu_selection=step > count-1-menu_selection ? count-1 : menu_selection+step;
    } else if (c == KEY_HOME) menu_selection=0;
    else if (c == KEY_END) menu_selection=count-1;
    else if (c == '\r' || c == '\n' || c == KEY_RIGHT || c == 'l') {
        if (menu_number >= 0 && ((size_t)menu_number < 1 || (size_t)menu_number > count)) {
            tell("Choose 1 through %zu.",count); menu_number=-1; return;
        }
        menu_run();
    }
}
static void execute(void)
{
    char parsed[COMMAND_SIZE], *verb = parsed, *arg;
    const CommandHelp *info;
    const char *canonical;
    size_t count, line, column, p;
    const char *rest;
    if (command_n >= sizeof command || command[command_n] != 0) {
        command_set(""); command_mode = true;
        tell("Invalid command input cleared; document unchanged."); return;
    }
    memcpy(parsed, command, command_n + 1);
    while (*verb == ' ' || *verb == '\t') verb++;
    if (*verb && !history_suppress) command_remember(verb);
    history_choice = -1;
    arg = verb;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
    command_mode = false; group = false;
    if (!*verb) return;
    /* The count-first form is restricted to deletion, to prevent a number
     * from accidentally running any other command or becoming a keybinding.
     * Example: 13 dline removes 13 consecutive lines at the cursor. */
    {
        size_t repeat;
        const char *tail;
        if (positive_number(verb, &tail, &repeat) && blank_tail(tail)) {
            char *sub = arg, *subarg;
            size_t first = current_line_number(), total = line_count();
            subarg = sub;
            while (*subarg && *subarg != ' ' && *subarg != '\t') subarg++;
            if (*subarg) { *subarg++ = 0; while (*subarg == ' ' || *subarg == '\t') subarg++; }
            info = command_lookup(sub);
            if (!info || strcmp(info->name, "dline") || !blank_tail(subarg)) {
                tell("Use COUNT dline; e.g. 13 dline. Nothing deleted."); return;
            }
            if (repeat > total - first + 1) {
                tell("Only %zu lines remain; nothing deleted.", total-first+1); return;
            }
            delete_line_range(first, first+repeat-1);
            return;
        }
    }
    info = command_lookup(verb);
    if (!info) { command_unknown(verb); return; }
    canonical = info->name;
    if (!strcmp(canonical,"find")) {
        if (!*arg) request_argument("find","Find> ");
        else start_search(arg);
        return;
    }
    if (!strcmp(canonical,"next") || !strcmp(canonical,"prev")) {
        if (!blank_tail(arg)) {
            command_usage(info,"%s takes no argument; use find TEXT to set the search.",canonical);
        } else (void)find_match(!strcmp(canonical,"next"),false);
        return;
    }
    if (!strcmp(canonical,"replace") || !strcmp(canonical,"replaceall")) {
        char *delimiter;
        bool all=!strcmp(canonical,"replaceall");
        if (!*arg) {
            replace_everywhere=all;
            menu_argument_mode=true;
            menu_argument_command="@replace-search";
            menu_argument_label="Find text> ";
            command_mode=true; command_set(""); history_choice=-1;
            return;
        }
        delimiter=strstr(arg," => ");
        if (delimiter) {
            *delimiter=0;
            run_replacement(arg,delimiter+4,all);
        } else {
            if (strlen(arg)>=sizeof replace_search) { tell("Search text too long."); return; }
            memcpy(replace_search,arg,strlen(arg)+1);
            replace_everywhere=all;
            menu_argument_mode=true;
            menu_argument_command="@replace-value";
            menu_argument_label="Replace with> ";
            command_mode=true; command_set(""); history_choice=-1;
        }
        return;
    }
    if (!strcmp(canonical,"read")) {
        if (editing_clipboard) {
            tell("Clipboard mode: use clipboard to return before reading another file."); return;
        }
        if (!*arg) ask_filename(true);
        else read_document(arg);
        return;
    }
    if (!strcmp(canonical,"ls")) {
        directory_list(*arg ? arg : "."); return;
    }
    if (!strcmp(canonical,"clipboard")) {
        if (!*arg) clipboard_open();
        else if (!strcmp(arg,"list")) clipboard_list();
        else {
            const char *slot=!strcmp(arg,"default") ? "" : arg;
            bool in_subeditor=editing_clipboard;
            char *target;
            struct stat st;
            if (*slot && !clipboard_slot_valid(slot)) {
                tell("Slot: 1-32 letters/digits, '-' or '_'; selected is reserved; default/list are commands.");
                return;
            }
            if (!strcmp(slot,selected_clipboard)) {
                tell("Clipboard %s already selected.", *slot ? slot : "default");return;
            }
            /* Reject an unsafe target before saving/closing this context. */
            target=clipboard_path_for(slot,true);
            if (!target) { tell("Clipboard path unavailable."); return; }
            if (lstat(target,&st)==0) {
                if (!S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
                    (st.st_mode & 077)) {
                    tell("Selected clipboard file has unsafe permissions.");
                    free(target);return;
                }
            } else if (errno!=ENOENT) {
                tell("Cannot inspect selected clipboard: %s.",strerror(errno));
                free(target);return;
            }
            free(target);
            if (in_subeditor) {
                clipboard_open(); /* save current slot and return, or stay on failure */
                if (editing_clipboard) return;
            }
            if (!clipboard_choose(slot)) {
                tell("Could not persist clipboard selection: %s.",strerror(errno));return;
            }
            if (in_subeditor) clipboard_open();
            else tell("Selected clipboard %s.", *slot ? slot : "default");
        }
        return;
    }
    if (!strcmp(canonical,"cliphistory")) {
        if (!*arg) clip_history_list();
        else if (!strcmp(arg,"on") || !strcmp(arg,"off")) {
            if (clipboard_history_setting(!strcmp(arg,"on")))
                tell("Clipboard history %s.",clip_history_enabled ? "enabled" : "disabled");
            else tell("Unable to save clipboard history setting.");
        } else tell("Use cliphistory [on|off].");
        return;
    }
    if (!strcmp(canonical,"clipuse")) {
        if (editing_clipboard) {
            tell("Use clipboard to return before restoring an entry."); return;
        }
        if (!*arg) { request_argument("clipuse","History number> "); return; }
        if (!positive_number(arg,&rest,&count) || !blank_tail(rest))
            tell("Use clipuse NUMBER (1 newest).");
        else clip_history_use(count);
        return;
    }
    if (!strcmp(canonical,"copy") || !strcmp(canonical,"cut")) {
        if (editing_clipboard) {
            tell("Copy/cut would replace the active clipboard; use clipboard to return.");
            return;
        }
        size_t last,from,end,remove_start,all=line_count();
        bool is_cut=!strcmp(canonical,"cut");
        int clip_result;
        const char *clip_warning;
        if (!text.n) { tell("Document is empty."); return; }
        if (!*arg) { line=current_line_number(); last=line; }
        else {
            if (!positive_number(arg,&rest,&line)) {
                tell("Use %s [LINE|FIRST-LAST].",canonical); return;
            }
            while (*rest==' ' || *rest=='\t') rest++;
            last=line;
            if (*rest=='-') {
                if (!positive_number(rest+1,&rest,&last)) {
                    tell("Use %s FIRST-LAST.",canonical); return;
                }
            }
            if (!blank_tail(rest)) { tell("Use %s [LINE|FIRST-LAST].",canonical);return; }
        }
        if (!line || last<line || last>all) {
            tell("Line range must be within 1-%zu.",all); return;
        }
        from=numbered_line(line);
        end=last==all ? text.n : numbered_line(last+1);
        if (from==SIZE_MAX || end==SIZE_MAX || from>end) {
            tell("Invalid line range."); return;
        }
        if (from==end && from==text.n && from>0 && text.s[from-1]=='\n') {
            size_t separator=(from>1 && text.s[from-2]=='\r') ? 2 : 1;
            clip_result=clipboard_set(text.s+from-separator,separator);
        } else clip_result=clipboard_set(text.s+from,end-from);
        if (!clip_result) return;
        clip_warning=clip_result == CLIP_SAVED_NO_HISTORY ?
            " Clipboard history save/cleanup incomplete." : "";
        if (is_cut) {
            remove_start=from;
            if (last==all && remove_start) {
                remove_start--;
                if (remove_start && text.s[remove_start-1]=='\r') remove_start--;
            }
            if (end>remove_start && !edit(remove_start,end-remove_start,NULL,0,false))
                tell("Cut failed; clipboard was updated.%s",clip_warning);
            else tell("Cut lines %zu-%zu.%s",line,last,clip_warning);
        } else tell("Copied lines %zu-%zu to clipboard %s.%s",line,last,
                    *selected_clipboard ? selected_clipboard : "default",clip_warning);
        return;
    }
    if (!strcmp(canonical,"paste")) {
        Buffer clip={0},insert={0};
        size_t at=line_start(cursor);
        int exists;
        const char *slot=selected_clipboard;
        if (*arg) {
            slot=!strcmp(arg,"default") ? "" : arg;
            if (*slot && !clipboard_slot_valid(slot)) {
                tell("Use paste [SLOT|default].");return;
            }
        }
        exists=clipboard_read_named(slot,&clip);
        if (exists<0) { tell("Clipboard read failed: %s.",strerror(errno));return; }
        if (exists==0 || clip.n==0) { tell("Clipboard empty.");free(clip.s);return; }
        /* Preserve original clipboard line endings; insert separators only
         * where a non-terminated last line would otherwise get joined. */
        {
            size_t lead=(at && text.s[at-1]!='\n') ? newline_n : 0;
            size_t tail=(at<text.n && clip.s[clip.n-1]!='\n') ? newline_n : 0;
            insert.n=lead+clip.n+tail;
            if (insert.n > LIMIT-text.n) tell("Paste exceeds 8 MiB file limit.");
            else if (!reserve(&insert,insert.n+1)) tell("Not enough memory to paste.");
            else {
                if (lead) memcpy(insert.s,newline,lead);
                memcpy(insert.s+lead,clip.s,clip.n);
                if (tail) memcpy(insert.s+lead+clip.n,newline,tail);
                insert.s[insert.n]=0;
                if (edit(at,0,insert.s,insert.n,false))
                    tell("Pasted clipboard %s.",*slot ? slot : "default");
            }
        }
        free(clip.s);free(insert.s);return;
    }
    if (!strcmp(canonical, "write")) {
        if (!*arg && !filename) ask_filename(false);
        else save_file(*arg ? arg : NULL, false);
        return;
    }
    if (!strcmp(canonical, "list")) {
        command_overview(arg,false); return;
    }
    if (!strcmp(canonical, "help")) {
        if (*arg) command_help(arg);
        else guide_open("Help - start here",help_topics,false);
        return;
    }
    if (!strcmp(canonical, "dline")) {
        size_t last;
        if (!*arg) { line = current_line_number(); last = line; }
        else {
            if (!positive_number(arg, &rest, &line)) {
                tell("Use dline [LINE|FIRST-LAST]; or COUNT dline."); return;
            }
            while (*rest == ' ' || *rest == '\t') rest++;
            last = line;
            if (*rest == '-') {
                if (!positive_number(rest+1, &rest, &last)) {
                    tell("Use dline FIRST-LAST, e.g. dline 30-43."); return;
                }
            }
            if (!blank_tail(rest)) {
                tell("Use dline [LINE|FIRST-LAST]; or COUNT dline."); return;
            }
        }
        delete_line_range(line, last); return;
    }
    if (!strcmp(canonical, "goto")) {
        if (!*arg) { request_argument("goto","Line [column]> "); return; }
        if (!positive_number(arg, &rest, &line) ||
            (p=numbered_line(line)) == SIZE_MAX) {
            command_usage(info,"Choose a line from 1 to %zu; nothing moved.",line_count()); return;
        }
        column = 1;
        if (!blank_tail(rest) && (!positive_number(rest,&rest,&column) || !blank_tail(rest))) {
            command_usage(info,"After the line, enter a positive column number, e.g. goto 5 12."); return;
        }
        cursor = position_at(p, column-1); goal_set=false; return;
    }
    if (!strcmp(canonical, "left") || !strcmp(canonical, "right") ||
        !strcmp(canonical, "up") || !strcmp(canonical, "down")) {
        count = 1;
        if (*arg && (!positive_number(arg,&rest,&count) || !blank_tail(rest))) {
            command_usage(info,"The count must be a positive number, e.g. %s 3.",canonical); return;
        }
        if (count > LIMIT) count = LIMIT;
        if (!strcmp(canonical, "left") || !strcmp(canonical, "right")) {
            while (count--) {
                size_t next = !strcmp(canonical,"left") ?
                    prev_pos(text.s,text.n,cursor) : next_pos(text.s,text.n,cursor);
                if (next == cursor) break;
                cursor = next;
            }
            goal_set=false;
        } else vertical(!strcmp(canonical,"up") ? -1 : 1, (int)count);
        return;
    }
    if (!strcmp(canonical, "undo") || !strcmp(canonical, "redo")) {
        count = 1;
        if (*arg && (!positive_number(arg, &rest, &count) || !blank_tail(rest))) {
            command_usage(info,"The count must be a positive number, e.g. %s 3.",canonical); return;
        }
        (void)history_move(!strcmp(canonical, "redo"), count, false);
        return;
    }
    if (!strcmp(canonical, "reverse")) {
        size_t target = 0, total = undo_depth() + redo_depth();
        if (*arg) {
            char *end;
            unsigned long long number;
            /* An absolute register index, including 0 for oldest. */
            if (*arg < '0' || *arg > '9') {
                tell("Use reverse [INDEX] (0 through %zu).", total); return;
            }
            errno = 0;
            number = strtoull(arg, &end, 10);
            if (errno || end == arg || !blank_tail(end) || number > total) {
                tell("Use reverse [INDEX] (0 through %zu).", total); return;
            }
            target = (size_t)number;
        }
        reverse_open();
        if (*arg && target != reverse_position) {
            size_t moved = history_move(target > reverse_position,
                                        target > reverse_position ? target - reverse_position :
                                        reverse_position - target, true);
            if (reverse_position != target)
                tell("Stopped at history entry %zu/%zu after %zu steps.",
                     reverse_position, reverse_total, moved);
        }
        return;
    }
    if (!strcmp(canonical, "movement") || !strcmp(canonical, "edit")) {
        if (!blank_tail(arg)) { command_usage(info,"Type just %s, without an argument.",canonical); return; }
        movement_mode = !strcmp(canonical, "movement");
        goal_set = false; group = false;
        tell("%s.", movement_mode ? "Movement (Enter: edit)" : "Edit");
        return;
    }
    if (!strcmp(canonical,"deleol") || !strcmp(canonical,"deleof") || !strcmp(canonical,"backbof")) {
        size_t start=cursor, length=0;
        if (!blank_tail(arg)) { command_usage(info,"Type just %s, without an argument.",canonical); return; }
        if (!strcmp(canonical,"deleol")) {
            size_t end=line_end(cursor);
            length=end>cursor ? end-cursor : 0;
        }
        else if (!strcmp(canonical,"deleof")) length=text.n-cursor;
        else { start=0; length=cursor; }
        if (!length) { tell("Already at boundary; nothing deleted."); return; }
        if (edit(start,length,NULL,0,false))
            tell("Deleted %zu byte%s (%s).",length,length==1?"":"s",canonical);
        return;
    }
    if (*arg && strcmp(canonical, "mouse") && strcmp(canonical, "signals") &&
        strcmp(canonical, "bind") && strcmp(canonical, "unbind")) {
        command_usage(info,"%s takes no argument. Type just %s.",canonical,canonical); return;
    }
    else if (!strcmp(canonical, "history"))
        tell("Undo: %zu steps; redo: %zu; saved after successful writes.",
             undo_depth(), redo_depth());
    else if (!strcmp(canonical, "top")) { cursor=0; goal_set=false; }
    else if (!strcmp(canonical, "bottom")) { cursor=text.n; goal_set=false; }
    else if (!strcmp(canonical, "pageup") || !strcmp(canonical, "pagedown"))
        vertical(!strcmp(canonical,"pageup") ? -1 : 1, rows>3 ? rows-3 : 1);
    else if (!strcmp(canonical, "menu")) {
        menu_preferred=true; menu_enter(MENU_ROOT);
    } else if (!strcmp(canonical, "cli")) {
        menu_preferred=false; tell("Esc now opens Command>.");
    } else if (!strcmp(canonical, "mouse")) {
        if (!*arg || !strcmp(arg,"toggle")) mouse_enabled=!mouse_enabled;
        else if (!strcmp(arg,"on")) mouse_enabled=true;
        else if (!strcmp(arg,"off")) mouse_enabled=false;
        else { tell("Use mouse on, mouse off, or mouse toggle."); return; }
        mouse_tracking(mouse_enabled);
        tell("Mouse %s.",mouse_enabled ? "enabled" : "disabled");
    } else if (!strcmp(canonical, "signals")) {
        if (!strcmp(arg, "toggle")) signal_controls = !signal_controls;
        else if (!strcmp(arg, "on")) signal_controls = 1;
        else if (!strcmp(arg, "off")) signal_controls = 0;
        else if (*arg) { tell("Use signals on, signals off, or signals toggle."); return; }
        tell("Signal handling %s: interrupt %s; stop suspends. Config: signal_controls=%s.",
             signal_controls ? "ON" : "OFF", signal_controls ? "cancels" : "exits",
             signal_controls ? "on" : "off");
    } else if (!strcmp(canonical, "bindings")) show_bindings();
    else if (!strcmp(canonical, "bind")) {
        char keyname[48]; size_t n=strcspn(arg," \t");
        const char *action=arg+n;
        while (*action==' ' || *action=='\t') action++;
        if (!n || n>=sizeof keyname || !*action) {
            tell("Use bind KEY COMMAND. See bindings for key names."); return;
        }
        memcpy(keyname,arg,n); keyname[n]=0;
        if (!binding_assign(keyname,action)) {
            tell("Key/command not supported. See bindings."); return;
        }
        if (save_bindings()) tell("Bound %s to %s.",keyname,action);
        else tell("Bound for this session; could not save keys.conf.");
    } else if (!strcmp(canonical, "unbind")) {
        char keyname[24]; int code,i;
        if (!*arg || strcspn(arg," \t") != strlen(arg)) {
            tell("Use unbind KEY."); return;
        }
        code=hotkey_code(arg,keyname); i=binding_index(code);
        if (code<0 || i<0) { tell("No binding for %s.",arg); return; }
        memmove(binds+i,binds+i+1,(bind_count-(size_t)i-1)*sizeof *binds);
        bind_count--;
        if (save_bindings()) tell("Unbound %s.",arg);
        else tell("Unbound for this session; could not save keys.conf.");
    }
    else if (!strcmp(canonical, "suspend")) suspended=1;
    else if (!strcmp(canonical, "about")) open_viewer("About",about_text);
    else if (!strcmp(canonical, "manual")) open_viewer("Manual",manual_text);
    else if (!strcmp(canonical, "quit!")) {
        running=false;
    } else if (!strcmp(canonical, "quit")) {
        if (changed() || (filename && journal_dirty && history) || parent_unsaved())
            tell("Unsaved work. Use write, clipboard, or quit! to discard.");
        else { ordinary_quit = true; running=false; }
    }
}

static void prompt_key(int c)
{
    int kind = prompt;
    bool yes = c == 'y' || c == 'Y';
    if (c == KEY_ESCAPE) {
        clipboard_leave_after_save=false;
        clear_prompt(); tell("Cancelled; document unchanged."); return;
    }
    if (kind == PROMPT_QUIT) {
        clear_prompt();
        if (yes) running=false;
        else tell("Cancelled.");
    } else if (kind == PROMPT_MENU_WRITE) {
        clear_prompt();
        if (yes) save_file(NULL, false);
        else ask_filename(false);
    } else if (kind == PROMPT_TARGET_OVERWRITE) {
        if (yes) {
            /* Detach the name before a new prompt can free the old one. */
            char *target = pending_write_path;
            pending_write_path = NULL;
            prompt = PROMPT_NONE;
            save_file(target, true);
            free(target);
            if (prompt == PROMPT_NONE) clear_prompt();
        } else {
            clear_prompt(); ask_filename(false);
        }
    } else if (kind == PROMPT_OVERWRITE) {
        if (yes) {
            /* The saved snapshot is valid only for the file version observed
             * before this individual confirmation. */
            prompt = PROMPT_NONE;
            save_file(NULL, true);
            if (prompt == PROMPT_NONE) clear_prompt();
        } else {
            clipboard_leave_after_save=false;
            clear_prompt();
            if (editing_clipboard) tell("Clipboard write cancelled; still editing clipboard.txt.");
            else ask_filename(false);
        }
    }
}

static void command_key(int c)
{
    size_t p;
    if (filename_mode) {
        if (c == KEY_ESCAPE) {
            filename_mode = command_mode = false;
            tell("Filename selection cancelled."); return;
        }
        if (c == '\r' || c == '\n') {
            if (blank_tail(command)) { tell("Enter a filename, or Esc to cancel."); return; }
            filename_mode = command_mode = false;
            group = false;
            if (filename_read_mode) read_document(command);
            else save_file(command, false);
            return;
        }
        /* Filename entry uses editing keys, not command-history navigation. */
        if (c == KEY_UP || c == KEY_DOWN) return;
    }
    if (c == KEY_ESCAPE) {
        command_mode = false;
        if (menu_argument_mode) { menu_argument_mode=false; tell("Menu input cancelled."); return; }
        if (blank_tail(command)) { prompt = PROMPT_QUIT; tell("Really quit? (Y/n)"); }
        return;
    }
    if (c == '\r' || c == '\n') {
        if (menu_argument_mode && menu_argument_command &&
            !strcmp(menu_argument_command,"find")) {
            menu_argument_mode = command_mode = false;
            menu_argument_command = menu_argument_label = NULL;
            if (command_n) start_search(command);
            else tell("No search text entered; cancelled.");
            return;
        }
        if (menu_argument_mode && menu_argument_command &&
            !strcmp(menu_argument_command,"@replace-search")) {
            if (!command_n) { tell("Search cannot be empty. Esc cancels."); return; }
            memcpy(replace_search,command,command_n+1);
            menu_argument_command="@replace-value";
            menu_argument_label="Replace with> ";
            command_set(""); return;
        }
        if (menu_argument_mode && menu_argument_command &&
            !strcmp(menu_argument_command,"@replace-value")) {
            char replacement[COMMAND_SIZE];
            memcpy(replacement,command,command_n+1);
            menu_argument_mode=false; command_mode=false;
            run_replacement(replace_search,replacement,replace_everywhere);
            return;
        }
        if (menu_argument_mode) {
            char input_text[COMMAND_SIZE], action[COMMAND_SIZE];
            if (!menu_argument_command) {
                menu_argument_mode = command_mode = false;
                tell("Input cancelled: no command selected."); return;
            }
            int n;
            if (blank_tail(command)) {
                menu_argument_mode=false; command_mode=false;
                tell("No argument entered; cancelled."); return;
            }
            memcpy(input_text,command,command_n+1);
            if (!strcmp(menu_argument_command,"@count-dline"))
                n=snprintf(action,sizeof action,"%s dline",input_text);
            else n=snprintf(action,sizeof action,"%s %s",menu_argument_command,input_text);
            menu_argument_mode=false;
            if (n<0 || (size_t)n>=sizeof action) {
                command_mode=false; tell("Argument too long; cancelled."); return;
            }
            command_set(action);
            history_suppress=true; execute(); history_suppress=false;
        } else execute();
        return;
    }
    if (menu_argument_mode && (c==KEY_UP || c==KEY_DOWN)) return;
    if (c == '\t') {
        if (menu_argument_mode && menu_argument_command &&
            (!strcmp(menu_argument_command,"find") ||
             !strncmp(menu_argument_command,"@replace-",9) ||
             !strcmp(menu_argument_command,"replace") ||
             !strcmp(menu_argument_command,"replaceall"))) return;
        if (filename_mode || menu_argument_mode) path_complete();
        else {
            size_t at=0,len;
            char verb[64];
            while (command[at]==' ' || command[at]=='\t') at++;
            len=strcspn(command+at," \t");
            if (len && len<sizeof verb && command[at+len]) {
                const CommandHelp *topic;
                memcpy(verb,command+at,len);verb[len]=0;
                topic=command_lookup(verb);
                if (topic && (!strcmp(topic->name,"read") ||
                              !strcmp(topic->name,"write") || !strcmp(topic->name,"ls")))
                    path_complete();
                else complete_command();
            } else complete_command();
        }
        return;
    }
    if (c == KEY_UP) { command_browse(-1); return; }
    if (c == KEY_DOWN) { command_browse(1); return; }
    if (c == KEY_LEFT) command_cursor = prev_pos(command, command_n, command_cursor);
    else if (c == KEY_RIGHT) command_cursor = next_pos(command, command_n, command_cursor);
    else if (c == KEY_HOME) command_cursor = 0;
    else if (c == KEY_END) command_cursor = command_n;
    else if ((c == 127 || c == 8) && command_cursor) {
        p = prev_pos(command, command_n, command_cursor);
        memmove(command + p, command + command_cursor, command_n - command_cursor + 1);
        command_n -= command_cursor - p; command_cursor = p;
    } else if (c == KEY_DELETE && command_cursor < command_n) {
        p = next_pos(command, command_n, command_cursor);
        memmove(command + command_cursor, command + p, command_n - p + 1);
        command_n -= p - command_cursor;
    } else if (c >= 32 && c < 256 && c != 127 && command_n < COMMAND_SIZE - 1) {
        memmove(command + command_cursor + 1, command + command_cursor,
                command_n - command_cursor + 1);
        command[command_cursor++] = (char)c; command_n++;
    }
}

static void vertical(int direction, int count)
{
    size_t start = line_start(cursor), p;
    if (!goal_set) { goal = column_at(start, cursor); goal_set = true; }
    while (count--) {
        if (direction < 0) {
            if (!start) break;
            start = line_start(start - 1);
        } else {
            p = next_line(start);
            if (p == start || (p == text.n && (!text.n || text.s[text.n-1] != '\n'))) break;
            start = p;
        }
    }
    cursor = position_at(start, goal);
}

/* Movement mode is a separate control surface, not a global keymap.
 * It never inserts or deletes bytes, including for ordinary Delete/Backspace.
 * It deliberately mirrors Coldkeys' repeatable navigation conventions. */
static bool word_byte(unsigned char c)
{
    return c >= 128 || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static size_t forward_word(size_t p)
{
    /* Advance over the current word, then any separator bytes. UTF-8
     * sequences are consumed as units by next_pos. No allocation needed. */
    if (p < text.n && word_byte((unsigned char)text.s[p]))
        do { p=next_pos(text.s,text.n,p); }
        while (p < text.n && word_byte((unsigned char)text.s[p]));
    while (p < text.n && !word_byte((unsigned char)text.s[p]))
        p=next_pos(text.s,text.n,p);
    return p;
}
static void movement_key(int c)
{
    group=false;
    message[0]=0; /* Reveal the mode hint again after ordinary movement. */
    if (c == '\r' || c == '\n') {
        movement_mode=false;goal_set=false;
        tell("Edit mode.");return;
    }
    if (c==KEY_UP || c=='k' || c=='p' || c==16) { vertical(-1,1);return; }
    if (c==KEY_DOWN || c=='j' || c=='n' || c==14) { vertical(1,1);return; }
    if (c==KEY_PAGEUP || c=='y' || c==25) {
        vertical(-1,rows>3 ? rows-3 : 1);return;
    }
    if (c==KEY_PAGEDOWN || c=='v' || c==22) {
        vertical(1,rows>3 ? rows-3 : 1);return;
    }
    goal_set=false;
    if (c==KEY_LEFT || c=='h' || c=='b' || c==2)
        cursor=prev_pos(text.s,text.n,cursor);
    else if (c==KEY_RIGHT || c=='l' || c=='f' || c==6)
        cursor=next_pos(text.s,text.n,cursor);
    else if (c==KEY_HOME || c=='0' || c=='a' || c==1)
        cursor=line_start(cursor);
    else if (c==KEY_END || c=='$' || c=='e' || c==5)
        cursor=line_end(cursor);
    else if (c==KEY_TOP || c=='g') cursor=0;
    else if (c==KEY_BOTTOM || c=='G') cursor=text.n;
    else if (c=='w' || c=='@') cursor=forward_word(cursor);
    /* Everything else is ignored, including text and deletion keys. */
}

static void text_key(int c)
{
    size_t p;
    char ch;
    const char *action=binding_action(c);
    if (action) {
        command_set(action);
        history_suppress=true;
        execute();
        history_suppress=false;
        return;
    }
    if (c == KEY_ESCAPE) {
        if (menu_preferred) {
            menu_enter(MENU_ROOT);
        } else { command_mode = true; command_set(""); history_choice = -1; }
        group = false; message[0] = 0; return;
    }
    if (c == KEY_UP || c == KEY_DOWN || c == KEY_PAGEUP || c == KEY_PAGEDOWN) {
        group = false;
        vertical(c == KEY_UP || c == KEY_PAGEUP ? -1 : 1,
                 c == KEY_UP || c == KEY_DOWN ? 1 : (rows > 3 ? rows - 3 : 1));
        return;
    }
    if (movement_mode) { movement_key(c); return; }
    goal_set = false;
    if (c == KEY_LEFT) cursor = prev_pos(text.s, text.n, cursor);
    else if (c == KEY_RIGHT) cursor = next_pos(text.s, text.n, cursor);
    else if (c == KEY_HOME) cursor = line_start(cursor);
    else if (c == KEY_END) cursor = line_end(cursor);
    else if (c == KEY_TOP) cursor = 0;
    else if (c == KEY_BOTTOM) cursor = text.n;
    else if (c == 127 || c == 8) {
        if (cursor) { p = prev_pos(text.s, text.n, cursor); (void)edit(p, cursor - p, NULL, 0, false); }
    } else if (c == KEY_DELETE) {
        if (cursor < text.n) (void)edit(cursor, next_pos(text.s, text.n, cursor) - cursor, NULL, 0, false);
    } else if (c == '\r' || c == '\n') {
        (void)edit(cursor, 0, newline, newline_n, false);
    } else if (c == '\t' || (c >= 32 && c < 256 && c != 127)) {
        ch = (char)c; (void)edit(cursor, 0, &ch, 1, true); return;
    }
    group = false;
}

/* Mouse is opt-in. SGR reports coordinates in 1-based terminal cells.
 * Menu entry by clicking the status bar is an alternate to Esc. */
static void mouse_key(void)
{
    int body=rows > 2 ? rows-2 : 1;
    size_t start, i, p;
    if (!mouse_enabled || !mouse_pressed) return;
    if (mouse_button & 64) {
        /* Wheel: 3 rows, or 3 menu positions. */
        int d=(mouse_button & 1) ? 1 : -1;
        for (i=0;i<3;i++) {
            if (viewer_text) viewer_key(d<0?KEY_UP:KEY_DOWN);
            else if (menu_open) menu_key(d<0?KEY_UP:KEY_DOWN);
            else if (!command_mode) vertical(d,1);
        }
        return;
    }
    if ((mouse_button & 3) != 0) return; /* left click only */
    if (viewer_text) return;
    if (menu_open) {
        if (mouse_y >= 2 && mouse_y <= (body<=1 ? 2 : body)) {
            size_t count;
            const char *title;
            (void)menu_entries(&count,&title);
            i=menu_first + (size_t)(mouse_y-2);
            if (i < count) {
                menu_selection=i; menu_number=-1; menu_discard_pending=false;
                menu_run();
            }
        }
        return;
    }
    if (mouse_y == body+1) {
        menu_enter(MENU_ROOT); command_mode=false; return;
    }
    if (command_mode || mouse_y < 1 || mouse_y > body) return;
    start=top;
    for (i=1;i<(size_t)mouse_y;i++) {
        p=next_line(start);
        if (p==start) break;
        start=p;
    }
    cursor=position_at(start,left+(size_t)(mouse_x-1));
    goal_set=false; group=false;
}

static void usage(void)
{
    puts("editor " VERSION " - one-file, dependency-light terminal text editor\n"
         "Usage: editor [--menu] [--mouse] [--movement|--edit] [--no-keys] [FILE|-]\n"
         "       editor --help | --manual | --about | --version\n\n"
         "FILE: edit named file.  - or piped stdin: edit text from a pipe.\n"
         "Interactive UI uses the controlling terminal when stdin/stdout are pipes.\n"
         "Use write - to output only document bytes to redirected stdout.\n\n"
         "Edit normally with arrows, typing, Backspace, Delete, and Enter.\n"
         "Esc: command prompt, or optional arrow/number/hjkl menu (--menu).\n"
         "--movement / --edit: override the startup setting for this launch.\n"
         "Config: ~/.config/editor/config or $XDG_CONFIG_HOME/editor/config.\n"
         "Set startup=movement or startup=edit (default); -- ends option parsing.\n"
         "signal_controls=off (default): native interrupt/stop; on: interrupt cancels.\n"
         "Ctrl-C/Z are not bound. Unsaved text is lost on a default interrupt exit.\n"
         "Click status bar to open menu if mouse enabled (--mouse).\n"
         "No default hotkeys. User bindings via 'bind' and 'unbind'.\n\n"
         "Type list for commands by task; help for examples and guidance.\n"
         "No curses, terminfo, readline or external pager.\n");
    puts(help_topics);
    puts("Build: cc -std=c99 -O2 -Wall -Wextra -pedantic editor.c -o editor");
}

int main(int argc, char **argv)
{
    int c, status=0;
    size_t p;
    bool dirty_screen=true, from_stdin=false;
    if (argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) { usage(); return 0; }
    if (argc==2 && !strcmp(argv[1],"--version")) { puts("editor " VERSION); return 0; }
    if (argc==2 && !strcmp(argv[1],"--about")) { puts(about_text); return 0; }
    if (argc==2 && !strcmp(argv[1],"--manual")) { puts(manual_text); return 0; }
    load_startup_config();
    {
        int i, file_arg=0;
        bool options = true;
        for (i=1; i<argc; i++) {
            if (options && !strcmp(argv[i], "--")) { options=false; continue; }
            if (!options) {
                if (file_arg) { fputs("Only one file may be opened.\n", stderr); return 2; }
                file_arg=i; continue;
            }
            if (!strcmp(argv[i],"--menu")) menu_preferred=true;
            else if (!strcmp(argv[i],"--mouse")) mouse_enabled=true;
            else if (!strcmp(argv[i],"--movement") || !strcmp(argv[i],"--move")) movement_mode=true;
            else if (!strcmp(argv[i],"--edit")) movement_mode=false;
            else if (!strcmp(argv[i],"--no-keys")) keys_disabled=true;
            else if (!file_arg && (!strcmp(argv[i],"-") || argv[i][0]!='-')) file_arg=i;
            else { fputs("Usage: editor [--menu] [--mouse] [--movement|--edit] [--no-keys] [FILE|-]\n",stderr); return 2; }
        }
        if (file_arg && !strcmp(argv[file_arg],"-")) from_stdin=true;
        else if (file_arg) filename=resolve_path(argv[file_arg]);
        if (file_arg && !from_stdin && !filename) die(strerror(errno));
    }
    (void)setlocale(LC_CTYPE, "");
    /* An explicitly named file always wins over redirected stdin.
     * Only a bare editor or explicit '-' consumes the pipe. */
    stream_input=from_stdin || (!filename && !isatty(STDIN_FILENO));
    {
        const char *terminal_type=getenv("TERM");
        if (terminal_type && !strcmp(terminal_type,"dumb"))
            die("a VT/xterm-compatible terminal is required");
    }
    /* Use /dev/tty exclusively for interactive UI if the standard streams are pipes.
     * A downstream filter sees only bytes sent explicitly with 'write -'. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        tty_extra=open("/dev/tty",O_RDWR);
        if (tty_extra < 0) die("no controlling terminal available for interactive editing");
        tty_in=tty_out=tty_extra;
    }
    if (!isatty(tty_in) || !isatty(tty_out)) die("terminal input and output required");
    if (stream_input) {
        if (!read_stream(STDIN_FILENO,&text)) die(strerror(errno));
    } else if (filename) {
        if (lstat(filename,&original)==0) {
            if (!read_file(filename,&text,&original)) die(strerror(errno));
            existed=true;
        } else if (errno!=ENOENT) die(strerror(errno));
    }
    if (text.n && memchr(text.s, 0, text.n)) die("NUL byte in file; this editor edits text");
    if (!copy_buffer(&saved, stream_input ? "" : (text.s ? text.s : ""),
                     stream_input ? 0 : text.n) || !reserve(&text, text.n+1))
        die("out of memory");
    text.s[text.n] = 0;
    if (filename) journal_load();
    for (p = 0; p < text.n; p++) if (text.s[p] == '\n') {
        if (p && text.s[p-1] == '\r') { newline[0] = '\r'; newline[1] = '\n'; newline_n = 2; }
        break;
    }
    if (tcgetattr(tty_in, &initial_termios) < 0) die(strerror(errno));
    load_bindings();
    clipboard_history_load_setting();
    clipboard_load_selection();
    if (atexit(terminal_restore) != 0) die("cannot register terminal cleanup");
    install_handler(SIGWINCH, signal_handler); install_handler(SIGINT, signal_handler);
    install_handler(SIGTSTP, signal_handler); install_handler(SIGCONT, signal_handler);
    install_handler(SIGTERM, signal_handler); install_handler(SIGHUP, signal_handler);
    install_handler(SIGQUIT, signal_handler);
    install_handler(SIGPIPE, SIG_IGN);
#ifdef SIGXFSZ
    /* A shell's file-size limit is a recoverable write error, not a reason
     * to terminate with the terminal raw and unsaved text still in memory. */
    install_handler(SIGXFSZ, SIG_IGN);
#endif
    terminal_start();
    if (journal_ignored) tell("Undo journal unusable; prior journal retained, no history loaded.");
    else if (journal_rebased) tell("External changes added as one undo; write to persist history.");
    else if (startup_config_warning) tell("Config entry ignored; see manual (startup or signal_controls).");
    else tell("editor " VERSION " | list | help | manual | about");
    while (running && !terminated) {
        if (suspended) {
            if (signal_controls && prompt != PROMPT_NONE) {
                clipboard_leave_after_save=false;
                clear_prompt(); command_mode = false;
            }
            terminal_restore(); suspended = 0;
            if (terminated) break;
            install_handler(SIGTSTP, SIG_DFL); (void)raise(SIGTSTP);
            install_handler(SIGTSTP, signal_handler);
            if (terminated) break;
            terminal_start(); dirty_screen = true;
        }
        if (interrupted) {
            filename_mode = filename_read_mode = false;
            menu_argument_mode = false;
            menu_argument_command = menu_argument_label = NULL;
            if (prompt != PROMPT_NONE) {
                clipboard_leave_after_save=false;
                clear_prompt(); command_mode = false; tell("Cancelled by interrupt.");
                interrupted = 0; dirty_screen = true; continue;
            }
            interrupted = 0;
            if (!paste_discard) input_at = input_n;
            discard_sequence = 0;
            viewer_text = viewer_title = NULL;
            reverse_mode = false;
            menu_open = false; menu_discard_pending = false; menu_argument_mode=false;
            command_mode = true; command_set(""); history_choice = -1;
            group = false; tell("Use write, quit, or quit!."); dirty_screen = true;
        }
        if (terminated || suspended || interrupted) continue;
        if (!terminal_active) {
            terminal_start();
            if (!terminal_active) continue;
            dirty_screen = true;
        }
        if (resized || (dirty_screen && input_at == input_n)) { draw(); dirty_screen = false; }
        c = key();
        if (c == KEY_EOF) { status = 1; break; }
        if (c == KEY_NONE) continue;
        if (c == KEY_PASTE) paste(); /* Swallow paste in modal modes. */
        else if (prompt != PROMPT_NONE) prompt_key(c);
        else if (reverse_mode) reverse_key(c);
        else if (c == KEY_MOUSE) mouse_key();
        else if (viewer_text) viewer_key(c);
        else if (menu_open) menu_key(c);
        else if (command_mode) command_key(c);
        else text_key(c);
        dirty_screen = true;
    }
    terminal_restore();
    /* With editing over and the terminal restored, a stop during the short
     * exit notice must still be a real job-control stop, not a swallowed flag. */
    install_handler(SIGTSTP, SIG_DFL);
    if (suspended && !terminated) { suspended = 0; (void)raise(SIGTSTP); }
    /* Only a successful ordinary quit earns the notice. Quick confirmation,
     * discard, EOF and signal exits remain immediate. Use the controlling
     * terminal, never stdout: write - pipelines must stay byte-exact. */
    if (ordinary_quit && !status && !terminated)
        (void)write_all(tty_out, exit_notice, sizeof exit_notice - 1);
    if (terminated) status = 128 + terminated;
    reset_screen(); clear_prompt();
    free_changes(history); free(text.s); free(saved.s); free(filename);
    free(loaded_journal_path);
    if (editing_clipboard) {
        /* These buffers were never freed while clipboard was open. */
        free_changes(parent_document.history);
        free(parent_document.text.s); free(parent_document.saved.s);
        free(parent_document.filename); free(parent_document.loaded_journal_path);
    }
    for (p = 0; p < command_history_n; p++) free(command_history[p]);
    if (tty_extra >= 0) close(tty_extra);
    if (terminated == SIGINT) {
        sigset_t set;
        /* Restore the actual signal exit status, after terminal and memory
         * cleanup. The shell sees an interrupted job, not a normal quit. */
        install_handler(SIGINT, SIG_DFL);
        sigemptyset(&set); sigaddset(&set, SIGINT);
        (void)sigprocmask(SIG_UNBLOCK, &set, NULL);
        (void)raise(SIGINT);
    }
    return status;
}
