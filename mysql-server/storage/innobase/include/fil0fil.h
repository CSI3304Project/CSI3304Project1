/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/fil0fil.h
The low-level file system

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#ifndef fil0fil_h
#define fil0fil_h

#include "univ.i"

#ifndef UNIV_INNOCHECKSUM

#include "dict0types.h"
#include "buf0types.h"
#include "hash0hash.h"
#include "page0size.h"
#include "mtr0types.h"
#include "ut0new.h"
#ifndef UNIV_HOTBACKUP
#include "ibuf0types.h"
#include "log0log.h"
#endif /* !UNIV_HOTBACKUP */

#include <list>
#include <vector>

// Forward declaration
struct trx_t;
class truncate_t;
struct btr_create_t;
class page_id_t;

typedef std::list<char*, ut_allocator<char*> >	space_name_list_t;

/** File types */
enum fil_type_t {
	/** temporary tablespace (temporary undo log or tables) */
	FIL_TYPE_TEMPORARY,
	/** a tablespace that is being imported (no logging until finished) */
	FIL_TYPE_IMPORT,
	/** persistent tablespace (for system, undo log or tables) */
	FIL_TYPE_TABLESPACE,
	/** redo log covering changes to files of FIL_TYPE_TABLESPACE */
	FIL_TYPE_LOG
};

/** Check if fil_type is any of FIL_TYPE_TEMPORARY, FIL_TYPE_IMPORT
or FIL_TYPE_TABLESPACE.
@param[in]	type	variable of type fil_type_t
@return true if any of FIL_TYPE_TEMPORARY, FIL_TYPE_IMPORT
or FIL_TYPE_TABLESPACE */
inline
bool
fil_type_is_data(
	fil_type_t	type)
{
	return(type == FIL_TYPE_TEMPORARY
	       || type == FIL_TYPE_IMPORT
	       || type == FIL_TYPE_TABLESPACE);
}

/** Tablespace or log data space */
struct fil_space_t {
	char*		name;	/*!< Tablespace name */
	ulint		id;	/*!< space id */
	int64_t		tablespace_version;
				/*!< in DISCARD/IMPORT this timestamp
				is used to check if we should ignore
				an insert buffer merge request for a
				page because it actually was for the
				previous incarnation of the space */
	lsn_t		max_lsn;
				/*!< LSN of the most recent fil_names_dirty().
				Reset to 0 by fil_names_clear().
				Protected by log_sys->mutex and
				sometimes by fil_system->mutex:

				Updates from nonzero to nonzero
				are only protected by log_sys->mutex.

				Updates between 0 and nonzero are
				protected by log_sys->mutex and
				fil_system->mutex.

				If and only if this is nonzero, the
				tablespace will be in named_spaces,
				which is protected by fil_system->mutex. */
	bool		stop_ios;/*!< true if we want to rename the
				.ibd file of tablespace and want to
				stop temporarily posting of new i/o
				requests on the file */
	bool		stop_new_ops;
				/*!< we set this true when we start
				deleting a single-table tablespace.
				When this is set following new ops
				are not allowed:
				* read IO request
				* ibuf merge
				* file flush
				Note that we can still possibly have
				new write operations because we don't
				check this flag when doing flush
				batches. */
	bool		is_being_truncated;
				/*!< this is set to true when we prepare to
				truncate a single-table tablespace and its
				.ibd file */
#ifdef UNIV_DEBUG
	ulint		redo_skipped_count;
				/*!< reference count for operations who want
				to skip redo log in the file space in order
				to make fsp_space_modify_check pass. */
#endif
	fil_type_t	purpose;/*!< purpose */
	UT_LIST_BASE_NODE_T(fil_node_t) chain;
				/*!< base node for the file chain */
	ulint		size;	/*!< space size in pages; 0 if a single-table
				tablespace whose size we do not know yet;
				last incomplete megabytes in data files may be
				ignored if space == 0 */
	ulint		flags;	/*!< tablespace flags; see
				fsp_flags_is_valid(),
				page_size_t(ulint) (constructor) */
	ulint		n_reserved_extents;
				/*!< number of reserved free extents for
				ongoing operations like B-tree page split */
	ulint		n_pending_flushes; /*!< this is positive when flushing
				the tablespace to disk; dropping of the
				tablespace is forbidden if this is positive */
	ulint		n_pending_ops;/*!< this is positive when we
				have pending operations against this
				tablespace. The pending operations can
				be ibuf merges or lock validation code
				trying to read a block.
				Dropping of the tablespace is forbidden
				if this is positive */
	hash_node_t	hash;	/*!< hash chain node */
	hash_node_t	name_hash;/*!< hash chain the name_hash table */
#ifndef UNIV_HOTBACKUP
	rw_lock_t	latch;	/*!< latch protecting the file space storage
				allocation */
#endif /* !UNIV_HOTBACKUP */
	UT_LIST_NODE_T(fil_space_t) unflushed_spaces;
				/*!< list of spaces with at least one unflushed
				file we have written to */
	UT_LIST_NODE_T(fil_space_t) named_spaces;
				/*!< list of spaces for which MLOG_FILE_NAME
				records have been issued */
	bool		is_in_unflushed_spaces;
				/*!< true if this space is currently in
				unflushed_spaces */
	UT_LIST_NODE_T(fil_space_t) space_list;
				/*!< list of all spaces */
	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */
};

