#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>

#ifdef ZTS
#include "TSRM.h"
#endif

#include <SAPI.h>
#include <php_ini.h>
#include "ext/standard/info.h"
#include <zend_extensions.h>
#include <zend_exceptions.h>
#include "fastdht_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <fdht_client.h>
#include <fdht_func.h>
#include <logger.h>

typedef struct
{
	zend_object zo;
	GroupArray *pGroupArray;
} php_fdht_t;

static int le_fdht;

static zend_class_entry *fdht_ce = NULL;
static zend_class_entry *fdht_exception_ce = NULL;
static zend_class_entry *spl_ce_RuntimeException = NULL;

#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION < 3)
const zend_fcall_info empty_fcall_info = { 0, NULL, NULL, NULL, NULL, 0, NULL, NULL, 0 };
#undef ZEND_BEGIN_ARG_INFO_EX
#define ZEND_BEGIN_ARG_INFO_EX(name, pass_rest_by_reference, return_reference, required_num_args) \
    static zend_arg_info name[] = {                                                               \
        { NULL, 0, NULL, 0, 0, 0, pass_rest_by_reference, return_reference, required_num_args },
#endif

#define TRIM_PHP_KEY(szKey, key_len)  \
	if (key_len > 0 && *(szKey + (key_len - 1)) == '\0') \
	{ \
		key_len--; \
	} \

// Every user visible function must have an entry in fastdht_client_functions[].
	function_entry fastdht_client_functions[] = {
		ZEND_FE(fastdht_set, NULL)
		ZEND_FE(fastdht_get, NULL)
		ZEND_FE(fastdht_inc, NULL)
		ZEND_FE(fastdht_batch_set, NULL)
		ZEND_FE(fastdht_batch_get, NULL)
		ZEND_FE(fastdht_batch_delete, NULL)
		{NULL, NULL, NULL}  /* Must be the last line */
	};

zend_module_entry fastdht_client_module_entry = {
	STANDARD_MODULE_HEADER,
	"fastdht_client",
	fastdht_client_functions,
	PHP_MINIT(fastdht_client),
	PHP_MSHUTDOWN(fastdht_client),
	NULL,//PHP_RINIT(fastdht_client),
	NULL,//PHP_RSHUTDOWN(fastdht_client),
	PHP_MINFO(fastdht_client),
	"1.06", 
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_FASTDHT_CLIENT
	ZEND_GET_MODULE(fastdht_client)
#endif

#define FASTDHT_FILL_KEY(key_info, szNamespace, szObjectId, szKey) \
	if (key_info.namespace_len > FDHT_MAX_NAMESPACE_LEN) \
	{ \
		key_info.namespace_len = FDHT_MAX_NAMESPACE_LEN; \
	} \
	if (key_info.obj_id_len > FDHT_MAX_OBJECT_ID_LEN) \
	{ \
		key_info.obj_id_len = FDHT_MAX_OBJECT_ID_LEN; \
	} \
	if (key_info.key_len > FDHT_MAX_SUB_KEY_LEN) \
	{ \
		key_info.key_len = FDHT_MAX_SUB_KEY_LEN; \
	} \
 \
	memcpy(key_info.szNameSpace, szNamespace, key_info.namespace_len); \
	memcpy(key_info.szObjectId, szObjectId, key_info.obj_id_len); \
	memcpy(key_info.szKey, szKey, key_info.key_len); \


#define FASTDHT_FILL_OBJECT(obj_info, szNamespace, szObjectId) \
	if (obj_info.namespace_len > FDHT_MAX_NAMESPACE_LEN) \
	{ \
		obj_info.namespace_len = FDHT_MAX_NAMESPACE_LEN; \
	} \
	if (obj_info.obj_id_len > FDHT_MAX_OBJECT_ID_LEN) \
	{ \
		obj_info.obj_id_len = FDHT_MAX_OBJECT_ID_LEN; \
	} \
 \
	memcpy(obj_info.szNameSpace, szNamespace, obj_info.namespace_len); \
	memcpy(obj_info.szObjectId, szObjectId, obj_info.obj_id_len); \

/*
int fastdht_set(string namespace, string object_id, string key, 
		string value [, int expires])
return 0 for success, != 0 for error
*/
ZEND_FUNCTION(fastdht_set)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	char *szKey;
	char *szValue;
	int value_len;
	long expires;
	FDHTKeyInfo key_info;

	argc = ZEND_NUM_ARGS();
	if (argc != 4 && argc != 5)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_set parameters count: %d != 4 or 5", 
			__LINE__, argc);
		RETURN_LONG(EINVAL);
	}

	expires = FDHT_EXPIRES_NEVER;
	if (zend_parse_parameters(argc TSRMLS_CC, "ssss|l", &szNamespace, 
		&key_info.namespace_len, &szObjectId, &key_info.obj_id_len, 
		&szKey, &key_info.key_len, &szValue, &value_len, &expires)
		 == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_set parameter parse error!", __LINE__);
		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_KEY(key_info, szNamespace, szObjectId, szKey)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), szKey=%s(%d), "
		"szValue=%s(%d), expires=%ld", 
		szNamespace, key_info.namespace_len, 
		szObjectId, key_info.obj_id_len, 
		szKey, key_info.key_len,
		szValue, value_len, expires);
	*/

	RETURN_LONG(fdht_set(&key_info, expires, szValue, value_len));
}

