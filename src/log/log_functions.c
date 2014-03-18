/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 *
 * All the display functions and error handling.
 *
 */
#include "config.h"

#include <stdlib.h>		/* for malloc */
#include <ctype.h>		/* for isdigit */
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <libgen.h>
#include <execinfo.h>
#include <sys/resource.h>

#include "log.h"
#include "rpc/rpc.h"
#include "common_utils.h"
#include "abstract_mem.h"

#ifdef USE_DBUS
#include "ganesha_dbus.h"
#endif

#include "nfs_core.h"
#include "config_parsing.h"

pthread_rwlock_t log_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* Variables to control log fields */

/**
 * @brief Define an index each of the log fields that are configurable.
 *
 * Ganesha log messages have several "header" fields used in every
 * message. Some of those fields may be configured (mostly display or
 * not display).
 *
 */
enum log_flag_index_t {
	LF_DATE,		/*< Date field. */
	LF_TIME,		/*< Time field. */
	LF_EPOCH,		/*< Server Epoch field (distinguishes server
				    instance. */
	LF_HOSTAME,		/*< Server host name field. */
	LF_PROGNAME,		/*< Ganesha program name field. */
	LF_PID,			/*< Ganesha process identifier. */
	LF_THREAD_NAME,		/*< Name of active thread logging message. */
	LF_FILE_NAME,		/*< Source file name message occured in. */
	LF_LINE_NUM,		/*< Source line number message occurred in. */
	LF_FUNCTION_NAME,	/*< Function name message occurred in. */
	LF_COMPONENT,		/*< Log component. */
	LF_LEVEL,		/*< Log level. */
};

/**
 * @brief Description of a flag tha controls a log field.
 *
 */
struct log_flag {
	int lf_idx;		/*< The log field index this flag controls. */
	bool_t lf_val;		/*< True/False value for the flag. */
	int lf_ext;		/*< Extended value for the flag,
				    if it has one. */
	char *lf_name;		/*< Name of the flag. */
};

/**
 * @brief Define a set of possible time and date formats.
 *
 * These values will be stored in lf_ext for the LF_DATE and LF_TIME flags.
 *
 */
enum timedate_formats_t {
	TD_NONE,		/*< No time/date. */
	TD_GANESHA,		/*< Legacy Ganesha time and date format. */
	TD_LOCAL,		/*< Use strftime local format for time/date. */
	TD_8601,		/*< Use ISO 8601 time/date format. */
	TD_SYSLOG,		/*< Use a typical syslog time/date format. */
	TD_SYSLOG_USEC,		/*< Use a typical syslog time/date format that
				    also includes microseconds. */
	TD_USER,		/* Use a user defined time/date format. */
};

/**
 * @brief Format control for log messages
 *
 */

struct logfields {
	bool disp_epoch;
	bool disp_host;
	bool disp_prog;
	bool disp_pid;
	bool disp_threadname;
	bool disp_filename;
	bool disp_linenum;
	bool disp_funct;
	bool disp_comp;
	bool disp_level;
	enum timedate_formats_t datefmt;
	enum timedate_formats_t timefmt;
	char *user_date_fmt;
	char *user_time_fmt;
};

/**
 * @brief Startup default log message format
 *
 * Baked in here so early startup has something to work with
 */

static struct logfields logfields = {
	.disp_epoch = true,
	.disp_host = true,
	.disp_prog = true,
	.disp_pid = true,
	.disp_threadname = true,
	.disp_filename = false,
	.disp_linenum = false,
	.disp_funct = true,
	.disp_comp = true,
	.disp_level = true,
	.datefmt = TD_GANESHA,
	.timefmt = TD_GANESHA
};
	
/* Define the maximum length of a user time/date format. */
#define MAX_TD_USER_LEN 64
/* Define the maximum overall time/date format length, should have room for both
 * user date and user time format plus room for blanks around them.
 */
#define MAX_TD_FMT_LEN (MAX_TD_USER_LEN * 2 + 4)

/**
 * @brief Actually define the flag variables with their default values.
 *
 * Flags that don't have an extended value will have lf_ext initialized to 0.
 *
 */
struct log_flag tab_log_flag[] = {
/*                          Extended
 * Index             Flag   Value       Name
 */
	{LF_DATE, TRUE, TD_GANESHA, "DATE"},
	{LF_TIME, TRUE, TD_GANESHA, "TIME"},
	{LF_EPOCH, TRUE, 0, "EPOCH"},
	{LF_HOSTAME, TRUE, 0, "HOSTNAME"},
	{LF_PROGNAME, TRUE, 0, "PROGNAME"},
	{LF_PID, TRUE, 0, "PID"},
	{LF_THREAD_NAME, TRUE, 0, "THREAD_NAME"},
	{LF_FILE_NAME, FALSE, 0, "FILE_NAME"},
	{LF_LINE_NUM, FALSE, 0, "LINE_NUM"},
	{LF_FUNCTION_NAME, TRUE, 0, "FUNCTION_NAME"},
	{LF_COMPONENT, TRUE, 0, "COMPONENT"},
	{LF_LEVEL, TRUE, 0, "LEVEL"},
};

static int log_to_syslog(struct log_facility *facility, log_levels_t level,
			 struct display_buffer *buffer, char *compstr,
			 char *message);

static int log_to_file(struct log_facility *facility, log_levels_t level,
		       struct display_buffer *buffer, char *compstr,
		       char *message);

static int log_to_stream(struct log_facility *facility, log_levels_t level,
			 struct display_buffer *buffer, char *compstr,
			 char *message);

/**
 * @brief Define the standard log facilities.
 *
 */
struct log_facility facilities[] = {
	{{NULL, NULL}, {NULL, NULL},
	 "SYSLOG", NIV_FULL_DEBUG,
	 LH_COMPONENT, log_to_syslog, NULL},
	{{NULL, NULL}, {NULL, NULL},
	 "FILE", NIV_FULL_DEBUG,
	 LH_ALL, log_to_file, "/var/log/ganesha"},
	{{NULL, NULL}, {NULL, NULL},
	 "STDERR", NIV_FULL_DEBUG,
	 LH_ALL, log_to_stream, NULL},
	{{NULL, NULL}, {NULL, NULL},
	 "STDOUT", NIV_FULL_DEBUG,
	 LH_ALL, log_to_stream, NULL},
	{{NULL, NULL}, {NULL, NULL},
	 "TEST", NIV_FULL_DEBUG,
	 LH_NONE, log_to_stream, NULL},
};

struct glist_head facility_list;
struct glist_head active_facility_list;

struct log_facility *default_facility = &facilities[SYSLOG];

log_header_t max_headers = LH_COMPONENT;

/**
 *
 * @brief Test if facility is active
 *
 * @param[in]  facility The facility to test
 *
 * @retval 1 if active
 * @retval 0 if not active
 *
 */
static inline int is_facility_active(struct log_facility *facility)
{
	return !glist_null(&facility->lf_active);
}

/**
 *
 * @brief Test if facility is registered
 *
 * @param[in]  facility The facility to test
 *
 * @retval 1 if registered
 * @retval 0 if not registered
 *
 */
static inline int is_facility_registered(struct log_facility *facility)
{
	return !glist_null(&facility->lf_list);
}

/**
 *
 * @brief Deactivate a facility
 *
 * Caller must hold log_rwlock in write mode.
 *
 * @param[in]  facility The facility to deactivate
 *
 */
void _deactivate_log_facility(struct log_facility *facility)
{
	if (!is_facility_active(facility))
		return;

	glist_del(&facility->lf_active);

	/* If this facility needed the headers, we need to walk the remaining
	 * facilities to determine if any still need headers.
	 */
	if (facility->lf_headers == max_headers) {
		struct glist_head *glist;
		struct log_facility *found;

		max_headers = LH_NONE;

		glist_for_each(glist, &active_facility_list) {
			found =
			    glist_entry(glist, struct log_facility, lf_active);

			if (found->lf_headers > max_headers)
				max_headers = found->lf_headers;
		}
	}
}

/**
 *
 * @brief Deactivate a facility
 *
 * Takes log_rwlock to be held for write.
 *
 * @param[in]  facility The facility to deactivate
 *
 */
void deactivate_log_facility(struct log_facility *facility)
{
	pthread_rwlock_wrlock(&log_rwlock);
	_deactivate_log_facility(facility);
	pthread_rwlock_unlock(&log_rwlock);
}

/**
 *
 * @brief Activate a facility
 *
 * Requires log_rwlock for write.
 *
 * @param[in]  facility The facility to activate
 *
 */
void _activate_log_facility(struct log_facility *facility)
{
	if (!is_facility_active(facility)) {
		glist_add_tail(&active_facility_list, &facility->lf_active);

		if (facility->lf_headers > max_headers)
			max_headers = facility->lf_headers;
	}
}

/**
 *
 * @brief Activate a facility
 *
 * Takes log_rwlock for write.
 *
 * @param[in]  facility The facility to activate
 *
 */
void activate_log_facility(struct log_facility *facility)
{
	pthread_rwlock_wrlock(&log_rwlock);
	_activate_log_facility(facility);
	pthread_rwlock_unlock(&log_rwlock);
}

/**
 *
 * @brief Register an additional log facility
 *
 * This function allows registering additional log facilities. The facility
 * may already have been referenced by the configuration by using the FACILITY
 * key to let the config know the facility is available, and the name of the
 * facility to provide a max log level for this facility (thus making the
 * facility active). The effects of the configuration will be transferred to
 * the newly registered facility.
 *
 * @param[in]  facility The facility to register.
 *
 * @retval -1 failure
 * @retval 0  success
 *
 */
int register_log_facility(struct log_facility *facility)
{
	struct log_facility *existing;

	pthread_rwlock_wrlock(&log_rwlock);

	existing = find_log_facility(facility->lf_name);

	if (existing != NULL) {
		/* Only allow re-registration of a facility
		 * created with FACILITY config key.
		 */
		if (existing->lf_func != NULL) {
			pthread_rwlock_unlock(&log_rwlock);

			LogMajor(COMPONENT_LOG,
				 "Attempt to re-register log facility %s",
				 existing->lf_name);

			return -1;
		}

		/* Copy the max logging level threshold from the
		 * existing facility into the registered facility.
		 */
		facility->lf_max_level = existing->lf_max_level;

		/* If existing facility was active, deactivate
		 * it and activate registered facility.
		 */
		if (is_facility_active(existing)) {
			_deactivate_log_facility(existing);
			_activate_log_facility(facility);
		}

		/* Finally, remove existing facility from list and free mem. */
		glist_del(&existing->lf_list);
		gsh_free(existing->lf_name);
		gsh_free(existing);
		existing = NULL;
	}

	glist_add_tail(&facility_list, &facility->lf_list);

	pthread_rwlock_unlock(&log_rwlock);

	LogInfo(COMPONENT_LOG, "Registered log facility %s", facility->lf_name);

	return 0;
}

