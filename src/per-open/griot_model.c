#include <string.h> // memset
#include <stdlib.h> // malloc
#include <time.h> // clock_gettime and CLOCK_MONOTONIC
#include "../shared/griot_model.h"
#include "../shared/hashmap.h"
#include "../shared/backtrace.h"
#include "../shared/log.h"
#include "griot_config.h"

/*
 * This file implements per-open-hash I/O call stack prediction with GrIOt
 *
 * Each open hash has its own graph.
 * Each opened file has a copy of its open hash graph, and its own context.
 * When closing a file, its open hash graph is updated with the file's graph
 *
 * We have an hashmap<fd, file_data> used to store per file data,
 * an hashmap<open_context, pred_data> used to store a graph per unique open_context,
 * a result struct, as well as a struct storing prediction and previous node pred_data.
 */

/*****************************
 * GrIOt main data structures
 */

struct
{
    uint64_t io_count;

    struct timespec app_start;
    uint64_t io_time;

    uint64_t read_volume;
    uint64_t write_volume;
    uint64_t total_volume;

    uint64_t mru_correct_prediction_count;
    uint64_t mru_correct_prediction_volume;
    uint64_t mru_correct_prediction_io_time;

    uint64_t mfu_correct_prediction_count;
    uint64_t mfu_correct_prediction_volume;
    uint64_t mfu_correct_prediction_io_time;

    uint64_t call_stack_instrumentation_count;
    uint64_t call_stack_instrumentation_time;
    uint64_t model_prediction_time;

    uint64_t highest_recorded_memory_footprint;
} griot_results;

struct
{
    // Has one reference prediction_table per unique open hash
    hashmap *per_fd_data;
} griot_model;

/**********************************
 * GrIOt secondary data structures
 */

typedef struct
{
    // The context hash of the most recent next I/O
    uint64_t mru_context_hash;

    // one weight per outgoing edge. Used for MFU.
    uint64_t *mfu_context_hash_list;
    uint64_t *mfu_weight_list;
    uint64_t mfu_lists_length;
} griot_prediction_data;

typedef struct{
    uint64_t *context;
    uint64_t context_hash;
    unsigned int context_size;
    int index;
} griot_context;

typedef struct
{
    // The file's prediction data
    hashmap *prediction_table;

    // The file's current context
    griot_context context;

    // When an I/O arrive, the next I/O is predicted here
    uint64_t mru_prediction;
    uint64_t mfu_prediction;

    // Fallback heuristic
    uint64_t previous_call_stack;

    // The prediction data of the previous I/O is kept from one I/O to another so it can be updated
    griot_prediction_data *previous_pred_data;
} griot_per_fd_data;

/**********************************
 * GrIOt hash maps data structures
 */

typedef struct
{
    griot_prediction_data *data;
    uint64_t call_stack_hash;
} griot_prediction_table_map_entry;

typedef struct
{
    griot_per_fd_data *data;
    uint64_t fd_hash;
} griot_per_fd_data_map_entry;

// And the hashmap functions associated with the above...
static uint64_t griot_hashmap_hash(const void *pred_data, uint64_t seed0, uint64_t seed1);
static int griot_hashmap_compare(const void *pred_data_1, const void *pred_data_2, void *udata);
static void griot_prediction_table_free(void *pred_data);
static void griot_per_fd_data_free(void *pred_data);

/**
 * Function used to compute the instantaneous memory footprint of griot
 */
static uint64_t griot_get_memory_footprint();

/***********************
 * GrIOt implementation
 */

static uint32_t context_size;
static uint32_t call_stack_depth;

/**
 * Called by GrIOt tracer when a process is created
 * This function should init all primary data structures
 */
