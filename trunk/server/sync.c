/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//sync.c

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "fdht_define.h"
#include "logger.h"
#include "sockopt.h"
#include "shared_func.h"
#include "ini_file_reader.h"
#include "global.h"
#include "func.h"
#include "sync.h"

#define SYNC_BINLOG_FILE_MAX_SIZE	1024 * 1024 * 1024
#define SYNC_BINLOG_FILE_PREFIX		"binlog"
#define SYNC_BINLOG_INDEX_FILENAME	SYNC_BINLOG_FILE_PREFIX".index"
#define SYNC_MARK_FILE_EXT		".mark"
#define SYNC_BINLOG_FILE_EXT_FMT	".%03d"
#define SYNC_DIR_NAME			"sync"
#define MARK_ITEM_BINLOG_FILE_INDEX	"binlog_index"
#define MARK_ITEM_BINLOG_FILE_OFFSET	"binlog_offset"
#define MARK_ITEM_NEED_SYNC_OLD		"need_sync_old"
#define MARK_ITEM_SYNC_OLD_DONE		"sync_old_done"
#define MARK_ITEM_UNTIL_TIMESTAMP	"until_timestamp"
#define MARK_ITEM_SCAN_ROW_COUNT	"scan_row_count"
#define MARK_ITEM_SYNC_ROW_COUNT	"sync_row_count"

int g_binlog_fd = -1;
int g_binlog_index = 0;
static off_t binlog_file_size = 0;

int g_fdht_sync_thread_count = 0;
static pthread_mutex_t sync_thread_lock;

/* save sync thread ids */
static pthread_t *sync_tids = NULL;

static int fdht_write_to_mark_file(BinLogReader *pReader);
static int fdht_binlog_reader_skip(BinLogReader *pReader);
static void fdht_reader_destroy(BinLogReader *pReader);

/**
4 bytes: filename bytes
4 bytes: file size
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename bytes : filename
file size bytes: file content
**/
static int fdht_sync_set(FDHTServerInfo *pStorageServer, \
			const BinLogRecord *pRecord, const char proto_cmd)
{
	return 0;
}

/**
send pkg format:
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
remain bytes: filename
**/
static int fdht_sync_del(FDHTServerInfo *pStorageServer, \
			const BinLogRecord *pRecord)
{
	return 0;
}

#define STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord) \
	if ((!pReader->need_sync_old) || pReader->sync_old_done || \
		(pRecord->timestamp > pReader->until_timestamp)) \
	{ \
		return 0; \
	} \

static int fdht_sync_data(BinLogReader *pReader, \
			FDHTServerInfo *pStorageServer, \
			const BinLogRecord *pRecord)
{
	int result;
	switch(pRecord->op_type)
	{
		case FDHT_OP_TYPE_SOURCE_SET:
			result = fdht_sync_set(pStorageServer, \
				pRecord, FDHT_PROTO_CMD_SYNC_SET);
			break;
		case FDHT_OP_TYPE_SOURCE_DEL:
			result = fdht_sync_del( \
				pStorageServer, pRecord);
			break;
		case FDHT_OP_TYPE_REPLICA_SET:
			STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord)
			result = fdht_sync_set(pStorageServer, \
				pRecord, FDHT_PROTO_CMD_SYNC_SET);
			break;
		case FDHT_OP_TYPE_REPLICA_DEL:
			STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord)
			result = fdht_sync_del( \
				pStorageServer, pRecord);
			break;
		default:
			return EINVAL;
	}

	if (result == 0)
	{
		pReader->sync_row_count++;
	}

	return result;
}

