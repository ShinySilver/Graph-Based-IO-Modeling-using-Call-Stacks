#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <libgen.h>

#include <iolib.h>
#include <iolib_locks.h>
#include <iolib_log.h>
#include <iolib_trace.h>
#include <iolib_hooks.h>
#include <iolib_module.h>
#include <stdatomic.h>

#include "backtrace.h"
#include "griot_model.h"
#include "griot_config.h"
#include "log.h"

static char *get_process_name();
static void get_dump_file_name(char *graph_dump_target, int array_size);
static void mkdir_recursive(const char *path);
static void initialize_trace_file();
static unsigned long iotracerNow();
static int thread_id();

/** Counters in order to produce a unique id for every thread and operation */
static _Atomic int thread_counter;
static __thread unsigned long long tid;

/** File handle for the log file that we don't want to trace*/
static FILE *target_trace_file;
static int target_fd;
static FILE *debug_trace_file = 0;
static int debug_fd = -1;

/** Mutex to safeguard fprintf output to trace*/
static struct iolib_lock mut = IOLIB_LOCK_INITIALIZER;

/** Default params when env variables are not set */
static unsigned int griot_context_size = 16;
static unsigned int griot_call_stack_depth = 16;

/** Variable used to store the target trace file path*/
static char base_dump_name[PATH_MAX];

/**
 * Griot Optimizer per-opened file data.
 *
 * @note this structure is protected by iolib_file->iof_lock.
 */
struct griot_file_metadata {
	bool srMustIgnore; /**< File can not be optimized by sro. Ignore it */
	int fd;
};

/** Allocate and Initialize Small Read Optimizer data
 * It is called by the iolib when a file is opened
 *
 * @param[out] _data the address of the allocated perf-file struct
 * @param[in] fd file descriptor
 * @param[in] on_open true if the per-file data allocation was triggered by an open()
 */
void griotInitFileHook(void *_data, int fd, bool on_open)
{
	struct griot_file_metadata *data = _data;
	memset(data, 0, sizeof(struct griot_file_metadata));
	data->fd = fd;

	/* Files opened in direct are not subject to small read optimization. */
	int flags = iolib_safe_fcntl(fd, F_GETFL);
	if (flags & O_DIRECT) {
		trace("DIRECT mode detected for fd=%d", fd);
		data->srMustIgnore = true;
	}
}

/**
 * Finalize and Free Small Read Optimizer data
 * It is called by the iolib when file is closed
 *
 * @param[in] _data sro data to finalize
 * @param[in] pathname path of the file associated with this data
 *
 */
void griotFiniFileHook(void  *_data, const char *pathname)
{
	
}


/**
 * @return size of the per-file private data for this module
 */
static int griotGetFileDataSize(void)
{
	return sizeof(struct griot_file_metadata);
}

/**
 * Initialize Griot Tracer component
 *
 * @return 0 upon successful completion, < 0 otherwise
 */
int griotInitializeTracer(int argc, char **argv, char **env)
{
	DISABLE_IOLIB();
	
	/* Optionally, ignore all processes on a given node, for example here the login node */
	char hostname[HOST_NAME_MAX];
	if(gethostname(hostname, HOST_NAME_MAX)>=0 && strncmp(hostname, GRIOT_IGNORE_NODE, GRIOT_IGNORE_NODE_STRLEN)==0) return -1;

	/* Initialize the backtrace table, and prepare the output file path */
	initialize_trace_file();
	iotracer_backtrace_table_init();

	/* Reading the context size from environment variable */
	char *context_size_str = getenv(GRIOT_ENV_CONTEXT_SIZE);
	if(context_size_str){
		long context_size = strtol(context_size_str, (char **)NULL, 10);
		if(context_size<=0){
			#ifdef GRIOT_ENABLE_DEBUG_LOG
			iolib_safe_fprintf(stderr, "[GrIOt] A negative, zero or invalid context size was passed to GrIOt. Default value \"%u\" will be used instead.", griot_context_size);
			#endif
		}else{
			griot_context_size = context_size>1024?1024:(unsigned int)context_size;
		}
	}

	/* Get the call stack depth from env */
	char *call_stack_depth_str = getenv(GRIOT_ENV_CALL_STACK_DEPTH);
	if(call_stack_depth_str){
		long call_stack_depth = strtol(call_stack_depth_str, (char **)NULL, 10);
		if(call_stack_depth<=0){
			#ifdef GRIOT_ENABLE_DEBUG_LOG
			iolib_safe_fprintf(stderr, "[GrIOt] A negative, zero or invalid call stack depth was passed to GrIOt. Default value \"%u\" will be used instead.", call_stack_depth);
			#endif
		}else{
			griot_call_stack_depth = (unsigned int)call_stack_depth;
		}
	}

	griot_init(griot_context_size, griot_call_stack_depth);

	iolib_module_set_label(MODULE_NAME, MODULE_NAME);
	iolib_module_set_as_accelerator(MODULE_NAME);

	ENABLE_IOLIB();

	return 0;
}

