#pragma once

/************************
 * GrIOt compilation time parameters
 */

/** Name of the iolib module */
#define MODULE_NAME "griot-per-open-hash"

/** Seed for the murmur hash function */
#define GRIOT_SEED 12345678

/** Whether or not GrIOt should ignore files in direct mode */
#define GRIOT_IGNORE_DIRECT_MODE_FILES false

/** The name of the login node that should be ignored by GrIOt */
#define GRIOT_IGNORE_NODE "kiwi0"
#define GRIOT_IGNORE_NODE_STRLEN (6)

#undef GRIOT_DEBUG
#undef GRIOT_DEBUG_VERBOSE

/*******************************
 * GrIOt environment parameters
 * It ain't much but it's honest work /j
 */

/** Name of the environment variable used to change the default GrIOt output folder. */
#define GRIOT_ENV_DUMP_FOLDER "GRIOT_DUMP_FOLDER"
#define GRIOT_ENV_EXPERIMENT_NAME "GRIOT_EXPERIMENT_NAME"
#define GRIOT_ENV_CONTEXT_SIZE "GRIOT_CONTEXT_SIZE"
#define GRIOT_ENV_CALL_STACK_DEPTH "GRIOT_CALL_STACK_DEPTH"