/*
int fastdht_batch_set(string namespace, string object_id, array key_list, 
		[, int expires])
return 0 for success, != 0 for error
*/
ZEND_FUNCTION(fastdht_batch_set)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	zval *key_values;
	HashTable *key_value_hash;
	zval **data;
	zval ***ppp;
	int key_count;
	int success_count;
	char *szKey;
	long index;
	long expires;
	int result;
	FDHTObjectInfo obj_info;
	FDHTKeyValuePair key_list[FDHT_MAX_KEY_COUNT_PER_REQ];
	zval zvalues[FDHT_MAX_KEY_COUNT_PER_REQ];
	zval *pValue;
	zval *pValueEnd;
	FDHTKeyValuePair *pKeyValue;
	FDHTKeyValuePair *pKeyValueEnd;
	HashPosition pointer;

	argc = ZEND_NUM_ARGS();
	if (argc != 3 && argc != 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_set parameters count: %d != 3 or 4", \
			__LINE__, argc);
		RETURN_LONG(EINVAL);
	}

	expires = FDHT_EXPIRES_NEVER;
	if (zend_parse_parameters(argc TSRMLS_CC, "ssa|l", &szNamespace, 
		&obj_info.namespace_len, &szObjectId, &obj_info.obj_id_len, 
		&key_values, &expires) == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set parameter parse error!", __LINE__);
		RETURN_LONG(EINVAL);
	}

	key_value_hash = Z_ARRVAL_P(key_values);
	key_count = zend_hash_num_elements(key_value_hash);
	if (key_count <= 0 || key_count > FDHT_MAX_KEY_COUNT_PER_REQ)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set, invalid key_count: %d!", \
			__LINE__, key_count);
		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_OBJECT(obj_info, szNamespace, szObjectId)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), "
		"expires=%ld", szNamespace, obj_info.namespace_len, 
		szObjectId, obj_info.obj_id_len, expires);
	*/

	memset(zvalues, 0, sizeof(zvalues));
	memset(key_list, 0, sizeof(key_list));
	pValue = zvalues;
	pKeyValue = key_list;
	ppp = &data;
	for (zend_hash_internal_pointer_reset_ex(key_value_hash, &pointer);
	     zend_hash_get_current_data_ex(key_value_hash, (void **)ppp,
		&pointer) == SUCCESS; zend_hash_move_forward_ex(
		key_value_hash, &pointer))
	{
		if (zend_hash_get_current_key_ex(key_value_hash, &szKey, \
			&(pKeyValue->key_len), &index, 0, &pointer) \
			!= HASH_KEY_IS_STRING)
		{
			logError("file: "__FILE__", line: %d, " \
				"fastdht_batch_set, invalid array element, " \
				"index=%d!", __LINE__, index);
			RETURN_LONG(EINVAL);
		}

		TRIM_PHP_KEY(szKey, pKeyValue->key_len)

		if (pKeyValue->key_len > FDHT_MAX_SUB_KEY_LEN)
		{
			pKeyValue->key_len = FDHT_MAX_SUB_KEY_LEN;
		}
		memcpy(pKeyValue->szKey, szKey, pKeyValue->key_len);

		if (Z_TYPE_PP(data) == IS_STRING)
		{
			pKeyValue->pValue = Z_STRVAL_PP(data);
			pKeyValue->value_len = Z_STRLEN_PP(data);
		}
		else
		{
			*pValue = **data;
			zval_copy_ctor(pValue);
			convert_to_string(pValue);
			pKeyValue->pValue = Z_STRVAL(*pValue);
			pKeyValue->value_len = Z_STRLEN(*pValue);

			pValue++;
		}

		/*
		logInfo("key=%s(%d), value=%s(%d)", \
			pKeyValue->szKey, pKeyValue->key_len, \
			pKeyValue->pValue, pKeyValue->value_len);
		*/

		pKeyValue++;
	}
	pValueEnd = pValue;

	result = fdht_batch_set(&obj_info, key_list, key_count, \
				expires, &success_count);
	for (pValue=zvalues; pValue<pValueEnd; pValue++)
	{
		zval_dtor(pValue);
	}

	if (result != 0)
	{
		RETURN_LONG(result);
	}

	if (success_count == key_count)
	{
		RETURN_LONG(result);
	}

	array_init(return_value);

	pKeyValueEnd = key_list + key_count;
	for (pKeyValue=key_list; pKeyValue<pKeyValueEnd; pKeyValue++)
	{
		add_assoc_long_ex(return_value, pKeyValue->szKey, \
				pKeyValue->key_len + 1, pKeyValue->status);
	}
}