/**
 *
 * @brief Unregister an additional log facility
 *
 * This function allows unregistering additional log facilities.
 *
 * @param[in]  facility The facility to unregister.
 *
 * @retval -1 failure
 * @retval 0  success
 *
 */
int unregister_log_facility(struct log_facility *facility)
{
	struct log_facility *existing;

	pthread_rwlock_wrlock(&log_rwlock);

	existing = find_log_facility(facility->lf_name);

	if (existing != facility) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG,
			 "Invalid attempt to un-register log facility %s",
			 facility->lf_name);

		return -1;
	} else if (!is_facility_registered(existing)) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG, "Log facility %s is not registered",
			 existing->lf_name);

		return -1;
	}

	/* Remove the facility from the list of registered facilities. */
	glist_del(&existing->lf_list);

	pthread_rwlock_unlock(&log_rwlock);

	LogInfo(COMPONENT_LOG, "Unregistered log facility %s",
		facility->lf_name);

	return 0;
}

/**
 *
 * @brief Create a dummy facility.
 *
 * This function is used when the FACILITY keyword is found in the config file.
 * It creates a facility with no logging function that serves as a place holder
 * to have a max log level specified and activating or deactivating the
 * facility.
 *
 * @param[in]  name The name of the facility to create.
 *
 * @retval NULL failure
 * @retval non-NULL the facility created (or the real facility
 *         if it already exists)
 *
 */
struct log_facility *create_null_facility(const char *name)
{
	struct log_facility *facility;

	pthread_rwlock_wrlock(&log_rwlock);

	facility = find_log_facility(name);

	if (facility != NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogInfo(COMPONENT_LOG, "Facility %s already exists", name);

		return facility;
	}

	facility = gsh_calloc(1, sizeof(*facility));

	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG, "Can not allocate a facility for %s",
			 name);

		return NULL;
	}

	facility->lf_name = gsh_strdup(name);

	if (facility->lf_name == NULL) {
		gsh_free(facility);

		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG,
			 "Can not allocate a facility name for %s", name);

		return NULL;
	}

	glist_add_tail(&facility_list, &facility->lf_list);

	pthread_rwlock_unlock(&log_rwlock);

	LogInfo(COMPONENT_LOG, "Registered NULL log facility %s",
		facility->lf_name);

	return facility;
}

/**
 *
 * @brief Registers a custom facility.
 *
 * This function is called by the FSAL when it wants to regsiter a callback
 * function for the custom facility. We had a dummy facility created earlier
 * when the config file was parsed. Now activate the custom facility
 *
 * @param[in]  name The name of the facility to create.
 *
 * @retval NULL failure
 * @retval non-NULL the facility created (or the real facility if it
 *         already exists)
 *
 */
int activate_custom_log_facility(struct log_facility *facility)
{
	struct log_facility *existing;

	pthread_rwlock_wrlock(&log_rwlock);

	existing = find_log_facility(facility->lf_name);

	/* Only allow activation of custom log facility
	 * created with FACILITY config key.
	 */
	if (existing == NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG,
			 "Attempt to activate non-existing custom log facility %s",
			 facility->lf_name);

		return -1;
	}

	/* Copy the max logging level threshold from the existing facility into
	 * the registered facility.
	 */
	facility->lf_max_level = existing->lf_max_level;

	/* If existing facility was active, deactivate
	 * it and activate registered facility.
	 */
	if (is_facility_active(existing)) {
		_deactivate_log_facility(existing);
		_activate_log_facility(facility);
	}

	/* Finally, remove existing facility from list and free memory. */
	glist_del(&existing->lf_list);
	gsh_free(existing->lf_name);
	gsh_free(existing);
	existing = NULL;

	glist_add_tail(&facility_list, &facility->lf_list);

	pthread_rwlock_unlock(&log_rwlock);

	LogInfo(COMPONENT_LOG, "Activated custom log facility %s",
		facility->lf_name);

	return 0;
}

char const_log_str[LOG_BUFF_LEN];
char date_time_fmt[MAX_TD_FMT_LEN];
char user_date_fmt[MAX_TD_USER_LEN];
char user_time_fmt[MAX_TD_USER_LEN];

/* La longueur d'une chaine */
#define MAX_NUM_FAMILY  50
#define UNUSED_SLOT -1

log_level_t tabLogLevel[] = {
	{NIV_NULL, "NIV_NULL", "NULL", LOG_NOTICE},
	{NIV_FATAL, "NIV_FATAL", "FATAL", LOG_CRIT},
	{NIV_MAJ, "NIV_MAJ", "MAJ", LOG_CRIT},
	{NIV_CRIT, "NIV_CRIT", "CRIT", LOG_ERR},
	{NIV_WARN, "NIV_WARN", "WARN", LOG_WARNING},
	{NIV_EVENT, "NIV_EVENT", "EVENT", LOG_NOTICE},
	{NIV_INFO, "NIV_INFO", "INFO", LOG_INFO},
	{NIV_DEBUG, "NIV_DEBUG", "DEBUG", LOG_DEBUG},
	{NIV_MID_DEBUG, "NIV_MID_DEBUG", "M_DBG", LOG_DEBUG},
	{NIV_FULL_DEBUG, "NIV_FULL_DEBUG", "F_DBG", LOG_DEBUG}
};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#endif

/* constants */
static int log_mask = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

/* Global variables */

static char program_name[1024];
static char hostname[256];
static int syslog_opened;

/*
 * Variables specifiques aux threads.
 */

struct thread_log_context {
	char *thread_name;
	struct display_buffer dspbuf;
	char buffer[LOG_BUFF_LEN + 1];

};

struct thread_log_context emergency_context = {
	"* log emergency *",
	{sizeof(emergency_context.buffer),
	 emergency_context.buffer,
	 emergency_context.buffer}
};

pthread_mutex_t emergency_mutex = PTHREAD_MUTEX_INITIALIZER;

/* threads keys */
static pthread_key_t thread_key;
static pthread_once_t once_key = PTHREAD_ONCE_INIT;

#define LogChanges(format, args...) \
	do { \
		if (LogComponents[COMPONENT_LOG].comp_log_level == \
		    NIV_FULL_DEBUG) \
			DisplayLogComponentLevel(COMPONENT_LOG, \
						 (char *) __FILE__, \
						 __LINE__, \
						 (char *) __func__, \
						 NIV_NULL, \
						 "LOG: " format, \
						 ## args); \
	} while (0)

cleanup_list_element *cleanup_list = NULL;

void RegisterCleanup(cleanup_list_element *clean)
{
	clean->next = cleanup_list;
	cleanup_list = clean;
}

void Cleanup(void)
{
	cleanup_list_element *c = cleanup_list;
	while (c != NULL) {
		c->clean();
		c = c->next;
	}
}

void Fatal(void)
{
	Cleanup();
	exit(2);
}

#ifdef _DONT_HAVE_LOCALTIME_R

/* Localtime is not reentrant...
 * So we are obliged to have a mutex for calling it.
 * pffff....
 */
static pthread_mutex_t mutex_localtime = PTHREAD_MUTEX_INITIALIZER;

/* thread-safe and PORTABLE version of localtime */

static struct tm *Localtime_r(const time_t *p_time, struct tm *p_tm)
{
	struct tm *p_tmp_tm;

	if (!p_tm) {
		errno = EFAULT;
		return NULL;
	}

	pthread_mutex_lock(&mutex_localtime);

	p_tmp_tm = localtime(p_time);

	/* copy the result */
	(*p_tm) = (*p_tmp_tm);

	pthread_mutex_unlock(&mutex_localtime);

	return p_tm;
}
#else
#define Localtime_r localtime_r
#endif

/* Init of pthread_keys */
static void init_keys(void)
{
	if (pthread_key_create(&thread_key, NULL) == -1)
		LogCrit(COMPONENT_LOG,
			"init_keys - pthread_key_create returned %d (%s)",
			errno, strerror(errno));
}				/* init_keys */

/**
 * GetThreadContext :
 * manages pthread_keys.
 */
static struct thread_log_context *Log_GetThreadContext(int ok_errors)
{
	struct thread_log_context *context;

	/* first, we init the keys if this is the first time */
	if (pthread_once(&once_key, init_keys) != 0) {
		if (ok_errors)
			LogCrit(COMPONENT_LOG_EMERG,
				"Log_GetThreadContext - pthread_once returned %d (%s)",
				errno, strerror(errno));
		return &emergency_context;
	}

	context = (struct thread_log_context *) pthread_getspecific(thread_key);

	/* we allocate the thread key if this is the first time */
	if (context == NULL) {
		/* allocates thread structure */
		context = gsh_malloc(sizeof(struct thread_log_context));

		if (context == NULL) {
			if (ok_errors)
				LogCrit(COMPONENT_LOG_EMERG,
					"Log_GetThreadContext - malloc returned %d (%s)",
					errno, strerror(errno));
			return &emergency_context;
		}

		/* inits thread structures */
		context->thread_name = emergency_context.thread_name;
		context->dspbuf.b_size = sizeof(context->buffer);
		context->dspbuf.b_start = context->buffer;
		context->dspbuf.b_current = context->buffer;

		/* set the specific value */
		pthread_setspecific(thread_key, context);

		if (ok_errors)
			LogFullDebug(COMPONENT_LOG_EMERG, "malloc => %p",
				     context);
	}

	return context;

}				/* Log_GetThreadContext */

static inline const char *Log_GetThreadFunction(int ok_errors)
{
	struct thread_log_context *context = Log_GetThreadContext(ok_errors);

	return context->thread_name;
}

/**
 * Log_FreeThreadContext
 *
 * Free context allocated whenever a log function is used in a thread.
 * This function should be called before a thread exits if log functions
 * have been used.
 */
void Log_FreeThreadContext()
{
	struct thread_log_context *context;

	/* Init the key if first time.
	 * Would if(once_key == PTHREAD_ONCE_INIT) be safe?
	 * (no race for thread variables) */
	if (pthread_once(&once_key, init_keys) != 0)
		return;

	context = (struct thread_log_context *) pthread_getspecific(thread_key);

	if (context) {
		pthread_setspecific(thread_key, NULL);
		gsh_free(context);
	}
}

/*
 * Convert a numeral log level in ascii to
 * the numeral value.
 */
int ReturnLevelAscii(const char *LevelInAscii)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(tabLogLevel); i++)
		if (!strcasecmp(tabLogLevel[i].str, LevelInAscii)
		    || !strcasecmp(tabLogLevel[i].str + 4, LevelInAscii)
		    || !strcasecmp(tabLogLevel[i].short_str, LevelInAscii))
			return tabLogLevel[i].value;

	/* If nothing found, return -1 */
	return -1;
}				/* ReturnLevelAscii */

static int ReturnComponentAscii(const char *ComponentInAscii)
{
	log_components_t component;

	for (component = COMPONENT_ALL; component < COMPONENT_COUNT;
	     component++) {
		if (!strcasecmp
		    (LogComponents[component].comp_name, ComponentInAscii)
		    || !strcasecmp(LogComponents[component].comp_name + 10,
				   ComponentInAscii)) {
			return component;
		}
	}

	return -1;
}

