#include "termdriver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define strneq(a, b, n) (strncmp(a, b, n) == 0)

#define CSI_MORE_SUBPARAM 0x80000000
#define CSI_NEXT_SUB(x) ((x)&CSI_MORE_SUBPARAM)
#define CSI_PARAM(x) ((x) & ~CSI_MORE_SUBPARAM)

struct XTermDriver {
    TickitTermDriver driver;

    int dcs_offset;
    char dcs_buffer[16];

    struct {
        unsigned int altscreen : 1;
        unsigned int cursorvis : 1;
        unsigned int cursorblink : 1;
        unsigned int cursorshape : 2;
        unsigned int mouse : 2;
        unsigned int keypad : 1;
    } mode;

    struct {
        unsigned int cursorshape : 1;
        unsigned int slrm : 1;
        unsigned int csi_sub_colon : 1;
        unsigned int rgb8 : 1;
    } cap;

    struct {
        unsigned int cursorvis : 1;
        unsigned int cursorblink : 1;
        unsigned int cursorshape : 2;
        unsigned int slrm : 1;
    } initialised;
};

static bool print(TickitTermDriver *ttd, const char *str, size_t len) {
    tickit_termdrv_write_str(ttd, str, len);
    return true;
}

static bool goto_abs(TickitTermDriver *ttd, int line, int col) {
    if (line != -1 && col > 0)
        tickit_termdrv_write_strf(ttd, "\e[%d;%dH", line + 1, col + 1);
    else if (line != -1 && col == 0)
        tickit_termdrv_write_strf(ttd, "\e[%dH", line + 1);
    else if (line != -1)
        tickit_termdrv_write_strf(ttd, "\e[%dd", line + 1);
    else if (col > 0)
        tickit_termdrv_write_strf(ttd, "\e[%dG", col + 1);
    else if (col != -1)
        tickit_termdrv_write_str(ttd, "\e[G", 3);

    return true;
}

static bool move_rel(TickitTermDriver *ttd, int downward, int rightward) {
    if (downward > 1)
        tickit_termdrv_write_strf(ttd, "\e[%dB", downward);
    else if (downward == 1)
        tickit_termdrv_write_str(ttd, "\e[B", 3);
    else if (downward == -1)
        tickit_termdrv_write_str(ttd, "\e[A", 3);
    else if (downward < -1)
        tickit_termdrv_write_strf(ttd, "\e[%dA", -downward);

    if (rightward > 1)
        tickit_termdrv_write_strf(ttd, "\e[%dC", rightward);
    else if (rightward == 1)
        tickit_termdrv_write_str(ttd, "\e[C", 3);
    else if (rightward == -1)
        tickit_termdrv_write_str(ttd, "\e[D", 3);
    else if (rightward < -1)
        tickit_termdrv_write_strf(ttd, "\e[%dD", -rightward);

    return true;
}

