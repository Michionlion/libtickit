/* We need strdup */
#define _XOPEN_SOURCE 600

#include "tickit.h"

#include <stdio.h>  // vsnprintf
#include <stdlib.h>
#include <string.h>

#include "linechars.inc"

#define RECT_PRINTF_FMT "[(%d,%d)..(%d,%d)]"
#define RECT_PRINTF_ARGS(r) (r).left, (r).top, tickit_rect_right(&(r)), tickit_rect_bottom(&(r))

/* must match .pm file */
enum TickitRenderBufferCellState {
    SKIP  = 0,
    TEXT  = 1,
    ERASE = 2,
    CONT  = 3,
    LINE  = 4,
    CHAR  = 5,
};

enum {
    NORTH_SHIFT = 0,
    EAST_SHIFT  = 2,
    SOUTH_SHIFT = 4,
    WEST_SHIFT  = 6,
};

// Internal cell structure definition
typedef struct {
    enum TickitRenderBufferCellState state;
    union {
        int startcol;  // for state == CONT
        int cols;      // otherwise
    };
    int maskdepth;   // -1 if not masked
    TickitPen *pen;  // state -> {TEXT, ERASE, LINE, CHAR}
    union {
        struct {
            TickitString *s;
            int offs;
        } text;  // state == TEXT
        struct {
            int mask;
        } line;  // state == LINE
        struct {
            int codepoint;
        } chr;  // state == CHAR
    } v;
} RBCell;

typedef struct RBStack RBStack;
struct RBStack {
    RBStack *prev;

    int vc_line, vc_col;
    int xlate_line, xlate_col;
    TickitRect clip;
    TickitPen *pen;
    unsigned int pen_only : 1;
};

struct TickitRenderBuffer {
    int lines, cols;  // Size
    RBCell **cells;

    unsigned int vc_pos_set : 1;
    int vc_line, vc_col;
    int xlate_line, xlate_col;
    TickitRect clip;
    TickitPen *pen;

    int depth;
    RBStack *stack;

    char *tmp;
    size_t tmplen;   // actually valid
    size_t tmpsize;  // allocated size

    int refcount;
};

static void debug_logf(TickitRenderBuffer *rb, const char *flag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char fmt_with_indent[strlen(fmt) + 3 * rb->depth + 1];
    {
        char *s = fmt_with_indent;
        for (int i = 0; i < rb->depth; i++)
            s += sprintf(s, "|  ");
        strcpy(s, fmt);
    }

    tickit_debug_vlogf(flag, fmt_with_indent, args);

    va_end(args);
}

#define DEBUG_LOGF            \
    if (tickit_debug_enabled) \
    debug_logf

static void free_stack(RBStack *stack) {
    while (stack) {
        RBStack *prev = stack->prev;
        if (stack->pen)
            tickit_pen_unref(stack->pen);
        free(stack);

        stack = prev;
    }
}

static void tmp_cat_utf8(TickitRenderBuffer *rb, long codepoint) {
    int seqlen = tickit_utf8_seqlen(codepoint);
    if (rb->tmpsize < rb->tmplen + seqlen) {
        rb->tmpsize *= 2;
        rb->tmp = realloc(rb->tmp, rb->tmpsize);
    }

    tickit_utf8_put(rb->tmp + rb->tmplen, rb->tmpsize - rb->tmplen, codepoint);
    rb->tmplen += seqlen;

    /* rb->tmp remains NOT nul-terminated */
}

static void tmp_alloc(TickitRenderBuffer *rb, size_t len) {
    if (rb->tmpsize < len) {
        free(rb->tmp);

        while (rb->tmpsize < len)
            rb->tmpsize *= 2;
        rb->tmp = malloc(rb->tmpsize);
    }
}

static int xlate_and_clip(TickitRenderBuffer *rb, int *line, int *col, int *cols, int *startcol) {
    *line += rb->xlate_line;
    *col += rb->xlate_col;

    const TickitRect *clip = &rb->clip;

    if (!clip->lines)
        return 0;

    if (*line < clip->top || *line >= tickit_rect_bottom(clip) || *col >= tickit_rect_right(clip))
        return 0;

    if (startcol)
        *startcol = 0;

    if (*col < clip->left) {
        *cols -= clip->left - *col;
        if (startcol)
            *startcol += clip->left - *col;
        *col = clip->left;
    }
    if (*cols <= 0)
        return 0;

    if (*cols > tickit_rect_right(clip) - *col)
        *cols = tickit_rect_right(clip) - *col;

    return 1;
}

static void cont_cell(RBCell *cell, int startcol) {
    switch (cell->state) {
        case TEXT:
            tickit_string_unref(cell->v.text.s);
            /* fallthrough */
        case ERASE:
        case LINE:
        case CHAR:
            tickit_pen_unref(cell->pen);
            break;
        case SKIP:
        case CONT:
            /* ignore */
            break;
    }

    cell->state     = CONT;
    cell->maskdepth = -1;
    cell->startcol  = startcol;
    cell->pen       = NULL;
}

