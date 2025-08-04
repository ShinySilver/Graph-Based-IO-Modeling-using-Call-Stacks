#include <string.h> // memset
#include <stdlib.h> // malloc
#include <time.h> // clock_gettime and CLOCK_MONOTONIC
#include "../shared/griot_model.h"
#include "../shared/hashmap.h"
#include "../shared/backtrace.h"
#include "../shared/log.h"
#include "griot_config.h"

/*
 * This file implements per-process I/O call stack prediction with GrIOt
 * We have a single hashmap<context, pred_data>, a result struct, as well as a struct storing
 * prediction and previous node pred_data.
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
} griot_results;

typedef struct
{
    // The context hash of the most recent next I/O
    uint64_t mru_context_hash;

    // one weight per outgoing edge. Used for MFU.
    uint64_t *mfu_context_hash_list;
    uint64_t *mfu_weight_list;
    uint64_t mfu_lists_length;
} griot_prediction_data;

struct
{
    // Host every prediction data
    hashmap *prediction_table;

    // Used in the fallback heuristic
    uint64_t previous_call_stack;

    // When an I/O arrive, the next I/O is predicted here
    uint64_t mru_prediction;
    uint64_t mfu_prediction;

    // The prediction data of the previous I/O is kept from one I/O to another so it can be updated
    griot_prediction_data *previous_pred_data;
} griot_model;

struct{
    uint64_t *context;
    uint64_t context_hash;
    unsigned int context_size;
    unsigned int call_stack_depth;
    int index;
} griot_context;

typedef struct
{
    griot_prediction_data *data;
    uint64_t call_stack_hash;
} griot_prediction_table_map_entry;

/**
 * Miscealenous function used in the prediction hashmap
 */
static uint64_t griot_hashmap_hash(const void *pred_data, uint64_t seed0, uint64_t seed1);
static int griot_hashmap_compare(const void *pred_data_1, const void *pred_data_2, void *udata);
static void griot_prediction_table_free(void *pred_data);

/**
 * Function used to compute the instantaneous memory footprint of griot
 */
static uint64_t griot_get_memory_footprint();

/**
 * Called by GrIOt tracer when a process is created
 */
void griot_init(uint32_t context_size, uint32_t call_stack_depth)
{
    memset(&griot_results, 0, sizeof(griot_results));
    griot_model.prediction_table = hashmap_new(sizeof(griot_prediction_table_map_entry), 0, 0, 0, griot_hashmap_hash,
        griot_hashmap_compare, griot_prediction_table_free, NULL);
    clock_gettime(CLOCK_MONOTONIC, &griot_results.app_start);

    memset(&griot_context, 0, sizeof(griot_context));
    griot_context.context = (uint64_t *)malloc(sizeof(uint64_t) * context_size);
    memset(griot_context.context, 0, sizeof(uint64_t) * context_size);
	griot_context.context_size = context_size;
    griot_context.call_stack_depth = call_stack_depth;
}

/**
 * Called by GrIOt tracer when a process is finished, just after printing the results
 */
void griot_finalize()
{
    hashmap_free(griot_model.prediction_table);
    free(griot_context.context);
}

/**
 * Called by GrIOt tracer when an I/O is intercepted
 */