char *ReturnLevelInt(int level)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(tabLogLevel); i++)
		if (tabLogLevel[i].value == level)
			return tabLogLevel[i].str;

	/* If nothing is found, return NULL. */
	return NULL;
}				/* ReturnLevelInt */

/*
 * Set the name of this program.
 */
void SetNamePgm(const char *nom)
{

	/* This function isn't thread-safe because the name of the program
	 * is common among all the threads. */
	if (strmaxcpy(program_name, nom, sizeof(program_name)) == -1)
		LogFatal(COMPONENT_LOG, "Program name %s too long", nom);
}				/* SetNamePgm */

/*
 * Set the hostname.
 */
void SetNameHost(const char *name)
{
	if (strmaxcpy(hostname, name, sizeof(hostname)) == -1)
		LogFatal(COMPONENT_LOG, "Host name %s too long", name);
}				/* SetNameHost */

/* Set the function name in progress. */
void SetNameFunction(const char *nom)
{
	struct thread_log_context *context = Log_GetThreadContext(0);
	if (context == NULL)
		return;
	if (context->thread_name != emergency_context.thread_name)
		gsh_free(context->thread_name);
	context->thread_name = gsh_strdup(nom);
}				/* SetNameFunction */

/* Installs a signal handler */
static void ArmSignal(int signal, void (*action) ())
{
	struct sigaction act;

	/* Placing fields of struct sigaction */
	act.sa_flags = 0;
	act.sa_handler = action;
	sigemptyset(&act.sa_mask);

	if (sigaction(signal, &act, NULL) == -1)
		LogCrit(COMPONENT_LOG, "Failed to arm signal %d, error %d (%s)",
			signal, errno, strerror(errno));
}				/* ArmSignal */

/*
 * Five functions to manage debug level throug signal
 * handlers.
 *
 * InitLogging
 * IncrementLevelDebug
 * DecrementLevelDebug
 * SetLevelDebug
 * ReturnLevelDebug
 */

static void _SetLevelDebug(int level_to_set)
{
	int i;

	if (level_to_set < NIV_NULL)
		level_to_set = NIV_NULL;

	if (level_to_set >= NB_LOG_LEVEL)
		level_to_set = NB_LOG_LEVEL - 1;

	for (i = COMPONENT_ALL; i < COMPONENT_COUNT; i++)
		LogComponents[i].comp_log_level = level_to_set;
}				/* _SetLevelDebug */

static void SetLevelDebug(int level_to_set)
{
	_SetLevelDebug(level_to_set);

	LogChanges("Setting log level for all components to %s",
		   ReturnLevelInt(LogComponents[COMPONENT_ALL].comp_log_level));
}

void SetComponentLogLevel(log_components_t component, int level_to_set)
{
	if (component == COMPONENT_ALL) {
		SetLevelDebug(level_to_set);
		return;
	}

	if (level_to_set < NIV_NULL)
		level_to_set = NIV_NULL;

	if (level_to_set >= NB_LOG_LEVEL)
		level_to_set = NB_LOG_LEVEL - 1;

	if (LogComponents[component].comp_env_set) {
		LogWarn(COMPONENT_CONFIG,
			"LOG %s level %s from config is ignored because %s"
			" was set in environment",
			LogComponents[component].comp_name,
			ReturnLevelInt(level_to_set),
			ReturnLevelInt(LogComponents[component].
				       comp_log_level));
		return;
	}

	if (LogComponents[component].comp_log_level != level_to_set) {
		LogChanges("Changing log level of %s from %s to %s",
			   LogComponents[component].comp_name,
			   ReturnLevelInt(LogComponents[component].
					  comp_log_level),
			   ReturnLevelInt(level_to_set));
		LogComponents[component].comp_log_level = level_to_set;
	}
}

inline int ReturnLevelDebug()
{
	return LogComponents[COMPONENT_ALL].comp_log_level;
}				/* ReturnLevelDebug */

static void IncrementLevelDebug()
{
	_SetLevelDebug(ReturnLevelDebug() + 1);

	LogChanges("SIGUSR1 Increasing log level for all components to %s",
		   ReturnLevelInt(LogComponents[COMPONENT_ALL].comp_log_level));
}				/* IncrementLevelDebug */

static void DecrementLevelDebug()
{
	_SetLevelDebug(ReturnLevelDebug() - 1);

	LogChanges("SIGUSR2 Decreasing log level for all components to %s",
		   ReturnLevelInt(LogComponents[COMPONENT_ALL].comp_log_level));
}				/* DecrementLevelDebug */

void set_const_log_str()
{
	struct display_buffer dspbuf = { sizeof(const_log_str),
		const_log_str,
		const_log_str
	};
	struct display_buffer tdfbuf = { sizeof(date_time_fmt),
		date_time_fmt,
		date_time_fmt
	};
	int b_left = display_start(&dspbuf);

	const_log_str[0] = '\0';

	if (b_left > 0 && logfields.disp_epoch)
		b_left = display_printf(&dspbuf, ": epoch %08x ", ServerEpoch);

	if (b_left > 0 && logfields.disp_host)
		b_left = display_printf(&dspbuf, ": %s ", hostname);

	if (b_left > 0 && logfields.disp_prog)
		b_left = display_printf(&dspbuf, ": %s", program_name);

	if (b_left > 0 && logfields.disp_prog
	    && logfields.disp_pid)
		b_left = display_cat(&dspbuf, "-");

	if (b_left > 0 && logfields.disp_pid)
		b_left = display_printf(&dspbuf, "%d", getpid());

	if (b_left > 0
	    && (logfields.disp_prog || logfields.disp_pid)
	    && !logfields.disp_threadname)
		b_left = display_cat(&dspbuf, " ");

	b_left = display_start(&tdfbuf);

	if (logfields.datefmt == TD_LOCAL
	    && logfields.timefmt == TD_LOCAL) {
		if (b_left > 0)
			b_left = display_cat(&tdfbuf, "%c ");
	} else {
		switch (logfields.datefmt) {
		case TD_GANESHA:
			b_left = display_cat(&tdfbuf, "%d/%m/%Y ");
			break;
		case TD_8601:
			b_left = display_cat(&tdfbuf, "%F ");
			break;
		case TD_LOCAL:
			b_left = display_cat(&tdfbuf, "%x ");
			break;
		case TD_SYSLOG:
			b_left = display_cat(&tdfbuf, "%b %e ");
			break;
		case TD_SYSLOG_USEC:
			if (logfields.timefmt == TD_SYSLOG_USEC)
				b_left = display_cat(&tdfbuf, "%F");
			else
				b_left = display_cat(&tdfbuf, "%F ");
			break;
		case TD_USER:
			b_left = display_printf(&tdfbuf, "%s ", user_date_fmt);
			break;
		case TD_NONE:
		default:
			break;
		}

		switch (logfields.timefmt) {
		case TD_GANESHA:
			b_left = display_cat(&tdfbuf, "%H:%M:%S ");
			break;
		case TD_SYSLOG:
		case TD_8601:
		case TD_LOCAL:
			b_left = display_cat(&tdfbuf, "%X ");
			break;
		case TD_SYSLOG_USEC:
			b_left = display_cat(&tdfbuf, "T%H:%M:%S.%%06u%z ");
			break;
		case TD_USER:
			b_left = display_printf(&tdfbuf, "%s ", user_time_fmt);
			break;
		case TD_NONE:
		default:
			break;
		}

	}
}

static void set_logging_from_env(void)
{
	char *env_value;
	int newlevel, component, oldlevel;

	for (component = COMPONENT_ALL; component < COMPONENT_COUNT;
	     component++) {
		env_value = getenv(LogComponents[component].comp_name);
		if (env_value == NULL)
			continue;
		newlevel = ReturnLevelAscii(env_value);
		if (newlevel == -1) {
			LogCrit(COMPONENT_LOG,
				"Environment variable %s exists, but the value %s is not a valid log level.",
				LogComponents[component].comp_name,
				env_value);
			continue;
		}
		oldlevel = LogComponents[component].comp_log_level;
		LogComponents[component].comp_log_level = newlevel;
		LogComponents[component].comp_env_set = true;
		LogChanges(
		     "Using environment variable to switch log level for %s from %s to %s",
		     LogComponents[component].comp_name,
		     ReturnLevelInt(oldlevel),
		     ReturnLevelInt(newlevel));
	}

}				/* InitLogging */

/**
 *
 * @brief Finds a log facility by name
 *
 * Must be called under the rwlock
 *
 * @param[in]  name The name of the facility to be found
 *
 * @retval NULL No facility by that name
 * @retval non-NULL Pointer to the facility structure
 *
 */
static struct log_facility *find_log_facility(const char *name)
{
	struct glist_head *glist;
	struct log_facility *facility;

	glist_for_each(glist, &facility_list) {
		facility = glist_entry(glist, struct log_facility, lf_list);
		if (!strcasecmp(name, facility->lf_name))
			return facility;
	}

	return NULL;
}

/**
 * @brief Create a logging facility
 *
 * A logging facility outputs log messages using the helper function
 * log_func.  See below for enabling/disabling.
 *
 * @param name       [IN] the name of the new logger
 * @param log_func   [IN] function pointer to the helper
 * @param max_level  [IN] maximum message level this logger will handle.
 * @param header     [IN] detail level for header part of messages
 * @param private    [IN] logger specific argument.
 *
 * @return 0 on success, -errno for failure
 */

int create_log_facility(char *name,
			lf_function_t *log_func,
			log_levels_t max_level,
			log_header_t header,
			void *private)
{
	struct log_facility *facility;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	if (max_level < NIV_NULL || max_level >= NB_LOG_LEVEL)
		return -EINVAL;
	if (log_func == log_to_file && private != NULL) {
		char *dir;
		int rc;

		if (*(char *)private == '\0' ||
		    strlen(private) >= MAXPATHLEN) {
			LogCrit(COMPONENT_LOG,
				 "New log file path empty or too long");
			return -EINVAL;
		}
		dir = alloca(strlen(private) + 1);
		strcpy(dir, private);
		dir = dirname(dir);
		rc = access(dir, W_OK);
		if (rc != 0) {
			rc = errno;
			LogCrit(COMPONENT_LOG,
				 "Cannot create new log file (%s), because: %s",
				 (char *)private, strerror(rc));
			return -rc;
		}
	}
	pthread_rwlock_wrlock(&log_rwlock);

	facility = find_log_facility(name);

	if (facility != NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogInfo(COMPONENT_LOG, "Facility %s already exists", name);

		return -EEXIST;
	}

	facility = gsh_calloc(1, sizeof(*facility));

	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogCrit(COMPONENT_LOG, "Can not allocate a facility for %s",
			 name);

		return -ENOMEM;
	}
	glist_init(&facility->lf_list);
	glist_init(&facility->lf_active);
	facility->lf_name = gsh_strdup(name);
	facility->lf_func = log_func;
	facility->lf_max_level = max_level;
	facility->lf_headers = header;
	if (log_func == log_to_file && private != NULL) {
		facility->lf_private = gsh_strdup(private);
		if (facility->lf_private == NULL)
			return -ENOMEM;
	} else
		facility->lf_private = private;
	glist_add_tail(&facility_list, &facility->lf_list);

	pthread_rwlock_unlock(&log_rwlock);

	LogInfo(COMPONENT_LOG, "Created log facility %s",
		facility->lf_name);

	return 0;
}

