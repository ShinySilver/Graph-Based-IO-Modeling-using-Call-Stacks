#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <execinfo.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdio.h>

#include "backtrace.h"
#include "griot_config.h"
#include "log.h"
//#include "iolib_locks.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>

/** Utility for hash function */
#define BIG_CONSTANT(x) (x##LLU)

/** The path to the maps file for relative backtrace extraction */
#ifndef MAPS_FILE /* this macro happens to  be defined when in unit tests */
  #define MAPS_FILE "/proc/self/maps"
#endif

/**
 * This struct represents the address range of an executable mapping in the current address space
 * These structs are stored in a simple linked list. That list should be short, but in the future
 * it might be a good idea to switch to a binary tree instead.
 */
struct lib_addr_range {
        unsigned long lr_start;
        unsigned long lr_end;
        struct lib_addr_range *lr_next;
};

/**
 * The linked list of memory ranges.
 * Updated atomically on library load and on dlopen()s
 */
static struct lib_addr_range *lib_addr_ranges;

/**
 * Protection for lib_addr_ranges
 * dlopen() takes it as a writer
 * backtrace users take it as a reader
 */
//pthread_mutex_t addr_ranges_lock;

/**
 * MurmurHash2, 64-bit versions, by Austin Appleby
 * The same caveats as 32-bit MurmurHash2 apply here - beware of alignment
 * and endian-ness issues if used across multiple platforms.
 *
 * @return 64-bit hash for 64-bit platforms
 */
u_int64_t MurmurHash64A(const void *key, int len, u_int64_t seed)
{ 
        const u_int64_t m = BIG_CONSTANT(0xc6a4a7935bd1e995);
        const int r = 47;

        u_int64_t h = seed ^ (len * m);

        const u_int64_t *data = (const u_int64_t *) key;
        const u_int64_t *end = data + (len / 8);

        while (data != end) {
                u_int64_t k = *data++;
                k *= m; k ^= k >> r; k *= m; h ^= k; h *= m;
        }

        const unsigned char *data2 = (const unsigned char *) data;

        switch (len & 7) {
        case 7: h ^= (u_int64_t)(data2[6]) << 48;
        case 6: h ^= (u_int64_t)(data2[5]) << 40;
        case 5: h ^= (u_int64_t)(data2[4]) << 32;
        case 4: h ^= (u_int64_t)(data2[3]) << 24;
        case 3: h ^= (u_int64_t)(data2[2]) << 16;
        case 2: h ^= (u_int64_t)(data2[1]) << 8;
        case 1: h ^= (u_int64_t)(data2[0]); h *= m;
        };

        h ^= h >> r; h *= m; h ^= h >> r;
        return h;
}

/**
 * Return the offset of an address relative to the library it belongs to.
 * For this, find the address range where this address fits and return the offset relative to the start.
 * Return 0 if not found.
 *
 * @note addr_ranges_lock must be read-locked
 */
static unsigned long get_lib_offset_for_addr(unsigned long addr)
{
        const struct lib_addr_range *r = lib_addr_ranges;
        while (r) {
                if ((r->lr_start <= addr) && (r->lr_end > addr))
                        return addr - r->lr_start;
                r = r->lr_next;
        }
        //iolib_safe_fprintf(stderr, "[GrIOt] Warning, address %lx not found\n", addr);
        return 0;
}

int fast_backtrace (void **array, int size)
{
        //iolib_mutex_unlock(&iotracer_lock);
        unw_cursor_t cursor;
        unw_context_t context;
        // grab the machine context
        if (unw_getcontext(&context) < 0){
                iolib_safe_fprintf(stderr, "[GrIOt] Cannot get local machine state. Program will exit.\n");
                exit(1);
        }

        // initialize the cursor
        if (unw_init_local(&cursor, &context) < 0){
                iolib_safe_fprintf(stderr, "[GrIOt] Cannot initialize cursor for local unwinding. Program will exit.\n");
                exit(1);
        }

        // currently the IP is within backtrace() itself. We are not skipping it.
        int i = 0;
        for(; i<size; i++){
                unw_word_t pc; // uintptr_t

                // Reading the instruction pointer....
                if (unw_get_reg(&cursor, UNW_REG_IP, &pc)){
                    iolib_safe_fprintf(stderr, "ERROR: cannot read program counter\n");
                    exit(1);
                }
        
                // Putting it into the array
                array[i] = (void *) pc;

                // Returning
                if(unw_step(&cursor) <= 0) break;
        }
    //iolib_mutex_lock(&iotracer_lock);
    return i;
}

/**
 * Get a hash for the current backtrace.
 */
