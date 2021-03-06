#ifndef USER_TRACE_H
#define USER_TRACE_H

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <execinfo.h>
#include <stdarg.h>

//#define die(code) exit(code)
#define die(code)	asm("int3")
#define assert(expr)	do {					\
	if (!(expr))						\
		error("Failed assert(%s)", #expr);		\
} while (0)

#include "kernel/trace.h"

#endif /* !USER_TRACE_H */