static bool scrollrect(TickitTermDriver *ttd, const TickitRect *rect, int downward, int rightward) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    if (!downward && !rightward)
        return true;

    int term_cols;
    tickit_term_get_size(ttd->tt, NULL, &term_cols);

    int right = tickit_rect_right(rect);

    /* Use DECSLRM only for 1 line of insert/delete, because any more and it's
     * likely better to use the generic system below
     */
    if (((xd->cap.slrm && rect->lines == 1) || (right == term_cols)) && downward == 0) {
        if (right < term_cols)
            tickit_termdrv_write_strf(ttd, "\e[;%ds", right);

        for (int line = rect->top; line < tickit_rect_bottom(rect); line++) {
            goto_abs(ttd, line, rect->left);
            if (rightward > 1)
                tickit_termdrv_write_strf(ttd, "\e[%dP", rightward); /* DCH */
            else if (rightward == 1)
                tickit_termdrv_write_str(ttd, "\e[P", 3); /* DCH1 */
            else if (rightward == -1)
                tickit_termdrv_write_str(ttd, "\e[@", 3); /* ICH1 */
            else if (rightward < -1)
                tickit_termdrv_write_strf(ttd, "\e[%d@", -rightward); /* ICH */
        }

        if (right < term_cols)
            tickit_termdrv_write_strf(ttd, "\e[s");

        return true;
    }

    if (xd->cap.slrm || (rect->left == 0 && rect->cols == term_cols && rightward == 0)) {
        tickit_termdrv_write_strf(ttd, "\e[%d;%dr", rect->top + 1, tickit_rect_bottom(rect));

        if (rect->left > 0 || right < term_cols)
            tickit_termdrv_write_strf(ttd, "\e[%d;%ds", rect->left + 1, right);

        goto_abs(ttd, rect->top, rect->left);

        if (downward > 1)
            tickit_termdrv_write_strf(ttd, "\e[%dM", downward); /* DL */
        else if (downward == 1)
            tickit_termdrv_write_str(ttd, "\e[M", 3); /* DL1 */
        else if (downward == -1)
            tickit_termdrv_write_str(ttd, "\e[L", 3); /* IL1 */
        else if (downward < -1)
            tickit_termdrv_write_strf(ttd, "\e[%dL", -downward); /* IL */

        if (rightward > 1)
            tickit_termdrv_write_strf(ttd, "\e[%d'~", rightward); /* DECDC */
        else if (rightward == 1)
            tickit_termdrv_write_str(ttd, "\e['~", 4); /* DECDC1 */
        else if (rightward == -1)
            tickit_termdrv_write_str(ttd, "\e['}", 4); /* DECIC1 */
        if (rightward < -1)
            tickit_termdrv_write_strf(ttd, "\e[%d'}", -rightward); /* DECIC */

        tickit_termdrv_write_str(ttd, "\e[r", 3);

        if (rect->left > 0 || right < term_cols)
            tickit_termdrv_write_str(ttd, "\e[s", 3);

        return true;
    }

    return false;
}

static bool erasech(TickitTermDriver *ttd, int count, TickitMaybeBool moveend) {
    if (count < 1)
        return true;

    /* Only use ECH if we're not in reverse-video mode. xterm doesn't do rv+ECH
     * properly
     */
    if (!tickit_pen_get_bool_attr(tickit_termdrv_current_pen(ttd), TICKIT_PEN_REVERSE)) {
        if (count == 1)
            tickit_termdrv_write_str(ttd, "\e[X", 3);
        else
            tickit_termdrv_write_strf(ttd, "\e[%dX", count);

        if (moveend == TICKIT_YES)
            move_rel(ttd, 0, count);
    } else {
        /* TODO: consider tickit_termdrv_write_chrfill(ttd, c, n)
         */
        char *spaces = tickit_termdrv_get_tmpbuffer(ttd, 64);
        memset(spaces, ' ', 64);
        while (count > 64) {
            tickit_termdrv_write_str(ttd, spaces, 64);
            count -= 64;
        }
        tickit_termdrv_write_str(ttd, spaces, count);

        if (moveend == TICKIT_NO)
            move_rel(ttd, 0, -count);
    }

    return true;
}

static bool clear(TickitTermDriver *ttd) {
    tickit_termdrv_write_strf(ttd, "\e[2J", 4);
    return true;
}

static struct SgrOnOff {
    int on, off;
} sgr_onoff[] = {
    {},       /* none */
    {30, 39}, /* fg */
    {40, 49}, /* bg */
    {1, 22},  /* bold */
    {4, 24},  /* under */
    {3, 23},  /* italic */
    {7, 27},  /* reverse */
    {9, 29},  /* strike */
    {10, 10}, /* altfont */
    {5, 25},  /* blink */
};

