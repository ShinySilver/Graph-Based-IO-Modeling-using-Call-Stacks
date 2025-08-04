# Graph‑Based I/O Modeling using Call Stacks (GrIOt)

This repository hosts the implementation of **GrIOt**, the graph-based modeling framework introduced in the paper:

> Nicolas, Louis-Marie, et al. "I/O patterns modeling of HPC applications with call stacks for predictive prefetch." Future Generation Computer Systems, vol. 175, 1 Feb. 2026, p. 108034, doi:10.1016/j.future.2025.108034.

## Overview

GrIOt model I/O behavior by capturing **call stacks** at each I/O operation. It constructs one or mutiple **directed graphs** where each node represents a call stack and edges encode possible sequential transitions. GrIOt enables the prediction of future I/O events, balancing accuracy with low runtime and memory overhead ([ScienceDirect](https://www.sciencedirect.com/science/article/pii/S0167739X25003292)).

## Building and Usage

The GrIOt tracer module depends on a proprietary library and cannot be directly compiled. However, you can still build and evaluate a standalone GrIOt model by compiling:

 - The shared header:
   `src/shared/griot_model.h`
 - The source files from one of the following model granularities:
   - `src/per-open/`  
   - `src/per-open-hash/`  
   - `src/per-process/`

The GrIOt Model should be called according to the content of `src/shared/griot_model.h`:

```c
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
```