/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** When mysqld is run, the default directory "." is the mysqld datadir,
but in the MySQL Embedded Server Library and mysqlbackup it is not the default
directory, and we must set the base file path explicitly */
extern const char*	fil_path_to_mysql_datadir;

/** Common InnoDB file extentions */
enum ib_extention {
	NO_EXT = 0,
	IBD = 1,
	ISL = 2,
	CFG = 3
};
extern const char* dot_ext[];
#define DOT_IBD dot_ext[IBD]
#define DOT_ISL dot_ext[ISL]
#define DOT_CFG dot_ext[CFG]

/** Initial size of a single-table tablespace in pages */
#define FIL_IBD_FILE_INITIAL_SIZE	4

/** 'null' (undefined) page offset in the context of file spaces */
#define	FIL_NULL	ULINT32_UNDEFINED

/* Space address data type; this is intended to be used when
addresses accurate to a byte are stored in file pages. If the page part
of the address is FIL_NULL, the address is considered undefined. */

typedef	byte	fil_faddr_t;	/*!< 'type' definition in C: an address
				stored in a file page is a string of bytes */
#define FIL_ADDR_PAGE	0	/* first in address is the page offset */
#define	FIL_ADDR_BYTE	4	/* then comes 2-byte byte offset within page*/
#endif /* !UNIV_INNOCHECKSUM */
#define	FIL_ADDR_SIZE	6	/* address size is 6 bytes */

#ifndef UNIV_INNOCHECKSUM

/** File space address */
struct fil_addr_t {
	ulint	page;		/*!< page number within a space */
	ulint	boffset;	/*!< byte offset within the page */
};

/** The null file address */
extern fil_addr_t	fil_addr_null;

#endif /* !UNIV_INNOCHECKSUM */

/** The byte offsets on a file page for various variables @{ */
#define FIL_PAGE_SPACE_OR_CHKSUM 0	/*!< in < MySQL-4.0.14 space id the
					page belongs to (== 0) but in later
					versions the 'new' checksum of the
					page */
#define FIL_PAGE_OFFSET		4	/*!< page offset inside space */
#define FIL_PAGE_PREV		8	/*!< if there is a 'natural'
					predecessor of the page, its
					offset.  Otherwise FIL_NULL.
					This field is not set on BLOB
					pages, which are stored as a
					singly-linked list.  See also
					FIL_PAGE_NEXT. */
#define FIL_PAGE_NEXT		12	/*!< if there is a 'natural' successor
					of the page, its offset.
					Otherwise FIL_NULL.
					B-tree index pages
					(FIL_PAGE_TYPE contains FIL_PAGE_INDEX)
					on the same PAGE_LEVEL are maintained
					as a doubly linked list via
					FIL_PAGE_PREV and FIL_PAGE_NEXT
					in the collation order of the
					smallest user record on each page. */
#define FIL_PAGE_LSN		16	/*!< lsn of the end of the newest
					modification log record to the page */
#define	FIL_PAGE_TYPE		24	/*!< file page type: FIL_PAGE_INDEX,...,
					2 bytes.

					The contents of this field can only
					be trusted in the following case:
					if the page is an uncompressed
					B-tree index page, then it is
					guaranteed that the value is
					FIL_PAGE_INDEX.
					The opposite does not hold.

					In tablespaces created by
					MySQL/InnoDB 5.1.7 or later, the
					contents of this field is valid
					for all uncompressed pages. */
#define FIL_PAGE_FILE_FLUSH_LSN	26	/*!< this is only defined for the
					first page of the system tablespace:
					the file has been flushed to disk
					at least up to this lsn */
#define	FIL_RTREE_SPLIT_SEQ_NUM	26	/*!< This overloads
					FIL_PAGE_FILE_FLUSH_LSN for RTREE
					Split Sequence Number */
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID  34 /*!< starting from 4.1.x this
					contains the space id of the page */
#define FIL_PAGE_SPACE_ID  FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID

