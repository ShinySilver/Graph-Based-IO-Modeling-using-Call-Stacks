#ifndef IOTRACER_BACKTRACE_H
#define IOTRACER_BACKTRACE_H

/**
 * Called at the library loading time.
 */
void iotracer_backtrace_table_init(void);

/**
 * Get a hash for the current backtrace, and register it into the internal hash map.
 */
unsigned long long get_hash_for_current_backtrace(unsigned int call_stack_depth);

/**
 * Write the backtrace hash map to the disk. Currently not implemented
 */
void export_backtrace_table(void);

u_int64_t MurmurHash64A(const void *key, int len, u_int64_t seed);

#ifdef IOTRACER_DLOPEN_SUPPORT
/**
 * In order to keep the lib loading table up to date, we create a wrapper around the dlopen function. The original pointer is kept here.
 * dlclose could also be monitored, but it's not really necessary
 */
void *(*iotracer_safe_dlopen)(const char *filename, int flag); 
#endif
#endif