static bool chpen(TickitTermDriver *ttd, const TickitPen *delta, const TickitPen *final) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    /* There can be at most 16 SGR parameters; 5 from each of 2 colours, and
     * 6 single attributes
     */
    int params[16];
    int pindex = 0;

    for (TickitPenAttr attr = 1; attr < TICKIT_N_PEN_ATTRS; attr++) {
        if (!tickit_pen_has_attr(delta, attr))
            continue;

        struct SgrOnOff *onoff = &sgr_onoff[attr];

        int val;

        switch (attr) {
            case TICKIT_PEN_FG:
            case TICKIT_PEN_BG:
                val = tickit_pen_get_colour_attr(delta, attr);
                if (val < 0)
                    params[pindex++] = onoff->off;
                else if (xd->cap.rgb8 && tickit_pen_has_colour_attr_rgb8(delta, attr)) {
                    TickitPenRGB8 rgb = tickit_pen_get_colour_attr_rgb8(delta, attr);
                    params[pindex++]  = (onoff->on + 8) | CSI_MORE_SUBPARAM;
                    params[pindex++]  = 2 | CSI_MORE_SUBPARAM;
                    params[pindex++]  = rgb.r | CSI_MORE_SUBPARAM;
                    params[pindex++]  = rgb.g | CSI_MORE_SUBPARAM;
                    params[pindex++]  = rgb.b;
                } else if (val < 8)
                    params[pindex++] = onoff->on + val;
                else if (val < 16)
                    params[pindex++] = onoff->on + 60 + val - 8;
                else {
                    params[pindex++] = (onoff->on + 8) | CSI_MORE_SUBPARAM;
                    params[pindex++] = 5 | CSI_MORE_SUBPARAM;
                    params[pindex++] = val;
                }
                break;

            case TICKIT_PEN_UNDER:
                val = tickit_pen_get_int_attr(delta, attr);
                if (!val)
                    params[pindex++] = onoff->off;
                else if (val == 1)
                    params[pindex++] = onoff->on;
                else {
                    params[pindex++] = onoff->on | CSI_MORE_SUBPARAM;
                    params[pindex++] = val;
                }
                break;

            case TICKIT_PEN_ALTFONT:
                val = tickit_pen_get_int_attr(delta, attr);
                if (val < 0 || val >= 10)
                    params[pindex++] = onoff->off;
                else
                    params[pindex++] = onoff->on + val;
                break;

            case TICKIT_PEN_BOLD:
            case TICKIT_PEN_ITALIC:
            case TICKIT_PEN_REVERSE:
            case TICKIT_PEN_STRIKE:
            case TICKIT_PEN_BLINK:
                val              = tickit_pen_get_bool_attr(delta, attr);
                params[pindex++] = val ? onoff->on : onoff->off;
                break;

            case TICKIT_N_PEN_ATTRS:
                break;
        }
    }

    if (pindex == 0)
        return true;

    /* If we're going to clear all the attributes then empty SGR is neater */
    if (!tickit_pen_is_nondefault(final))
        pindex = 0;

    /* Render params[] into a CSI string */

    size_t len = 3; /* ESC [ ... m */
    for (int i = 0; i < pindex; i++)
        len += snprintf(NULL, 0, "%d", CSI_PARAM(params[i])) + 1;
    if (pindex > 0)
        len--; /* Last one has no final separator */

    char *buffer = tickit_termdrv_get_tmpbuffer(ttd, len + 1);
    char *s      = buffer;

    s += sprintf(s, "\e[");
    for (int i = 0; i < pindex - 1; i++)
        s += sprintf(s, "%d%c", CSI_PARAM(params[i]),
            CSI_NEXT_SUB(params[i]) && xd->cap.csi_sub_colon ? ':' : ';');
    if (pindex > 0)
        s += sprintf(s, "%d", CSI_PARAM(params[pindex - 1]));
    sprintf(s, "m");

    tickit_termdrv_write_str(ttd, buffer, len);

    return true;
}

