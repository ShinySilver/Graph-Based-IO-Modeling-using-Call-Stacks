#ifndef GRIOT_LOG_H
#define GRIOT_LOG_H

#include <stdio.h>
#include <stdlib.h>

#define INFO(_message, ...) {printf("[GRIOT_INFO] %s:%d ", __FILE__, __LINE__); printf(_message, ##__VA_ARGS__); printf("\n");}
#define WARN(_message, ...) {printf("[GRIOT_WARN] %s:%d ", __FILE__, __LINE__); printf(_message, ##__VA_ARGS__); printf("\n");}
#define ERROR(_message, ...) {fprintf(stderr, "[GRIOT_ERROR] %s:%d ", __FILE__, __LINE__); fprintf(stderr, _message, ##__VA_ARGS__); fprintf(stderr, "\n");}
#define FATAL(_message, ...) {fprintf(stderr, "[GRIOT_FATAL] %s:%d ", __FILE__, __LINE__); fprintf(stderr, _message, ##__VA_ARGS__); fprintf(stderr, "\n"); exit(0);}

ssize_t iolib_safe_fprintf(FILE *restrict f, const char *restrict fmt, ...);

#endif