/*
int fastdht_batch_get(string namespace, string object_id, array key_list, 
		[, int expires])
return 0 for success, != 0 for error
*/
ZEND_FUNCTION(fastdht_batch_get)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	zval *key_values;
	HashTable *key_value_hash;
	zval **data;
	zval ***ppp;
	int key_count;
	int success_count;
	char *szKey;
	long index;
	long expires;
	int result;
	FDHTObjectInfo obj_info;
	FDHTKeyValuePair key_list[FDHT_MAX_KEY_COUNT_PER_REQ];
	FDHTKeyValuePair *pKeyValue;
	FDHTKeyValuePair *pKeyValueEnd;
	HashPosition pointer;

	argc = ZEND_NUM_ARGS();
	if (argc != 3 && argc != 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_set parameters count: %d != 3 or 4", \
			__LINE__, argc);
		RETURN_LONG(EINVAL);
	}

	expires = FDHT_EXPIRES_NEVER;
	if (zend_parse_parameters(argc TSRMLS_CC, "ssa|l", &szNamespace, 
		&obj_info.namespace_len, &szObjectId, &obj_info.obj_id_len, 
		&key_values, &expires) == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set parameter parse error!", __LINE__);
		RETURN_LONG(EINVAL);
	}

	key_value_hash = Z_ARRVAL_P(key_values);
	key_count = zend_hash_num_elements(key_value_hash);
	if (key_count <= 0 || key_count > FDHT_MAX_KEY_COUNT_PER_REQ)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set, invalid key_count: %d!", \
			__LINE__, key_count);
		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_OBJECT(obj_info, szNamespace, szObjectId)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), "
		"expires=%ld", szNamespace, obj_info.namespace_len, 
		szObjectId, obj_info.obj_id_len, expires);
	*/

	memset(key_list, 0, sizeof(key_list));
	pKeyValue = key_list;
	ppp = &data;
	for (zend_hash_internal_pointer_reset_ex(key_value_hash, &pointer);
	     zend_hash_get_current_data_ex(key_value_hash, (void **)ppp,
		&pointer) == SUCCESS; zend_hash_move_forward_ex(
		key_value_hash, &pointer))
	{
		if (zend_hash_get_current_key_ex(key_value_hash, &szKey, \
			&(pKeyValue->key_len), &index, 0, &pointer) \
			== HASH_KEY_IS_STRING)
		{
			TRIM_PHP_KEY(szKey, pKeyValue->key_len)
		}
		else if (Z_TYPE_PP(data) == IS_STRING)
		{
			szKey = Z_STRVAL_PP(data);
			pKeyValue->key_len = Z_STRLEN_PP(data);
		}
		else
		{
			logError("file: "__FILE__", line: %d, " \
				"fastdht_batch_get, invalid array element, "\
				"index=%d!", __LINE__, index);
			RETURN_LONG(EINVAL);
		}

		if (pKeyValue->key_len > FDHT_MAX_SUB_KEY_LEN)
		{
			pKeyValue->key_len = FDHT_MAX_SUB_KEY_LEN;
		}
		memcpy(pKeyValue->szKey, szKey, pKeyValue->key_len);

		/*
		logInfo("key=%s(%d)", \
			pKeyValue->szKey, pKeyValue->key_len);
		*/

		pKeyValue++;
	}

	result = fdht_batch_get_ex1((&g_group_array), g_keep_alive, \
			&obj_info, key_list, key_count, expires, \
			_emalloc, &success_count);
	if (result != 0)
	{
		RETURN_LONG(result);
	}

	array_init(return_value);

	pKeyValueEnd = key_list + key_count;
	for (pKeyValue=key_list; pKeyValue<pKeyValueEnd; pKeyValue++)
	{
		if (pKeyValue->status == 0)
		{
			add_assoc_stringl_ex(return_value, pKeyValue->szKey, \
				pKeyValue->key_len + 1, pKeyValue->pValue, \
				pKeyValue->value_len, 0);
		}
		else
		{
			add_assoc_long_ex(return_value, pKeyValue->szKey, \
				pKeyValue->key_len + 1, pKeyValue->status);
		}
	}
}