/**
 * Terminate Griot Tracer
 * It is called when iolib terminates
 *
 */
void griotTerminateTracer(void)
{
	griot_results_dump(target_trace_file);

	if(debug_trace_file != 0){
		iolib_safe_close(fileno(debug_trace_file));
		debug_trace_file = 0;
	}

	if(target_trace_file != 0){
		iolib_safe_close(fileno(target_trace_file));
		target_trace_file = 0;
	}

	griot_finalize();
}

/**
 * Fcntl postprocess routine.
 *
 * This function is called after a call to fcntl(F_SETFL). According to the O_DIRECT flag, it sets
 * or resets the srMustIgnore field of griot_file_metadata.
 *
 * iof_lock is held by the caller.
 *
 * @param[in] data sro data associated with this file
 * @param[in] fd file descriptor of the file in which the operation occurs
 * @param[in] flags fcntl flags parameter
 */
static void griotFcntlPostProcess(void *data, int fd, int flags)
{
	struct griot_file_metadata *srod = (struct griot_file_metadata *)data;

	if (flags & O_DIRECT) {
		trace("DIRECT mode detected for fd=%d", fd);
		srod->srMustIgnore = true;
	}
	else {
		if (srod->srMustIgnore) {
			trace("DIRECT mode is reset for fd=%d", fd);
			srod->srMustIgnore = false;
		} 
	}
}

/**
 * GrIOt Optimizer Read Hook - called from the 'files' iolib layer
 *
 * This function will update the GrIOt model, and then call the prefetch_read function.
 * 
 * @note we only consider creating a model, and not reusing it. small modification needed for reuse.
 *
 * @param[in] fd file descriptor of the file in which the operation occurs
 * @param[in] offset offset in the file for the operation
 * @param[in] length size of the operation in bytes
 * @param[out] pomd pomd for the current operation
 * @param[in,out] data optimizer data for this file
 *
 * @note some time in the future we'll probably want to add action_before in order to have I/O durations
 */
static void griotReadHook(void *_data, struct pomd *pomd, int fd, off_t offset, size_t length, struct iolib_etime *elapsed){

	// Get the file data through iolib if needed
	struct griot_file_metadata *data = (struct griot_file_metadata *) _data;
	if(data->srMustIgnore || fd==target_fd || (debug_fd!=-1 && fd==debug_fd)) return;

	iolib_mutex_lock(&mut);
	on_io(iotracerNow(), thread_id(), fd, offset, length, iolib_etime_elapsed_ns(elapsed), GRIOT_READ, debug_trace_file);
	iolib_mutex_unlock(&mut);
}

/**
 * See griotReadHook().
 */
static void griotWriteHook(void *_data, struct pomd *pomd, int fd, off_t offset, size_t length, struct iolib_etime *elapsed){

	// Get the file data through iolib if needed
	struct griot_file_metadata *data = (struct griot_file_metadata *) _data;
	if(data->srMustIgnore || fd==target_fd || (debug_fd!=-1 && fd==debug_fd)) return;

	iolib_mutex_lock(&mut);
	on_io(iotracerNow(), thread_id(), fd, offset, length, iolib_etime_elapsed_ns(elapsed), GRIOT_WRITE, debug_trace_file);
	iolib_mutex_unlock(&mut);

}

void griot_record_open_file(void *_data, const char *pathname, int fd, int flags, mode_t mode, struct iolib_etime *elapsed){
	struct griot_file_metadata *data = _data;
	if(data->srMustIgnore) return;

	iolib_mutex_lock(&mut);
	on_io(iotracerNow(), thread_id(), data->fd, 0ul, 0ul, 0ul, GRIOT_OPEN, debug_trace_file);
	iolib_mutex_unlock(&mut);
}

void griot_record_close_file(void * _data, int fd, struct iolib_etime *elapsed){
	struct griot_file_metadata *data = _data;
	if(data->srMustIgnore) return;

	iolib_mutex_lock(&mut);
	on_io(iotracerNow(), thread_id(), fd, 0ul, 0ul, 0ul, GRIOT_CLOSE, debug_trace_file);
	iolib_mutex_unlock(&mut);
}

/**
 * Follow Fork
 *
 * This function is called in the *child* process after a fork. We close the parent trace file & open a new one
 */
void iotracerFollowFork(){
	mut = (struct iolib_lock)IOLIB_LOCK_INITIALIZER;
	
	if(debug_trace_file != 0){
		iolib_safe_close(fileno(debug_trace_file));
		debug_trace_file = 0;
	}
	if(target_trace_file != 0){
		iolib_safe_close(fileno(target_trace_file));
		target_trace_file = 0;
	}
	initialize_trace_file();
	griot_results_reset();
}

