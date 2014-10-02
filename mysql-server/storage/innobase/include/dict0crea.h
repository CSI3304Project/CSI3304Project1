/*****************************************************************************

Copyright (c) 1996, 2014, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/dict0crea.h
Database object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0crea_h
#define dict0crea_h

#include "univ.i"
#include "dict0types.h"
#include "dict0dict.h"
#include "que0types.h"
#include "row0types.h"
#include "mtr0mtr.h"

/*********************************************************************//**
Creates a table create graph.
@return own: table create node */

tab_node_t*
tab_create_graph_create(
/*====================*/
	dict_table_t*	table,		/*!< in: table to create, built as
					a memory data structure */
	mem_heap_t*	heap,		/*!< in: heap where created */
	bool		commit);	/*!< in: true if the commit node
					should be added to the query graph */

/*********************************************************************//**
Creates an index create graph.
@return own: index create node */

ind_node_t*
ind_create_graph_create(
/*====================*/
	dict_index_t*	index,		/*!< in: index to create, built
					as a memory data structure */
	mem_heap_t*	heap,		/*!< in: heap where created */
	bool		commit);	/*!< in: true if the commit node
					should be added to the query graph */

/***********************************************************//**
Creates a table. This is a high-level function used in SQL execution graphs.
@return query thread to run next or NULL */

que_thr_t*
dict_create_table_step(
/*===================*/
	que_thr_t*	thr);		/*!< in: query thread */

/***************************************************************//**
Builds a tablespace, if configured.
@return DB_SUCCESS or error code */

dberr_t
dict_build_tablespace(
/*==================*/
	dict_table_t*	table,		/*!< in/out: table */
	trx_t*		trx);		/*!< in/out: InnoDB transaction
					handle */

/***********************************************************//**
Creates an index. This is a high-level function used in SQL execution
graphs.
@return query thread to run next or NULL */

que_thr_t*
dict_create_index_step(
/*===================*/
	que_thr_t*	thr);		/*!< in: query thread */

/***************************************************************//**
Builds an index definition but doesn't update sys_table.
@return DB_SUCCESS or error code */

void
dict_build_index_def(
/*=================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index,	/*!< in/out: index */
	trx_t*			trx);	/*!< in/out: InnoDB transaction
					handle */
/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
Don't update SYSTEM TABLES.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */

dberr_t
dict_create_index_tree(
/*===================*/
	dict_index_t*	index,	/*!< in/out: index */
	const trx_t*	trx);	/*!< in: InnoDB transaction handle */

/*******************************************************************//**
Recreate the index tree associated with a row in SYS_INDEXES table.
@return	new root page number, or FIL_NULL on failure */

ulint
dict_recreate_index_tree(
/*======================*/
	const dict_table_t*	table,	/*!< in: the table the index
					belongs to */
	btr_pcur_t*		pcur,	/*!< in/out: persistent cursor pointing
					to record in the clustered index of
					SYS_INDEXES table. The cursor may be
					repositioned in this call. */
	mtr_t*			mtr);	/*!< in: mtr having the latch
					on the record page. The mtr may be
					committed and restarted in this call. */

/*******************************************************************//**
Drops the index tree associated with a row in SYS_INDEXES table.
@return index root page number or FIL_NULL if it was already freed. */

ulint
dict_drop_index_tree(
/*=================*/
	rec_t*		rec,		/*!< in/out: record in the clustered
					index of SYS_INDEXES table */
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor
					pointing to record in the clustered
					index of SYS_INDEXES table. The cursor
					may be repositioned in this call. */
	bool		is_drop,	/*!< in: true if we are dropping
					a table */
	mtr_t*		mtr);		/*!< in: mtr having the latch on
					the record page */

/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
Don't update SYSTEM TABLES.
@return	DB_SUCCESS or DB_OUT_OF_FILE_SPACE */

dberr_t
dict_create_index_tree_in_mem(
/*==========================*/
	dict_index_t*	index,		/*!< in/out: index */
	const trx_t*	trx);		/*!< in: InnoDB transaction handle */

/*******************************************************************//**
Truncates the index tree but don't update SYSTEM TABLES.
@return DB_SUCCESS or error */

dberr_t
dict_truncate_index_tree_in_mem(
/*============================*/
	dict_index_t*	index);		/*!< in/out: index */

/*******************************************************************//**
Drops the index tree but don't update SYS_INDEXES table. */