static int write_to_binlog_index()
{
	char full_filename[MAX_PATH_SIZE];
	char buff[16];
	int fd;
	int len;

	snprintf(full_filename, sizeof(full_filename), \
			"%s/data/"SYNC_DIR_NAME"/%s", g_base_path, \
			SYNC_BINLOG_INDEX_FILENAME);
	if ((fd=open(full_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	len = sprintf(buff, "%d", g_binlog_index);
	if (write(fd, buff, len) != len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, full_filename, \
			errno, strerror(errno));
		close(fd);
		return errno != 0 ? errno : EIO;
	}

	close(fd);
	return 0;
}

static char *get_writable_binlog_filename(char *full_filename)
{
	static char buff[MAX_PATH_SIZE];

	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/"SYNC_BINLOG_FILE_PREFIX"" \
			SYNC_BINLOG_FILE_EXT_FMT, \
			g_base_path, g_binlog_index);
	return full_filename;
}

static int open_next_writable_binlog()
{
	char full_filename[MAX_PATH_SIZE];

	fdht_sync_destroy();

	get_writable_binlog_filename(full_filename);
	if (fileExists(full_filename))
	{
		if (unlink(full_filename) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"unlink file \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, full_filename, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}

		logError("file: "__FILE__", line: %d, " \
			"binlog file \"%s\" already exists, truncate", \
			__LINE__, full_filename);
	}

	g_binlog_fd = open(full_filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (g_binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	return 0;
}

int fdht_sync_init()
{
	char data_path[MAX_PATH_SIZE];
	char sync_path[MAX_PATH_SIZE];
	char full_filename[MAX_PATH_SIZE];
	char file_buff[64];
	int bytes;
	int result;
	int fd;

	snprintf(data_path, sizeof(data_path), \
			"%s/data", g_base_path);
	if (!fileExists(data_path))
	{
		if (mkdir(data_path, 0755) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"mkdir \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, data_path, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}
	}

	snprintf(sync_path, sizeof(sync_path), \
			"%s/"SYNC_DIR_NAME, data_path);
	if (!fileExists(sync_path))
	{
		if (mkdir(sync_path, 0755) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"mkdir \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, sync_path, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}
	}

	snprintf(full_filename, sizeof(full_filename), \
			"%s/%s", sync_path, SYNC_BINLOG_INDEX_FILENAME);
	if ((fd=open(full_filename, O_RDONLY)) >= 0)
	{
		bytes = read(fd, file_buff, sizeof(file_buff) - 1);
		close(fd);
		if (bytes <= 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"read file \"%s\" fail, bytes read: %d", \
				__LINE__, full_filename, bytes);
			return errno != 0 ? errno : EIO;
		}

		file_buff[bytes] = '\0';
		g_binlog_index = atoi(file_buff);
		if (g_binlog_index < 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"in file \"%s\", binlog_index: %d < 0", \
				__LINE__, full_filename, g_binlog_index);
			return EINVAL;
		}
	}
	else
	{
		g_binlog_index = 0;
		if ((result=write_to_binlog_index()) != 0)
		{
			return result;
		}
	}

	get_writable_binlog_filename(full_filename);
	g_binlog_fd = open(full_filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (g_binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	binlog_file_size = lseek(g_binlog_fd, 0, SEEK_END);
	if (binlog_file_size < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"ftell file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		fdht_sync_destroy();
		return errno != 0 ? errno : EIO;
	}

	/*
	//printf("full_filename=%s, binlog_file_size=%d\n", \
			full_filename, binlog_file_size);
	*/
	
	if ((result=init_pthread_lock(&sync_thread_lock)) != 0)
	{
		return result;
	}

	load_local_host_ip_addrs();

	return 0;
}

int fdht_sync_destroy()
{
	int result;
	if (g_binlog_fd >= 0)
	{
		close(g_binlog_fd);
		g_binlog_fd = -1;
	}

	if ((result=pthread_mutex_destroy(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_destroy fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
		return result;
	}


	return 0;
}

int kill_fdht_sync_threads()
{
	int result;

	if (sync_tids != NULL)
	{
		result = kill_work_threads(sync_tids, \
				g_fdht_sync_thread_count);

		free(sync_tids);
		sync_tids = NULL;
	}
	else
	{
		result = 0;
	}

	return result;
}

static char *get_binlog_readable_filename(BinLogReader *pReader, \
		char *full_filename)
{
	static char buff[MAX_PATH_SIZE];

	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/"SYNC_BINLOG_FILE_PREFIX"" \
			SYNC_BINLOG_FILE_EXT_FMT, \
			g_base_path, pReader->binlog_index);
	return full_filename;
}

static int fdht_open_readable_binlog(BinLogReader *pReader)
{
	char full_filename[MAX_PATH_SIZE];

	if (pReader->binlog_fd >= 0)
	{
		close(pReader->binlog_fd);
	}

	get_binlog_readable_filename(pReader, full_filename);
	pReader->binlog_fd = open(full_filename, O_RDONLY);
	if (pReader->binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open binlog file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	if (pReader->binlog_offset > 0 && \
	    lseek(pReader->binlog_fd, pReader->binlog_offset, SEEK_SET) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"seek binlog file \"%s\" fail, file offset="INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, pReader->binlog_offset, \
			errno, strerror(errno));

		close(pReader->binlog_fd);
		pReader->binlog_fd = -1;
		return errno != 0 ? errno : ESPIPE;
	}

	return 0;
}

static char *get_mark_filename(const void *pArg, \
			char *full_filename)
{
	const BinLogReader *pReader;
	static char buff[MAX_PATH_SIZE];

	pReader = (const BinLogReader *)pArg;
	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/%s_%d%s", g_base_path, \
			pReader->ip_addr, g_server_port, SYNC_MARK_FILE_EXT);
	return full_filename;
}

static int fdht_reader_sync_init(BinLogReader *pReader)
{
	int result;

	if (g_server_stat.success_set_count + g_server_stat.success_inc_count \
		+ g_server_stat.success_delete_count == 0)
	{
	}

	if ((result=fdht_write_to_mark_file(pReader)) != 0)
	{
		return result;
	}

	return 0;
}

static int fdht_reader_init(FDHTServerInfo *pStorage, \
			BinLogReader *pReader)
{
	char full_filename[MAX_PATH_SIZE];
	IniItemInfo *items;
	int nItemCount;
	int result;
	bool bFileExist;

	memset(pReader, 0, sizeof(BinLogReader));
	pReader->mark_fd = -1;
	pReader->binlog_fd = -1;

	strcpy(pReader->ip_addr, pStorage->ip_addr);
	get_mark_filename(pReader, full_filename);
	bFileExist = fileExists(full_filename);
	if (bFileExist)
	{
		if ((result=iniLoadItems(full_filename, &items, &nItemCount)) \
			 != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"load from mark file \"%s\" fail, " \
				"error code: %d", \
				__LINE__, full_filename, result);
			return result;
		}

		if (nItemCount < 7)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", item count: %d < 7", \
				__LINE__, full_filename, nItemCount);
			return ENOENT;
		}

		pReader->binlog_index = iniGetIntValue( \
				MARK_ITEM_BINLOG_FILE_INDEX, \
				items, nItemCount, -1);
		pReader->binlog_offset = iniGetInt64Value( \
				MARK_ITEM_BINLOG_FILE_OFFSET, \
				items, nItemCount, -1);
		pReader->need_sync_old = iniGetBoolValue(   \
				MARK_ITEM_NEED_SYNC_OLD, \
				items, nItemCount);
		pReader->sync_old_done = iniGetBoolValue(  \
				MARK_ITEM_SYNC_OLD_DONE, \
				items, nItemCount);
		pReader->until_timestamp = iniGetIntValue( \
				MARK_ITEM_UNTIL_TIMESTAMP, \
				items, nItemCount, -1);
		pReader->scan_row_count = iniGetInt64Value( \
				MARK_ITEM_SCAN_ROW_COUNT, \
				items, nItemCount, 0);
		pReader->sync_row_count = iniGetInt64Value( \
				MARK_ITEM_SYNC_ROW_COUNT, \
				items, nItemCount, 0);

		if (pReader->binlog_index < 0)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", " \
				"binlog_index: %d < 0", \
				__LINE__, full_filename, \
				pReader->binlog_index);
			return EINVAL;
		}
		if (pReader->binlog_offset < 0)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", " \
				"binlog_offset: "INT64_PRINTF_FORMAT" < 0", \
				__LINE__, full_filename, \
				pReader->binlog_offset);
			return EINVAL;
		}

		iniFreeItems(items);
	}
	else
	{
		if ((result=fdht_reader_sync_init(pReader)) != 0)
		{
			return result;
		}
	}

	pReader->last_write_row_count = pReader->scan_row_count;

	pReader->mark_fd = open(full_filename, O_WRONLY | O_CREAT, 0644);
	if (pReader->mark_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open mark file \"%s\" fail, " \
			"error no: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	if ((result=fdht_open_readable_binlog(pReader)) != 0)
	{
		close(pReader->mark_fd);
		pReader->mark_fd = -1;
		return result;
	}

	if (!bFileExist)
	{
        	if (!pReader->need_sync_old && pReader->until_timestamp > 0)
		{
			if ((result=fdht_binlog_reader_skip(pReader)) != 0)
			{
				fdht_reader_destroy(pReader);
				return result;
			}
		}

		if ((result=fdht_write_to_mark_file(pReader)) != 0)
		{
			fdht_reader_destroy(pReader);
			return result;
		}
	}

	return 0;
}

static void fdht_reader_destroy(BinLogReader *pReader)
{
	if (pReader->mark_fd >= 0)
	{
		close(pReader->mark_fd);
		pReader->mark_fd = -1;
	}

	if (pReader->binlog_fd >= 0)
	{
		close(pReader->binlog_fd);
		pReader->binlog_fd = -1;
	}
}

static int fdht_write_to_mark_file(BinLogReader *pReader)
{
	char buff[256];
	int len;
	int result;

	len = sprintf(buff, 
		"%s=%d\n"  \
		"%s="INT64_PRINTF_FORMAT"\n"  \
		"%s=%d\n"  \
		"%s=%d\n"  \
		"%s=%d\n"  \
		"%s="INT64_PRINTF_FORMAT"\n"  \
		"%s="INT64_PRINTF_FORMAT"\n", \
		MARK_ITEM_BINLOG_FILE_INDEX, pReader->binlog_index, \
		MARK_ITEM_BINLOG_FILE_OFFSET, pReader->binlog_offset, \
		MARK_ITEM_NEED_SYNC_OLD, pReader->need_sync_old, \
		MARK_ITEM_SYNC_OLD_DONE, pReader->sync_old_done, \
		MARK_ITEM_UNTIL_TIMESTAMP, (int)pReader->until_timestamp, \
		MARK_ITEM_SCAN_ROW_COUNT, pReader->scan_row_count, \
		MARK_ITEM_SYNC_ROW_COUNT, pReader->sync_row_count);
	if ((result=fdht_write_to_fd(pReader->mark_fd, get_mark_filename, \
		pReader, buff, len)) == 0)
	{
		pReader->last_write_row_count = pReader->scan_row_count;
	}

	return result;
}

static int rewind_to_prev_rec_end(BinLogReader *pReader, \
			const int record_length)
{
	if (lseek(pReader->binlog_fd, -1 * record_length, \
			SEEK_CUR) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"seek binlog file \"%s\"fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	return 0;
}

#define BINLOG_FIX_FIELDS_LENGTH  3 * 10 + 1 + 4 * 1

int fdht_binlog_write(const char op_type, const char *pKey, const int key_len, \
		const char *pValue, const int value_len)
{
	struct flock lock;
	char buff[64];
	int write_bytes;
	int result;

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	if (fcntl(g_binlog_fd, F_SETLKW, &lock) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"lock binlog file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	while (1)
	{
		write_bytes = sprintf(buff, "%10d %c %10d %10d ", \
				(int)time(NULL), op_type,  \
				key_len, value_len);
		if (write(g_binlog_fd, buff, write_bytes) != write_bytes)
		{
			logError("file: "__FILE__", line: %d, " \
				"write to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}

		if (write(g_binlog_fd, pKey, key_len) != key_len)
		{
			logError("file: "__FILE__", line: %d, " \
				"write to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}

		if (write(g_binlog_fd, " ", 1) != 1)
		{
			logError("file: "__FILE__", line: %d, " \
				"write to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}

		if (value_len > 0 && write(g_binlog_fd, pValue, \
			value_len) != value_len)
		{
			logError("file: "__FILE__", line: %d, " \
				"write to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}
		if (write(g_binlog_fd, "\n", 1) != 1)
		{
			logError("file: "__FILE__", line: %d, " \
				"write to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}

		if (fsync(g_binlog_fd) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"sync to binlog file \"%s\" fail, " \
				"errno: %d, error info: %s",  \
				__LINE__, get_writable_binlog_filename(NULL), \
				errno, strerror(errno));
			result = errno != 0 ? errno : EIO;
			break;
		}

		binlog_file_size += BINLOG_FIX_FIELDS_LENGTH + key_len + 1 + \
					value_len + 1;

		if (binlog_file_size >= SYNC_BINLOG_FILE_MAX_SIZE)
		{
			g_binlog_index++;
			if ((result=write_to_binlog_index()) == 0)
			{
				result = open_next_writable_binlog();
			}

			binlog_file_size = 0;
			if (result != 0)
			{
				g_continue_flag = false;
				logCrit("file: "__FILE__", line: %d, " \
					"open binlog file \"%s\" fail, " \
					"program exit!", \
					__LINE__, \
					get_writable_binlog_filename(NULL));
			}
		}
		else
		{
			result = 0;
		}

		break;
	}

	lock.l_type = F_UNLCK;
	if (fcntl(g_binlog_fd, F_SETLKW, &lock) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"unlock binlog file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	return result;
}

static int fdht_binlog_read(BinLogReader *pReader, \
			BinLogRecord *pRecord, int *record_length)
{
	char buff[BINLOG_FIX_FIELDS_LENGTH + 1];
	int result;
	int read_bytes;
	int nItem;

	*record_length = 0;
	while (1)
	{
		read_bytes = read(pReader->binlog_fd, buff, \
				BINLOG_FIX_FIELDS_LENGTH);
		if (read_bytes == 0)  //end of file
		{
			if (pReader->binlog_index < g_binlog_index) //rotate
			{
				pReader->binlog_index++;
				pReader->binlog_offset = 0;
				if ((result=fdht_open_readable_binlog( \
						pReader)) != 0)
				{
					return result;
				}

				if ((result=fdht_write_to_mark_file( \
						pReader)) != 0)
				{
					return result;
				}

				continue;  //read next binlog
			}

			return ENOENT;

		}

		if (read_bytes < 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"read from binlog file \"%s\" fail, " \
				"file offset: "INT64_PRINTF_FORMAT", " \
				"errno: %d, error info: %s", __LINE__, \
				get_binlog_readable_filename(pReader, NULL), \
				pReader->binlog_offset, errno, strerror(errno));
			return errno != 0 ? errno : EIO;
		}

		break;
	}

	if (read_bytes != BINLOG_FIX_FIELDS_LENGTH)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
				read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, \
			BINLOG_FIX_FIELDS_LENGTH);
		return ENOENT;
	}

	buff[read_bytes] = '\0';
	if ((nItem=sscanf(buff, "%10d %c %10d %10d ", \
			(int *)&(pRecord->timestamp), \
			&(pRecord->op_type), &(pRecord->key.length), \
			&(pRecord->value.length))) != 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"data format invalid, binlog file: %s, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read item: %d != 4", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, nItem);
		return ENOENT;
	}

	if (pRecord->key.length <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"item \"key length\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"key length: %d <= 0", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, pRecord->key.length);
		return EINVAL;
	}
	if (pRecord->value.length < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"item \"value length\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"value length: %d < 0", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, pRecord->value.length);
		return EINVAL;
	}

	if (pRecord->key.length > pRecord->key.size)
	{
		pRecord->key.size = pRecord->key.length;
		pRecord->key.data = (char *)realloc(pRecord->key.data, \
						pRecord->key.size);
		if (pRecord->key.data == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"realloc %d bytes fail, " \
				"errno: %d, error info: %s", \
				pRecord->key.size, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}
	}

	read_bytes = read(pReader->binlog_fd, pRecord->key.data, \
			pRecord->key.length);
	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (read_bytes != pRecord->key.length)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, pRecord->key.length);
		return ENOENT;
	}

	if (read(pReader->binlog_fd, buff, 1) != 1) //skip space
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != 1", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes);
		return ENOENT;
	}

	if (pRecord->value.length > pRecord->value.size)
	{
		pRecord->value.size = pRecord->value.length;
		pRecord->value.data = (char *)realloc(pRecord->value.data, \
						pRecord->value.size);
		if (pRecord->value.data == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"realloc %d bytes fail, " \
				"errno: %d, error info: %s", \
				pRecord->value.size, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}
	}

	read_bytes = read(pReader->binlog_fd, pRecord->value.data, \
			pRecord->value.length);
	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (read_bytes != pRecord->value.length)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length + 1 + \
			read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, pRecord->value.length);
		return ENOENT;
	}

	if (read(pReader->binlog_fd, buff, 1) != 1) //skip \n
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length + 1 + \
			pRecord->value.length)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != 1", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes);
		return ENOENT;
	}

	*record_length = BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length + 1 + \
			pRecord->value.length + 1;

	printf("timestamp=%d, op_type=%c, key len=%d, value len=%d, " \
		"record length=%d, offset=%d\n", \
		(int)pRecord->timestamp, pRecord->op_type, \
		pRecord->key.length, pRecord->value.length, \
		*record_length, (int)pReader->binlog_offset);

	return 0;
}

static int fdht_binlog_reader_skip(BinLogReader *pReader)
{
	BinLogRecord record;
	int result;
	int record_len;

	while (1)
	{
		result = fdht_binlog_read(pReader, \
				&record, &record_len);
		if (result != 0)
		{
			if (result == ENOENT)
			{
				return 0;
			}

			return result;
		}

		if (record.timestamp >= pReader->until_timestamp)
		{
			result = rewind_to_prev_rec_end( \
					pReader, record_len);
			break;
		}

		pReader->binlog_offset += record_len;
	}

	return result;
}

static void* fdht_sync_thread_entrance(void* arg)
{
	FDHTServerInfo *pStorage;
	BinLogReader reader;
	BinLogRecord record;
	FDHTServerInfo fdht_server;
	char local_ip_addr[IP_ADDRESS_SIZE];
	int read_result;
	int sync_result;
	int conn_result;
	int result;
	int record_len;
	int previousCode;
	int nContinuousFail;
	
	memset(local_ip_addr, 0, sizeof(local_ip_addr));
	memset(&reader, 0, sizeof(reader));

	pStorage = (FDHTServerInfo *)arg;

	strcpy(fdht_server.ip_addr, pStorage->ip_addr);
	fdht_server.port = g_server_port;
	fdht_server.sock = -1;
	while (g_continue_flag)
	{
		if (fdht_reader_init(pStorage, &reader) != 0)
		{
			logCrit("file: "__FILE__", line: %d, " \
				"fdht_reader_init fail, program exit!", \
				__LINE__);
			g_continue_flag = false;
			break;
		}

		/*
		while (g_continue_flag && \
			(pStorage->status != FDFS_FDHT_STATUS_ACTIVE && \
			pStorage->status != FDFS_FDHT_STATUS_WAIT_SYNC && \
			pStorage->status != FDFS_FDHT_STATUS_SYNCING))
		{
			sleep(1);
		}
		*/

		previousCode = 0;
		nContinuousFail = 0;
		conn_result = 0;
		while (g_continue_flag)
		{
			fdht_server.sock = \
				socket(AF_INET, SOCK_STREAM, 0);
			if(fdht_server.sock < 0)
			{
				logCrit("file: "__FILE__", line: %d," \
					" socket create fail, " \
					"errno: %d, error info: %s. " \
					"program exit!", __LINE__, \
					errno, strerror(errno));
				g_continue_flag = false;
				break;
			}

			if ((conn_result=connectserverbyip(fdht_server.sock,\
				fdht_server.ip_addr, g_server_port)) == 0)
			{
				char szFailPrompt[36];
				if (nContinuousFail == 0)
				{
					*szFailPrompt = '\0';
				}
				else
				{
					sprintf(szFailPrompt, \
						", continuous fail count: %d", \
						nContinuousFail);
				}
				logInfo("file: "__FILE__", line: %d, " \
					"successfully connect to " \
					"storage server %s:%d%s", __LINE__, \
					fdht_server.ip_addr, \
					g_server_port, szFailPrompt);
				nContinuousFail = 0;
				break;
			}

			if (previousCode != conn_result)
			{
				logError("file: "__FILE__", line: %d, " \
					"connect to storage server %s:%d fail" \
					", errno: %d, error info: %s", \
					__LINE__, \
					fdht_server.ip_addr, g_server_port, \
					conn_result, strerror(conn_result));
				previousCode = conn_result;
			}

			nContinuousFail++;
			close(fdht_server.sock);
			fdht_server.sock = -1;
			sleep(1);
		}

		if (nContinuousFail > 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"connect to storage server %s:%d fail, " \
				"try count: %d, errno: %d, error info: %s", \
				__LINE__, fdht_server.ip_addr, \
				g_server_port, nContinuousFail, \
				conn_result, strerror(conn_result));
		}

		if (!g_continue_flag)
		{
			break;
		}

		getSockIpaddr(fdht_server.sock, \
			local_ip_addr, IP_ADDRESS_SIZE);

		/*
		//printf("file: "__FILE__", line: %d, " \
			"fdht_server.ip_addr=%s, " \
			"local_ip_addr: %s\n", \
			__LINE__, fdht_server.ip_addr, local_ip_addr);
		*/

		if (strcmp(local_ip_addr, fdht_server.ip_addr) == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"ip_addr %s belong to the local host," \
				" sync thread exit.", \
				__LINE__, fdht_server.ip_addr);
			fdht_quit(&fdht_server);
			break;
		}

		/*
		if (pStorage->status == FDFS_FDHT_STATUS_WAIT_SYNC)
		{
			pStorage->status = FDFS_FDHT_STATUS_SYNCING;
			fdht_report_fdht_status(pStorage->ip_addr, \
				pStorage->status);
		}
		if (pStorage->status == FDFS_FDHT_STATUS_SYNCING)
		{
			if (reader.need_sync_old && reader.sync_old_done)
			{
				pStorage->status = FDFS_FDHT_STATUS_ONLINE;
				fdht_report_fdht_status(  \
					pStorage->ip_addr, \
					pStorage->status);
			}
		}
		*/

		sync_result = 0;
		while (g_continue_flag)
		{
			read_result = fdht_binlog_read(&reader, \
					&record, &record_len);
			if (read_result == ENOENT)
			{
				if (reader.need_sync_old && \
					!reader.sync_old_done)
				{
				reader.sync_old_done = true;
				if (fdht_write_to_mark_file(&reader) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"fdht_write_to_mark_file " \
						"fail, program exit!", \
						__LINE__);
					g_continue_flag = false;
					break;
				}

				/*
				if (pStorage->status == \
					FDFS_FDHT_STATUS_SYNCING)
				{
					pStorage->status = \
						FDFS_FDHT_STATUS_ONLINE;
					fdht_report_fdht_status(  \
						pStorage->ip_addr, \
						pStorage->status);
				}
				*/
				}

				usleep(g_sync_wait_usec);
				continue;
			}
			else if (read_result != 0)
			{
				logCrit("file: "__FILE__", line: %d, " \
					"fdht_binlog_read fail, " \
					"program exit!", __LINE__);
				g_continue_flag = false;
				break;
			}

			if ((sync_result=fdht_sync_data(&reader, \
				&fdht_server, &record)) != 0)
			{
				if (rewind_to_prev_rec_end( \
					&reader, record_len) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"rewind_to_prev_rec_end fail, "\
						"program exit!", __LINE__);
					g_continue_flag = false;
				}

				break;
			}

			reader.binlog_offset += record_len;
			if (++reader.scan_row_count % 100 == 0)
			{
				if (fdht_write_to_mark_file(&reader) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"fdht_write_to_mark_file " \
						"fail, program exit!", \
						__LINE__);
					g_continue_flag = false;
					break;
				}
			}
		}

		if (reader.last_write_row_count != reader.scan_row_count)
		{
			if (fdht_write_to_mark_file(&reader) != 0)
			{
				logCrit("file: "__FILE__", line: %d, " \
					"fdht_write_to_mark_file fail, " \
					"program exit!", __LINE__);
				g_continue_flag = false;
				break;
			}
		}

		close(fdht_server.sock);
		fdht_server.sock = -1;
		fdht_reader_destroy(&reader);

		if (!g_continue_flag)
		{
			break;
		}

		if (!(sync_result == ENOTCONN || sync_result == EIO))
		{
			sleep(1);
		}
	}

	if (fdht_server.sock >= 0)
	{
		close(fdht_server.sock);
	}
	fdht_reader_destroy(&reader);

	if ((result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}
	g_fdht_sync_thread_count--;
	if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return NULL;
}