void griot_init(uint32_t griot_context_size, uint32_t griot_call_stack_depth)
{
    // Init griot_results
    memset(&griot_results, 0, sizeof(griot_results));
    clock_gettime(CLOCK_MONOTONIC, &griot_results.app_start);

    // Init griot_model
    griot_model.per_fd_data = hashmap_new(sizeof(griot_per_fd_data_map_entry), 0, 0, 0, griot_hashmap_hash,
        griot_hashmap_compare, griot_per_fd_data_free, NULL);

    // Saving context size for future use
    context_size = griot_context_size;
    call_stack_depth = griot_call_stack_depth;
}

/**
 * Called by GrIOt tracer when a process is finished, just after printing the results
 */
void griot_finalize()
{
    // Updating memory footprint stat
    uint64_t memory_footprint = griot_get_memory_footprint();
    if(memory_footprint>griot_results.highest_recorded_memory_footprint)griot_results.highest_recorded_memory_footprint=memory_footprint;

    // Free griot model hash maps
    hashmap_free(griot_model.per_fd_data);
}

/**
 * Called when a file is opened. per_fd_data (pred table, context, etc) should be initialized here.
 * The initialization value is obtained from the per_open_hash_data hash map.
 * If there is no value in that hashmap, we juste create an empty pred table and context.
 */
void on_open(uint64_t timestamp, int32_t thread_id, int fd)
{
    // Let's create a new per_fd_data
    griot_per_fd_data *per_fd_data = (griot_per_fd_data *)malloc(sizeof(griot_per_fd_data));
    if(!per_fd_data) FATAL("Out of memory");

    // Filling it with zeros
    memset(per_fd_data, 0, sizeof(griot_per_fd_data));

    // Setting up the context
    per_fd_data->context.context = (uint64_t *)malloc(sizeof(uint64_t) * context_size);
    memset(per_fd_data->context.context, 0, sizeof(uint64_t) * context_size);
    per_fd_data->context.context_size = context_size;

    // Creating the file's prediction hashmap
    per_fd_data->prediction_table = hashmap_new(sizeof(griot_prediction_table_map_entry), 0, 0, 0, griot_hashmap_hash,
        griot_hashmap_compare, griot_prediction_table_free, NULL);

    // Placing the new per_fd_data in the fd hashmap
    hashmap_set(griot_model.per_fd_data, &(griot_per_fd_data_map_entry){.data=per_fd_data, .fd_hash=fd});
}

/**
 * Called when a file is closed. per_fd_data should be freed here, and per_open_hash_data  hash map is updated with its value.
 */
void on_close(uint64_t timestamp, int32_t thread_id, int fd)
{
    // Getting the per fd data
    const griot_per_fd_data_map_entry *map_entry = hashmap_get(griot_model.per_fd_data, &(griot_per_fd_data_map_entry){.fd_hash=fd});

    // If it's null, the file was opened and used out of the scope of GrIOt. We can just return
    if(map_entry==NULL)
    {
        #ifdef GRIOT_DEBUG
        WARN("File descriptor %d was created out of the scope of GrIOt and never used until now. Strange.", fd);
        #endif
        return;
    }
    griot_per_fd_data *per_fd_data = map_entry->data;

    // Updating memory footprint stat
    uint64_t memory_footprint = griot_get_memory_footprint();
    if(memory_footprint>griot_results.highest_recorded_memory_footprint)griot_results.highest_recorded_memory_footprint=memory_footprint;

    // Freeing the per fd data's context
    free(per_fd_data->context.context);

    // Freeing the per fd data's prediction table
    hashmap_free(per_fd_data->prediction_table);

    // Yeah, I nearly forget about this one. Thx asan.
    free(per_fd_data);

    // Freeing the per fd data itself
    hashmap_delete(griot_model.per_fd_data, &(griot_per_fd_data_map_entry){.fd_hash=fd});
}

/**
 * Called by GrIOt tracer when an I/O is intercepted
 */