#define FIL_PAGE_DATA		38	/*!< start of the data on the page */
/* @} */
/** File page trailer @{ */
#define FIL_PAGE_END_LSN_OLD_CHKSUM 8	/*!< the low 4 bytes of this are used
					to store the page checksum, the
					last 4 bytes should be identical
					to the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_DATA_END	8	/*!< size of the page trailer */
/* @} */

/** File page types (values of FIL_PAGE_TYPE) @{ */
#define FIL_PAGE_INDEX		17855	/*!< B-tree node */
#define FIL_PAGE_RTREE		17854	/*!< B-tree node */
#define FIL_PAGE_UNDO_LOG	2	/*!< Undo log page */
#define FIL_PAGE_INODE		3	/*!< Index node */
#define FIL_PAGE_IBUF_FREE_LIST	4	/*!< Insert buffer free list */
/* File page types introduced in MySQL/InnoDB 5.1.7 */
#define FIL_PAGE_TYPE_ALLOCATED	0	/*!< Freshly allocated page */
#define FIL_PAGE_IBUF_BITMAP	5	/*!< Insert buffer bitmap */
#define FIL_PAGE_TYPE_SYS	6	/*!< System page */
#define FIL_PAGE_TYPE_TRX_SYS	7	/*!< Transaction system data */
#define FIL_PAGE_TYPE_FSP_HDR	8	/*!< File space header */
#define FIL_PAGE_TYPE_XDES	9	/*!< Extent descriptor page */
#define FIL_PAGE_TYPE_BLOB	10	/*!< Uncompressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB	11	/*!< First compressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB2	12	/*!< Subsequent compressed BLOB page */
#define FIL_PAGE_TYPE_LAST	FIL_PAGE_TYPE_ZBLOB2
					/*!< Last page type */
/* @} */

/** macro to check whether the page type is index (Btree or Rtree) type */
#define fil_page_type_is_index(page_type)                          \
        (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_RTREE)

/** Check whether the page is index page (either regular Btree index or Rtree
index */
#define fil_page_index_page_check(page)                         \
        fil_page_type_is_index(fil_page_get_type(page))

#ifndef UNIV_INNOCHECKSUM

/** The number of fsyncs done to the log */
extern ulint	fil_n_log_flushes;

/** Number of pending redo log flushes */
extern ulint	fil_n_pending_log_flushes;
/** Number of pending tablespace flushes */
extern ulint	fil_n_pending_tablespace_flushes;

/** Number of files currently open */
extern ulint	fil_n_file_opened;

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Returns the version number of a tablespace, -1 if not found.
@return version number, -1 if the tablespace does not exist in the
memory cache */

int64_t
fil_space_get_version(
/*==================*/
	ulint	id);	/*!< in: space id */

/** Returns the latch of a file space.
@param[in]	id	space id
@param[out]	flags	tablespace flags
@return latch protecting storage allocation */
rw_lock_t*
fil_space_get_latch(
	ulint	id,
	ulint*	flags);

/** Gets the type of a file space.
@param[in]	id	tablespace identifier
@return file type */

fil_type_t
fil_space_get_type(
	ulint	id);

/** Note that a tablespace has been imported.
It is initially marked as FIL_TYPE_IMPORT so that no logging is
done during the import process when the space ID is stamped to each page.
Now we change it to FIL_SPACE_TABLESPACE to start redo and undo logging.
NOTE: temporary tablespaces are never imported.
@param[in]	id	tablespace identifier */

void
fil_space_set_imported(
	ulint	id);

# ifdef UNIV_DEBUG
/** Determine if a tablespace is temporary.
@param[in]	id	tablespace identifier
@return whether it is a temporary tablespace */

bool
fsp_is_temporary(ulint id)
__attribute__((warn_unused_result, pure));
# endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */

/** Append a file to the chain of files of a space.
@param[in]	name	file name of a file that is not open
@param[in]	size	file size in entire database blocks
@param[in,out]	space	tablespace from fil_space_create()
@param[in]	is_raw	whether this is a raw device or partition
@return pointer to the file name, or NULL on error */

char*
fil_node_create(
	const char*	name,
	ulint		size,
	fil_space_t*	space,
	bool		is_raw)
	__attribute__((warn_unused_result));
/** Create a space memory object and put it to the fil_system hash table.
Error messages are issued to the server log.
@param[in]	name	tablespace name
@param[in]	id	tablespace identifier
@param[in]	flags	tablespace flags
@param[in]	purpose	tablespace purpose
@return pointer to created tablespace, to be filled in with fil_node_create()
@retval NULL on failure (such as when the same tablespace exists) */