/**
 * @brief Release a logger facility
 *
 * Release the named facility and all its resources.
 * disable it first if it is active.  It will refuse to
 * release the default logger because that could leave the server
 * with no place to send messages.
 *
 * @param name [IN] name of soon to be deceased logger
 *
 * @returns always.  The logger is not disabled or released on errors
 */

void release_log_facility(char *name)
{
	struct log_facility *facility;

	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Attempting release of non-existant log facility (%s)",
			 name);
		return;
	}
	if (facility == default_facility) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Attempting to release default log facility (%s)",
			 name);
		return;
	}
	if (!glist_empty(&facility->lf_active))
		glist_del(&facility->lf_active);
	glist_del(&facility->lf_list);
	pthread_rwlock_unlock(&log_rwlock);
	if (facility->lf_func == log_to_file &&
	    facility->lf_private != NULL)
		gsh_free(facility->lf_private);
	gsh_free(facility->lf_name);
	gsh_free(facility);
	return;
}

/**
 * @brief Enable the named logger
 *
 * Enabling a logger adds it to the list of facilites that will be
 * used to report messages.
 *
 * @param name [IN] the name of the logger to enable
 *
 * @return 0 on success, -errno on errors.
 */

int enable_log_facility(char *name)
{
	struct log_facility *facility;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogInfo(COMPONENT_LOG, "Facility %s does not exist", name);
		return -EEXIST;
	}
	if (!glist_empty(&facility->lf_active)) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Log facility (%s) is already enabled",
			 name);
		return -EINVAL;
	}
	glist_add_tail(&active_facility_list, &facility->lf_active);
	if (facility->lf_headers > max_headers)
		max_headers = facility->lf_headers;
	pthread_rwlock_unlock(&log_rwlock);
	return 0;
}

/**
 * @brief Disable the named logger
 *
 * Disabling a logger ends logging output to that facility.
 * Disabling the default logger is not allowed.  Another facility
 * must be set instead.  Loggers can be re-enabled at any time.
 *
 * @param name [IN] the name of the logger to enable
 *
 * @return 0 on success, -errno on errors.
 */

int disable_log_facility(char * name)
{
	struct log_facility *facility;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogInfo(COMPONENT_LOG, "Facility %s already exists", name);
		return -EEXIST;
	}
	if (glist_empty(&facility->lf_active)) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Log facility (%s) is already disabled",
			 name);
		return -EINVAL;
	}
	if (facility == default_facility) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Cannot disable the default logger (%s)",
			 default_facility->lf_name);
		return -EPERM;
	}
	glist_del(&facility->lf_active);
	glist_init(&facility->lf_active);
	if (facility->lf_headers == max_headers) {
		struct glist_head *glist;
		struct log_facility *found;

		max_headers = LH_NONE;
		glist_for_each(glist, &active_facility_list) {
			found = glist_entry(glist,
					    struct log_facility, lf_active);
			if (found->lf_headers > max_headers)
				max_headers = found->lf_headers;
		}
	}
	pthread_rwlock_unlock(&log_rwlock);
	return 0;
}

/**
 * @brief Set the named logger as the default logger
 *
 * The default logger can not be released sp we set another one as
 * the default instead.  The previous default logger is disabled.
 *
 * @param name [IN] the name of the logger to enable
 *
 * @return 0 on success, -errno on errors.
 */

static int set_default_log_facility(char *name)
{
	struct log_facility *facility;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	
	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG, "Facility %s does not exist", name);
		return -EEXIST;
	}
	if (glist_empty(&facility->lf_active))
		glist_add_tail(&active_facility_list, &facility->lf_active);
	if (default_facility != NULL) {
		assert(!glist_empty(&default_facility->lf_active));
		glist_del(&default_facility->lf_active);
		glist_init(&default_facility->lf_active);
		if (facility->lf_headers == max_headers) {
			struct glist_head *glist;
			struct log_facility *found;

			max_headers = LH_NONE;
			glist_for_each(glist, &active_facility_list) {
				found = glist_entry(glist,
						    struct log_facility,
						    lf_active);
				if (found->lf_headers > max_headers)
					max_headers = found->lf_headers;
			}
		}
	}
	default_facility = facility;
	pthread_rwlock_unlock(&log_rwlock);
	return 0;
}

/**
 * @brief Set the destination for logger
 *
 * This function only works if the facility outputs to files.
 *
 * @param name [IN] the name of the facility
 * @param dest [IN] "stdout", "stderr", "syslog", or a file path
 *
 * @return 0 on success, -errno on errors
 */

int set_log_destination(char *name, char *dest)
{
	struct log_facility *facility;
	int rc;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	if (dest == NULL ||
	    *dest == '\0' ||
	    strlen(dest) >= MAXPATHLEN) {
		LogCrit(COMPONENT_LOG,
			 "New log file path empty or too long");
		return -EINVAL;
	}
	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "No such log facility (%s)",
			 name);
		return -ENOENT;
	}
	if (facility->lf_func == log_to_file) {
		char *logfile, *dir;

		dir = alloca(strlen(dest) + 1);
		strcpy(dir, dest);
		dir = dirname(dir);
		rc = access(dir, W_OK);
		if (rc != 0) {
			LogCrit(COMPONENT_LOG,
				"Cannot create new log file (%s), because: %s",
				dest, strerror(errno));
			return -errno;
		}
		logfile = gsh_strdup(dest);
		if (logfile == NULL) {
			pthread_rwlock_unlock(&log_rwlock);
			LogCrit(COMPONENT_LOG,
				"No memory for log file name (%s) for %s",
				dest, facility->lf_name);
			return -ENOMEM;
		}
		if (facility->lf_private != NULL)
			gsh_free(facility->lf_private);
		facility->lf_private = logfile;
	} else if (facility->lf_func == log_to_stream) {
		FILE *out;

		if (strcasecmp(dest, "stdout") == 0) {
			out = stdout;
		} else if (strcasecmp(dest, "stderr") == 0) {
			out = stderr;
		} else {
			pthread_rwlock_unlock(&log_rwlock);
			LogCrit(COMPONENT_LOG,
				"Expected STDERR or STDOUT, not (%s)",
				dest);
			return -EINVAL;
		}
		facility->lf_private = out;
	} else {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "Log facility %s destination is not changable",
			facility->lf_name);
		return -EINVAL;
	}
	pthread_rwlock_unlock(&log_rwlock);
	return 0;
}

/**
 * @brief Set maximum logging level for a facilty
 *
 * @param name [IN] the name of the facility
 * @param max_level [IN] Maximum level
 *
 *
 * @return 0 on success, -errno on errors
 */

int set_log_level(char *name, log_levels_t max_level)
{
	struct log_facility *facility;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	if (max_level < NIV_NULL || max_level >= NB_LOG_LEVEL)
		return -EINVAL;
	pthread_rwlock_wrlock(&log_rwlock);
	facility = find_log_facility(name);
	if (facility == NULL) {
		pthread_rwlock_unlock(&log_rwlock);
		LogCrit(COMPONENT_LOG,
			 "No such log facility (%s)",
			 name);
		return -ENOENT;
	}
	facility->lf_max_level = max_level;
	pthread_rwlock_unlock(&log_rwlock);
	return 0;
}

/**
 * @brief Initialize Logging
 *
 * Called very early in server init to make logging available as
 * soon as possible. Create a logger to stderr first and make it
 * the default.  We are forced to fprintf to stderr by hand until
 * this happens.  Once this is up, the logger is working.
 * We then get stdout and syslog loggers init'd.
 * If log_path (passed in via -L on the command line), we get a
 * FILE logger going and make it our default logger.  Otherwise,
 * we use syslog as the default.
 *
 * @param log_path    [IN] optarg from -L, otherwise NULL
 * @param debug_level [IN] global debug level from -N optarg
 */

void init_logging(const char *log_path, const int debug_level)
{
	int rc;

	/* Finish initialization of and register log facilities. */
	glist_init(&facility_list);
	glist_init(&active_facility_list);

	/* Initialize const_log_str to defaults. Ganesha can start logging
	 * before the LOG config is processed (in fact, LOG config can itself
	 * issue log messages to indicate errors.
	 */
	set_const_log_str();

	rc = create_log_facility("STDERR", log_to_stream,
				 NIV_FULL_DEBUG, LH_ALL, stderr);
	if (rc != 0) {
		fprintf(stderr, "Create error (%s) for STDERR log facility!",
			strerror(-rc));
		Fatal();
	}
	rc = set_default_log_facility("STDERR");
	if (rc != 0) {
		fprintf(stderr, "Enable error (%s) for STDERR log facility!",
			strerror(-rc));
		Fatal();
	}
	rc = create_log_facility("STDOUT", log_to_stream,
				 NIV_FULL_DEBUG, LH_ALL, stdout);
	if (rc != 0)
		LogFatal(COMPONENT_LOG,
			 "Create error (%s) for STDOUT log facility!",
			strerror(-rc));
	rc = create_log_facility("SYSLOG", log_to_syslog,
				 NIV_FULL_DEBUG, LH_COMPONENT, NULL);
	if (rc != 0)
		LogFatal(COMPONENT_LOG,
			 "Create error (%s) for SYSLOG log facility!",
			 strerror(-rc));
	if (log_path) {
		rc = create_log_facility("FILE", log_to_file,
					 NIV_FULL_DEBUG, LH_ALL,
					 (void *)log_path);
		if (rc != 0)
			LogFatal(COMPONENT_LOG,
				 "Create error (%s) for FILE (%s) logging!",
				 strerror(-rc), log_path);
		rc = set_default_log_facility("FILE");
		if (rc != 0)
			LogFatal(COMPONENT_LOG,
				 "Enable error (%s) for FILE (%s) logging!",
				 strerror(-rc), log_path);
	} else {
		/* Fall back to SYSLOG as the first default facility */
		rc = set_default_log_facility("SYSLOG");
		if (rc != 0)
			LogFatal(COMPONENT_LOG,
				 "Enable error (%s) for SYSLOG logging!",
				 strerror(-rc));
	}
	if (debug_level >= 0)
		SetLevelDebug(debug_level);

	set_logging_from_env();

	ArmSignal(SIGUSR1, IncrementLevelDebug);
	ArmSignal(SIGUSR2, DecrementLevelDebug);
}

/*
 * Routines for managing error messages
 */