void on_io(uint64_t timestamp, int32_t thread_id, int fd, off_t offset, size_t length, uint64_t duration_ns, op_type op_type, FILE *optional_debug_file)
{
    // (0) Ignore open/close. Only reads and writes are predicted.
    if(op_type==GRIOT_OPEN) on_open(timestamp, thread_id, fd);
    //if(op_type==GRIOT_CLOSE) on_close(timestamp, thread_id, fd);
    //if(op_type!=GRIOT_READ && op_type!=GRIOT_WRITE) return;

    // (0) Get the call stack
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t call_stack = get_hash_for_current_backtrace(call_stack_depth);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long dt_ns = (double)(t1.tv_sec - t0.tv_sec) * 1.0e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    griot_results.call_stack_instrumentation_count += 1;
    griot_results.call_stack_instrumentation_time += dt_ns;

    // (1) Update the stats
    clock_gettime(CLOCK_MONOTONIC, &t0);
    griot_results.io_count+=1;
    griot_results.io_time += duration_ns;
    griot_results.total_volume += length;
    if(op_type==GRIOT_READ) griot_results.read_volume += length;
    else if(op_type==GRIOT_WRITE) griot_results.write_volume += length;

    // (2) Get the per fd data
    griot_per_fd_data *per_fd_data;
    {
        const griot_per_fd_data_map_entry *map_entry = hashmap_get(griot_model.per_fd_data, &(griot_per_fd_data_map_entry){.fd_hash=fd});
        if(map_entry==NULL){
            #ifdef GRIOT_DEBUG
            ERROR("Intercepting an I/O to fd=%d we have never heard of before. It's either a fd inherited from a fork"
                ", or the application is using dup or similar.\n", fd);
            #endif
            on_open(timestamp, thread_id, fd);
            per_fd_data = ((const griot_per_fd_data_map_entry *)hashmap_get(griot_model.per_fd_data, &(griot_per_fd_data_map_entry){.fd_hash=fd}))->data;
        }else{
            per_fd_data = map_entry->data;
        }
    }

    // (3) Compute the new context
    per_fd_data->context.context[per_fd_data->context.index] = call_stack;
    per_fd_data->context.index += 1;
    if(per_fd_data->context.index>=per_fd_data->context.context_size) per_fd_data->context.index = 0;    
    uint64_t ordered_context[per_fd_data->context.context_size];
    for(int i=per_fd_data->context.index; i<per_fd_data->context.context_size; i++){ ordered_context[i-per_fd_data->context.index]=per_fd_data->context.context[i]; }
    for(int i=0; i<per_fd_data->context.index; i++){ ordered_context[per_fd_data->context.context_size-per_fd_data->context.index+i]=per_fd_data->context.context[i]; }
    per_fd_data->context.context_hash = MurmurHash64A(ordered_context, per_fd_data->context.context_size*sizeof(unsigned long), GRIOT_SEED);

    // (?) Debug
    #ifdef GRIOT_DEBUG_VERBOSE
    INFO("New context hash: %lu, predicted: %lu\n", per_fd_data->context.context_hash%0xFFFFFF, per_fd_data->mru_prediction%0xFFFFFF);
    #endif

    // (4) Check if the previously made prediction was right. If it was, increment the stats again
    if(per_fd_data->mru_prediction == per_fd_data->context.context_hash || (per_fd_data->mfu_prediction == 0 && per_fd_data->previous_call_stack == call_stack)){
        griot_results.mru_correct_prediction_count+=1;
        griot_results.mru_correct_prediction_volume+=length;
        griot_results.mru_correct_prediction_io_time+=duration_ns;
    }
    if(per_fd_data->mfu_prediction == per_fd_data->context.context_hash || (per_fd_data->mfu_prediction == 0 && per_fd_data->previous_call_stack == call_stack)){
        griot_results.mfu_correct_prediction_count+=1;
        griot_results.mfu_correct_prediction_volume+=length;
        griot_results.mfu_correct_prediction_io_time+=duration_ns;
    }

    // (5) Update the information of the previous node
    if(per_fd_data->previous_pred_data!=NULL)
    {
        // For MRU, it's easy
        per_fd_data->previous_pred_data->mru_context_hash = per_fd_data->context.context_hash;

        // For MFU, it's harder. Either the node's pred data contains the new context hash and it's just an increment...
        bool found = false;
        for(int i = 0; i<per_fd_data->previous_pred_data->mfu_lists_length; i++){
            if(per_fd_data->previous_pred_data->mfu_context_hash_list[i]==per_fd_data->context.context_hash){
                per_fd_data->previous_pred_data->mfu_weight_list[i]+=1;
                found = true;
                break;
            }
        }

        // ... or it doesn't and we must resize it.
        if(!found){
            // incrementing the length
            per_fd_data->previous_pred_data->mfu_lists_length+=1;
            
            // new context hash
            per_fd_data->previous_pred_data->mfu_context_hash_list = realloc(per_fd_data->previous_pred_data->mfu_context_hash_list,
                sizeof(uint64_t)*per_fd_data->previous_pred_data->mfu_lists_length);
            per_fd_data->previous_pred_data->mfu_context_hash_list[per_fd_data->previous_pred_data->mfu_lists_length-1] = per_fd_data->context.context_hash;

            // new weight
            per_fd_data->previous_pred_data->mfu_weight_list = realloc(per_fd_data->previous_pred_data->mfu_weight_list,
                sizeof(uint64_t)*per_fd_data->previous_pred_data->mfu_lists_length);
            per_fd_data->previous_pred_data->mfu_weight_list[per_fd_data->previous_pred_data->mfu_lists_length-1] = 1;
        }
    }

    // (6) Make a new prediction using the prediction table, eventually creating an entry for the new context value
    griot_prediction_data *pred_data;
    {
        const griot_prediction_table_map_entry *map_entry = hashmap_get(per_fd_data->prediction_table, &(griot_prediction_table_map_entry){.call_stack_hash=per_fd_data->context.context_hash});
        if(map_entry==NULL){
            // If there is no map entry for this context, let's create it. We make our prediction using our default heuristic.
            pred_data = (griot_prediction_data *)malloc(sizeof(griot_prediction_data));
            memset(pred_data, 0, sizeof(griot_prediction_data));
            hashmap_set(per_fd_data->prediction_table, &(griot_prediction_table_map_entry){.call_stack_hash=per_fd_data->context.context_hash, .data=pred_data});
            pred_data->mru_context_hash = per_fd_data->context.context_hash;
        }else{
            // If there is a map entry already, making our prediction is easy.
            pred_data = map_entry->data;
        }
    }

    // MRU
    per_fd_data->mru_prediction=pred_data->mru_context_hash;

    // MFU
    // If we have no data, use fallback heuristics. Else, iterate over the weights, and keep the highest frequency context.
    if(pred_data->mfu_lists_length==0){
        per_fd_data->mfu_prediction=pred_data->mru_context_hash;
    }else{
        uint64_t best = 0;
        uint64_t min_weight = 0; // starting weight is 1 for our implementation of MFU
        for(int i = 0; i<pred_data->mfu_lists_length; i++){
            if(pred_data->mfu_weight_list[i]>min_weight){
                min_weight = pred_data->mfu_weight_list[i];
                best = pred_data->mfu_context_hash_list[i];
            }
        }
        per_fd_data->mfu_prediction = best;
    }

    // Fallback heuristic
    per_fd_data->previous_call_stack = call_stack;

    // (7) Setting the new "previous pred data"
    per_fd_data->previous_pred_data = pred_data;

    // (8) Updating timers
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dt_ns = (double)(t1.tv_sec - t0.tv_sec) * 1.0e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    griot_results.model_prediction_time += dt_ns;

    // (9) ...
    if(op_type==GRIOT_CLOSE) on_close(timestamp, thread_id, fd);
}