static RBCell *make_span(TickitRenderBuffer *rb, int line, int col, int cols) {
    int end        = col + cols;
    RBCell **cells = rb->cells;

    // If the following cell is a CONT, it needs to become a new start
    if (end < rb->cols && cells[line][end].state == CONT) {
        int spanstart    = cells[line][end].cols;
        RBCell *spancell = &cells[line][spanstart];
        int spanend      = spanstart + spancell->startcol;
        int afterlen     = spanend - end;
        RBCell *endcell  = &cells[line][end];

        switch (spancell->state) {
            case SKIP:
                endcell->state = SKIP;
                endcell->cols  = afterlen;
                break;
            case TEXT:
                endcell->state       = TEXT;
                endcell->cols        = afterlen;
                endcell->pen         = tickit_pen_ref(spancell->pen);
                endcell->v.text.s    = tickit_string_ref(spancell->v.text.s);
                endcell->v.text.offs = spancell->v.text.offs + end - spanstart;
                break;
            case ERASE:
                endcell->state = ERASE;
                endcell->cols  = afterlen;
                endcell->pen   = tickit_pen_ref(spancell->pen);
                break;
            case LINE:
            case CHAR:
            case CONT:
                abort();
        }

        // We know these are already CONT cells
        for (int c = end + 1; c < spanend; c++)
            cells[line][c].cols = end;
    }

    // If the initial cell is a CONT, shorten its start
    if (cells[line][col].state == CONT) {
        int beforestart  = cells[line][col].cols;
        RBCell *spancell = &cells[line][beforestart];
        int beforelen    = col - beforestart;

        switch (spancell->state) {
            case SKIP:
            case TEXT:
            case ERASE:
                spancell->cols = beforelen;
                break;
            case LINE:
            case CHAR:
            case CONT:
                abort();
        }
    }

    // cont_cell() also frees any pens in the range
    for (int c = col; c < end; c++)
        cont_cell(&cells[line][c], col);

    cells[line][col].cols = cols;

    return &cells[line][col];
}

// cell creation functions

static int put_string(TickitRenderBuffer *rb, int line, int col, TickitString *s) {
    TickitStringPos endpos;
    size_t len = tickit_utf8_ncount(tickit_string_get(s), tickit_string_len(s), &endpos, NULL);
    if (1 + len == 0)
        return -1;

    int cols = endpos.columns;
    int ret  = cols;

    int startcol;
    if (!xlate_and_clip(rb, &line, &col, &cols, &startcol))
        return ret;

    RBCell *linecells = rb->cells[line];

    while (cols) {
        while (cols && linecells[col].maskdepth > -1) {
            col++;
            cols--;
            startcol++;
        }
        if (!cols)
            break;

        int spanlen = 0;
        while (cols && linecells[col + spanlen].maskdepth == -1) {
            spanlen++;
            cols--;
        }
        if (!spanlen)
            break;

        RBCell *cell      = make_span(rb, line, col, spanlen);
        cell->state       = TEXT;
        cell->pen         = tickit_pen_ref(rb->pen);
        cell->v.text.s    = tickit_string_ref(s);
        cell->v.text.offs = startcol;

        col += spanlen;
        startcol += spanlen;
    }

    return ret;
}

static int put_text(TickitRenderBuffer *rb, int line, int col, const char *text, size_t len) {
    TickitString *s = tickit_string_new(text, len == -1 ? strlen(text) : len);

    int ret = put_string(rb, line, col, s);

    tickit_string_unref(s);

    return ret;
}

static int put_vtextf(TickitRenderBuffer *rb, int line, int col, const char *fmt, va_list args) {
    /* It's likely the string will fit in, say, 64 bytes */
    char buffer[64];
    size_t len;
    {
        va_list args_for_size;
        va_copy(args_for_size, args);

        len = vsnprintf(buffer, sizeof buffer, fmt, args_for_size);

        va_end(args_for_size);
    }

    if (len < sizeof buffer)
        return put_text(rb, line, col, buffer, len);

    tmp_alloc(rb, len + 1);
    vsnprintf(rb->tmp, rb->tmpsize, fmt, args);
    return put_text(rb, line, col, rb->tmp, len);
}

static void put_char(TickitRenderBuffer *rb, int line, int col, long codepoint) {
    int cols = 1;

    if (!xlate_and_clip(rb, &line, &col, &cols, NULL))
        return;

    if (rb->cells[line][col].maskdepth > -1)
        return;

    RBCell *cell          = make_span(rb, line, col, cols);
    cell->state           = CHAR;
    cell->pen             = tickit_pen_ref(rb->pen);
    cell->v.chr.codepoint = codepoint;
}

static void skip(TickitRenderBuffer *rb, int line, int col, int cols) {
    if (!xlate_and_clip(rb, &line, &col, &cols, NULL))
        return;

    RBCell *linecells = rb->cells[line];

    while (cols) {
        while (cols && linecells[col].maskdepth > -1) {
            col++;
            cols--;
        }
        if (!cols)
            break;

        int spanlen = 0;
        while (cols && linecells[col + spanlen].maskdepth == -1) {
            spanlen++;
            cols--;
        }
        if (!spanlen)
            break;

        RBCell *cell = make_span(rb, line, col, spanlen);
        cell->state  = SKIP;

        col += spanlen;
    }
}