fil_space_t*
fil_space_create(
	const char*	name,
	ulint		id,
	ulint		flags,
	fil_type_t	purpose)
	__attribute__((warn_unused_result));

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */

bool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id);	/*!< in/out: space id */

/** Frees a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param[in]	id		tablespace identifier
@param[in]	x_latched	whether the caller holds X-mode space->latch
@return true if success */

bool
fil_space_free(
	ulint		id,
	bool		x_latched);

/** Returns the path from the first fil_node_t found with this space ID.
The caller is responsible for freeing the memory allocated here for the
value returned.
@param[in]	id	Tablespace ID
@return own: A copy of fil_node_t::path, NULL if space ID is zero
or not found. */

char*
fil_space_get_first_path(
	ulint		id);

/*******************************************************************//**
Returns the size of the space in pages. The tablespace must be cached in the
memory cache.
@return space size, 0 if space not found */

ulint
fil_space_get_size(
/*===============*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the flags of the space. The tablespace must be cached
in the memory cache.
@return flags, ULINT_UNDEFINED if space not found */

ulint
fil_space_get_flags(
/*================*/
	ulint	id);	/*!< in: space id */

/** Check if table is mark for truncate.
@param[in]	id	space id
@return true if tablespace is marked for truncate. */

bool
fil_space_is_being_truncated(
	ulint id);

/** Returns the page size of the space and whether it is compressed or not.
The tablespace must be cached in the memory cache.
@param[in]	id	space id
@param[out]	found	true if tablespace was found
@return page size */
const page_size_t
fil_space_get_page_size(
	ulint	id,
	bool*	found);

/*******************************************************************//**
Checks if the pair space, page_no refers to an existing page in a tablespace
file space. The tablespace must be cached in the memory cache.
@return true if the address is meaningful */

bool
fil_check_adress_in_tablespace(
/*===========================*/
	ulint	id,	/*!< in: space id */
	ulint	page_no);/*!< in: page number */
/****************************************************************//**
Initializes the tablespace memory cache. */

void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open);	/*!< in: max number of open files */
/*******************************************************************//**
Initializes the tablespace memory cache. */

void
fil_close(void);
/*===========*/
/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */

void
fil_open_log_and_system_tablespace_files(void);
/*==========================================*/
/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */

void
fil_close_all_files(void);
/*=====================*/
/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */

void
fil_close_log_files(
/*================*/
	bool	free);	/*!< in: whether to free the memory object */
/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */

void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id);/*!< in: maximum known id */
#ifndef UNIV_HOTBACKUP
/** Write the flushed LSN to the page header of the first page in the
system tablespace.
@param[in]	lsn	flushed LSN
@return DB_SUCCESS or error number */

dberr_t
fil_write_flushed_lsn(
	lsn_t	lsn);

/*******************************************************************//**
Increments the count of pending operation, if space is not being deleted.
@return true if being deleted, and operation should be skipped */