static int log_to_syslog(struct log_facility *facility, log_levels_t level,
			 struct display_buffer *buffer, char *compstr,
			 char *message)
{
	if (!syslog_opened) {
		openlog("nfs-ganesha", LOG_PID, LOG_USER);
		syslog_opened = 1;
	}

	/* Writing to syslog. */
	syslog(tabLogLevel[level].syslog_level, "%s", compstr);

	return 0;
}

static int log_to_file(struct log_facility *facility, log_levels_t level,
		       struct display_buffer *buffer, char *compstr,
		       char *message)
{
	int fd, my_status, len, rc = 0;
	char *path = facility->lf_private;

	len = display_buffer_len(buffer);

	/* Add newline to end of buffer */
	buffer->b_start[len] = '\n';
	buffer->b_start[len + 1] = '\0';

	fd = open(path, O_WRONLY | O_SYNC | O_APPEND | O_CREAT, log_mask);

	if (fd != -1) {
		rc = write(fd, buffer->b_start, len + 1);

		if (rc < (len + 1)) {
			if (rc >= 0)
				my_status = ENOSPC;
			else
				my_status = errno;

			(void)close(fd);

			goto error;
		}

		rc = close(fd);

		if (rc == 0)
			goto out;
	}

	my_status = errno;

 error:

	fprintf(stderr,
		"Error: couldn't complete write to the log file %s"
		"status=%d (%s) message was:\n%s", path, my_status,
		strerror(my_status), buffer->b_start);

 out:

	/* Remove newline from buffer */
	buffer->b_start[len] = '\0';

	return rc;
}

static int log_to_stream(struct log_facility *facility, log_levels_t level,
			 struct display_buffer *buffer, char *compstr,
			 char *message)
{
	FILE *stream = facility->lf_private;
	int rc;
	char *msg = buffer->b_start;
	int len;

	len = display_buffer_len(buffer);

	/* Add newline to end of buffer */
	buffer->b_start[len] = '\n';
	buffer->b_start[len + 1] = '\0';

	switch (facility->lf_headers) {
	case LH_NONE:
		msg = message;
		break;
	case LH_COMPONENT:
		msg = compstr;
		break;
	case LH_ALL:
		msg = buffer->b_start;
		break;
	}

	rc = fputs(msg, stream);

	if (rc != EOF)
		rc = fflush(stream);

	/* Remove newline from buffer */
	buffer->b_start[len] = '\0';

	if (rc == EOF)
		return -1;
	else
		return 0;
}

static int display_log_header(struct thread_log_context *context)
{
	int b_left = display_start(&context->dspbuf);

	if (b_left <= 0 || max_headers < LH_ALL)
		return b_left;

	/* Print date and/or time if either flag is enabled. */
	if (b_left > 0
	    && (logfields.datefmt != TD_NONE
		|| logfields.timefmt != TD_NONE)) {
		struct tm the_date;
		char tbuf[MAX_TD_FMT_LEN];
		time_t tm;
		struct timeval tv;

		if (logfields.timefmt == TD_SYSLOG_USEC) {
			gettimeofday(&tv, NULL);
			tm = tv.tv_sec;
		} else {
			tm = time(NULL);
		}

		Localtime_r(&tm, &the_date);

		/* Earlier we build the date/time format string in
		 * date_time_fmt, now use that to format the time and/or date.
		 * If time format is TD_SYSLOG_USEC, then we need an additional
		 * step to add the microseconds (since strftime just takes a
		 * struct tm which was filled in from a time_t and thus does not
		 * have microseconds.
		 */
		if (strftime(tbuf,
			     sizeof(tbuf),
			     date_time_fmt,
			     &the_date) != 0) {
			if (logfields.timefmt == TD_SYSLOG_USEC)
				b_left =
				    display_printf(&context->dspbuf, tbuf,
						   tv.tv_usec);
			else
				b_left = display_cat(&context->dspbuf, tbuf);
		}
	}

	if (b_left > 0 && const_log_str[0] != '\0')
		b_left = display_cat(&context->dspbuf, const_log_str);

	/* If thread name will not follow, need a : separator */
	if (b_left > 0 && !logfields.disp_threadname)
		b_left = display_cat(&context->dspbuf, ": ");

	/* If we overflowed the buffer with the header, just skip it. */
	if (b_left == 0) {
		display_reset_buffer(&context->dspbuf);
		b_left = display_start(&context->dspbuf);
	}

	/* The message will now start at context->dspbuf.b_current */
	return b_left;
}

static int display_log_component(struct thread_log_context *context,
				 log_components_t component, char *file,
				 int line, const char *function, int level)
{
	int b_left = display_start(&context->dspbuf);

	if (b_left <= 0 || max_headers < LH_COMPONENT)
		return b_left;

	if (b_left > 0 && logfields.disp_threadname)
		b_left =
		    display_printf(&context->dspbuf, "[%s] ",
				   context->thread_name);

	if (b_left > 0 && logfields.disp_filename) {
		if (logfields.disp_linenum)
			b_left = display_printf(&context->dspbuf, "%s:", file);
		else
			b_left = display_printf(&context->dspbuf, "%s :", file);
	}

	if (b_left > 0 && logfields.disp_linenum)
		b_left = display_printf(&context->dspbuf, "%d :", line);

	if (b_left > 0 && logfields.disp_funct)
		b_left = display_printf(&context->dspbuf, "%s :", function);

	if (b_left > 0 && logfields.disp_comp)
		b_left =
		    display_printf(&context->dspbuf, "%s :",
				   LogComponents[component].comp_str);

	if (b_left > 0 && logfields.disp_level)
		b_left =
		    display_printf(&context->dspbuf, "%s :",
				   tabLogLevel[level].short_str);

	/* If we overflowed the buffer with the header, just skip it. */
	if (b_left == 0) {
		display_reset_buffer(&context->dspbuf);
		b_left = display_start(&context->dspbuf);
	}

	return b_left;
}

void display_log_component_level(log_components_t component, char *file,
				 int line, char *function, log_levels_t level,
				 char *format, va_list arguments)
{
	struct thread_log_context *context;
	char *compstr;
	char *message;
	int b_left;
	struct glist_head *glist;
	struct log_facility *facility;

	context = Log_GetThreadContext(component != COMPONENT_LOG_EMERG);

	if (context != &emergency_context) {
		/* Reset and verify the buffer. */
		display_reset_buffer(&context->dspbuf);

		b_left = display_start(&context->dspbuf);

		if (b_left <= 0)
			context = &emergency_context;
	}

	if (context == &emergency_context)
		pthread_mutex_lock(&emergency_mutex);

	/* Build up the messsage and capture the various positions in it. */
	b_left = display_log_header(context);

	if (b_left > 0)
		compstr = context->dspbuf.b_current;
	else
		compstr = context->dspbuf.b_start;

	if (b_left > 0)
		b_left =
		    display_log_component(context, component, file, line,
					  function, level);

	if (b_left > 0)
		message = context->dspbuf.b_current;
	else
		message = context->dspbuf.b_start;

	if (b_left > 0)
		b_left = display_vprintf(&context->dspbuf, format, arguments);

	pthread_rwlock_rdlock(&log_rwlock);

	glist_for_each(glist, &active_facility_list) {
		facility = glist_entry(glist, struct log_facility, lf_active);

		if (level <= facility->lf_max_level
		    && facility->lf_func != NULL)
			facility->lf_func(facility, level, &context->dspbuf,
					  compstr, message);
	}

	pthread_rwlock_unlock(&log_rwlock);

	if (context == &emergency_context)
		pthread_mutex_unlock(&emergency_mutex);

	if (level == NIV_FATAL)
		Fatal();
}

struct log_component_info LogComponents[COMPONENT_COUNT] = {
	[COMPONENT_ALL] = {
		.comp_name = "COMPONENT_ALL",
		.comp_str = "",
		.comp_log_level = NB_LOG_LEVEL,}, /* marks set/not set */
	[COMPONENT_LOG] = {
		.comp_name = "COMPONENT_LOG",
		.comp_str = "LOG",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_LOG_EMERG] = {
		.comp_name = "COMPONENT_LOG_EMERG",
		.comp_str = "LOG_EMERG",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_MEMALLOC] = {
		.comp_name = "COMPONENT_MEMALLOC",
		.comp_str = "ALLOC",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_MEMLEAKS] = {
		.comp_name = "COMPONENT_MEMLEAKS",
		.comp_str = "LEAKS",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_FSAL] = {
		.comp_name = "COMPONENT_FSAL",
		.comp_str = "FSAL",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFSPROTO] = {
		.comp_name = "COMPONENT_NFSPROTO",
		.comp_str = "NFS3",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_V4] = {
		.comp_name = "COMPONENT_NFS_V4",
		.comp_str = "NFS4",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_V4_PSEUDO] = {
		.comp_name = "COMPONENT_NFS_V4_PSEUDO",
		.comp_str = "NFS4 PSEUDO",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_FILEHANDLE] = {
		.comp_name = "COMPONENT_FILEHANDLE",
		.comp_str = "FH",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_SHELL] = {
		.comp_name = "COMPONENT_NFS_SHELL",
		.comp_str = "NFS SHELL",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_DISPATCH] = {
		.comp_name = "COMPONENT_DISPATCH",
		.comp_str = "DISP",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_CACHE_INODE] = {
		.comp_name = "COMPONENT_CACHE_INODE",
		.comp_str = "INODE",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_CACHE_INODE_GC] = {
		.comp_name = "COMPONENT_CACHE_INODE_GC",
		.comp_str = "INODE GC",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_CACHE_INODE_LRU] = {
		.comp_name = "COMPONENT_CACHE_INODE_LRU",
		.comp_str = "INODE LRU",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_HASHTABLE] = {
		.comp_name = "COMPONENT_HASHTABLE",
		.comp_str = "HT",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_HASHTABLE_CACHE] = {
		.comp_name = "COMPONENT_HASHTABLE_CACHE",
		"HT CACHE",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_LRU] = {
		.comp_name = "COMPONENT_LRU",
		.comp_str = "LRU",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_DUPREQ] = {
		.comp_name = "COMPONENT_DUPREQ",
		.comp_str = "DUPREQ",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_RPCSEC_GSS] = {
		.comp_name = "COMPONENT_RPCSEC_GSS",
		.comp_str = "RPCSEC GSS",
		NIV_EVENT,},
	[COMPONENT_INIT] = {
		.comp_name = "COMPONENT_INIT",
		.comp_str = "NFS STARTUP",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_MAIN] = {
		.comp_name = "COMPONENT_MAIN",
		.comp_str = "MAIN",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_IDMAPPER] = {
		.comp_name = "COMPONENT_IDMAPPER",
		.comp_str = "ID MAPPER",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_READDIR] = {
		.comp_name = "COMPONENT_NFS_READDIR",
		.comp_str = "NFS READDIR",
		NIV_EVENT,},
	[COMPONENT_NFS_V4_LOCK] = {
		.comp_name = "COMPONENT_NFS_V4_LOCK",
		.comp_str = "NFS4 LOCK",
		NIV_EVENT,},
	[COMPONENT_NFS_V4_XATTR] = {
		.comp_name = "COMPONENT_NFS_V4_XATTR",
		.comp_str = "NFS4 XATTR",
		NIV_EVENT,},
	[COMPONENT_NFS_V4_REFERRAL] = {
		.comp_name = "COMPONENT_NFS_V4_REFERRAL",
		.comp_str = "NFS4 REFERRAL",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_MEMCORRUPT] = {
		.comp_name = "COMPONENT_MEMCORRUPT",
		.comp_str = "MEM CORRUPT",
		NIV_EVENT,},
	[COMPONENT_CONFIG] = {
		.comp_name = "COMPONENT_CONFIG",
		.comp_str = "CONFIG",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_CLIENTID] = {
		.comp_name = "COMPONENT_CLIENTID",
		.comp_str = "CLIENT ID",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_STDOUT] = {
		.comp_name = "COMPONENT_STDOUT",
		.comp_str = "STDOUT",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_SESSIONS] = {
		.comp_name = "COMPONENT_SESSIONS",
		.comp_str = "SESSIONS",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_PNFS] = {
		.comp_name = "COMPONENT_PNFS",
		.comp_str = "PNFS",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_RPC_CACHE] = {
		.comp_name = "COMPONENT_RPC_CACHE",
		.comp_str = "RPC CACHE",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_RW_LOCK] = {
		.comp_name = "COMPONENT_RW_LOCK",
		.comp_str = "RW LOCK",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NLM] = {
		.comp_name = "COMPONENT_NLM",
		.comp_str = "NLM",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_RPC] = {
		.comp_name = "COMPONENT_RPC",
		.comp_str = "RPC",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_CB] = {
		.comp_name = "COMPONENT_NFS_CB",
		.comp_str = "NFS CB",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_THREAD] = {
		.comp_name = "COMPONENT_THREAD",
		.comp_str = "THREAD",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_NFS_V4_ACL] = {
		.comp_name = "COMPONENT_NFS_V4_ACL",
		.comp_str = "NFS4 ACL",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_STATE] = {
		.comp_name = "COMPONENT_STATE",
		.comp_str = "STATE",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_9P] = {
		.comp_name = "COMPONENT_9P",
		.comp_str = "9P",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_9P_DISPATCH] = {
		.comp_name = "COMPONENT_9P_DISPATCH",
		.comp_str = "9P DISP",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_FSAL_UP] = {
		.comp_name = "COMPONENT_FSAL_UP",
		.comp_str = "FSAL_UP",
		.comp_log_level = NIV_EVENT,},
	[COMPONENT_DBUS] = {
		.comp_name = "COMPONENT_DBUS",
		.comp_str = "DBUS",
		.comp_log_level = NIV_EVENT,},
};

