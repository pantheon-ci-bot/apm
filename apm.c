/*
 +----------------------------------------------------------------------+
 |  APM stands for Alternative PHP Monitor                              |
 +----------------------------------------------------------------------+
 | Copyright (c) 2008-2009  Davide Mendolia, Patrick Allaert            |
 +----------------------------------------------------------------------+
 | This source file is subject to version 3.01 of the PHP license,      |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.php.net/license/3_01.txt                                  |
 | If you did not receive a copy of the PHP license and are unable to   |
 | obtain it through the world-wide-web, please send a note to          |
 | license@php.net so we can mail you a copy immediately.               |
 +----------------------------------------------------------------------+
 */

#define APM_VERSION "1.0.0RC1"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include "php.h"
#include "php_ini.h"
#include "zend_exceptions.h"
#include "php_apm.h"
#include "ext/standard/info.h"
#include "ext/standard/php_smart_str.h"
#include "ext/standard/php_var.h"
#include "Zend/zend_builtin_functions.h"

#define DB_FILE "/events"
#define SEC_TO_USEC(sec) ((sec) * 1000000.00)
#define USEC_TO_SEC(usec) ((usec) / 1000000.00)

ZEND_API void (*old_error_cb)(int type, const char *error_filename,
                              const uint error_lineno, const char *format,
                              va_list args);
void apm_error_cb(int type, const char *error_filename, 
                  const uint error_lineno, const char *format,
                  va_list args);

void apm_throw_exception_hook(zval *exception TSRMLS_DC);

static int callback(void *, int, char **, char **);
static int callback_slow_request(void *, int, char **, char **);
static int perform_db_access_checks();
static void insert_event(int, char *, uint, char *);
void debug_print_backtrace_args(zval * TSRMLS_DC);

sqlite3 *event_db;
char *db_file;
static int odd_event_list = 1;
static int odd_slow_request = 1;

/* recorded timestamp for the request */
struct timeval begin_tp;

function_entry apm_functions[] = {
        PHP_FE(apm_get_events, NULL)
        PHP_FE(apm_get_slow_requests, NULL)
	{NULL, NULL, NULL}
};

zend_module_entry apm_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"apm",
	apm_functions,
	PHP_MINIT(apm),
	PHP_MSHUTDOWN(apm),
	PHP_RINIT(apm),	
	PHP_RSHUTDOWN(apm),
	PHP_MINFO(apm),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1.0",
#endif
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_APM
ZEND_GET_MODULE(apm)
#endif

ZEND_DECLARE_MODULE_GLOBALS(apm)

PHP_INI_BEGIN()
	/* Boolean controlling whether the extension is globally active or not */
	STD_PHP_INI_BOOLEAN("apm.enabled",                "1",                      PHP_INI_ALL, OnUpdateBool,   enabled,                zend_apm_globals, apm_globals)
	/* Boolean controlling whether the event monitoring is active or not */
	STD_PHP_INI_BOOLEAN("apm.event_enabled",          "1",                      PHP_INI_ALL, OnUpdateBool,   event_enabled,          zend_apm_globals, apm_globals)
	/* Boolean controlling whether the slow request monitoring is active or not */
	STD_PHP_INI_BOOLEAN("apm.slow_request_enabled",   "1",                      PHP_INI_ALL, OnUpdateBool,   slow_request_enabled,   zend_apm_globals, apm_globals)
	/* Path to the SQLite database file */
	STD_PHP_INI_ENTRY("apm.max_event_insert_timeout", "100",                    PHP_INI_ALL, OnUpdateLong,   timeout,                zend_apm_globals, apm_globals)
	/* Max timeout to wait for storing the event in the DB */
	STD_PHP_INI_ENTRY("apm.db_path",                  "/var/php/apm/db",        PHP_INI_ALL, OnUpdateString, db_path,                zend_apm_globals, apm_globals)
	/* Time (in ms) before a request is considered 'slow' */
	STD_PHP_INI_ENTRY("apm.slow_request_duration",    "100",                    PHP_INI_ALL, OnUpdateLong,   slow_request_duration,  zend_apm_globals, apm_globals)
PHP_INI_END()
 
static void apm_init_globals(zend_apm_globals *apm_globals)
{
}