unsigned long long get_hash_for_current_backtrace(unsigned int call_stack_depth)
{
        unsigned long addrs[call_stack_depth];
        int n = fast_backtrace((void **)addrs, call_stack_depth);
        int i;

        /* Make all addresses relative to the start of their lib */
        //pthread_mutex_lock(&addr_ranges_lock);
        for (i = 0; i < n; i++)
                addrs[i] = get_lib_offset_for_addr(addrs[i]);
        //pthread_mutex_unlock(&addr_ranges_lock);

        return MurmurHash64A(addrs, n * sizeof(unsigned long), GRIOT_SEED);
}



/**
 * Add a single address range to the list
 *
 * @param[in] start start address of the range
 * @param[in] end end address of the range. (Does not actually belong to the range)
 * @param[in] list_head pointer to the head of the list where to add the new ranges
 */
static void add_lib_addr_range(struct lib_addr_range **list_head, unsigned long start, unsigned long end)
{
        struct lib_addr_range *r = (struct lib_addr_range*)malloc(sizeof(*r));
        if (!r) {
                iolib_safe_fprintf(stderr, "*** Fatal: add_lib_addr_range: out of memory\n");
                exit(1);
        }
        r->lr_start = start;
        r->lr_end = end;
        r->lr_next = *list_head;
        *list_head = r;
}

/**
 * Build the lib address range list for the current process.
 * Question: WHEN should this be done ? Should we handle dlclose() ?
 *
 * @param[in] list_head pointer to the head of the list where to add the new ranges
 */
static void build_lib_addr_range_list(struct lib_addr_range **list_head)
{
        const int LINE = 1000;
        char line[LINE];

#ifdef IOTRACER_STDIO_HOOKS
        FILE *f = iotracer_safe_fopen(MAPS_FILE, "r");
#else
        FILE *f = fopen(MAPS_FILE, "r");
#endif
        char perms[5];
        unsigned long start, end;
        if(!f){
                iolib_safe_fprintf(stderr, "[fastio-iotracer] could not open %s\n", MAPS_FILE);
                exit(1);
        }
        for (;;) {
                #ifdef IOTRACER_STDIO_HOOKS
                if (!iotracer_safe_fgets(line, LINE, f))
                #else
                if (!fgets(line, LINE, f))
                #endif
                        break;
                if (3 != sscanf(line, "%lx-%lx %4s", &start, &end, perms))
                        break;
                if ('x' == perms[2])
                        add_lib_addr_range(list_head, start, end);
        }
#ifdef IOTRACER_STDIO_HOOKS
        iotracer_safe_fclose(f);
#else
        fclose(f);
#endif
}

/* Rebuild the list of address ranges.
 * May be called after dlopen(), and possibly dlclose(), but I don't see the point of that.
 */
void rebuild_lib_addr_range_list(void)
{
        struct lib_addr_range *new_head = NULL;
        struct lib_addr_range *oldlist = lib_addr_ranges;
        build_lib_addr_range_list(&new_head);
        /* switch to new list */
        lib_addr_ranges = new_head;

        /* Free old list */
        while (oldlist) {
                struct lib_addr_range *next = oldlist->lr_next;
                free(oldlist);
                oldlist = next;
        }
}

void export_backtrace_table(){
    char cwd[PATH_MAX];
    char trace_path[256];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
                if(snprintf(trace_path, PATH_MAX-1, "%s/backtrace_table_pid%d.dict", cwd, getpid())<0){
                        iolib_safe_fprintf(stderr, "fastio-iotracer trace file creation failed: target trace file path was too long\n");
                        exit(0);
                }
        }else{
                iolib_safe_fprintf(stderr, "fastio-iotracer trace file creation failed: current working directory could not be found\n");
                exit(0);
        }
        // TODO: implement backtrace table & exportation
    printf("[fastio-iotracer] callstack map file created at \"%s\" for process \"%s\"\n", trace_path, program_invocation_name);
}

#ifdef IOTRACER_DLOPEN_SUPPORT
void *dlopen(const char *filename, int flag){
        void *return_value = iotracer_safe_dlopen(filename, flag);
		
        pthread_mutex_lock(&addr_ranges_lock);
        rebuild_lib_addr_range_list();
        pthread_mutex_unlock(&addr_ranges_lock);

        return return_value;
}
#endif

void iotracer_backtrace_table_init(void){
	rebuild_lib_addr_range_list();
#ifdef IOTRACER_DLOPEN_SUPPORT
	if(iotracer_safe_dlopen==0) iotracer_safe_dlopen = dlsym(RTLD_NEXT, "dlopen");
    if(!iotracer_safe_dlopen){
        iolib_safe_fprintf(stderr, "fastio-iotracer failed to map dlopen symbol\n");
        exit(1);
    }
#endif
}