void DisplayLogComponentLevel(log_components_t component, char *file, int line,
			      char *function, log_levels_t level, char *format,
			      ...)
{
	va_list arguments;

	va_start(arguments, format);

	display_log_component_level(component, file, line, function, level,
				    format, arguments);

	va_end(arguments);
}

static int isValidLogPath(const char *pathname)
{
	char tempname[MAXPATHLEN + 1];

	char *directory_name;
	int rc;

	if (strmaxcpy(tempname, pathname, sizeof(tempname)) == -1)
		return 0;

	directory_name = dirname(tempname);
	if (directory_name == NULL)
		return 0;

	rc = access(directory_name, W_OK);
	switch (rc) {
	case 0:
		break;		/* success !! */
	case EACCES:
		LogCrit(COMPONENT_LOG,
			"Either access is denied to the file or denied to one of the directories in %s",
			directory_name);
		break;
	case ELOOP:
		LogCrit(COMPONENT_LOG,
			"Too many symbolic links were encountered in resolving %s",
			directory_name);
		break;
	case ENAMETOOLONG:
		LogCrit(COMPONENT_LOG,
			"%s is too long of a pathname.",
			directory_name);
		break;
	case ENOENT:
		LogCrit(COMPONENT_LOG,
			"A component of %s does not exist.",
			directory_name);
		break;
	case ENOTDIR:
		LogCrit(COMPONENT_LOG,
			"%s is not a directory.",
			directory_name);
		break;
	case EROFS:
		LogCrit(COMPONENT_LOG,
			"Write permission was requested for a file on a read-only file system.");
		break;
	case EFAULT:
		LogCrit(COMPONENT_LOG,
			"%s points outside your accessible address space.",
			directory_name);
		break;

	default:
		break;
	}

	return 1;
}

void SetLogFile(char *name)
{
	struct log_facility *facility;
	char *tmp;

	pthread_rwlock_wrlock(&log_rwlock);

	facility = &facilities[FILELOG];

	if (!isValidLogPath(name)) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG,
			 "Could not set default logging to %s (invalid path)",
			 name);
		return;
	}

	tmp = gsh_strdup(name);

	if (tmp == NULL) {
		pthread_rwlock_unlock(&log_rwlock);

		LogMajor(COMPONENT_LOG,
			 "Could not set default logging to %s (no memory)",
			 name);
		return;
	}

	if (facility->lf_private != NULL)
		gsh_free(facility->lf_private);

	facility->lf_private = tmp;

	pthread_rwlock_unlock(&log_rwlock);

	LogEvent(COMPONENT_LOG, "Changing log file to %s", name);
}

/*
 * Sets the default logging method (whether to a specific filepath or syslog.
 * During initialization this is used and separate layer logging defaults to
 * this destination.
 */
void SetDefaultLogging(const char *name)
{
	struct log_facility *facility;

	pthread_rwlock_wrlock(&log_rwlock);

	facility = find_log_facility(name);

	if (facility == NULL) {
		/* Try to use FILELOG facility */
		char *tmp;

		facility = &facilities[FILELOG];

		if (!isValidLogPath(name)) {
			pthread_rwlock_unlock(&log_rwlock);

			LogMajor(COMPONENT_LOG,
				 "Could not set default logging to %s (invalid path)",
				 name);
			return;
		}

		tmp = gsh_strdup(name);

		if (tmp == NULL) {
			pthread_rwlock_unlock(&log_rwlock);

			LogMajor(COMPONENT_LOG,
				 "Could not set default logging to %s (no memory)",
				 name);
			return;
		}

		if (facility->lf_private != NULL)
			gsh_free(facility->lf_private);

		facility->lf_private = tmp;
	}

	if (default_facility != facility)
		_deactivate_log_facility(default_facility);

	default_facility = facility;

	_activate_log_facility(facility);

	pthread_rwlock_unlock(&log_rwlock);

	LogEvent(COMPONENT_LOG,
		 "Setting default log destination to name %s",
		 name);
}				/* SetDefaultLogging */

/*
 *  Re-export component logging to TI-RPC internal logging
 */
void rpc_warnx(char *fmt, ...)
{
	va_list ap;

	if (LogComponents[COMPONENT_RPC].comp_log_level < NIV_DEBUG)
		return;

	va_start(ap, fmt);

	display_log_component_level(COMPONENT_RPC, "<no-file>", 0, "rpc",
				    NIV_DEBUG, fmt, ap);

	va_end(ap);

}

/** @todo turn these into dbus admin methods */

#if 0				/* _SNMP_ADM_ACTIVE */

int getComponentLogLevel(snmp_adm_type_union *param, void *opt)
{
	long component = (long)opt;

	strncpy(param->string,
		ReturnLevelInt(LogComponents[component].comp_log_level),
		sizeof(param->string));
	param->string[sizeof(param->string) - 1] = '\0';
	return 0;
}

int setComponentLogLevel(const snmp_adm_type_union *param, void *opt)
{
	long component = (long)opt;
	int level_to_set = ReturnLevelAscii(param->string);

	if (level_to_set == -1)
		return -1;

	if (component == COMPONENT_ALL) {
		_SetLevelDebug(level_to_set);

		LogChanges(
		     "SNMP request changing log level for all components to %s",
		     ReturnLevelInt(level_to_set));
	} else {
		LogChanges(
		     "SNMP request changing log level of %s from %s to %s.",
		     LogComponents[component].comp_name,
		     ReturnLevelInt(LogComponents[component].comp_log_level),
		     ReturnLevelInt(level_to_set));
		LogComponents[component].comp_log_level = level_to_set;
	}

	return 0;
}

#endif				/* _SNMP_ADM_ACTIVE */

#ifdef USE_DBUS

static bool dbus_prop_get(log_components_t component, DBusMessageIter *reply)
{
	char *level_code;

	level_code = ReturnLevelInt(LogComponents[component].comp_log_level);
	if (level_code == NULL)
		return false;
	if (!dbus_message_iter_append_basic
	    (reply, DBUS_TYPE_STRING, &level_code))
		return false;
	return true;
}

static bool dbus_prop_set(log_components_t component, DBusMessageIter *arg)
{
	char *level_code;
	long log_level;

	if (dbus_message_iter_get_arg_type(arg) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(arg, &level_code);
	log_level = ReturnLevelAscii(level_code);
	if (component == COMPONENT_ALL) {
		_SetLevelDebug(log_level);
		LogChanges("Dbus set log level for all components to %s",
			   level_code);
	} else {
		LogChanges("Dbus set log level for %s from %s to %s.",
			   LogComponents[component].comp_name,
			   ReturnLevelInt(LogComponents[component].
					  comp_log_level),
			   ReturnLevelInt(log_level));
		LogComponents[component].comp_log_level = log_level;
	}
	return true;
}

/* Macros to make mapping properties table to components enum etc. easier
 * expands to table entries and shim functions.
 */

#define HANDLE_PROP(component) \
static bool dbus_prop_get_##component(DBusMessageIter *reply) \
{ \
	return dbus_prop_get(component, reply);\
} \
\
static bool dbus_prop_set_##component(DBusMessageIter *args) \
{ \
	return dbus_prop_set(component, args);\
} \
\
static struct gsh_dbus_prop component##_prop = { \
	.name = #component,			 \
	.access = DBUS_PROP_READWRITE,		 \
	.type =  "s",				 \
	.get = dbus_prop_get_##component,	 \
	.set = dbus_prop_set_##component	 \
}

#define LOG_PROPERTY_ITEM(component) (&component##_prop)

/**
 * @brief Log property handlers.
 *
 * Expands to get/set functions that match dbus_prop_get/set protos
 * and call common handler with component enum as arg.
 * There is one line per log_components_t enum.
 * These must also match LOG_PROPERTY_ITEM
 */

