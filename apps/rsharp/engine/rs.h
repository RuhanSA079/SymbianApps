/*
 * rs.h: rSharp's interpreter for a subset of C# (expressions, statements,
 * lambdas, LINQ methods and query syntax). Portable C++: no STL, no
 * exceptions, no static constructors, so it builds for Symbian and for the
 * host tests (apps/rsharp/tests) alike.
 *
 * One run at a time per process: the interpreter keeps its state in globals
 * while it runs. Everything a run allocates lives in an arena that is freed
 * when the run ends; only the output text outlives it.
 */
#ifndef RS_H
#define RS_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t rs_char;               /* UTF-16, as C# strings */

struct rs_options
    {
    void *(*alloc)(size_t aSize);       /* big blocks; NULL: malloc */
    void (*release)(void *aPtr);        /* NULL: free */
    size_t mem_limit;                   /* bytes; 0: 16 MB */
    size_t stack_limit;                 /* bytes of C stack to use; 0: 256 KB */
    volatile int *cancel;               /* set non-zero (another thread) to stop */
    int int_views;                      /* hex and binary under an integer result */
    size_t out_limit;                   /* output characters kept; 0: 200000 */
    };

enum
    {
    RS_OK = 0,
    RS_COMPILE_ERROR = 1,               /* nothing ran */
    RS_RUNTIME_ERROR = 2,               /* the output ends with the exception */
    RS_CANCELLED = 3
    };

/* Runs aSrc. *aOut (length *aOutLen) is the output: what the program wrote,
 * then its result, then any error. It stays valid until rs_free_output(). */
int rs_run(const rs_char *aSrc, int aLen, const rs_options *aOpt,
           const rs_char **aOut, int *aOutLen);
void rs_free_output(void);

/* Where the last run's error is in the source (a UTF-16 offset), or -1. */
int rs_error_pos(void);

/* The last run's error message, if any, is the last rs_error_length()
 * characters of its output (newline included); 0 if there was none. */
int rs_error_length(void);

#endif