PHP_MINIT_FUNCTION(apm)
{
	ZEND_INIT_MODULE_GLOBALS(apm, apm_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	if (APM_G(enabled)) {
		if (perform_db_access_checks() == FAILURE) {
			return FAILURE;
		}

		/* Defining full path to db file */
		db_file = (char *) malloc((strlen(APM_G(db_path)) + strlen(DB_FILE) + 1) * sizeof(char));

		strcpy(db_file, APM_G(db_path));
		strcat(db_file, DB_FILE);
	}

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(apm)
{
	UNREGISTER_INI_ENTRIES();

	/* Restoring saved error callback function */
	zend_error_cb = old_error_cb;

	return SUCCESS;
}

PHP_RINIT_FUNCTION(apm)
{
	/* Storing actual error callback function for later restore */
	old_error_cb = zend_error_cb;

	if (APM_G(enabled)) {
		if (APM_G(event_enabled)) {
			struct timezone begin_tz;
			
			/* storing timestamp of request */
			gettimeofday(&begin_tp, &begin_tz);
		}
		/* Opening the sqlite database file */
		if (sqlite3_open(db_file, &event_db)) {
			/*
			 Closing DB file and stop loading the extension
			 in case of error while opening the database file
			 */
			sqlite3_close(event_db);
			return FAILURE;
		}

		sqlite3_busy_timeout(event_db, APM_G(timeout));

		/* Making the connection asynchronous, not waiting for data being really written to the disk */
		sqlite3_exec(event_db, "PRAGMA synchronous = OFF", NULL, NULL, NULL);

		/* Replacing current error callback function with apm's one */
		zend_error_cb = apm_error_cb;
		zend_throw_exception_hook = apm_throw_exception_hook;
	}
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(apm)
{
	if (APM_G(enabled) && APM_G(slow_request_enabled)) {
		float duration;
		struct timeval end_tp;
		struct timezone end_tz;

		gettimeofday(&end_tp, &end_tz);

		/* Request longer than accepted thresold ? */
		duration = SEC_TO_USEC(end_tp.tv_sec - begin_tp.tv_sec) + end_tp.tv_usec - begin_tp.tv_usec;
		if (duration > 1000.0 * APM_G(slow_request_duration)) {
			zval **array;
			zval **token;
			char *script_filename = NULL;
			char *sql;

			if (zend_hash_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER"), (void **) &array) == SUCCESS &&
				Z_TYPE_PP(array) == IS_ARRAY &&
#if (PHP_MAJOR_VERSION < 6)
				zend_hash_find
#else
				zend_ascii_hash_find
#endif
					(Z_ARRVAL_PP(array), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME"), (void **) &token) == SUCCESS) {
#if (PHP_MAJOR_VERSION < 6)
				script_filename = Z_STRVAL_PP(token);
#else
				script_filename = zend_unicode_to_ascii(Z_USTRVAL_PP(token), Z_USTRLEN_PP(token) TSRMLS_CC);
#endif
			}

			/* Building SQL insert query */
			sql = sqlite3_mprintf("INSERT INTO slow_request (ts, duration, file) VALUES (datetime(), %f, %Q);",
			                      USEC_TO_SEC(duration), script_filename);

			/* Executing SQL insert query */
			sqlite3_exec(event_db, sql, NULL, NULL, NULL);

			sqlite3_free(sql);
		}
	}

	/* Restoring saved error callback function */
	zend_error_cb = old_error_cb;
	zend_throw_exception_hook = NULL;
	return SUCCESS;
}

PHP_MINFO_FUNCTION(apm)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "APM support", "enabled");
	php_info_print_table_row(2, "Version", APM_VERSION);
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

/* {{{ void apm_error(int type, const char *format, ...)
 *    This function provides a hook for error */
void apm_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
	if (APM_G(event_enabled)) {
		char *msg;
		va_list args_copy;

		/* A copy of args is needed to be used for the old_error_cb */
		va_copy(args_copy, args);
		vspprintf(&msg, 0, format, args_copy);

		/* We need to see if we have an uncaught exception fatal error now */
		if (type == E_ERROR && strncmp(msg, "Uncaught exception", 18) == 0) {

		} else {
			insert_event(type, (char *) error_filename, error_lineno, msg);
		}
		efree(msg);
	}

	/* Calling saved callback function for error handling */
	old_error_cb(type, error_filename, error_lineno, format, args);
}
/* }}} */


void apm_throw_exception_hook(zval *exception TSRMLS_DC)
{
	if (APM_G(event_enabled)) {
		zval *message, *file, *line;
		zend_class_entry *default_ce, *exception_ce;

		if (!exception) {
			return;
		}

		default_ce = zend_exception_get_default(TSRMLS_C);
		exception_ce = zend_get_class_entry(exception TSRMLS_CC);

		message = zend_read_property(default_ce, exception, "message", sizeof("message")-1, 0 TSRMLS_CC);
		file =    zend_read_property(default_ce, exception, "file",    sizeof("file")-1,    0 TSRMLS_CC);
		line =    zend_read_property(default_ce, exception, "line",    sizeof("line")-1,    0 TSRMLS_CC);

		insert_event(E_ERROR, Z_STRVAL_P(file), Z_LVAL_P(line), Z_STRVAL_P(message));
	}
}


/* Return an HTML table with all events */
PHP_FUNCTION(apm_get_events)
{
	sqlite3 *db;
	/* Opening the sqlite database file */
	if (sqlite3_open(db_file, &db)) {
		/* Closing DB file and returning false */
		sqlite3_close(db);
		RETURN_FALSE;
	}

	/* Results are printed in an HTML table */
	odd_event_list = 1;
	php_printf("<table id=\"event-list\"><tr><th>#</th><th>Time</th><th>Type</th><th>File</th><th>Line</th><th>Message</th><th>Backtrace</th></tr>\n");
	sqlite3_exec(db, "SELECT id, ts, CASE type \
                          WHEN 1 THEN 'E_ERROR' \
                          WHEN 2 THEN 'E_WARNING' \
                          WHEN 4 THEN 'E_PARSE' \
                          WHEN 8 THEN 'E_NOTICE' \
                          WHEN 16 THEN 'E_CORE_ERROR' \
                          WHEN 32 THEN 'E_CORE_WARNING' \
                          WHEN 64 THEN 'E_COMPILE_ERROR' \
                          WHEN 128 THEN 'E_COMPILE_WARNING' \
                          WHEN 256 THEN 'E_USER_ERROR' \
                          WHEN 512 THEN 'E_USER_WARNING' \
                          WHEN 1024 THEN 'E_USER_NOTICE' \
                          WHEN 2048 THEN 'E_STRICT' \
                          WHEN 4096 THEN 'E_RECOVERABLE_ERROR' \
                          WHEN 8192 THEN 'E_DEPRECATED' \
                          WHEN 16384 THEN 'E_USER_DEPRECATED' \
                          END, \
                          file, line, message, backtrace FROM event ORDER BY id DESC", callback, NULL, NULL);
	php_printf("</table>");

	sqlite3_close(db);
	RETURN_TRUE;
}

/* Return an HTML table with all events */
PHP_FUNCTION(apm_get_slow_requests)
{
	sqlite3 *db;
	/* Opening the sqlite database file */
	if (sqlite3_open(db_file, &db)) {
		/* Closing DB file and returning false */
		sqlite3_close(db);
		RETURN_FALSE;
	}

	/* Results are printed in an HTML table */
	odd_slow_request = 1;
	php_printf("<table id=\"slow-request-list\"><tr><th>#</th><th>Time</th><th>Duration</th><th>File</th></tr>\n");
	sqlite3_exec(db, "SELECT id, ts, duration, file FROM slow_request ORDER BY id DESC", callback_slow_request, NULL, NULL);
	php_printf("</table>");

	sqlite3_close(db);
	RETURN_TRUE;
}

/* Function called for every row returned by event query */
static int callback(void *data, int num_fields, char **fields, char **col_name)
{
	php_printf("<tr class=\"%s %s\"><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td><pre>%s</pre></td></tr>\n",
                   fields[2], odd_event_list ? "odd" : "even", fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6]);
	odd_event_list = !odd_event_list;

	return 0;
}

/* Function called for every row returned by slow request query */
static int callback_slow_request(void *data, int num_fields, char **fields, char **col_name)
{
	php_printf("<tr class=\"%s\"><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n",
                   odd_slow_request ? "odd" : "even", fields[0], fields[1], fields[2], fields[3]);
	odd_slow_request = !odd_slow_request;

	return 0;
}

/* Perform access checks on the DB path */
static int perform_db_access_checks()
{
	struct stat db_path_stat;

	/* Does db_path exists ? */
	if (stat(APM_G(db_path), &db_path_stat) != 0) {
		zend_error(E_CORE_WARNING, "APM cannot be loaded, an error occured while accessing %s", APM_G(db_path));
		return FAILURE;
	}

	/* Is this a directory ? */
	if (! S_ISDIR(db_path_stat.st_mode)) {
		zend_error(E_CORE_WARNING, "APM cannot be loaded, %s should be a directory", APM_G(db_path));
		return FAILURE;
	}

	/* Does it have the correct permissions ? */
	if (access(APM_G(db_path), R_OK | W_OK | X_OK) != 0) {
		zend_error(E_CORE_WARNING, "APM cannot be loaded, %s should be readable, writable and executable", APM_G(db_path));
		return FAILURE;
	}
	return SUCCESS;
}

static zval *debug_backtrace_get_args(void ***curpos TSRMLS_DC)
{
#if PHP_API_VERSION >= 20090626
	void **p = *curpos;
#else
        void **p = *curpos - 2;
#endif
        zval *arg_array, **arg;
	int arg_count = (int)(zend_uintptr_t) *p;
#if PHP_API_VERSION < 20090626
 	*curpos -= (arg_count+2);

#endif
	MAKE_STD_ZVAL(arg_array);
#if PHP_API_VERSION >= 20090626
	array_init_size(arg_array, arg_count);
#else
	array_init(arg_array);
#endif
	p -= arg_count;

	while (--arg_count >= 0) {
		arg = (zval **) p++;
		if (*arg) {
			if (Z_TYPE_PP(arg) != IS_OBJECT) {
				SEPARATE_ZVAL_TO_MAKE_IS_REF(arg);
			}
#if PHP_API_VERSION >= 20090626
			Z_ADDREF_PP(arg);
#else
			(*arg)->refcount++;
#endif
			add_next_index_zval(arg_array, *arg);
		} else {
			add_next_index_null(arg_array);
		}
	}

#if PHP_API_VERSION < 20090626
	/* skip args from incomplete frames */
	while ((((*curpos)-1) > EG(argument_stack).elements) && *((*curpos)-1)) {
		(*curpos)--;
	}

#endif
	return arg_array;
}


/* Insert an event in the backend */
static void insert_event(int type, char * error_filename, uint error_lineno, char * msg)
{
/* backtrace variables */
        zend_execute_data *ptr, *skip;
	int lineno;
	char *function_name;
	char *filename;
	char *class_name = NULL;
	char *call_type;
	char *include_filename = NULL;
        zval *arg_array = NULL;
#if PHP_API_VERSION < 20090626
	void **cur_arg_pos = EG(argument_stack).top_element;
	void **args = cur_arg_pos;
	int arg_stack_consistent = 0;
	int frames_on_stack = 0;
#endif
        int indent = 0;

/* sql variables */
        char *sql;
	smart_str trace_str = {0};

#if PHP_API_VERSION < 20090626
	while (--args > EG(argument_stack).elements) {
		if (*args--) {
			break;
		}
		args -= *(ulong*)args;
		frames_on_stack++;

		/* skip args from incomplete frames */
		while (((args-1) > EG(argument_stack).elements) && *(args-1)) {
			args--;
		}

		if ((args-1) == EG(argument_stack).elements) {
			arg_stack_consistent = 1;
			break;
		}
	}
#endif
	ptr = EG(current_execute_data);

	ptr = ptr->prev_execute_data;
#if PHP_API_VERSION < 20090626
	cur_arg_pos -= 2;
	frames_on_stack--;
#endif
        
	while (ptr) {
		char *free_class_name = NULL;

		class_name = call_type = NULL;   
		arg_array = NULL;

		skip = ptr;
		/* skip internal handler */
		if (!skip->op_array &&
		    skip->prev_execute_data &&
		    skip->prev_execute_data->opline &&
		    skip->prev_execute_data->opline->opcode != ZEND_DO_FCALL &&
		    skip->prev_execute_data->opline->opcode != ZEND_DO_FCALL_BY_NAME &&
		    skip->prev_execute_data->opline->opcode != ZEND_INCLUDE_OR_EVAL) {
		  skip = skip->prev_execute_data;
		}

		if (skip->op_array) {
			filename = skip->op_array->filename;
			lineno = skip->opline->lineno;
		} else {
			filename = NULL;
			lineno = 0;
		}

		function_name = ptr->function_state.function->common.function_name;

		if (function_name) {
			if (ptr->object) {
				if (ptr->function_state.function->common.scope) {
					class_name = ptr->function_state.function->common.scope->name;
				} else {
					zend_uint class_name_len;
					int dup;

					dup = zend_get_object_classname(ptr->object, &class_name, &class_name_len TSRMLS_CC);
					if(!dup) {
						free_class_name = class_name;
					}
				}

				call_type = "->";
			} else if (ptr->function_state.function->common.scope) {
				class_name = ptr->function_state.function->common.scope->name;
				call_type = "::";
			} else {
				class_name = NULL;
				call_type = NULL;
			}
			if ((! ptr->opline) || ((ptr->opline->opcode == ZEND_DO_FCALL_BY_NAME) || (ptr->opline->opcode == ZEND_DO_FCALL))) {
#if PHP_API_VERSION >= 20090626
				if (ptr->function_state.arguments) {
					arg_array = debug_backtrace_get_args(&ptr->function_state.arguments TSRMLS_CC);
				}
#else
                            if (arg_stack_consistent && (frames_on_stack > 0)) {
					arg_array = debug_backtrace_get_args(&cur_arg_pos TSRMLS_CC);
					frames_on_stack--;
				}
#endif
			}
		} else {
                        /*TODO continue review with debug_print_backtrace */
			/* i know this is kinda ugly, but i'm trying to avoid extra cycles in the main execution loop */
			zend_bool build_filename_arg = 1;

			if (!ptr->opline || ptr->opline->opcode != ZEND_INCLUDE_OR_EVAL) {
				/* can happen when calling eval from a custom sapi */
				function_name = "unknown";
				build_filename_arg = 0;
			} else
			switch (Z_LVAL(ptr->opline->op2.u.constant)) {
				case ZEND_EVAL:
					function_name = "eval";
					build_filename_arg = 0;
					break;
				case ZEND_INCLUDE:
					function_name = "include";
					break;
				case ZEND_REQUIRE:
					function_name = "require";
					break;
				case ZEND_INCLUDE_ONCE:
					function_name = "include_once";
					break;
				case ZEND_REQUIRE_ONCE:
					function_name = "require_once";
					break;
				default:
					/* this can actually happen if you use debug_backtrace() in your error_handler and 
					 * you're in the top-scope */
					function_name = "unknown"; 
					build_filename_arg = 0;
					break;
			}

			if (build_filename_arg && include_filename) {
				MAKE_STD_ZVAL(arg_array);
				array_init(arg_array);
				add_next_index_string(arg_array, include_filename, 1);
			}
			call_type = NULL;
		}
		smart_str_appendc(&trace_str, '#');
		smart_str_append_long(&trace_str, indent);
		smart_str_appendc(&trace_str, ' ');
		if (class_name) {
			smart_str_appends(&trace_str, class_name);
			smart_str_appends(&trace_str, call_type);
		}
		smart_str_appends(&trace_str, function_name?function_name:"main");
		smart_str_appendc(&trace_str, '(');
		if (arg_array) {
			debug_print_backtrace_args(arg_array TSRMLS_CC);
			zval_ptr_dtor(&arg_array);
		}
		if (filename) {
			smart_str_appends(&trace_str, ") called at [");
			smart_str_appends(&trace_str, filename);
			smart_str_appendc(&trace_str, ':');
			smart_str_append_long(&trace_str, lineno);
			smart_str_appends(&trace_str, "]\n");
		} else {
			zend_execute_data *prev = skip->prev_execute_data;

			while (prev) {
				if (prev->function_state.function &&
					prev->function_state.function->common.type != ZEND_USER_FUNCTION) {
					prev = NULL;
					break;
				}				    
				if (prev->op_array) {
					smart_str_appends(&trace_str, ") called at [");
					smart_str_appends(&trace_str, prev->op_array->filename);
					smart_str_appendc(&trace_str, ':');
					smart_str_append_long(&trace_str, prev->opline->lineno);
					smart_str_appends(&trace_str, "]\n");
					break;
				}
				prev = prev->prev_execute_data;
			}
			if (!prev) {
				smart_str_appends(&trace_str, ")\n");
			}
		}
		include_filename = filename;
		ptr = skip->prev_execute_data;
		++indent;
		if (free_class_name) {
			efree(free_class_name);
		}
	}

	smart_str_0(&trace_str);

	/* Builing SQL insert query */
	sql = sqlite3_mprintf("INSERT INTO event (ts, type, file, line, message, backtrace) VALUES (datetime(), %d, %Q, %d, %Q, %Q);",
		                  type, error_filename ? error_filename : "", error_lineno, msg ? msg : "", trace_str.c ? trace_str.c : "");
	/* Executing SQL insert query */
	sqlite3_exec(event_db, sql, NULL, NULL, NULL);

	smart_str_free(&trace_str);
	sqlite3_free(sql);
}