bool
fil_inc_pending_ops(
/*================*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Decrements the count of pending operations. */

void
fil_decr_pending_ops(
/*=================*/
	ulint	id);	/*!< in: space id */
#endif /* !UNIV_HOTBACKUP */
/********************************************************//**
Creates the database directory for a table if it does not exist yet. */

void
fil_create_directory_for_tablename(
/*===============================*/
	const char*	name);	/*!< in: name in the standard
				'databasename/tablename' format */
/********************************************************//**
Recreates table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */

dberr_t
fil_recreate_table(
/*===============*/
	ulint			space_id,	/*!< in: space id */
	ulint			format_flags,	/*!< in: page format */
	ulint			flags,		/*!< in: tablespace flags */
	const char*		name,		/*!< in: table name */
	truncate_t&		truncate);	/*!< in/out: The information of
						TRUNCATE log record */
/********************************************************//**
Recreates the tablespace and table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */

dberr_t
fil_recreate_tablespace(
/*====================*/
	ulint			space_id,	/*!< in: space id */
	ulint			format_flags,	/*!< in: page format */
	ulint			flags,		/*!< in: tablespace flags */
	const char*		name,		/*!< in: table name */
	truncate_t&		truncate,	/*!< in/out: The information of
						TRUNCATE log record */
	lsn_t			recv_lsn);	/*!< in: the end LSN of
						the log record */
/** Replay a file rename operation if possible.
@param[in]	space_id	tablespace identifier
@param[in]	first_page_no	first page number in the file
@param[in]	name		old file name
@param[in]	new_name	new file name
@return	whether the operation was successfully applied
(the name did not exist, or new_name did not exist and
name was successfully renamed to new_name)  */

bool
fil_op_replay_rename(
	ulint		space_id,
	ulint		first_page_no,
	const char*	name,
	const char*	new_name)
	__attribute__((warn_unused_result));
/*******************************************************************//**
Deletes a single-table tablespace. The tablespace must be cached in the
memory cache.
@return true if success */

dberr_t
fil_delete_tablespace(
/*==================*/
	ulint		id,		/*!< in: space id */
	buf_remove_t	buf_remove);	/*!< in: specify the action to take
					on the tables pages in the buffer
					pool */

/** Truncate the tablespace to needed size.
@param[in]	space_id	id of tablespace to truncate
@param[in]	size_in_pages	truncate size.
@return true if truncate was successful. */

bool
fil_truncate_tablespace(
	ulint		space_id,
	ulint		size_in_pages);

/** Check if an index tree is freed by checking a descriptor bit of
index root page.
@param[in]	space_id	space id
@param[in]	root_page_no	root page no of an index tree
@param[in]	page_size	page size
@return true if the index tree is freed */
bool
fil_index_tree_is_freed(
	ulint			space_id,
	ulint			root_page_no,
	const page_size_t&	page_size);

/*******************************************************************//**
Prepare for truncating a single-table tablespace. The tablespace
must be cached in the memory cache.
1) Check pending operations on a tablespace;
2) Remove all insert buffer entries for the tablespace;
@return DB_SUCCESS or error */

dberr_t
fil_prepare_for_truncate(
/*=====================*/
	ulint	id);			/*!< in: space id */
/**********************************************************************//**
Reinitialize the original tablespace header with the same space id
for single tablespace */

void
fil_reinit_space_header(
/*====================*/
	ulint		id,	/*!< in: space id */
	ulint		size);	/*!< in: size in blocks */
/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return DB_SUCCESS or error */

dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*	trx,	/*!< in/out: Transaction covering the close */
	ulint	id);	/*!< in: space id */
#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. When the user does IMPORT TABLESPACE, the tablespace will have the
    same id as it originally had.

 4. Free all the pages in use by the tablespace if rename=true.
@return DB_SUCCESS or error */

dberr_t
fil_discard_tablespace(
/*===================*/
	ulint	id)	/*!< in: space id */
	__attribute__((warn_unused_result));
#endif /* !UNIV_HOTBACKUP */

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		tablespace identifier
@param[in]	old_path	old file name
@param[in]	new_name	new table name in the
databasename/tablename format
@param[in]	new_path_in	new file name,
or NULL if it is located in the normal data directory
@return true if success */

bool
fil_rename_tablespace(
	ulint		id,
	const char*	old_path,
	const char*	new_name,
	const char*	new_path_in);

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the direcory path or the full path and filename.
@param[in] name NULL if path is full, or Table/Tablespace name
@param[in] suffix NULL or the file extention to use.
@return own: file name */

char*
fil_make_filepath(
	const char*	path,
	const char*	name,
	ib_extention	suffix,
	bool		strip_name);

/*******************************************************************//**
Creates a new tablespace in a database directory of MySQL.
Database directories are under the 'datadir' of MySQL. The datadir is the
directory of a running mysqld program. We can refer to it by simply the
path '.'. Tables created with CREATE TEMPORARY TABLE we place in the temp
dir of the mysqld server.
@return DB_SUCCESS or error code */

dberr_t
fil_create_new_single_table_tablespace(
/*===================================*/
	ulint		space_id,	/*!< in: space id */
	const char*	tablename,	/*!< in: the table name in the usual
					databasename/tablename format
					of InnoDB */
	const char*	dir_path,	/*!< in: NULL or a dir path */
	ulint		flags,		/*!< in: tablespace flags */
	ulint		flags2,		/*!< in: table flags2 */
	ulint		size)		/*!< in: the initial size of the
					tablespace file in pages,
					must be >= FIL_IBD_FILE_INITIAL_SIZE */
	__attribute__((warn_unused_result));
#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Tries to open a single-table tablespace and optionally checks the space id is
right in it. If does not succeed, prints an error message to the .err log. This
function is used to open a tablespace when we start up mysqld, and also in
IMPORT TABLESPACE.
NOTE that we assume this operation is used either at the database startup
or under the protection of the dictionary mutex, so that two users cannot
race here. This operation does not leave the file associated with the
tablespace open, but closes it after we have looked at the space id in it.

If the validate boolean is set, we read the first page of the file and
check that the space id in the file is what we expect. We assume that
this function runs much faster if no check is made, since accessing the
file inode probably is much faster (the OS caches them) than accessing
the first page of the file.  This boolean may be initially false, but if
a remote tablespace is found it will be changed to true.