/*
int fastdht_batch_delete(string namespace, string object_id, array key_list)
return 0 for success, != 0 for error
*/
ZEND_FUNCTION(fastdht_batch_delete)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	zval *key_values;
	HashTable *key_value_hash;
	zval **data;
	zval ***ppp;
	int key_count;
	int success_count;
	char *szKey;
	long index;
	int result;
	FDHTObjectInfo obj_info;
	FDHTKeyValuePair key_list[FDHT_MAX_KEY_COUNT_PER_REQ];
	FDHTKeyValuePair *pKeyValue;
	FDHTKeyValuePair *pKeyValueEnd;
	HashPosition pointer;

	argc = ZEND_NUM_ARGS();
	if (argc != 3)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_set parameters count: %d != 3", \
			__LINE__, argc);
		RETURN_LONG(EINVAL);
	}

	if (zend_parse_parameters(argc TSRMLS_CC, "ssa", &szNamespace, 
		&obj_info.namespace_len, &szObjectId, &obj_info.obj_id_len, 
		&key_values) == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set parameter parse error!", __LINE__);
		RETURN_LONG(EINVAL);
	}

	key_value_hash = Z_ARRVAL_P(key_values);
	key_count = zend_hash_num_elements(key_value_hash);
	if (key_count <= 0 || key_count > FDHT_MAX_KEY_COUNT_PER_REQ)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_batch_set, invalid key_count: %d!", \
			__LINE__, key_count);
		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_OBJECT(obj_info, szNamespace, szObjectId)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), "
		"expires=%ld", szNamespace, obj_info.namespace_len, 
		szObjectId, obj_info.obj_id_len, expires);
	*/

	memset(key_list, 0, sizeof(key_list));
	pKeyValue = key_list;
	ppp = &data;
	for (zend_hash_internal_pointer_reset_ex(key_value_hash, &pointer);
	     zend_hash_get_current_data_ex(key_value_hash, (void **)ppp,
		&pointer) == SUCCESS; zend_hash_move_forward_ex(
		key_value_hash, &pointer))
	{
		if (zend_hash_get_current_key_ex(key_value_hash, &szKey, \
			&(pKeyValue->key_len), &index, 0, &pointer) \
			== HASH_KEY_IS_STRING)
		{
			TRIM_PHP_KEY(szKey, pKeyValue->key_len)
		}
		else if (Z_TYPE_PP(data) == IS_STRING)
		{
			szKey = Z_STRVAL_PP(data);
			pKeyValue->key_len = Z_STRLEN_PP(data);
		}
		else
		{
			logError("file: "__FILE__", line: %d, " \
				"fastdht_batch_delete, invalid array element, "\
				"index=%d!", __LINE__, index);
			RETURN_LONG(EINVAL);
		}

		if (pKeyValue->key_len > FDHT_MAX_SUB_KEY_LEN)
		{
			pKeyValue->key_len = FDHT_MAX_SUB_KEY_LEN;
		}
		memcpy(pKeyValue->szKey, szKey, pKeyValue->key_len);

		/*
		logInfo("key=%s(%d)", 
			pKeyValue->szKey, pKeyValue->key_len);
		*/

		pKeyValue++;
	}

	result = fdht_batch_delete(&obj_info, key_list, key_count, \
				&success_count);
	if (result != 0)
	{
		RETURN_LONG(result);
	}

	if (success_count == key_count)
	{
		RETURN_LONG(result);
	}

	array_init(return_value);

	pKeyValueEnd = key_list + key_count;
	for (pKeyValue=key_list; pKeyValue<pKeyValueEnd; pKeyValue++)
	{
		add_assoc_long_ex(return_value, pKeyValue->szKey, \
				pKeyValue->key_len + 1, pKeyValue->status);
	}
}