static void erase(TickitRenderBuffer *rb, int line, int col, int cols) {
    if (!xlate_and_clip(rb, &line, &col, &cols, NULL))
        return;

    RBCell *linecells = rb->cells[line];

    while (cols) {
        while (cols && linecells[col].maskdepth > -1) {
            col++;
            cols--;
        }
        if (!cols)
            break;

        int spanlen = 0;
        while (cols && linecells[col + spanlen].maskdepth == -1) {
            spanlen++;
            cols--;
        }
        if (!spanlen)
            break;

        RBCell *cell = make_span(rb, line, col, spanlen);
        cell->state  = ERASE;
        cell->pen    = tickit_pen_ref(rb->pen);

        col += spanlen;
    }
}

TickitRenderBuffer *tickit_renderbuffer_new(int lines, int cols) {
    TickitRenderBuffer *rb = malloc(sizeof(TickitRenderBuffer));

    rb->lines = lines;
    rb->cols  = cols;

    rb->cells = malloc(rb->lines * sizeof(RBCell *));
    for (int line = 0; line < rb->lines; line++) {
        rb->cells[line] = malloc(rb->cols * sizeof(RBCell));

        rb->cells[line][0].state     = SKIP;
        rb->cells[line][0].maskdepth = -1;
        rb->cells[line][0].cols      = rb->cols;
        rb->cells[line][0].pen       = NULL;

        for (int col = 1; col < rb->cols; col++) {
            rb->cells[line][col].state     = CONT;
            rb->cells[line][col].maskdepth = -1;
            rb->cells[line][col].cols      = 0;
        }
    }

    rb->vc_pos_set = 0;

    rb->xlate_line = 0;
    rb->xlate_col  = 0;

    tickit_rect_init_sized(&rb->clip, 0, 0, rb->lines, rb->cols);

    rb->pen = tickit_pen_new();

    rb->stack = NULL;
    rb->depth = 0;

    rb->tmpsize = 256;  // hopefully enough but will grow if required
    rb->tmp     = malloc(rb->tmpsize);
    rb->tmplen  = 0;

    rb->refcount = 1;

    return rb;
}

void tickit_renderbuffer_destroy(TickitRenderBuffer *rb) {
    for (int line = 0; line < rb->lines; line++) {
        for (int col = 0; col < rb->cols; col++) {
            RBCell *cell = &rb->cells[line][col];
            switch (cell->state) {
                case TEXT:
                    tickit_string_unref(cell->v.text.s);
                    /* fallthrough */
                case ERASE:
                case LINE:
                case CHAR:
                    tickit_pen_unref(cell->pen);
                    break;
                case SKIP:
                case CONT:
                    break;
            }
        }
        free(rb->cells[line]);
    }

    free(rb->cells);
    rb->cells = NULL;

    tickit_pen_unref(rb->pen);

    if (rb->stack)
        free_stack(rb->stack);

    free(rb->tmp);

    free(rb);
}

TickitRenderBuffer *tickit_renderbuffer_ref(TickitRenderBuffer *rb) {
    rb->refcount++;
    return rb;
}

void tickit_renderbuffer_unref(TickitRenderBuffer *rb) {
    if (rb->refcount < 1) {
        fprintf(stderr, "tickit_renderbuffer_unref: invalid refcount %d\n", rb->refcount);
        abort();
    }
    rb->refcount--;
    if (!rb->refcount)
        tickit_renderbuffer_destroy(rb);
}

void tickit_renderbuffer_get_size(const TickitRenderBuffer *rb, int *lines, int *cols) {
    if (lines)
        *lines = rb->lines;

    if (cols)
        *cols = rb->cols;
}

void tickit_renderbuffer_translate(TickitRenderBuffer *rb, int downward, int rightward) {
    DEBUG_LOGF(rb, "Bt", "Translate (%+d,%+d)", rightward, downward);

    rb->xlate_line += downward;
    rb->xlate_col += rightward;
}

void tickit_renderbuffer_clip(TickitRenderBuffer *rb, TickitRect *rect) {
    DEBUG_LOGF(rb, "Bt", "Clip " RECT_PRINTF_FMT, RECT_PRINTF_ARGS(*rect));

    TickitRect other;

    other = *rect;
    other.top += rb->xlate_line;
    other.left += rb->xlate_col;

    if (!tickit_rect_intersect(&rb->clip, &rb->clip, &other))
        rb->clip.lines = 0;
}