/**
 * Called by GrIOt tracer in child processes in order to avoid counting any I/O more than once
 */
void griot_results_reset()
{
    memset(&griot_results, 0, sizeof(griot_results));
}

/**
 * Called by GrIOt tracer at the end of a process in order to print the results
 */
void griot_results_dump(FILE *file)
{
    // Updating memory footprint stat
    uint64_t memory_footprint = griot_get_memory_footprint();
    if(memory_footprint>griot_results.highest_recorded_memory_footprint)griot_results.highest_recorded_memory_footprint=memory_footprint;

    // Dumping...
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    uint64_t app_duration_ns = (double)(current_time.tv_sec - griot_results.app_start.tv_sec) * 1.0e9 + (double)(current_time.tv_nsec - griot_results.app_start.tv_nsec); 
    
    iolib_safe_fprintf(file, "context_size=%u\ncall_stack_depth=%u\ngranularity=%s\noverall_app_duration=%lu\nio_time_ns=%lu\nio_count=%lu\nio_volume=%lu\nread_volume=%lu\nwrite_volume=%lu\nmru_correct_prediction_count=%lu\n"
            "mru_correct_prediction_volume=%lu\nmru_correct_prediction_io_time=%lu\nmfu_correct_prediction_count=%lu\nmfu_correct_prediction_volume=%lu\nmfu_correct_prediction_io_time=%lu\n"
            "call_stack_instrumentation_count=%lu\ncall_stack_instrumentation_time_ns=%lu\nmodel_prediction_time_ns=%lu\nmodel_memory_footprint=%lu\n",
            context_size,
            call_stack_depth,
            MODULE_NAME,
            app_duration_ns,
            griot_results.io_time,
            griot_results.io_count,
            griot_results.read_volume+griot_results.write_volume,
            griot_results.read_volume,
            griot_results.write_volume,
            griot_results.mru_correct_prediction_count,
            griot_results.mru_correct_prediction_volume,
            griot_results.mru_correct_prediction_io_time,
            griot_results.mfu_correct_prediction_count,
            griot_results.mfu_correct_prediction_volume,
            griot_results.mfu_correct_prediction_io_time,
            griot_results.call_stack_instrumentation_count,
            griot_results.call_stack_instrumentation_time,
            griot_results.model_prediction_time,
            griot_results.highest_recorded_memory_footprint);
    fflush(file);
}