struct iolib_module_ops module_operations = {
	.module_name               = MODULE_NAME,
	.init_module               = griotInitializeTracer,
	.pre_terminate_module      = griotTerminateTracer,
	.get_file_data_size        = griotGetFileDataSize,
	.analysis_after_read       = griotReadHook,
	.analysis_after_write      = griotWriteHook,
	.record_open_file 		   = griot_record_open_file,
	.record_close_file 		   = griot_record_close_file,
	.init_file_data            = griotInitFileHook,
	.terminate_file_data       = griotFiniFileHook,
	.fcntl_postprocess         = griotFcntlPostProcess,
	.follow_fork	 		   = iotracerFollowFork
};

//###############################

static char *get_process_name(){
	#if defined(_GNU_SOURCE)
	char *name =  program_invocation_name;
	#else
	char *name =  "?";
	#endif

	char *array = name;
	char *start = name;
	while(*array){
		if(*array=='/')
			start=array+1;
		//if(!isalpha(*array))
		//	*array = '#';
		array++;
	}
	return start;
}

static void mkdir_recursive(const char *path){
    char *subpath, *fullpath;
    
    fullpath = strdup(path);
    subpath = dirname(fullpath);
    if (strlen(subpath) > 1)
        mkdir_recursive(subpath);
    mkdir(path, 0777);
    free(fullpath);
}

static void get_dump_file_name(char *graph_dump_target, int array_size)
{
	char *griot_dump_folder_base = getenv(GRIOT_ENV_DUMP_FOLDER);
	char *griot_experiment_name = getenv(GRIOT_ENV_EXPERIMENT_NAME);
	if(griot_dump_folder_base!=NULL){
		if(snprintf(graph_dump_target, PATH_MAX-1, "%s/%s/%s/", griot_dump_folder_base, griot_experiment_name==NULL?"":griot_experiment_name, MODULE_NAME)<0){
			iolib_safe_fprintf(stderr, "[GrIOt] Model dump was enabled through GRIOT_ENV_ENABLE_DUMP and GRIOT_ENV_DUMP_FOLDER but the final path was too long. Giving up.\n");
			return;
		}
	}else{
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			if(snprintf(graph_dump_target, array_size-1, "%s/%s/", cwd, MODULE_NAME)<0){
				iolib_safe_fprintf(stderr, "[GrIOt] Model dump was enabled through GRIOT_ENV_ENABLE_DUMP but the final path was too long. Giving up.\n");
				return;
			}
		}
	}
	mkdir_recursive(graph_dump_target); // fails silently if folder already exists
}

static void initialize_trace_file()
{
	DISABLE_IOLIB();
	char hostname[HOST_NAME_MAX];
	if(gethostname(hostname, HOST_NAME_MAX)<0){
		iolib_safe_fprintf(stderr, "[GrIOt] Model dump was enabled through GRIOT_ENV_ENABLE_DUMP and GRIOT_ENV_DUMP_FOLDER but hostname was not found. Giving up.\n");
		return;
	}

	get_dump_file_name(base_dump_name, PATH_MAX);

	char griot_tracer_target_file[PATH_MAX];
	if(snprintf(griot_tracer_target_file, PATH_MAX, "%s/%s_%s_pid%d.csv", base_dump_name, hostname, get_process_name(), getpid())<0){
		iolib_safe_fprintf(stderr, "[GrIOt] Model dump was enabled but the dump path was too long. Giving up.\n");
		exit(-1);
	}

	target_trace_file = fopen(griot_tracer_target_file, "w");
	if(target_trace_file == 0){
			iolib_safe_fprintf(stderr, "iotracer initialization failed: trace target file at path \"%s\" could not be opened\n",
					griot_tracer_target_file);
			exit(-1);
	}

	#ifdef GRIOT_DEBUG_MODEL
	char griot_tracer_debug_file[PATH_MAX];
	if(snprintf(griot_tracer_debug_file, PATH_MAX, "%s/%s_%s_pid%d.debug", base_dump_name, hostname, get_process_name(), getpid())<0){
		iolib_safe_fprintf(stderr, "[GrIOt] Model dump was enabled but the dump path was too long. Giving up.\n");
		exit(-1);
	}

	debug_trace_file = fopen(griot_tracer_debug_file, "w");
	if(debug_trace_file == 0){
			iolib_safe_fprintf(stderr, "iotracer initialization failed: trace target file at path \"%s\" could not be opened\n",
					griot_tracer_debug_file);
			exit(-1);
	}
	#endif

	//setvbuf(target_trace_file, NULL, _IONBF, 0);
	target_fd = fileno(target_trace_file);
	#ifdef GRIOT_DEBUG_MODEL
	debug_fd = fileno(debug_trace_file);
	#endif
	ENABLE_IOLIB();
}

static int thread_id(){
	if(tid==0){
		tid = ++thread_counter;
	}
	return tid;
}

/**
 * @return current time in milliseconds
 */
static unsigned long iotracerNow(){
	struct timeval ts;
	gettimeofday(&ts, NULL);
	return ts.tv_sec * 1000 + ts.tv_usec / 1000;
}