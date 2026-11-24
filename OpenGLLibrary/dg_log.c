/* Minimal logger: appends text to "OpenGLLibrary.log" in the default folder
 * (the application's folder under the Finder), at most 16 KiB. A shared
 * library has no console, and failures must not be silent. */
#include <MacTypes.h>
#include <Files.h>
#include <string.h>
#include <stdarg.h>
#include "dg_client.h"

#define LOG_LIMIT 16384
#ifndef fnfErr
#define fnfErr (-43)
#endif

static void Put(char **o, char *end, const char *s, size_t n)
{
	while (n-- && *o < end)
		*(*o)++ = *s++;
}

static void PutNumber(char **o, char *end, unsigned long v, unsigned base, int neg)
{
	char t[24];
	int i = 0;

	do {
		t[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (neg)
		Put(o, end, "-", 1);
	while (i)
		Put(o, end, &t[--i], 1);
}

void DGLog(const char *fmt, ...)
{
	static int written;
	char buf[256], *o = buf, *end = buf + sizeof buf - 1;
	va_list ap;
	FSSpec spec;
	short ref;
	long count;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		int l = 0;

		if (*fmt != '%') {
			Put(&o, end, fmt, 1);
			continue;
		}
		fmt++;
		if (*fmt == 'l') {
			l = 1;
			fmt++;
		}
		switch (*fmt) {
		case 'd': {
			long v = l ? va_arg(ap, long) : va_arg(ap, int);
			PutNumber(&o, end, v < 0 ? (unsigned long)-v : (unsigned long)v, 10, v < 0);
			break;
		}
		case 'u': PutNumber(&o, end, l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0); break;
		case 'x': PutNumber(&o, end, l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0); break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			Put(&o, end, s ? s : "(null)", s ? strlen(s) : 6);
			break;
		}
		case 'c': { char c = (char)va_arg(ap, int); Put(&o, end, &c, 1); break; }
		case '%': Put(&o, end, "%", 1); break;
		case 0: fmt--; break;
		default: break;
		}
	}
	va_end(ap);
	*o++ = '\r';
	count = o - buf;

	if (written + count > LOG_LIMIT)
		return;
	if (FSMakeFSSpec(0, 0, "\pOpenGLLibrary.log", &spec) == fnfErr)
		FSpCreate(&spec, '????', 'TEXT', 0);
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref))
		return;
	SetFPos(ref, fsFromLEOF, 0);
	FSWrite(ref, &count, buf);
	FSClose(ref);
	written += count;
}