// ######################

static uint64_t griot_hashmap_hash(const void *pred_data, uint64_t seed0, uint64_t seed1)
{
    const griot_prediction_table_map_entry *data = pred_data;
    return data->call_stack_hash;
}

static int griot_hashmap_compare(const void *pred_data_1, const void *pred_data_2, void *udata)
{
    const griot_prediction_table_map_entry *data_1 = pred_data_1;
    const griot_prediction_table_map_entry *data_2 = pred_data_2;
    return data_1->call_stack_hash==data_2->call_stack_hash?0:(data_1->call_stack_hash>data_2->call_stack_hash?1:-1);
}

static void griot_prediction_table_free(void *pred_data)
{
    const griot_prediction_table_map_entry *data = pred_data;
    free(data->data->mfu_context_hash_list);
    free(data->data->mfu_weight_list);
    free(data->data);
}

static void griot_per_fd_data_free(void *pred_data)
{
    const griot_per_fd_data_map_entry *data = pred_data;
    hashmap_free(data->data->prediction_table);
    free(data->data);
}

static uint64_t griot_get_memory_footprint()
{
    uint64_t size = 0;

    // griot model
    size += sizeof(griot_model);

    // iterating over the per fd data
    size_t iter = 0;
    void *item;
    while (hashmap_iter(griot_model.per_fd_data, &iter, &item)) {
        const griot_per_fd_data_map_entry *map_entry = item;
        size += sizeof(griot_per_fd_data_map_entry);
        size += sizeof(griot_per_fd_data);
        size += (sizeof(griot_prediction_table_map_entry)+sizeof(griot_prediction_data))*hashmap_count(map_entry->data->prediction_table);
    }

    return size;
}