/*
string/int fastdht_get(string namespace, string object_id, string key
		[, int expires])
return string value for success, int value (errno) for error
*/
ZEND_FUNCTION(fastdht_get)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	char *szKey;
	char *pValue;
	int value_len;
	long expires;
	int result;
	FDHTKeyInfo key_info;
	
	argc = ZEND_NUM_ARGS();
	if (argc != 3 && argc != 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_get parameters: %d != 3 or 4", 
			__LINE__, argc);

		RETURN_LONG(EINVAL);
	}

	expires = FDHT_EXPIRES_NONE;
	if (zend_parse_parameters(argc TSRMLS_CC, "sss|l", &szNamespace, 
		&key_info.namespace_len, &szObjectId, &key_info.obj_id_len, 
		&szKey, &key_info.key_len, &expires)
		 == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_get parameter parse error!", __LINE__);

		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_KEY(key_info, szNamespace, szObjectId, szKey)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), szKey=%s(%d), expires=%ld", 
		szNamespace, key_info.namespace_len, 
		szObjectId, key_info.obj_id_len, 
		szKey, key_info.key_len, expires);
	*/

	pValue = NULL;
	value_len = 0;
	if ((result=fdht_get_ex1(&g_group_array, g_keep_alive, &key_info, \
			expires, &pValue, &value_len, _emalloc)) != 0)
	{
		RETURN_LONG(result);
	}

	RETURN_STRINGL(pValue, value_len, 0);
}

/*
string/int fastdht_inc(string namespace, string object_id, string key, 
		int increment [, int expires])
return string value for success, int value (errno) for error
*/
ZEND_FUNCTION(fastdht_inc)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	char *szKey;
	char szValue[32];
	int value_len;
	long increment;
	long expires;
	int result;
	FDHTKeyInfo key_info;
	
	argc = ZEND_NUM_ARGS();
	if (argc != 4 && argc != 5)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_inc parameters: %d != 4 or 5", 
			__LINE__, argc);

		RETURN_LONG(EINVAL);
	}

	expires = FDHT_EXPIRES_NEVER;
	if (zend_parse_parameters(argc TSRMLS_CC, "sssl|l", &szNamespace, 
		&key_info.namespace_len, &szObjectId, &key_info.obj_id_len, 
		&szKey, &key_info.key_len, &increment, &expires)
		 == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_inc parameter parse error!", __LINE__);

		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_KEY(key_info, szNamespace, szObjectId, szKey)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), szKey=%s(%d), "
		"increment=%ld, expires=%ld", 
		szNamespace, key_info.namespace_len, 
		szObjectId, key_info.obj_id_len, 
		szKey, key_info.key_len, increment, expires);
	*/

	value_len = sizeof(szValue);
	if ((result=fdht_inc(&key_info, expires, increment, 
			szValue, &value_len)) != 0)
	{
		RETURN_LONG(result);
	}

	RETURN_STRINGL(szValue, value_len, 1);
}

