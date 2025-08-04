#ifndef GRIOT_MODEL_H
#define GRIOT_MODEL_H

#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>

typedef enum {GRIOT_READ, GRIOT_WRITE, GRIOT_OPEN, GRIOT_CLOSE} op_type;

/**
 * Called by GrIOt tracer when a process is created
 */
void griot_init(uint32_t context_size, uint32_t griot_call_stack_depth);

/**
 * Called by GrIOt tracer when a process is finished, just after printing the results
 */
void griot_finalize();

/**
 * Called by GrIOt tracer when an I/O is intercepted
 */
void on_io(uint64_t timestamp, int32_t thread_id, int fd, off_t offset, size_t length, uint64_t duration_ns, op_type op_type, FILE *optional_debug_file);

/**
 * Called by GrIOt tracer in child processes in order to avoid counting any I/O more than once
 */
void griot_results_reset();

/**
 * Called by GrIOt tracer at the end of a process in order to print the results
 */
void griot_results_dump(FILE *file);

#endif