int fdht_sync_thread_start(const FDHTServerInfo *pStorage)
{
	int result;
	pthread_attr_t pattr;
	pthread_t tid;

	if (is_local_host_ip(pStorage->ip_addr)) //can't self sync to self
	{
		return 0;
	}

	if ((result=init_pthread_attr(&pattr)) != 0)
	{
		return result;
	}

	/*
	//printf("start storage ip_addr: %s, g_fdht_sync_thread_count=%d\n", 
			pStorage->ip_addr, g_fdht_sync_thread_count);
	*/

	if ((result=pthread_create(&tid, &pattr, fdht_sync_thread_entrance, \
		(void *)pStorage)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"create thread failed, errno: %d, " \
			"error info: %s", \
			__LINE__, result, strerror(result));

		pthread_attr_destroy(&pattr);
		return result;
	}

	if ((result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	g_fdht_sync_thread_count++;
	sync_tids = (pthread_t *)realloc(sync_tids, sizeof(pthread_t) * \
					g_fdht_sync_thread_count);
	if (sync_tids == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, sizeof(pthread_t) * \
			g_fdht_sync_thread_count, \
			errno, strerror(errno));
	}
	else
	{
		sync_tids[g_fdht_sync_thread_count - 1] = tid;
	}

	if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	pthread_attr_destroy(&pattr);

	return 0;
}
