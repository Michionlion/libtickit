#include <stdarg.h>

void plan_tests(int n);
void ok(int cmp, char *name);
void pass(char *name);
void fail(char *name);
void diag(char *fmt, ...);
void is_int(int got, int expect, char *name);
void is_ptr(void *got, void *expect, char *name);
void is_str(const char *got, const char *expect, char *name);
void is_str_escape(const char *got, const char *expect, char *name);
int exit_status(void);

void tap_skip(int n, const char *fmt, ...);

#define skip(test, ...)                  \
    do {                                 \
        if (test) {                      \
            tap_skip(__VA_ARGS__, NULL); \
            break;                       \
        }
#define end_skip \
    }            \
    while (0)