static bool getctl_int(TickitTermDriver *ttd, TickitTermCtl ctl, int *value) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    switch (ctl) {
        case TICKIT_TERMCTL_ALTSCREEN:
            *value = xd->mode.altscreen;
            return true;

        case TICKIT_TERMCTL_CURSORVIS:
            *value = xd->mode.cursorvis;
            return true;

        case TICKIT_TERMCTL_CURSORBLINK:
            *value = xd->mode.cursorblink;
            return true;

        case TICKIT_TERMCTL_MOUSE:
            *value = xd->mode.mouse;
            return true;

        case TICKIT_TERMCTL_CURSORSHAPE:
            *value = xd->mode.cursorshape;
            return true;

        case TICKIT_TERMCTL_KEYPAD_APP:
            *value = xd->mode.keypad;
            return true;

        case TICKIT_TERMCTL_COLORS:
            *value = 256;
            return true;

        default:
            return false;
    }
}

static int mode_for_mouse(TickitTermMouseMode mode) {
    switch (mode) {
        case TICKIT_TERM_MOUSEMODE_CLICK:
            return 1000;
        case TICKIT_TERM_MOUSEMODE_DRAG:
            return 1002;
        case TICKIT_TERM_MOUSEMODE_MOVE:
            return 1003;

        case TICKIT_TERM_MOUSEMODE_OFF:
            break;
    }
    return 0;
}

static bool setctl_int(TickitTermDriver *ttd, TickitTermCtl ctl, int value) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    switch (ctl) {
        case TICKIT_TERMCTL_ALTSCREEN:
            if (!xd->mode.altscreen == !value)
                return true;

            tickit_termdrv_write_str(ttd, value ? "\e[?1049h" : "\e[?1049l", 0);
            xd->mode.altscreen = !!value;
            return true;

        case TICKIT_TERMCTL_CURSORVIS:
            if (!xd->mode.cursorvis == !value)
                return true;

            tickit_termdrv_write_str(ttd, value ? "\e[?25h" : "\e[?25l", 0);
            xd->mode.cursorvis = !!value;
            return true;

        case TICKIT_TERMCTL_CURSORBLINK:
            if (xd->initialised.cursorblink && !xd->mode.cursorblink == !value)
                return true;

            tickit_termdrv_write_str(ttd, value ? "\e[?12h" : "\e[?12l", 0);
            xd->mode.cursorblink = !!value;
            return true;

        case TICKIT_TERMCTL_MOUSE:
            if (xd->mode.mouse == value)
                return true;

            /* Modes 1000, 1002 and 1003 are mutually exclusive; enabling any one
             * disables the other two
             */
            if (!value)
                tickit_termdrv_write_strf(ttd, "\e[?%dl\e[?1006l", mode_for_mouse(xd->mode.mouse));
            else
                tickit_termdrv_write_strf(ttd, "\e[?%dh\e[?1006h", mode_for_mouse(value));

            xd->mode.mouse = value;
            return true;

        case TICKIT_TERMCTL_CURSORSHAPE:
            if (xd->initialised.cursorshape && xd->mode.cursorshape == value)
                return true;

            if (xd->cap.cursorshape)
                tickit_termdrv_write_strf(
                    ttd, "\e[%d q", value * 2 + (xd->mode.cursorblink ? -1 : 0));
            xd->mode.cursorshape = value;
            return true;

        case TICKIT_TERMCTL_KEYPAD_APP:
            if (!xd->mode.keypad == !value)
                return true;

            tickit_termdrv_write_strf(ttd, value ? "\e=" : "\e>");
            return true;

        default:
            return false;
    }
}

static bool setctl_str(TickitTermDriver *ttd, TickitTermCtl ctl, const char *value) {
    switch (ctl) {
        case TICKIT_TERMCTL_ICON_TEXT:
            tickit_termdrv_write_strf(ttd, "\e]1;%s\e\\", value);
            return true;

        case TICKIT_TERMCTL_TITLE_TEXT:
            tickit_termdrv_write_strf(ttd, "\e]2;%s\e\\", value);
            return true;

        case TICKIT_TERMCTL_ICONTITLE_TEXT:
            tickit_termdrv_write_strf(ttd, "\e]0;%s\e\\", value);
            return true;

        default:
            return false;
    }
}

