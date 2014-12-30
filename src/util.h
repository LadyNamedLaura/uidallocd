#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "macro.h"


#define new(t, n) ((t*) malloc_multiply(sizeof(t), (n)))

#define new0(t, n) ((t*) calloc((n), sizeof(t)))

#define newa(t, n) ((t*) alloca(sizeof(t)*(n)))

#define newa0(t, n) ((t*) alloca0(sizeof(t)*(n)))

#define newdup(t, p, n) ((t*) memdup_multiply(p, sizeof(t), (n)))

#define malloc0(n) (calloc((n), 1))

#define alloca0(n)                                      \
        ({                                              \
                char *_new_;                            \
                size_t _len_ = n;                       \
                _new_ = alloca(_len_);                  \
                (void *) memset(_new_, 0, _len_);       \
        })

#define streq(a,b) (strcmp((a),(b)) == 0)

#define log_error(str, ...) fprintf(stderr, "ERROR: "str"\n", __VA_ARGS__)

_malloc_  _alloc_(1, 2) static inline void *malloc_multiply(size_t a, size_t b) {
        if (_unlikely_(b != 0 && a > ((size_t) -1) / b))
                return NULL;

        return malloc(a * b);
}

#define DEFINE_TRIVIAL_CLEANUP_FUNC(type, func)                 \
        static inline void func##p(type *p) {                   \
                if (*p)                                         \
                        func(*p);                               \
        }                                                       \
        struct __useless_struct_to_allow_trailing_semicolon__

static inline char *startswith(const char *s, const char *prefix) {
        size_t l;

        l = strlen(prefix);
        if (strncmp(s, prefix, l) == 0)
                return (char*) s + l;

        return NULL;
}

void random_bytes(void *p, size_t n);