void
dict_drop_index_tree_in_mem(
/*========================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			page_no);/*!< in: index page-no */

/****************************************************************//**
Creates the foreign key constraints system tables inside InnoDB
at server bootstrap or server start if they are not found or are
not of the right form.
@return DB_SUCCESS or error code */

dberr_t
dict_create_or_check_foreign_constraint_tables(void);
/*================================================*/

/********************************************************************//**
Generate a foreign key constraint name when it was not named by the user.
A generated constraint has a name of the format dbname/tablename_ibfk_NUMBER,
where the numbers start from 1, and are given locally for this table, that is,
the number is not global, as it used to be before MySQL 4.0.18.  */
UNIV_INLINE
dberr_t
dict_create_add_foreign_id(
/*=======================*/
	ulint*		id_nr,		/*!< in/out: number to use in id
					generation; incremented if used */
	const char*	name,		/*!< in: table name */
	dict_foreign_t*	foreign);	/*!< in/out: foreign key */

/** Adds the given set of foreign key objects to the dictionary tables
in the database. This function does not modify the dictionary cache. The
caller must ensure that all foreign key objects contain a valid constraint
name in foreign->id.
@param[in]	local_fk_set	set of foreign key objects, to be added to
the dictionary tables
@param[in]	table		table to which the foreign key objects in
local_fk_set belong to
@param[in,out]	trx		transaction
@return error code or DB_SUCCESS */
dberr_t
dict_create_add_foreigns_to_dictionary(
/*===================================*/
	const dict_foreign_set&	local_fk_set,
	const dict_table_t*	table,
	trx_t*			trx)
	__attribute__((nonnull, warn_unused_result));
/****************************************************************//**
Creates the tablespaces and datafiles system tables inside InnoDB
at server bootstrap or server start if they are not found or are
not of the right form.
@return DB_SUCCESS or error code */

dberr_t
dict_create_or_check_sys_tablespace(void);
/*=====================================*/

/********************************************************************//**
Add a single tablespace definition to the data dictionary tables in the
database.
@return error code or DB_SUCCESS */

dberr_t
dict_create_add_tablespace_to_dictionary(
/*=====================================*/
	ulint		space,		/*!< in: tablespace id */
	const char*	name,		/*!< in: tablespace name */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	path,		/*!< in: tablespace path */
	trx_t*		trx,		/*!< in: transaction */
	bool		commit);	/*!< in: if true then commit the
					transaction */
/********************************************************************//**
Add a foreign key definition to the data dictionary tables.
@return error code or DB_SUCCESS */

dberr_t
dict_create_add_foreign_to_dictionary(
/*==================================*/
	const char*		name,	/*!< in: table name */
	const dict_foreign_t*	foreign,/*!< in: foreign key */
	trx_t*			trx)	/*!< in/out: dictionary transaction */
	__attribute__((nonnull, warn_unused_result));

/* Table create node structure */
struct tab_node_t{
	que_common_t	common;		/*!< node type: QUE_NODE_TABLE_CREATE */
	dict_table_t*	table;		/*!< table to create, built as a
					memory data structure with
					dict_mem_... functions */
	ins_node_t*	tab_def;	/*!< child node which does the insert of
					the table definition; the row to be
					inserted is built by the parent node  */
	ins_node_t*	col_def;	/*!< child node which does the inserts
					of the column definitions; the row to
					be inserted is built by the parent
					node  */
	commit_node_t*	commit_node;	/*!< child node which performs a
					commit after a successful table
					creation */
	/*----------------------*/
	/* Local storage for this graph node */
	ulint		state;		/*!< node execution state */
	ulint		col_no;		/*!< next column definition to insert */
	mem_heap_t*	heap;		/*!< memory heap used as auxiliary
					storage */
};

/* Table create node states */
#define	TABLE_BUILD_TABLE_DEF	1
#define	TABLE_BUILD_COL_DEF	2
#define	TABLE_COMMIT_WORK	3
#define	TABLE_ADD_TO_CACHE	4
#define	TABLE_COMPLETED		5

/* Index create node struct */

struct ind_node_t{
	que_common_t	common;		/*!< node type: QUE_NODE_INDEX_CREATE */
	dict_index_t*	index;		/*!< index to create, built as a
					memory data structure with
					dict_mem_... functions */
	ins_node_t*	ind_def;	/*!< child node which does the insert of
					the index definition; the row to be
					inserted is built by the parent node  */
	ins_node_t*	field_def;	/*!< child node which does the inserts
					of the field definitions; the row to
					be inserted is built by the parent
					node  */
	commit_node_t*	commit_node;	/*!< child node which performs a
					commit after a successful index
					creation */
	/*----------------------*/
	/* Local storage for this graph node */
	ulint		state;		/*!< node execution state */
	ulint		page_no;	/* root page number of the index */
	dict_table_t*	table;		/*!< table which owns the index */
	dtuple_t*	ind_row;	/* index definition row built */
	ulint		field_no;	/* next field definition to insert */
	mem_heap_t*	heap;		/*!< memory heap used as auxiliary
					storage */
};

/* Index create node states */
#define	INDEX_BUILD_INDEX_DEF	1
#define	INDEX_BUILD_FIELD_DEF	2
#define	INDEX_CREATE_INDEX_TREE	3
#define	INDEX_COMMIT_WORK	4
#define	INDEX_ADD_TO_CACHE	5

#ifndef UNIV_NONINL
#include "dict0crea.ic"
#endif

#endif