/*
int fastdht_delete(string namespace, string object_id, string key)
return 0 for success, != 0 for error
*/
ZEND_FUNCTION(fastdht_delete)
{
	int argc;
	char *szNamespace;
	char *szObjectId;
	char *szKey;
	FDHTKeyInfo key_info;
	
	argc = ZEND_NUM_ARGS();
	if (argc != 3)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_delete parameters: %d != 3", 
			__LINE__, argc);

		RETURN_LONG(EINVAL);
	}

	if (zend_parse_parameters(argc TSRMLS_CC, "sss", &szNamespace, 
		&key_info.namespace_len, &szObjectId, &key_info.obj_id_len, 
		&szKey, &key_info.key_len) == FAILURE)
	{
		logError("file: "__FILE__", line: %d, " \
			"fastdht_delete parameter parse error!", __LINE__);

		RETURN_LONG(EINVAL);
	}

	FASTDHT_FILL_KEY(key_info, szNamespace, szObjectId, szKey)

	/*
	logInfo("szNamespace=%s(%d), szObjectId=%s(%d), szKey=%s(%d)", 
		szNamespace, key_info.namespace_len, 
		szObjectId, key_info.obj_id_len, 
		szKey, key_info.key_len);
	*/

	RETURN_LONG(fdht_delete(&key_info));
}

/* {{{ constructor/destructor */
static void php_fdht_destroy(php_fdht_t *i_obj TSRMLS_DC)
{
	fprintf(stderr, "php_fdht_destroy , obj=%X, zo=%X\n", (int)i_obj, (int)(&i_obj->zo));
	if (i_obj->pGroupArray != NULL && i_obj->pGroupArray != &g_group_array)
	{
		fdht_free_group_array(i_obj->pGroupArray);
		i_obj->pGroupArray = NULL;
	}

	efree(i_obj);
}

ZEND_RSRC_DTOR_FUNC(php_fdht_dtor)
{
	if (rsrc->ptr != NULL)
	{
		php_fdht_t *i_obj = (php_fdht_t *)rsrc->ptr;

		php_fdht_destroy(i_obj TSRMLS_CC);
		rsrc->ptr = NULL;
	}
}

/* {{{ FastDHT::__construct([bool shared])
   Creates a FastDHT object */
static PHP_METHOD(FastDHT, __construct)
{
	zval *object = getThis();
	php_fdht_t *i_obj;

	/*
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &persistent_id,
							  &persistent_id_len) == FAILURE) {
		ZVAL_NULL(object);
		return;
	}
	*/

	i_obj = (php_fdht_t *) zend_object_store_get_object(object TSRMLS_CC);
	i_obj->pGroupArray = &g_group_array;
}

/* {{{ FastDHT::get(string key [, mixed callback [, double &cas_token ] ])
   Returns a value for the given key or false */
PHP_METHOD(FastDHT, get)
{
	zval *object = getThis();
	php_fdht_t *i_obj;

	i_obj = (php_fdht_t *) zend_object_store_get_object(object TSRMLS_CC);
	RETURN_STRING("ok", 1);
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_get, 0, 0, 3)
	ZEND_ARG_INFO(0, szNamespace)
	ZEND_ARG_INFO(0, szObjectId)
	ZEND_ARG_INFO(0, szKey)
	ZEND_ARG_INFO(0, expires)
ZEND_END_ARG_INFO()


/* {{{ fdht_class_methods */
#define FDHT_ME(name, args) PHP_ME(FastDHT, name, args, ZEND_ACC_PUBLIC)
static zend_function_entry fdht_class_methods[] = {
    FDHT_ME(__construct,        arginfo___construct)
    FDHT_ME(get,                arginfo_get)
    { NULL, NULL, NULL }
};
#undef FDHT_ME
/* }}} */