void tickit_renderbuffer_mask(TickitRenderBuffer *rb, TickitRect *mask) {
    DEBUG_LOGF(rb, "Bt", "Mask " RECT_PRINTF_FMT, RECT_PRINTF_ARGS(*mask));

    TickitRect hole;

    hole = *mask;
    hole.top += rb->xlate_line;
    hole.left += rb->xlate_col;

    if (hole.top < 0) {
        hole.lines += hole.top;
        hole.top = 0;
    }
    if (hole.left < 0) {
        hole.cols += hole.left;
        hole.left = 0;
    }

    for (int line = hole.top; line < tickit_rect_bottom(&hole) && line < rb->lines; line++) {
        for (int col = hole.left; col < tickit_rect_right(&hole) && col < rb->cols; col++) {
            RBCell *cell = &rb->cells[line][col];
            if (cell->maskdepth == -1)
                cell->maskdepth = rb->depth;
        }
    }
}

bool tickit_renderbuffer_has_cursorpos(const TickitRenderBuffer *rb) { return rb->vc_pos_set; }

void tickit_renderbuffer_get_cursorpos(const TickitRenderBuffer *rb, int *line, int *col) {
    if (rb->vc_pos_set && line)
        *line = rb->vc_line;
    if (rb->vc_pos_set && col)
        *col = rb->vc_col;
}

void tickit_renderbuffer_goto(TickitRenderBuffer *rb, int line, int col) {
    rb->vc_pos_set = 1;
    rb->vc_line    = line;
    rb->vc_col     = col;
}

void tickit_renderbuffer_ungoto(TickitRenderBuffer *rb) { rb->vc_pos_set = 0; }

void tickit_renderbuffer_setpen(TickitRenderBuffer *rb, const TickitPen *pen) {
    TickitPen *prevpen = rb->stack ? rb->stack->pen : NULL;

    /* never mutate the pen inplace; make a new one */
    TickitPen *newpen = tickit_pen_new();

    if (pen)
        tickit_pen_copy(newpen, pen, 1);
    if (prevpen)
        tickit_pen_copy(newpen, prevpen, 0);

    tickit_pen_unref(rb->pen);
    rb->pen = newpen;
}

void tickit_renderbuffer_reset(TickitRenderBuffer *rb) {
    for (int line = 0; line < rb->lines; line++) {
        // cont_cell also frees pen
        for (int col = 0; col < rb->cols; col++)
            cont_cell(&rb->cells[line][col], 0);

        rb->cells[line][0].state     = SKIP;
        rb->cells[line][0].maskdepth = -1;
        rb->cells[line][0].cols      = rb->cols;
    }

    rb->vc_pos_set = 0;

    rb->xlate_line = 0;
    rb->xlate_col  = 0;

    tickit_rect_init_sized(&rb->clip, 0, 0, rb->lines, rb->cols);

    tickit_pen_unref(rb->pen);
    rb->pen = tickit_pen_new();

    if (rb->stack) {
        free_stack(rb->stack);
        rb->stack = NULL;
        rb->depth = 0;
    }
}

void tickit_renderbuffer_clear(TickitRenderBuffer *rb) {
    DEBUG_LOGF(rb, "Bd", "Clear");

    for (int line = 0; line < rb->lines; line++)
        erase(rb, line, 0, rb->cols);
}

void tickit_renderbuffer_save(TickitRenderBuffer *rb) {
    DEBUG_LOGF(rb, "Bs", "+-Save");

    RBStack *stack = malloc(sizeof(struct RBStack));

    stack->vc_line    = rb->vc_line;
    stack->vc_col     = rb->vc_col;
    stack->xlate_line = rb->xlate_line;
    stack->xlate_col  = rb->xlate_col;
    stack->clip       = rb->clip;
    stack->pen        = tickit_pen_ref(rb->pen);
    stack->pen_only   = 0;

    stack->prev = rb->stack;
    rb->stack   = stack;
    rb->depth++;
}

void tickit_renderbuffer_savepen(TickitRenderBuffer *rb) {
    DEBUG_LOGF(rb, "Bs", "+-Savepen");

    RBStack *stack = malloc(sizeof(struct RBStack));

    stack->pen      = tickit_pen_ref(rb->pen);
    stack->pen_only = 1;

    stack->prev = rb->stack;
    rb->stack   = stack;
    rb->depth++;
}

void tickit_renderbuffer_restore(TickitRenderBuffer *rb) {
    RBStack *stack;

    if (!rb->stack)
        return;

    stack     = rb->stack;
    rb->stack = stack->prev;

    if (!stack->pen_only) {
        rb->vc_line    = stack->vc_line;
        rb->vc_col     = stack->vc_col;
        rb->xlate_line = stack->xlate_line;
        rb->xlate_col  = stack->xlate_col;
        rb->clip       = stack->clip;
    }

    tickit_pen_unref(rb->pen);
    rb->pen = stack->pen;
    // We've now definitely taken ownership of the old stack frame's pen, so
    //   it doesn't need destroying now

    rb->depth--;

    // TODO: this could be done more efficiently by remembering the edges of masking
    for (int line = 0; line < rb->lines; line++)
        for (int col = 0; col < rb->cols; col++)
            if (rb->cells[line][col].maskdepth > rb->depth)
                rb->cells[line][col].maskdepth = -1;

    free(stack);

    DEBUG_LOGF(rb, "Bs", "+-Restore");
}