static void start(TickitTermDriver *ttd) {
    // Enable DECSLRM
    tickit_termdrv_write_strf(ttd, "\e[?69h");

    // Find out if DECSLRM is actually supported
    tickit_termdrv_write_strf(ttd, "\e[?69$p");

    // Also query the current cursor visibility, blink status, and shape
    tickit_termdrv_write_strf(ttd, "\e[?25$p\e[?12$p\eP$q q\e\\");

    // Try to work out whether the terminal supports 24bit cololurs (RGB8) and
    // whether it understands : to separate sub-params
    tickit_termdrv_write_strf(ttd, "\e[38;5;255m\e[38:2:0:1:2m\eP$qm\e\\\e[m");

    /* Some terminals (e.g. xfce4-terminal) don't understand DECRQM and print
     * the raw bytes directly as output, while still claiming to be TERM=xterm
     * It doens't hurt at this point to clear the current line just in case.
     */
    tickit_termdrv_write_strf(ttd, "\e[G\e[K");
}

static bool started(TickitTermDriver *ttd) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    return xd->initialised.cursorvis && xd->initialised.cursorblink &&
           xd->initialised.cursorshape && xd->initialised.slrm;
}

static void gotkey_modereport(struct XTermDriver *xd, int initial, int mode, int value) {
    if (initial == '?')  // DEC mode
        switch (mode) {
            case 12:  // Cursor blink
                if (value == 1)
                    xd->mode.cursorblink = 1;
                xd->initialised.cursorblink = 1;
                break;
            case 25:  // DECTCEM == Cursor visibility
                if (value == 1)
                    xd->mode.cursorvis = 1;
                xd->initialised.cursorvis = 1;
                break;
            case 69:  // DECVSSM
                if (value == 1 || value == 2)
                    xd->cap.slrm = 1;
                xd->initialised.slrm = 1;
                break;
        }
}

static void gotkey_decrqss(struct XTermDriver *xd, const char *args, size_t arglen) {
    if (strneq(args + arglen - 2, " q", 2)) {  // DECSCUSR
        int value;
        if (sscanf(args, "%d", &value)) {
            // value==1 or 2 => shape == 1, 3 or 4 => 2, etc..
            int shape            = (value + 1) / 2;
            xd->mode.cursorshape = shape;
            xd->cap.cursorshape  = 1;
        }
        xd->initialised.cursorshape = 1;
    } else if (strneq(args + arglen - 1, "m", 1)) {  // SGR
        // skip the initial number, find the first separator
        while (arglen && args[0] >= '0' && args[0] <= '9')
            args++, arglen--;
        if (!arglen)
            return;

        if (args[0] == ':')
            xd->cap.csi_sub_colon = 1;
        args++, arglen--;

        // If the palette index is 2 then the terminal understands rgb8
        int value;
        if (sscanf(args, "%d", &value) && value == 2)
            xd->cap.rgb8 = 1;
    }
}

static int gotkey(TickitTermDriver *ttd, TermKey *tk, const TermKeyKey *key) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    if (key->type == TERMKEY_TYPE_MODEREPORT) {
        int initial, mode, value;
        termkey_interpret_modereport(tk, key, &initial, &mode, &value);
        gotkey_modereport(xd, initial, mode, value);

        return 1;
    } else if (key->type == TERMKEY_TYPE_DCS) {
        const char *dcs;
        if (termkey_interpret_string(tk, key, &dcs) != TERMKEY_RES_KEY)
            return 0;

        if (strneq(dcs, "1$r", 3)) {  // Successful DECRQSS
            gotkey_decrqss(xd, dcs + 3, strlen(dcs + 3));
        }

        // Just eat all the DCSes
        return 1;
    }

    return 0;
}

static void teardown(TickitTermDriver *ttd) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    if (xd->mode.mouse)
        tickit_termdrv_write_strf(ttd, "\e[?%dl\e[?1006l", mode_for_mouse(xd->mode.mouse));
    if (!xd->mode.cursorvis)
        tickit_termdrv_write_str(ttd, "\e[?25h", 0);
    if (xd->mode.altscreen)
        tickit_termdrv_write_str(ttd, "\e[?1049l", 0);
    if (xd->mode.keypad)
        tickit_termdrv_write_strf(ttd, "\e>");

    // Reset pen
    tickit_termdrv_write_str(ttd, "\e[m", 3);
}