static void php_fdht_free_storage(php_fdht_t *i_obj TSRMLS_DC)
{
	fprintf(stderr, "php_fdht_free_storage, obj=%X, zo=%X\n", (int)i_obj, (int)(&i_obj->zo));

	zend_object_std_dtor(&i_obj->zo TSRMLS_CC);
	php_fdht_destroy(i_obj TSRMLS_CC);
}

zend_object_value php_fdht_new(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value retval;
	php_fdht_t *i_obj;
	zval *tmp;

	i_obj = ecalloc(1, sizeof(*i_obj));
	zend_object_std_init( &i_obj->zo, ce TSRMLS_CC );
	zend_hash_copy(i_obj->zo.properties, &ce->default_properties, \
		(copy_ctor_func_t) zval_add_ref, (void *)&tmp, sizeof(zval *));

	retval.handle = zend_objects_store_put(i_obj, \
		(zend_objects_store_dtor_t)zend_objects_destroy_object, \
		(zend_objects_free_object_storage_t)php_fdht_free_storage, \
		NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();

	return retval;
}

PHP_FASTDHT_API zend_class_entry *php_fdht_get_ce(void)
{
	return fdht_ce;
}

PHP_FASTDHT_API zend_class_entry *php_fdht_get_exception(void)
{
	return fdht_exception_ce;
}

PHP_FASTDHT_API zend_class_entry *php_fdht_get_exception_base(int root TSRMLS_DC)
{
#if HAVE_SPL
	if (!root)
	{
		if (!spl_ce_RuntimeException)
		{
			zend_class_entry **pce;
			zend_class_entry ***ppce;

			ppce = &pce;
			if (zend_hash_find(CG(class_table), "runtimeexception",
			   sizeof("RuntimeException"), (void **) ppce) == SUCCESS)
			{
				spl_ce_RuntimeException = *pce;
				return *pce;
			}
		}
		else
		{
			return spl_ce_RuntimeException;
		}
	}
#endif
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2)
	return zend_exception_get_default();
#else
	return zend_exception_get_default(TSRMLS_C);
#endif
}


PHP_MINIT_FUNCTION(fastdht_client)
{
	#define ITEM_NAME_CONF_FILE "fastdht_client.config_file"
	zval conf_filename;
	zend_class_entry ce;

	if (zend_get_configuration_directive(ITEM_NAME_CONF_FILE, 
		sizeof(ITEM_NAME_CONF_FILE), &conf_filename) != SUCCESS)
	{
		fprintf(stderr, "file: "__FILE__", line: %d, " \
			"get param: %s from fastdht_client.ini fail!\n", 
			__LINE__, ITEM_NAME_CONF_FILE);

		return FAILURE;
	}

	if (fdht_client_init(conf_filename.value.str.val) != 0)
	{
		return FAILURE;
	}

	le_fdht = zend_register_list_destructors_ex(NULL, php_fdht_dtor, "FastDHT", module_number);

	INIT_CLASS_ENTRY(ce, "FastDHT", fdht_class_methods);
	fdht_ce = zend_register_internal_class(&ce TSRMLS_CC);
	fdht_ce->create_object = php_fdht_new;

	INIT_CLASS_ENTRY(ce, "FastDHTException", NULL);
	fdht_exception_ce = zend_register_internal_class_ex(&ce, php_fdht_get_exception_base(0 TSRMLS_CC), NULL TSRMLS_CC);

	REGISTER_LONG_CONSTANT("FDHT_EXPIRES_NEVER", 0, CONST_CS|CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("FDHT_EXPIRES_NONE", -1, CONST_CS|CONST_PERSISTENT);

	fprintf(stderr, "init done.\n");
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(fastdht_client)
{
	if (g_keep_alive)
	{
		fdht_disconnect_all_servers(&g_group_array);
	}

	fdht_client_destroy();

	return SUCCESS;
}

PHP_RINIT_FUNCTION(fastdht_client)
{
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(fastdht_client)
{
	fprintf(stderr, "request shut down. file: "__FILE__", line: %d\n", __LINE__);
	return SUCCESS;
}

PHP_MINFO_FUNCTION(fastdht_client)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "fastdht_client support", "enabled");
	php_info_print_table_end();

}