If the fix_dict boolean is set, then it is safe to use an internal SQL
statement to update the dictionary tables if they are incorrect.

@param[in] validate True if we should validate the tablespace.
@param[in] fix_dict True if the dictionary is available to be fixed.
@param[in] purpose FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in] id Tablespace ID
@param[in] flags Tablespace flags
@param[in] tablename Table name in the databasename/tablename format.
@param[in] path_in Tablespace filepath if found in SYS_DATAFILES
@return DB_SUCCESS or error code */

dberr_t
fil_open_single_table_tablespace(
	bool		validate,
	bool		fix_dict,
	fil_type_t	purpose,
	ulint		id,
	ulint		flags,
	const char*	tablename,
	const char*	path_in)
	__attribute__((warn_unused_result));

enum fil_load_status {
	/** The tablespace file(s) were found and valid. */
	FIL_LOAD_OK,
	/** The name no longer matches space_id */
	FIL_LOAD_ID_CHANGED,
	/** The file(s) were not found */
	FIL_LOAD_NOT_FOUND,
	/** The file(s) were not valid */
	FIL_LOAD_INVALID
};

/** Open a tablespace file and add it to the InnoDB data structures.
@param[in]	space_id	tablespace ID
@param[in]	filename	path/to/databasename/tablename.ibd
@param[in]	filename_len	the length of the filename, in bytes
@param[out]	space		the tablespace, or NULL on error
@return status of the operation */

enum fil_load_status
fil_load_single_table_tablespace(
	ulint		space_id,
	const char*	filename,
	ulint		filename_len,
	fil_space_t*&	space)
	__attribute__((warn_unused_result));

#endif /* !UNIV_HOTBACKUP */
/***********************************************************************//**
A fault-tolerant function that tries to read the next file name in the
directory. We retry 100 times if os_file_readdir_next_file() returns -1. The
idea is to read as much good data as we can and jump over bad data.
@return 0 if ok, -1 if error even after the retries, 1 if at the end
of the directory */

int
fil_file_readdir_next_file(
/*=======================*/
	dberr_t*	err,	/*!< out: this is set to DB_ERROR if an error
				was encountered, otherwise not changed */
	const char*	dirname,/*!< in: directory name or path */
	os_file_dir_t	dir,	/*!< in: directory stream */
	os_file_stat_t*	info);	/*!< in/out: buffer where the
				info is returned */
/*******************************************************************//**
Returns true if a single-table tablespace does not exist in the memory cache,
or is being deleted there.
@return true if does not exist or is being deleted */

bool
fil_tablespace_deleted_or_being_deleted_in_mem(
/*===========================================*/
	ulint		id,	/*!< in: space id */
	int64_t		version);/*!< in: tablespace_version should be this; if
				you pass -1 as the value of this, then this
				parameter is ignored */
/** Look up a tablespace in the memory cache.
@param[in]	id	tablespace ID
@return tablespace if exists, NULL if not */

fil_space_t*
fil_tablespace_exists_in_mem(
	ulint	id);
#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Returns true if a matching tablespace exists in the InnoDB tablespace memory
cache. Note that if we have not done a crash recovery at the database startup,
there may be many tablespaces which are not yet in the memory cache.
@return true if a matching tablespace exists in the memory cache */

bool
fil_space_for_table_exists_in_mem(
/*==============================*/
	ulint		id,		/*!< in: space id */
	const char*	name,		/*!< in: table name in the standard
					'databasename/tablename' format */
	bool		print_error_if_does_not_exist,
					/*!< in: print detailed error
					information to the .err log if a
					matching tablespace is not found from
					memory */
	bool		adjust_space,	/*!< in: whether to adjust space id
					when find table space mismatch */
	mem_heap_t*	heap,		/*!< in: heap memory */
	table_id_t	table_id);	/*!< in: table id */
#else /* !UNIV_HOTBACKUP */
/********************************************************************//**
Extends all tablespaces to the size stored in the space header. During the
mysqlbackup --apply-log phase we extended the spaces on-demand so that log
records could be appllied, but that may have left spaces still too small
compared to the size stored in the space header. */

void
fil_extend_tablespaces_to_stored_len(void);
/*======================================*/
#endif /* !UNIV_HOTBACKUP */
/**********************************************************************//**
Tries to extend a data file so that it would accommodate the number of pages
given. The tablespace must be cached in the memory cache. If the space is big
enough already, does nothing.
@return true if success */

bool
fil_extend_space_to_desired_size(
/*=============================*/
	ulint*	actual_size,	/*!< out: size of the space after extension;
				if we ran out of disk space this may be lower
				than the desired size */
	ulint	space_id,	/*!< in: space id */
	ulint	size_after_extend);/*!< in: desired size in pages after the
				extension; if the current space size is bigger
				than this already, the function does nothing */