void tickit_renderbuffer_skip_at(TickitRenderBuffer *rb, int line, int col, int cols) {
    DEBUG_LOGF(rb, "Bd", "Skip (%d..%d,%d)", col, col + cols, line);

    skip(rb, line, col, cols);
}

void tickit_renderbuffer_skip(TickitRenderBuffer *rb, int cols) {
    if (!rb->vc_pos_set)
        return;

    DEBUG_LOGF(rb, "Bd", "Skip (%d..%d,%d) +%d", rb->vc_col, rb->vc_col + cols, rb->vc_line, cols);

    skip(rb, rb->vc_line, rb->vc_col, cols);
    rb->vc_col += cols;
}

void tickit_renderbuffer_skip_to(TickitRenderBuffer *rb, int col) {
    if (!rb->vc_pos_set)
        return;

    DEBUG_LOGF(rb, "Bd", "Skip (%d..%d,%d) +%d", rb->vc_col, col, rb->vc_line, col - rb->vc_col);

    if (rb->vc_col < col)
        skip(rb, rb->vc_line, rb->vc_col, col - rb->vc_col);

    rb->vc_col = col;
}

void tickit_renderbuffer_skiprect(TickitRenderBuffer *rb, TickitRect *rect) {
    DEBUG_LOGF(rb, "Bd", "Skip [(%d,%d)..(%d,%d)]", rect->left, rect->top, tickit_rect_right(rect),
        tickit_rect_bottom(rect));

    for (int line = rect->top; line < tickit_rect_bottom(rect); line++)
        skip(rb, line, rect->left, rect->cols);
}

int tickit_renderbuffer_text_at(TickitRenderBuffer *rb, int line, int col, const char *text) {
    return tickit_renderbuffer_textn_at(rb, line, col, text, -1);
}

int tickit_renderbuffer_textn_at(
    TickitRenderBuffer *rb, int line, int col, const char *text, size_t len) {
    int cols = put_text(rb, line, col, text, len);

    DEBUG_LOGF(rb, "Bd", "Text (%d..%d,%d)", col, col + cols, line);

    return cols;
}

int tickit_renderbuffer_text(TickitRenderBuffer *rb, const char *text) {
    return tickit_renderbuffer_textn(rb, text, -1);
}

int tickit_renderbuffer_textn(TickitRenderBuffer *rb, const char *text, size_t len) {
    if (!rb->vc_pos_set)
        return -1;

    int cols = put_text(rb, rb->vc_line, rb->vc_col, text, len);

    DEBUG_LOGF(rb, "Bd", "Text (%d..%d,%d) +%d", rb->vc_col, rb->vc_col + cols, rb->vc_line, cols);

    rb->vc_col += cols;
    return cols;
}

int tickit_renderbuffer_textf_at(TickitRenderBuffer *rb, int line, int col, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int ret = tickit_renderbuffer_vtextf_at(rb, line, col, fmt, args);

    va_end(args);

    return ret;
}

int tickit_renderbuffer_vtextf_at(
    TickitRenderBuffer *rb, int line, int col, const char *fmt, va_list args) {
    int cols = put_vtextf(rb, line, col, fmt, args);

    DEBUG_LOGF(rb, "Bd", "Text (%d..%d,%d)", col, col + cols, line);

    return cols;
}

int tickit_renderbuffer_textf(TickitRenderBuffer *rb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int ret = tickit_renderbuffer_vtextf(rb, fmt, args);

    va_end(args);

    return ret;
}

int tickit_renderbuffer_vtextf(TickitRenderBuffer *rb, const char *fmt, va_list args) {
    if (!rb->vc_pos_set)
        return -1;

    int cols = put_vtextf(rb, rb->vc_line, rb->vc_col, fmt, args);

    DEBUG_LOGF(rb, "Bd", "Text (%d..%d,%d) +%d", rb->vc_col, rb->vc_col + cols, rb->vc_line, cols);

    rb->vc_col += cols;
    return cols;
}

void tickit_renderbuffer_erase_at(TickitRenderBuffer *rb, int line, int col, int cols) {
    DEBUG_LOGF(rb, "Bd", "Erase (%d..%d,%d)", col, col + cols, line);

    erase(rb, line, col, cols);
}

void tickit_renderbuffer_erase(TickitRenderBuffer *rb, int cols) {
    if (!rb->vc_pos_set)
        return;

    DEBUG_LOGF(rb, "Bd", "Erase (%d..%d,%d) +%d", rb->vc_col, rb->vc_col + cols, rb->vc_line, cols);

    erase(rb, rb->vc_line, rb->vc_col, cols);
    rb->vc_col += cols;
}

void tickit_renderbuffer_erase_to(TickitRenderBuffer *rb, int col) {
    if (!rb->vc_pos_set)
        return;

    DEBUG_LOGF(rb, "Bd", "Erase (%d..%d,%d) +%d", rb->vc_col, col, rb->vc_line, col - rb->vc_col);

    if (rb->vc_col < col)
        erase(rb, rb->vc_line, rb->vc_col, col - rb->vc_col);

    rb->vc_col = col;
}

