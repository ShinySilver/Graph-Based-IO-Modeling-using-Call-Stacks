#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <iolib_hooks.h>

ssize_t iolib_safe_fprintf(FILE *restrict f, const char *restrict fmt, ...){
	char *fstring;
	va_list ap;
	va_start(ap, fmt);
	int ret = vasprintf(&fstring, fmt, ap);
	va_end(ap);
	if(ret>=0){
		ret = iolib_safe_write(fileno(f), fstring, ret);
		free(fstring);
	}
	return ret;
}