static void resume(TickitTermDriver *ttd) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    if (xd->mode.keypad)
        tickit_termdrv_write_strf(ttd, "\e=");
    if (xd->mode.altscreen)
        tickit_termdrv_write_str(ttd, "\e[?1049h", 0);
    if (!xd->mode.cursorvis)
        tickit_termdrv_write_str(ttd, "\e[?25l", 0);
    if (xd->mode.mouse)
        tickit_termdrv_write_strf(ttd, "\e[?%dh\e[?1006h", mode_for_mouse(xd->mode.mouse));
}

static void destroy(TickitTermDriver *ttd) {
    struct XTermDriver *xd = (struct XTermDriver *)ttd;

    free(xd);
}

static TickitTermDriverVTable xterm_vtable = {
    .destroy    = destroy,
    .start      = start,
    .started    = started,
    .stop       = teardown,
    .pause      = teardown,
    .resume     = resume,
    .print      = print,
    .goto_abs   = goto_abs,
    .move_rel   = move_rel,
    .scrollrect = scrollrect,
    .erasech    = erasech,
    .clear      = clear,
    .chpen      = chpen,
    .getctl_int = getctl_int,
    .setctl_int = setctl_int,
    .setctl_str = setctl_str,
    .gotkey     = gotkey,
};

static TickitTermDriver *new (const TickitTermProbeArgs *args) {
    const char *termtype = args->termtype;

    // added based on popular entries from
    // https://gist.github.com/XVilka/8346728
    // they may not end up being the actual termtype string,
    // but hopefully will provide something at least
    // clang-format off
    if (
        // straight $TERM comparison
        strncmp(termtype, "xterm", 5)       != 0 ||
        strncmp(termtype, "st", 2)          != 0 ||
        strncmp(termtype, "xst", 3)         != 0 ||
        strncmp(termtype, "pangoterm", 9)   != 0 ||
        strncmp(termtype, "termux", 6)      != 0 ||
        strncmp(termtype, "konsole", 7)     != 0 ||
        strncmp(termtype, "hterm", 5)       != 0 ||
        strncmp(termtype, "kitty", 5)       != 0 ||
        strncmp(termtype, "iterm", 5)       != 0 ||
        strncmp(termtype, "macterm", 7)     != 0 ||
        strncmp(termtype, "mintty", 6)      != 0 ||
        strncmp(termtype, "conemu", 6)      != 0 ||
        strncmp(termtype, "tinyterm", 8)    != 0 ||
        strncmp(termtype, "terminator", 6)  != 0 ||
        strncmp(termtype, "lxterminal", 6)  != 0 ||
        strncmp(termtype, "lxterminal", 6)  != 0 ||
        // substring finding
        strstr(termtype, "truecolor")   != NULL ||
        strstr(termtype, "TRUECOLOR")   != NULL ||
        strstr(termtype, "RGB")         != NULL ||
        strstr(termtype, "rgb")         != NULL ||
        strstr(termtype, "24bit")       != NULL ||
        strstr(termtype, "24-bit")      != NULL ||
        strstr(termtype, "libvte")      != NULL) {
        return NULL;
    }
    // clang-format on

    switch (termtype[5]) {
        case 0:
        case '-':
            break;
        default:
            return NULL;
    }

    struct XTermDriver *xd = malloc(sizeof(struct XTermDriver));
    xd->driver.vtable      = &xterm_vtable;

    xd->dcs_offset = -1;

    memset(&xd->mode, 0, sizeof xd->mode);
    xd->mode.cursorvis = 1;

    memset(&xd->cap, 0, sizeof xd->cap);

    memset(&xd->initialised, 0, sizeof xd->initialised);

    return (TickitTermDriver *)xd;
}

TickitTermDriverProbe tickit_termdrv_probe_xterm = {
    .new = new,
};