void tickit_renderbuffer_eraserect(TickitRenderBuffer *rb, TickitRect *rect) {
    DEBUG_LOGF(rb, "Bd", "Erase [(%d,%d)..(%d,%d)]", rect->left, rect->top, tickit_rect_right(rect),
        tickit_rect_bottom(rect));

    for (int line = rect->top; line < tickit_rect_bottom(rect); line++)
        erase(rb, line, rect->left, rect->cols);
}

void tickit_renderbuffer_char_at(TickitRenderBuffer *rb, int line, int col, long codepoint) {
    DEBUG_LOGF(rb, "Bd", "Char (%d.,%d,%d)", col, col + 1, line);

    put_char(rb, line, col, codepoint);
}

void tickit_renderbuffer_char(TickitRenderBuffer *rb, long codepoint) {
    if (!rb->vc_pos_set)
        return;

    DEBUG_LOGF(rb, "Bd", "Char (%d..%d,%d) +%d", rb->vc_col, rb->vc_col + 1, rb->vc_line, 1);

    put_char(rb, rb->vc_line, rb->vc_col, codepoint);
    // TODO: might not be 1; would have to look it up
    rb->vc_col += 1;
}

static void linecell(TickitRenderBuffer *rb, int line, int col, int bits) {
    int cols = 1;

    if (!xlate_and_clip(rb, &line, &col, &cols, NULL))
        return;

    if (rb->cells[line][col].maskdepth > -1)
        return;

    RBCell *cell = &rb->cells[line][col];
    if (cell->state != LINE) {
        make_span(rb, line, col, cols);
        cell->state       = LINE;
        cell->cols        = 1;
        cell->pen         = tickit_pen_ref(rb->pen);
        cell->v.line.mask = 0;
    } else if (!tickit_pen_equiv(cell->pen, rb->pen)) {
        tickit_pen_unref(cell->pen);
        cell->pen = tickit_pen_ref(rb->pen);
    }

    cell->v.line.mask |= bits;
}

void tickit_renderbuffer_hline_at(TickitRenderBuffer *rb, int line, int startcol, int endcol,
    TickitLineStyle style, TickitLineCaps caps) {
    DEBUG_LOGF(rb, "Bd", "HLine (%d..%d,%d)", startcol, endcol, line);

    int east = style << EAST_SHIFT;
    int west = style << WEST_SHIFT;

    linecell(rb, line, startcol, east | (caps & TICKIT_LINECAP_START ? west : 0));
    for (int col = startcol + 1; col <= endcol - 1; col++)
        linecell(rb, line, col, east | west);
    linecell(rb, line, endcol, (caps & TICKIT_LINECAP_END ? east : 0) | west);
}

void tickit_renderbuffer_vline_at(TickitRenderBuffer *rb, int startline, int endline, int col,
    TickitLineStyle style, TickitLineCaps caps) {
    DEBUG_LOGF(rb, "Bd", "VLine (%d,%d..%d)", col, startline, endline);

    int north = style << NORTH_SHIFT;
    int south = style << SOUTH_SHIFT;

    linecell(rb, startline, col, south | (caps & TICKIT_LINECAP_START ? north : 0));
    for (int line = startline + 1; line <= endline - 1; line++)
        linecell(rb, line, col, south | north);
    linecell(rb, endline, col, (caps & TICKIT_LINECAP_END ? south : 0) | north);
}

void tickit_renderbuffer_flush_to_term(TickitRenderBuffer *rb, TickitTerm *tt) {
    DEBUG_LOGF(rb, "Bf", "Flush to term");

    for (int line = 0; line < rb->lines; line++) {
        int phycol = -1; /* column where the terminal cursor physically is */

        for (int col = 0; col < rb->cols; /**/) {
            RBCell *cell = &rb->cells[line][col];

            if (cell->state == SKIP) {
                col += cell->cols;
                continue;
            }

            if (phycol < col)
                tickit_term_goto(tt, line, col);
            phycol = col;

            switch (cell->state) {
                case TEXT: {
                    TickitStringPos start, end, limit;
                    const char *text = tickit_string_get(cell->v.text.s);

                    tickit_stringpos_limit_columns(&limit, cell->v.text.offs);
                    tickit_utf8_count(text, &start, &limit);

                    limit.columns += cell->cols;
                    end = start;
                    tickit_utf8_countmore(text, &end, &limit);

                    tickit_term_setpen(tt, cell->pen);
                    tickit_term_printn(tt, text + start.bytes, end.bytes - start.bytes);

                    phycol += cell->cols;
                } break;
                case ERASE: {
                    /* No need to set moveend=true to erasech unless we actually
                     * have more content */
                    int moveend = col + cell->cols < rb->cols &&
                                  rb->cells[line][col + cell->cols].state != SKIP;

                    tickit_term_setpen(tt, cell->pen);
                    tickit_term_erasech(tt, cell->cols, moveend ? TICKIT_YES : TICKIT_MAYBE);

                    if (moveend)
                        phycol += cell->cols;
                    else
                        phycol = -1;
                } break;
                case LINE: {
                    TickitPen *pen = cell->pen;

                    do {
                        tmp_cat_utf8(rb, linemask_to_char[cell->v.line.mask]);

                        col++;
                        phycol += cell->cols;
                    } while (col < rb->cols && (cell = &rb->cells[line][col]) &&
                             cell->state == LINE && tickit_pen_equiv(cell->pen, pen));

                    tickit_term_setpen(tt, pen);
                    tickit_term_printn(tt, rb->tmp, rb->tmplen);
                    rb->tmplen = 0;
                }
                    continue; /* col already updated */
                case CHAR: {
                    tmp_cat_utf8(rb, cell->v.chr.codepoint);

                    tickit_term_setpen(tt, cell->pen);
                    tickit_term_printn(tt, rb->tmp, rb->tmplen);
                    rb->tmplen = 0;

                    phycol += cell->cols;
                } break;
                case SKIP:
                case CONT:
                    /* unreachable */
                    abort();
            }

            col += cell->cols;
        }
    }

    tickit_renderbuffer_reset(rb);
}