HANDLE_PROP(COMPONENT_ALL);
HANDLE_PROP(COMPONENT_LOG);
HANDLE_PROP(COMPONENT_LOG_EMERG);
HANDLE_PROP(COMPONENT_MEMALLOC);
HANDLE_PROP(COMPONENT_MEMLEAKS);
HANDLE_PROP(COMPONENT_FSAL);
HANDLE_PROP(COMPONENT_NFSPROTO);
HANDLE_PROP(COMPONENT_NFS_V4);
HANDLE_PROP(COMPONENT_NFS_V4_PSEUDO);
HANDLE_PROP(COMPONENT_FILEHANDLE);
HANDLE_PROP(COMPONENT_DISPATCH);
HANDLE_PROP(COMPONENT_CACHE_INODE);
HANDLE_PROP(COMPONENT_CACHE_INODE_GC);
HANDLE_PROP(COMPONENT_CACHE_INODE_LRU);
HANDLE_PROP(COMPONENT_HASHTABLE);
HANDLE_PROP(COMPONENT_HASHTABLE_CACHE);
HANDLE_PROP(COMPONENT_LRU);
HANDLE_PROP(COMPONENT_DUPREQ);
HANDLE_PROP(COMPONENT_RPCSEC_GSS);
HANDLE_PROP(COMPONENT_INIT);
HANDLE_PROP(COMPONENT_MAIN);
HANDLE_PROP(COMPONENT_IDMAPPER);
HANDLE_PROP(COMPONENT_NFS_READDIR);
HANDLE_PROP(COMPONENT_NFS_V4_LOCK);
HANDLE_PROP(COMPONENT_NFS_V4_XATTR);
HANDLE_PROP(COMPONENT_NFS_V4_REFERRAL);
HANDLE_PROP(COMPONENT_MEMCORRUPT);
HANDLE_PROP(COMPONENT_CONFIG);
HANDLE_PROP(COMPONENT_CLIENTID);
HANDLE_PROP(COMPONENT_STDOUT);
HANDLE_PROP(COMPONENT_SESSIONS);
HANDLE_PROP(COMPONENT_PNFS);
HANDLE_PROP(COMPONENT_RPC_CACHE);
HANDLE_PROP(COMPONENT_RW_LOCK);
HANDLE_PROP(COMPONENT_NLM);
HANDLE_PROP(COMPONENT_RPC);
HANDLE_PROP(COMPONENT_NFS_CB);
HANDLE_PROP(COMPONENT_THREAD);
HANDLE_PROP(COMPONENT_NFS_V4_ACL);
HANDLE_PROP(COMPONENT_STATE);
HANDLE_PROP(COMPONENT_9P);
HANDLE_PROP(COMPONENT_9P_DISPATCH);
HANDLE_PROP(COMPONENT_FSAL_UP);
HANDLE_PROP(COMPONENT_DBUS);

static struct gsh_dbus_prop *log_props[] = {
	LOG_PROPERTY_ITEM(COMPONENT_ALL),
	LOG_PROPERTY_ITEM(COMPONENT_LOG),
	LOG_PROPERTY_ITEM(COMPONENT_LOG_EMERG),
	LOG_PROPERTY_ITEM(COMPONENT_MEMALLOC),
	LOG_PROPERTY_ITEM(COMPONENT_MEMLEAKS),
	LOG_PROPERTY_ITEM(COMPONENT_FSAL),
	LOG_PROPERTY_ITEM(COMPONENT_NFSPROTO),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4_PSEUDO),
	LOG_PROPERTY_ITEM(COMPONENT_FILEHANDLE),
	LOG_PROPERTY_ITEM(COMPONENT_DISPATCH),
	LOG_PROPERTY_ITEM(COMPONENT_CACHE_INODE),
	LOG_PROPERTY_ITEM(COMPONENT_CACHE_INODE_GC),
	LOG_PROPERTY_ITEM(COMPONENT_CACHE_INODE_LRU),
	LOG_PROPERTY_ITEM(COMPONENT_HASHTABLE),
	LOG_PROPERTY_ITEM(COMPONENT_HASHTABLE_CACHE),
	LOG_PROPERTY_ITEM(COMPONENT_LRU),
	LOG_PROPERTY_ITEM(COMPONENT_DUPREQ),
	LOG_PROPERTY_ITEM(COMPONENT_RPCSEC_GSS),
	LOG_PROPERTY_ITEM(COMPONENT_INIT),
	LOG_PROPERTY_ITEM(COMPONENT_MAIN),
	LOG_PROPERTY_ITEM(COMPONENT_IDMAPPER),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_READDIR),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4_LOCK),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4_XATTR),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4_REFERRAL),
	LOG_PROPERTY_ITEM(COMPONENT_MEMCORRUPT),
	LOG_PROPERTY_ITEM(COMPONENT_CONFIG),
	LOG_PROPERTY_ITEM(COMPONENT_CLIENTID),
	LOG_PROPERTY_ITEM(COMPONENT_STDOUT),
	LOG_PROPERTY_ITEM(COMPONENT_SESSIONS),
	LOG_PROPERTY_ITEM(COMPONENT_PNFS),
	LOG_PROPERTY_ITEM(COMPONENT_RPC_CACHE),
	LOG_PROPERTY_ITEM(COMPONENT_RW_LOCK),
	LOG_PROPERTY_ITEM(COMPONENT_NLM),
	LOG_PROPERTY_ITEM(COMPONENT_RPC),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_CB),
	LOG_PROPERTY_ITEM(COMPONENT_THREAD),
	LOG_PROPERTY_ITEM(COMPONENT_NFS_V4_ACL),
	LOG_PROPERTY_ITEM(COMPONENT_STATE),
	LOG_PROPERTY_ITEM(COMPONENT_9P),
	LOG_PROPERTY_ITEM(COMPONENT_9P_DISPATCH),
	LOG_PROPERTY_ITEM(COMPONENT_FSAL_UP),
	LOG_PROPERTY_ITEM(COMPONENT_DBUS),
	NULL
};

struct gsh_dbus_interface log_interface = {
	.name = "org.ganesha.nfsd.log",
	.signal_props = false,
	.props = log_props,
	.methods = NULL,
	.signals = NULL
};

#endif				/* USE_DBUS */

/**
 * @brief Enumerated time and date format parameters
 */

static struct config_item_list timeformats[] = {
	CONFIG_LIST_TOK("ganesha", TD_GANESHA),
	CONFIG_LIST_TOK("true", TD_GANESHA),
	CONFIG_LIST_TOK("local", TD_LOCAL),
	CONFIG_LIST_TOK("8601", TD_8601),
	CONFIG_LIST_TOK("ISO-8601", TD_8601),
	CONFIG_LIST_TOK("ISO 8601", TD_8601),
	CONFIG_LIST_TOK("ISO", TD_8601),
	CONFIG_LIST_TOK("syslog", TD_SYSLOG),
	CONFIG_LIST_TOK("syslog_usec",TD_SYSLOG_USEC),
	CONFIG_LIST_TOK("false", TD_NONE),
	CONFIG_LIST_TOK("none", TD_NONE),
	CONFIG_LIST_TOK("user_defined", TD_USER),
	CONFIG_LIST_EOL
};

/**
 * @brief Logging format parameters
 */

static struct config_item format_options[] = {
	CONF_ITEM_ENUM("date_format", TD_GANESHA, timeformats,
		       logfields, datefmt),
	CONF_ITEM_ENUM("time_format", TD_GANESHA, timeformats,
		       logfields, timefmt),
	CONF_ITEM_STR("user_date_format", 1, MAX_TD_FMT_LEN, NULL,
		       logfields, user_date_fmt),
	CONF_ITEM_STR("user_time_format", 1, MAX_TD_FMT_LEN, NULL,
		       logfields, user_time_fmt),
	CONF_ITEM_BOOL("EPOCH", true,
		       logfields, disp_epoch),
	CONF_ITEM_BOOL("HOSTNAME", true,
		       logfields, disp_host),
	CONF_ITEM_BOOL("PROGNAME", true,
		       logfields, disp_prog),
	CONF_ITEM_BOOL("PID", true,
		       logfields, disp_pid),
	CONF_ITEM_BOOL("THREAD_NAME", true,
		       logfields, disp_threadname),
	CONF_ITEM_BOOL("FILE_NAME", true,
		       logfields, disp_filename),
	CONF_ITEM_BOOL("LINE_NUM", true,
		       logfields, disp_linenum),
	CONF_ITEM_BOOL("FUNCTION_NAME", true,
		       logfields, disp_funct),
	CONF_ITEM_BOOL("COMPONENT", true,
		       logfields, disp_comp),
	CONF_ITEM_BOOL("LEVEL", true,
		       logfields, disp_level),
	CONFIG_EOL
};

/**
 * @brief Log component levels
 */

static struct config_item_list log_levels[] = {
	CONFIG_LIST_TOK("NIV_NULL", NIV_NULL),
	CONFIG_LIST_TOK("NULL", NIV_NULL),
	CONFIG_LIST_TOK("NIV_FATAL", NIV_FATAL),
	CONFIG_LIST_TOK("FATAL", NIV_FATAL),
	CONFIG_LIST_TOK("NIV_MAJ", NIV_MAJ),
	CONFIG_LIST_TOK("MAJ", NIV_MAJ),
	CONFIG_LIST_TOK("NIV_CRIT", NIV_CRIT),
	CONFIG_LIST_TOK("CRIT", NIV_CRIT),
	CONFIG_LIST_TOK("NIV_WARN", NIV_WARN),
	CONFIG_LIST_TOK("WARN", NIV_WARN),
	CONFIG_LIST_TOK("NIV_EVENT", NIV_EVENT),
	CONFIG_LIST_TOK("EVENT", NIV_EVENT),
	CONFIG_LIST_TOK("NIV_INFO", NIV_INFO),
	CONFIG_LIST_TOK("INFO", NIV_INFO),
	CONFIG_LIST_TOK("NIV_DEBUG", NIV_DEBUG),
	CONFIG_LIST_TOK("DEBUG", NIV_DEBUG),
	CONFIG_LIST_TOK("NIV_MID_DEBUG", NIV_MID_DEBUG),
	CONFIG_LIST_TOK("M_DBG", NIV_MID_DEBUG),
	CONFIG_LIST_TOK("NIV_FULL_DEBUG", NIV_FULL_DEBUG),
	CONFIG_LIST_TOK("F_DBG", NIV_FULL_DEBUG),
	CONFIG_LIST_EOL
};

/**
 * @brief Logging components
 */