/*******************************************************************//**
Tries to reserve free extents in a file space.
@return true if succeed */

bool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve);	/*!< in: how many one wants to reserve */
/*******************************************************************//**
Releases free extents in a file space. */

void
fil_space_release_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_reserved);	/*!< in: how many one reserved */
/*******************************************************************//**
Gets the number of reserved extents. If the database is silent, this number
should be zero. */

ulint
fil_space_get_n_reserved_extents(
/*=============================*/
	ulint	id);		/*!< in: space id */

/** Reads or writes data. This operation could be asynchronous (aio).
@param[in]	type		OS_FILE_READ or OS_FILE_WRITE, ORed to
OS_FILE_LOG, if a log i/o and ORed to OS_AIO_SIMULATED_WAKE_LATER if
simulated aio and we want to post a batch of IOs; NOTE that a simulated
batch may introduce hidden chances of deadlocks, because IOs are not
actually handled until all have been posted: use with great caution!
@param[in]	sync		true if synchronous aio is desired
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read or write; this must
not cross a file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store read data or from where
to write; in aio this must be appropriately aligned
@param[in]	message		message for aio handler if non-sync aio used,
else ignored
@return DB_SUCCESS, DB_TABLESPACE_DELETED or DB_TABLESPACE_TRUNCATED
if we are trying to do i/o on a tablespace which does not exist */
dberr_t
fil_io(
	ulint			type,
	bool			sync,
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message);

/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */

void
fil_aio_wait(
/*=========*/
	ulint	segment);	/*!< in: the number of the segment in the aio
				array to wait for */
/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */

void
fil_flush(
/*======*/
	ulint	space_id);	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG */

void
fil_flush_file_spaces(
	fil_type_t	purpose);
/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */

bool
fil_validate(void);
/*==============*/
/********************************************************************//**
Returns true if file address is undefined.
@return true if undefined */

bool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr);	/*!< in: address */
/********************************************************************//**
Get the predecessor of a file page.
@return FIL_PAGE_PREV */

ulint
fil_page_get_prev(
/*==============*/
	const byte*	page);	/*!< in: file page */
/********************************************************************//**
Get the successor of a file page.
@return FIL_PAGE_NEXT */

ulint
fil_page_get_next(
/*==============*/
	const byte*	page);	/*!< in: file page */
/*********************************************************************//**
Sets the file page type. */

void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type);	/*!< in: type */
/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */

ulint
fil_page_get_type(
/*==============*/
	const byte*	page);	/*!< in: file page */

/*******************************************************************//**
Returns true if a single-table tablespace is redo skipped.
@return true if redo skipped */

bool
fil_tablespace_is_being_deleted(
/*============================*/
	ulint		id);	/*!< in: space id */

#ifdef UNIV_DEBUG
/** Increase redo skipped of a tablespace.
@param[in]	id	space id */
void
fil_space_inc_redo_skipped_count(
	ulint		id);

/** Decrease redo skipped of a tablespace.
@param[in]	id	space id */
void
fil_space_dec_redo_skipped_count(
	ulint		id);

/*******************************************************************//**
Check whether a single-table tablespace is redo skipped.
@return true if redo skipped */
bool
fil_space_is_redo_skipped(
/*======================*/
	ulint		id);	/*!< in: space id */
#endif

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */

void
fil_delete_file(
/*============*/
	const char*	path);	/*!< in: filepath of the ibd tablespace */

/** Callback functor. */
struct PageCallback {

	/** Default constructor */
	PageCallback()
		:
		m_page_size(0, 0, false),
		m_filepath() UNIV_NOTHROW {}

	virtual ~PageCallback() UNIV_NOTHROW {}

	/** Called for page 0 in the tablespace file at the start.
	@param file_size size of the file in bytes
	@param block contents of the first page in the tablespace file
	@retval DB_SUCCESS or error code.*/
	virtual dberr_t init(
		os_offset_t		file_size,
		const buf_block_t*	block) UNIV_NOTHROW = 0;

	/** Called for every page in the tablespace. If the page was not
	updated then its state must be set to BUF_PAGE_NOT_USED. For
	compressed tables the page descriptor memory will be at offset:
	block->frame + UNIV_PAGE_SIZE;
	@param offset physical offset within the file
	@param block block read from file, note it is not from the buffer pool
	@retval DB_SUCCESS or error code. */
	virtual dberr_t operator()(
		os_offset_t 	offset,
		buf_block_t*	block) UNIV_NOTHROW = 0;