static void copyrect(TickitRenderBuffer *dst, TickitRenderBuffer *src, const TickitRect *dstrect,
    const TickitRect *srcrect, bool copy_skip) {
    if (srcrect->lines == 0 || srcrect->cols == 0)
        return;

    /* TODO:
     *   * consider how this works in the presence of a translation offset
     *     defined on src
     */
    int lineoffs = dstrect->top - srcrect->top, coloffs = dstrect->left - srcrect->left;

    int bottom = tickit_rect_bottom(srcrect), right = tickit_rect_right(srcrect);

    /* Several steps have to be done somewhat specially for copies into the same
     * RB
     */
    bool samerb = dst == src;

    if (samerb && lineoffs == 0 && coloffs == 0)
        return;

    /* iterate lines from the bottom upward if we're coping down in the same RB */
    bool upwards = samerb && (lineoffs > 0);
    /* iterate columns leftward if we're copying rightward in the same RB */
    bool leftwards = samerb && (lineoffs == 0) && (coloffs > 0);

    for (int line = upwards ? bottom - 1 : srcrect->top;
         upwards ? line >= srcrect->top : line < bottom; upwards ? line-- : line++) {
        for (int col = leftwards ? right - 1 : srcrect->left;
             leftwards ? col >= srcrect->left : col < right;
            /**/) {
            RBCell *cell = &src->cells[line][col];

            int offset = 0;

            if (cell->state == CONT) {
                int startcol = cell->startcol;
                cell         = &src->cells[line][startcol];

                if (leftwards) {
                    col = startcol;
                    if (col < srcrect->left)
                        col = srcrect->left;
                }

                offset = col - startcol;
            }

            int cols = cell->cols;

            if (col + cols > tickit_rect_right(srcrect))
                cols = tickit_rect_right(srcrect) - col;

            if (cell->state != SKIP) {
                tickit_renderbuffer_savepen(dst);
                tickit_renderbuffer_setpen(dst, cell->pen);
            }

            switch (cell->state) {
                case SKIP:
                    if (copy_skip)
                        skip(dst, line + lineoffs, col + coloffs, cols);
                    break;
                case TEXT: {
                    TickitStringPos start, end, limit;
                    const char *text = tickit_string_get(cell->v.text.s);

                    tickit_stringpos_limit_columns(&limit, cell->v.text.offs + offset);
                    tickit_utf8_count(text, &start, &limit);

                    limit.columns += cols;
                    end = start;
                    tickit_utf8_countmore(text, &end, &limit);

                    if (start.bytes > 0 || end.bytes < tickit_string_len(cell->v.text.s))
                        put_text(dst, line + lineoffs, col + coloffs, text + start.bytes,
                            end.bytes - start.bytes);
                    else
                        // We can just cheaply copy the entire string
                        put_string(dst, line + lineoffs, col + coloffs, cell->v.text.s);
                } break;
                case ERASE:
                    erase(dst, line + lineoffs, col + coloffs, cols);
                    break;
                case LINE:
                    linecell(dst, line + lineoffs, col + coloffs, cell->v.line.mask);
                    break;
                case CHAR:
                    put_char(dst, line + lineoffs, col + coloffs, cell->v.chr.codepoint);
                    break;
                case CONT:
                    /* unreachable */
                    abort();
            }

            if (cell->state != SKIP)
                tickit_renderbuffer_restore(dst);

            if (leftwards)
                col--; /* we'll jump back to the beginning of a CONT region on the
                          next iteration
                        */
            else
                col += cell->cols;
        }
    }
}

void tickit_renderbuffer_blit(TickitRenderBuffer *dst, TickitRenderBuffer *src) {
    copyrect(dst, src, &(TickitRect){.top = 0, .left = 0, .lines = src->lines, .cols = src->cols},
        &(TickitRect){.top = 0, .left = 0, .lines = src->lines, .cols = src->cols}, false);
}

void tickit_renderbuffer_copyrect(
    TickitRenderBuffer *rb, const TickitRect *dest, const TickitRect *src) {
    copyrect(rb, rb, dest, src, true);
}