static struct config_item component_levels[] = {
	CONF_INDEX_ENUM("COMPONENT_ALL", NIV_NULL, log_levels,
			COMPONENT_ALL,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_LOG", NIV_EVENT, log_levels,
			COMPONENT_LOG,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_LOG_EMERG", NIV_EVENT, log_levels,
			COMPONENT_LOG_EMERG,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_MEMALLOC", NIV_EVENT, log_levels,
			COMPONENT_MEMALLOC,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_MEMLEAKS", NIV_EVENT, log_levels,
			COMPONENT_MEMLEAKS,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_FSAL", NIV_EVENT, log_levels,
			COMPONENT_FSAL,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFSPROTO", NIV_EVENT, log_levels,
			COMPONENT_NFSPROTO,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4_PSEUDO", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4_PSEUDO,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_FILEHANDLE", NIV_EVENT, log_levels,
			COMPONENT_FILEHANDLE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_SHELL", NIV_EVENT, log_levels,
			COMPONENT_NFS_SHELL,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_DISPATCH", NIV_EVENT, log_levels,
			COMPONENT_DISPATCH,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_CACHE_INODE", NIV_EVENT, log_levels,
			COMPONENT_CACHE_INODE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_CACHE_INODE_GC", NIV_EVENT, log_levels,
			COMPONENT_CACHE_INODE_GC,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_CACHE_INODE_LRU", NIV_EVENT, log_levels,
			COMPONENT_CACHE_INODE_LRU,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_HASHTABLE", NIV_EVENT, log_levels,
			COMPONENT_HASHTABLE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_HASHTABLE_CACHE", NIV_EVENT, log_levels,
			COMPONENT_HASHTABLE_CACHE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_LRU", NIV_EVENT, log_levels,
			COMPONENT_LRU,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_DUPREQ", NIV_EVENT, log_levels,
			COMPONENT_DUPREQ,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_RPCSEC_GSS", NIV_EVENT, log_levels,
			COMPONENT_RPCSEC_GSS,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_INIT", NIV_EVENT, log_levels,
			COMPONENT_INIT,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_MAIN", NIV_EVENT, log_levels,
			COMPONENT_MAIN,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_IDMAPPER", NIV_EVENT, log_levels,
			COMPONENT_IDMAPPER,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_READDIR", NIV_EVENT, log_levels,
			COMPONENT_NFS_READDIR,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4_LOCK", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4_LOCK,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4_XATTR", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4_XATTR,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4_REFERRAL", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4_REFERRAL,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_MEMCORRUPT", NIV_EVENT, log_levels,
			COMPONENT_MEMCORRUPT,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_CONFIG", NIV_EVENT, log_levels,
			COMPONENT_CONFIG,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_CLIENTID", NIV_EVENT, log_levels,
			COMPONENT_CLIENTID,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_STDOUT", NIV_EVENT, log_levels,
			COMPONENT_STDOUT,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_SESSIONS", NIV_EVENT, log_levels,
			COMPONENT_SESSIONS,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_PNFS", NIV_EVENT, log_levels,
			COMPONENT_PNFS,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_RPC_CACHE", NIV_EVENT, log_levels,
			COMPONENT_RPC_CACHE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_RW_LOCK", NIV_EVENT, log_levels,
			COMPONENT_RW_LOCK,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NLM", NIV_EVENT, log_levels,
			COMPONENT_NLM,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_RPC", NIV_EVENT, log_levels,
			COMPONENT_RPC,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_CB", NIV_EVENT, log_levels,
			COMPONENT_NFS_CB,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_THREAD", NIV_EVENT, log_levels,
			COMPONENT_THREAD,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_NFS_V4_ACL", NIV_EVENT, log_levels,
			COMPONENT_NFS_V4_ACL,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_STATE", NIV_EVENT, log_levels,
			COMPONENT_STATE,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_9P", NIV_EVENT, log_levels,
			COMPONENT_9P,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_9P_DISPATCH", NIV_EVENT, log_levels,
			COMPONENT_9P_DISPATCH,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_FSAL_UP", NIV_EVENT, log_levels,
			COMPONENT_FSAL_UP,
			log_component_info, comp_log_level),
	CONF_INDEX_ENUM("COMPONENT_DBUS", NIV_EVENT, log_levels,
			COMPONENT_DBUS,
			log_component_info, comp_log_level),
	CONFIG_EOL
};

static struct config_item_list header_options[] = {
	CONFIG_LIST_TOK("none", LH_NONE),
	CONFIG_LIST_TOK("component", LH_COMPONENT),
	CONFIG_LIST_TOK("all", LH_ALL),
	CONFIG_LIST_EOL
};

struct facility_config {
	char *facility_name;
	char *dest;
	log_header_t headers;
	log_levels_t max_level;
};

static struct config_item facility_params[] = {
	CONF_ITEM_STR("name", 1, 20, NULL,
		      facility_config, facility_name),
	CONF_MAND_STR("destination", 1, MAXPATHLEN, NULL,
		      facility_config, dest),
	CONF_ITEM_ENUM("max_level", NB_LOG_LEVEL, log_levels,
		       facility_config, max_level),
	CONF_ITEM_ENUM("headers", LH_COMPONENT, header_options,
		       facility_config, headers),
	CONFIG_EOL
};

/**
 * @brief Initialize the log message format parameters
 */

static void *format_init(void *link_mem, void *self_struct)
{
	assert(link_mem != NULL || self_struct != NULL);

	if (self_struct == NULL)
		return &logfields;
	else
		return NULL;
}

/**
 * @brief Commit the log format parameters
 *
 * I'd prefer that Date_format and Time_format be enums but they are not.
 * They are enums except when they are not and we do hope that whatever
 * that is can be digested by printf...
 */

static int format_commit(void *node, void *link_mem, void *self_struct)
{
	struct logfields *log = (struct logfields *)self_struct;

	if (log->datefmt == TD_USER && log->user_date_fmt == NULL) {
		LogWarn(COMPONENT_CONFIG,
			"Date is \"user_set\" with empty date format.");
		log->datefmt = TD_GANESHA;
	}
	if (log->datefmt != TD_USER && log->user_date_fmt != NULL) {
		LogWarn(COMPONENT_CONFIG,
			"Set user date format (%s) but not \"user_set\" format",
			log->user_date_fmt);
		gsh_free(log->user_date_fmt);
		log->user_date_fmt = NULL;
	}
	if (log->timefmt == TD_USER && log->user_time_fmt == NULL) {
		LogWarn(COMPONENT_CONFIG,
			"Time is \"user_set\" with empty time format.");
		log->timefmt = TD_GANESHA;
	}
	if (log->timefmt != TD_USER && log->user_time_fmt != NULL) {
		LogWarn(COMPONENT_CONFIG,
			"Set time format string (%s) but not \"user_set\" format",
			log->user_time_fmt);
		gsh_free(log->user_time_fmt);
		log->user_time_fmt = NULL;
	}
	/* rebuild const_log_str with new format params. */
	set_const_log_str();
	return 0;
}

static void *component_init(void *link_mem, void *self_struct)
{
	assert(link_mem != NULL || self_struct != NULL);

	if (self_struct == NULL)
		return &LogComponents;
	else
		return NULL;
}

static int component_commit(void *node, void *link_mem, void *self_struct)
{
	if (LogComponents[COMPONENT_ALL].comp_log_level != NB_LOG_LEVEL)
		SetLevelDebug(LogComponents[COMPONENT_ALL].comp_log_level);
	else
		LogComponents[COMPONENT_ALL].comp_log_level = NIV_EVENT;
	return 0;
}

static void *facility_init(void *link_mem, void *self_struct)
{
	assert(link_mem != NULL || self_struct != NULL);

	if (link_mem == NULL)
		return NULL;
	else if (self_struct == NULL)
		return gsh_calloc(1, sizeof(struct facility_config));
	else
		gsh_free(self_struct);
	return NULL;
}

static int facility_commit(void *node, void *link_mem, void *self_struct)
{
	struct facility_config *conf = (struct facility_config *)self_struct;
	void *dest = NULL;
	int errcnt = 0;
	lf_function_t *func = NULL;
	int rc;

	if (conf->facility_name == NULL) {
		LogCrit(COMPONENT_LOG,
			"No facility name given");
		errcnt++;
		goto out;
	}
	if (conf->dest != NULL) {
		if ((strcasecmp(conf->dest, "stderr") == 0) ||
		    (strcasecmp(conf->dest, "stdout") == 0)) {
			func = log_to_stream;
			dest = (strcasecmp(conf->dest, "stdout") == 0)?
				stdout : stderr;
		} else if (strcasecmp(conf->dest, "syslog") == 0) {
			func = log_to_syslog;
		} else {
			func = log_to_file;
			dest = conf->dest;
		}
	} else {
		LogCrit(COMPONENT_LOG,
			"No facility destination given");
		errcnt++;
		goto out;
	}
	rc = create_log_facility(conf->facility_name,
				 func,
				 (conf->max_level != NB_LOG_LEVEL ?
				  conf->max_level : NIV_FULL_DEBUG),
				 conf->headers,
				 dest);
	if (rc < 0) {
		if (rc == -EEXIST) {
			if (conf->dest != NULL) {
				rc = set_log_destination(conf->facility_name,
							 conf->dest);
				if (rc < 0) {
					errcnt++;
					goto out;
				}
			}
			if (conf->max_level != NB_LOG_LEVEL) {
				rc =  set_log_level(conf->facility_name,
						    conf->max_level);
				if (rc < 0)  {
					LogCrit(COMPONENT_LOG,
						"Invalid logging severity level");
					errcnt++;
					goto out;
				}
			}
		}
	}
		
out:
	gsh_free(self_struct); /* got the bits, be done with it */
	return errcnt;
}

struct logger_config {
	log_levels_t default_level;
};

static struct config_item logging_params[] = {
	CONF_ITEM_ENUM("Default_log_level", NB_LOG_LEVEL, log_levels,
		       logger_config, default_level),
	CONF_ITEM_BLOCK("Facility", facility_params,
			facility_init, facility_commit,
			facility_config, facility_name),
	CONF_ITEM_BLOCK("Format", format_options,
			format_init, format_commit,
			logfields, datefmt),
	CONF_ITEM_BLOCK("Components", component_levels,
			component_init, component_commit,
			log_component_info, comp_log_level),
	CONFIG_EOL
};

struct config_block logging_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.log",
	.blk_desc.name = "LOG",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = logging_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/**
 *
 * read_log_config: Read the configuration ite; for the logging component.
 *
 * Read the configuration ite; for the logging component.
 *
 * @param in_config [IN] configuration file handle
 * @param pparam [OUT] read parameters
 *
 * @return 0 if ok, -1 if failed, 1 is stanza is not there
 *
 */
int read_log_config(config_file_t in_config)
{
	struct logger_config logger;
	int rc;

	rc = load_config_from_parse(in_config,
				    &logging_param,
				    &logger,
				    true);
	return rc ? 1 : 0;
}				/* read_log_config */

void reread_log_config()
{
	int status = 0;
	int i;
	config_file_t config_struct;

	/* Clear out the flag indicating component was set from environment. */
	for (i = COMPONENT_ALL; i < COMPONENT_COUNT; i++)
		LogComponents[i].comp_env_set = false;

	/* If no configuration file is given, then the caller must want to
	 * reparse the configuration file from startup.
	 */
	if (config_path[0] == '\0') {
		LogCrit(COMPONENT_CONFIG,
			"No configuration file was specified for reloading log config.");
		return;
	}

	/* Attempt to parse the new configuration file */
	config_struct = config_ParseFile(config_path);
	if (!config_struct) {
		LogCrit(COMPONENT_CONFIG,
			"Error while parsing new configuration file %s: %s",
			config_path, config_GetErrorMsg());
		return;
	}

	/* Create the new exports list */
	status = read_log_config(config_struct);
	if (status < 0)
		LogCrit(COMPONENT_CONFIG, "Error while parsing LOG entries");

	config_Free(config_struct);
}