	/** Set the name of the physical file and the file handle that is used
	to open it for the file that is being iterated over.
	@param filename then physical name of the tablespace file.
	@param file OS file handle */
	void set_file(const char* filename, os_file_t file) UNIV_NOTHROW
	{
		m_file = file;
		m_filepath = filename;
	}

	/**
	@return the space id of the tablespace */
	virtual ulint get_space_id() const UNIV_NOTHROW = 0;

	/** Set the tablespace table size.
	@param[in] page a page belonging to the tablespace */
	void set_page_size(const buf_frame_t* page) UNIV_NOTHROW;

	/** The compressed page size
	@return the compressed page size */
	const page_size_t& get_page_size() const
	{
		return(m_page_size);
	}

	/** The tablespace page size. */
	page_size_t		m_page_size;

	/** File handle to the tablespace */
	os_file_t		m_file;

	/** Physical file path. */
	const char*		m_filepath;

protected:
	// Disable copying
	PageCallback(const PageCallback&);
	PageCallback& operator=(const PageCallback&);
};

/********************************************************************//**
Iterate over all the pages in the tablespace.
@param table the table definiton in the server
@param n_io_buffers number of blocks to read and write together
@param callback functor that will do the page updates
@return DB_SUCCESS or error code */

dberr_t
fil_tablespace_iterate(
/*===================*/
	dict_table_t*		table,
	ulint			n_io_buffers,
	PageCallback&		callback)
	__attribute__((warn_unused_result));

/********************************************************************//**
Looks for a pre-existing fil_space_t with the given tablespace ID
and, if found, returns the name and filepath in newly allocated buffers that the caller must free.
@param[in] space_id The tablespace ID to search for.
@param[out] name Name of the tablespace found.
@param[out] fileapth The filepath of the first datafile for thtablespace found.
@return true if tablespace is found, false if not. */

bool
fil_space_read_name_and_filepath(
	ulint	space_id,
	char**	name,
	char**	filepath);

/*******************************************************************//**
Checks if a single-table tablespace for a given table name exists in the
tablespace memory cache.
@return space id, ULINT_UNDEFINED if not found */

ulint
fil_get_space_id_for_table(
/*=======================*/
	const char*	name);	/*!< in: table name in the standard
				'databasename/tablename' format */

/**
Iterate over all the spaces in the space list and fetch the
tablespace names. It will return a copy of the name that must be
freed by the caller using: delete[].
@return DB_SUCCESS if all OK. */

dberr_t
fil_get_space_names(
/*================*/
	space_name_list_t&	space_name_list)
				/*!< in/out: Vector for collecting the names. */
	__attribute__((warn_unused_result));

/** Generate redo log for swapping two .ibd files
@param[in]	old_table	old table
@param[in]	new_table	new table
@param[in]	tmp_name	temporary table name
@param[in,out]	mtr		mini-transaction
@return	whether the operation succeeded */

bool
fil_mtr_rename_log(
	const dict_table_t*	old_table,
	const dict_table_t*	new_table,
	const char*		tmp_name,
	mtr_t*			mtr)
	__attribute__((warn_unused_result));

/** Write a MLOG_FILE_NAME record for a non-predefined tablespace.
@param[in]	space_id	tablespace identifier
@param[in,out]	mtr		mini-transaction
@return	tablespace */

fil_space_t*
fil_names_write(
	ulint		space_id,
	mtr_t*		mtr)
	__attribute__((warn_unused_result));

/** Note that a non-predefined persistent tablespace has been modified.
@param[in,out]	space	tablespace
@return whether this is the first dirtying since fil_names_clear() */

bool
fil_names_dirty(
	fil_space_t*	space)
	__attribute__((warn_unused_result));

/** On a log checkpoint, reset fil_names_dirty() flags
and write out MLOG_FILE_NAME and MLOG_CHECKPOINT if needed.
@param[in]	lsn		checkpoint LSN
@param[in]	do_write	whether to always write MLOG_CHECKPOINT
@return whether anything was written to the redo log
@retval false	if no flags were set and nothing written
@retval true	if anything was written to the redo log */

bool
fil_names_clear(
	lsn_t	lsn,
	bool	do_write);

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
/**
Try and enable FusionIO atomic writes.
@param[in] file		OS file handle
@return true if successful */

bool
fil_fusionio_enable_atomic_write(os_file_t file);
#endif /* !NO_FALLOCATE && UNIV_LINUX */

#ifdef UNIV_COMPILE_TEST_FUNCS
void test_make_filepath();
#endif /* UNIV_COMPILE_TEST_FUNCS */

#endif /* !UNIV_INNOCHECKSUM */

#endif /* fil0fil_h */