void tickit_renderbuffer_moverect(
    TickitRenderBuffer *rb, const TickitRect *dest, const TickitRect *src) {
    copyrect(rb, rb, dest, src, true);

    /* Calculate what area of the RB needs skipping due to move */
    TickitRectSet *cleararea = tickit_rectset_new();
    tickit_rectset_add(cleararea, src);
    tickit_rectset_subtract(cleararea,
        &(TickitRect){
            .top = dest->top, .left = dest->left, .lines = src->lines, .cols = src->cols});

    size_t n = tickit_rectset_rects(cleararea);
    for (size_t i = 0; i < n; i++) {
        TickitRect rect;
        tickit_rectset_get_rect(cleararea, i, &rect);

        tickit_renderbuffer_skiprect(rb, &rect);
    }

    tickit_rectset_destroy(cleararea);
}

static RBCell *get_span(TickitRenderBuffer *rb, int line, int col, int *offset) {
    int cols = 1;
    if (!xlate_and_clip(rb, &line, &col, &cols, NULL))
        return NULL;

    *offset      = 0;
    RBCell *cell = &rb->cells[line][col];
    if (cell->state == CONT) {
        *offset = col - cell->startcol;
        cell    = &rb->cells[line][cell->startcol];
    }

    return cell;
}

static size_t get_span_text(
    TickitRenderBuffer *rb, RBCell *span, int offset, int one_grapheme, char *buffer, size_t len) {
    size_t bytes;

    switch (span->state) {
        case CONT:  // should be unreachable
            return -1;

        case SKIP:
        case ERASE:
            bytes = 0;
            break;

        case TEXT: {
            const char *text = tickit_string_get(span->v.text.s);
            TickitStringPos start, end, limit;

            tickit_stringpos_limit_columns(&limit, span->v.text.offs + offset);
            tickit_utf8_count(text, &start, &limit);

            if (one_grapheme)
                tickit_stringpos_limit_graphemes(&limit, start.graphemes + 1);
            else
                tickit_stringpos_limit_columns(&limit, span->cols);
            end = start;
            tickit_utf8_countmore(text, &end, &limit);

            bytes = end.bytes - start.bytes;

            if (buffer) {
                if (len < bytes)
                    return -1;
                strncpy(buffer, text + start.bytes, bytes);
                buffer[bytes] = 0;
            }
            break;
        }
        case LINE:
            bytes = tickit_utf8_put(buffer, len, linemask_to_char[span->v.line.mask]);
            break;

        case CHAR:
            bytes = tickit_utf8_put(buffer, len, span->v.chr.codepoint);
            break;
    }

    if (buffer && len > bytes)
        buffer[bytes] = 0;

    return bytes;
}

int tickit_renderbuffer_get_cell_active(TickitRenderBuffer *rb, int line, int col) {
    int offset;
    RBCell *span = get_span(rb, line, col, &offset);
    if (!span)
        return -1;

    return span->state != SKIP;
}

size_t tickit_renderbuffer_get_cell_text(
    TickitRenderBuffer *rb, int line, int col, char *buffer, size_t len) {
    int offset;
    RBCell *span = get_span(rb, line, col, &offset);
    if (!span || span->state == CONT)
        return -1;

    return get_span_text(rb, span, offset, 1, buffer, len);
}

TickitRenderBufferLineMask tickit_renderbuffer_get_cell_linemask(
    TickitRenderBuffer *rb, int line, int col) {
    int offset;
    RBCell *span = get_span(rb, line, col, &offset);
    if (!span || span->state != LINE)
        return (TickitRenderBufferLineMask){0};

    return (TickitRenderBufferLineMask){
        .north = (span->v.line.mask >> NORTH_SHIFT) & 0x03,
        .south = (span->v.line.mask >> SOUTH_SHIFT) & 0x03,
        .east  = (span->v.line.mask >> EAST_SHIFT) & 0x03,
        .west  = (span->v.line.mask >> WEST_SHIFT) & 0x03,
    };
}

TickitPen *tickit_renderbuffer_get_cell_pen(TickitRenderBuffer *rb, int line, int col) {
    int offset;
    RBCell *span = get_span(rb, line, col, &offset);
    if (!span || span->state == SKIP)
        return NULL;

    return span->pen;
}

size_t tickit_renderbuffer_get_span(TickitRenderBuffer *rb, int line, int startcol,
    struct TickitRenderBufferSpanInfo *info, char *text, size_t len) {
    int offset;
    RBCell *span = get_span(rb, line, startcol, &offset);
    if (!span || span->state == CONT)
        return -1;

    if (info)
        info->n_columns = span->cols - offset;

    if (span->state == SKIP) {
        if (info)
            info->is_active = 0;
        return 0;
    }

    if (info)
        info->is_active = 1;

    if (info && info->pen) {
        tickit_pen_clear(info->pen);
        tickit_pen_copy(info->pen, span->pen, 1);
    }

    size_t retlen = get_span_text(rb, span, offset, 0, text, len);
    if (info) {
        info->len  = retlen;
        info->text = text;
    }
    return len;
}