void on_io(uint64_t timestamp, int32_t thread_id, int fd, off_t offset, size_t length, uint64_t duration_ns, op_type op_type, FILE *optional_debug_file)
{
    // (0) Ignore open/close. Only reads and writes are predicted.
    // if(op_type!=GRIOT_READ && op_type!=GRIOT_WRITE) return;

    // (0) Get the call stack
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t call_stack = get_hash_for_current_backtrace(griot_context.call_stack_depth);
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

    // (2) Compute the new context
    griot_context.context[griot_context.index] = call_stack;
    griot_context.index += 1;
    if(griot_context.index>=griot_context.context_size) griot_context.index = 0;    
    uint64_t ordered_context[griot_context.context_size];
    for(int i=griot_context.index; i<griot_context.context_size; i++){ ordered_context[i-griot_context.index]=griot_context.context[i]; }
    for(int i=0; i<griot_context.index; i++){ ordered_context[griot_context.context_size-griot_context.index+i]=griot_context.context[i]; }
    griot_context.context_hash = MurmurHash64A(ordered_context, griot_context.context_size*sizeof(unsigned long), GRIOT_SEED);

    // (?) Debug
    #ifdef GRIOT_DEBUG_VERBOSE
    INFO("New context hash: %lu, predicted: %lu\n", griot_context.context_hash%0xFFFFFF, griot_model.mru_prediction%0xFFFFFF);
    //iolib_safe_fprintf(stderr, "context=[");
    //for(int i = 0; i<griot_context.context_size; i++){
    //    if(i!=0) iolib_safe_fprintf(stderr, ", ");
    //    iolib_safe_fprintf(stderr, "%lu", ordered_context[griot_context.context_size-i-1]%0xFFFF);
    //}
    //iolib_safe_fprintf(stderr, "]\n");
    #endif

    // (3) Check if the previously made prediction was right. If it was, increment the stats again
    if(griot_model.mru_prediction == griot_context.context_hash || (griot_model.mru_prediction == 0 && griot_model.previous_call_stack == call_stack)){
        griot_results.mru_correct_prediction_count+=1;
        griot_results.mru_correct_prediction_volume+=length;
        griot_results.mru_correct_prediction_io_time+=duration_ns;
    }
    if(griot_model.mfu_prediction == griot_context.context_hash || (griot_model.mfu_prediction == 0 && griot_model.previous_call_stack == call_stack)){
        griot_results.mfu_correct_prediction_count+=1;
        griot_results.mfu_correct_prediction_volume+=length;
        griot_results.mfu_correct_prediction_io_time+=duration_ns;
    }

    // (4) Update the information of the previous node
    if(griot_model.previous_pred_data!=NULL){

        // For MRU, it's easy
        griot_model.previous_pred_data->mru_context_hash = griot_context.context_hash;

        // For MFU, it's harder. Either the node's pred data contains the new context hash and it's just an increment...
        bool found = false;
        for(int i = 0; i<griot_model.previous_pred_data->mfu_lists_length; i++){
            if(griot_model.previous_pred_data->mfu_context_hash_list[i]==griot_context.context_hash){
                griot_model.previous_pred_data->mfu_weight_list[i]+=1;
                found = true;
                break;
            }
        }

        // ... or it doesn't and we must resize it.
        if(!found){
            // incrementing the length
            griot_model.previous_pred_data->mfu_lists_length+=1;
            
            // new context hash
            griot_model.previous_pred_data->mfu_context_hash_list = realloc(griot_model.previous_pred_data->mfu_context_hash_list,
                sizeof(uint64_t)*griot_model.previous_pred_data->mfu_lists_length);
            griot_model.previous_pred_data->mfu_context_hash_list[griot_model.previous_pred_data->mfu_lists_length-1] = griot_context.context_hash;

            // new weight
            griot_model.previous_pred_data->mfu_weight_list = realloc(griot_model.previous_pred_data->mfu_weight_list,
                sizeof(uint64_t)*griot_model.previous_pred_data->mfu_lists_length);
            griot_model.previous_pred_data->mfu_weight_list[griot_model.previous_pred_data->mfu_lists_length-1] = 1;
        }
    }

    // (5) Make a new prediction using the prediction table, eventually creating an entry for the new context value
    const griot_prediction_table_map_entry *map_entry = hashmap_get(griot_model.prediction_table, &(griot_prediction_table_map_entry){.call_stack_hash=griot_context.context_hash});
    griot_prediction_data *pred_data;
    if(map_entry==NULL){
        // If there is no map entry for this context, let's create it. We make our prediction using our default heuristic.
        pred_data = (griot_prediction_data *)malloc(sizeof(griot_prediction_data));
        memset(pred_data, 0, sizeof(griot_prediction_data));
        hashmap_set(griot_model.prediction_table, &(griot_prediction_table_map_entry){.call_stack_hash=griot_context.context_hash, .data=pred_data});
        // pred_data->mru_context_hash = griot_context.context_hash;
    }else{
        // If there is a map entry already, use it.
        pred_data = map_entry->data;
    }

    // MRU
    griot_model.mru_prediction=pred_data->mru_context_hash;

    // MFU
    // If we have no data, use fallback heuristic. Else, iterate over the weights, and keep the highest frequency context.
    if(pred_data->mfu_lists_length==0){
        griot_model.mfu_prediction=pred_data->mru_context_hash;
    }else{
        uint64_t best = 0;
        uint64_t min_weight = 0; // starting weight is 1 for our implementation of MFU
        for(int i = 0; i<pred_data->mfu_lists_length; i++){
            if(pred_data->mfu_weight_list[i]>min_weight){
                min_weight = pred_data->mfu_weight_list[i];
                best = pred_data->mfu_context_hash_list[i];
            }
        }
        griot_model.mfu_prediction = best;
    }

    // Fallback heuristic
    griot_model.previous_call_stack = call_stack;

    // (optional) Debug logs
    if(optional_debug_file){
        iolib_safe_fprintf(optional_debug_file, "timestamp=%lu, io_call_stack=%lu, io_context=%lu, mru_next_context=%lu, mfu_next_context=%lu\n", timestamp, call_stack, griot_context.context_hash, griot_model.mru_prediction, griot_model.mfu_prediction);
    }

    // (6) Setting the new "previous pred data"
    griot_model.previous_pred_data = pred_data;

    // (7) Updating timers
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dt_ns = (double)(t1.tv_sec - t0.tv_sec) * 1.0e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    griot_results.model_prediction_time += dt_ns;
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
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    uint64_t app_duration_ns = (double)(current_time.tv_sec - griot_results.app_start.tv_sec) * 1.0e9 + (double)(current_time.tv_nsec - griot_results.app_start.tv_nsec); 
    
    iolib_safe_fprintf(file, "context_size=%u\ncall_stack_depth=%d\ngranularity=%s\noverall_app_duration=%lu\nio_time_ns=%lu\nio_count=%lu\nio_volume=%lu\nread_volume=%lu\nwrite_volume=%lu\nmru_correct_prediction_count=%lu\n"
            "mru_correct_prediction_volume=%lu\nmru_correct_prediction_io_time=%lu\nmfu_correct_prediction_count=%lu\nmfu_correct_prediction_volume=%lu\nmfu_correct_prediction_io_time=%lu\n"
            "call_stack_instrumentation_count=%lu\ncall_stack_instrumentation_time_ns=%lu\nmodel_prediction_time_ns=%lu\nmodel_memory_footprint=%lu\n",
            griot_context.context_size,
            griot_context.call_stack_depth,
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
            griot_get_memory_footprint());
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

static uint64_t griot_get_memory_footprint()
{
    // size of griot context
    uint64_t size = sizeof(griot_context) +sizeof(uint64_t)*griot_context.context_size;

    // size of griot model
    size += sizeof(griot_model) + (sizeof(griot_prediction_table_map_entry)+sizeof(griot_prediction_data))*hashmap_count(griot_model.prediction_table);

    return size;
}