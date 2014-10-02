/*****************************************************************************

Copyright (c) 2013, 2014, Oracle and/or its affiliates. All Rights Reserved.

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
@file fsp/fsp0file.cc
Tablespace data file implementation

Created 2013-7-26 by Kevin Lewis
*******************************************************/

#include "ha_prototypes.h"

#include "fsp0sysspace.h"
#include "os0file.h"
#include "page0page.h"
#include "srv0start.h"
#include "ut0new.h"

/** Initialize the name, size and order of this datafile
@param[in]	name		space name, shutdown() will free it
@param[in]	filepath	file name, shutdown() fill free it;
can be NULL if not determined
@param[in]	size		size in database pages
@param[in]	order		ordinal position or the datafile
in the tablespace */

void
Datafile::init(
	char*	name,
	char*	filepath,
	ulint	size,
	ulint	order)
{
	ut_ad(m_name == NULL);
	ut_ad(name != NULL);

	m_name = name;
	m_filepath = filepath;
	m_size = size;
	m_order = order;
}

/** Initialize the name, size and order of this datafile
@param[in]	name	tablespace name, will be copied
@param[in]	size	size in database pages
@param[in]	order	ordinal position or the datafile
in the tablespace */

void
Datafile::init(
	const char*	name,
	ulint		size,
	ulint		order)
{
	init(mem_strdup(name), NULL, size, order);
}

/** Release the resources. */

void
Datafile::shutdown()
{
	close();

	ut_free(m_name);
	m_name = NULL;

	free_filepath();

	free_first_page();
}

/** Create/open a data file.
@param[in]	read_only_mode	if true, then readonly mode checks are enforced.
@return DB_SUCCESS or error code */
dberr_t
Datafile::open_or_create(bool read_only_mode)
{
	bool success;
	ut_a(m_filepath != NULL);
	ut_ad(m_handle == OS_FILE_CLOSED);

	m_handle = os_file_create(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_FILE_NORMAL, OS_DATA_FILE, read_only_mode, &success);

	if (!success) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile '" << m_filepath << "'";
		return(DB_CANNOT_OPEN_FILE);
	}

	return(DB_SUCCESS);
}

/** Open a data file in read-only mode to check if it exists so that it
can be validated.
@param[in]	strict	whether to issue error messages
@return DB_SUCCESS or error code */
dberr_t
Datafile::open_read_only(bool strict)
{
	bool	success = false;
	ut_ad(m_handle == OS_FILE_CLOSED);

	/* This function can be called for file objects that do not need
	to be opened, which is the case when the m_filepath is NULL */
	if (m_filepath == NULL) {
		return(DB_ERROR);
	}

	set_open_flags(OS_FILE_OPEN);
	m_handle = os_file_create_simple_no_error_handling(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_FILE_READ_ONLY, true, &success);

	if (success) {
		m_exists = true;
		return(DB_SUCCESS);
	}

	if (strict) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile for read-only: '"
			<< m_filepath << "'";
	}

	return(DB_CANNOT_OPEN_FILE);
}

/** Open a data file in read-write mode during start-up so that
doublewrite pages can be restored and then it can be validated.*
@param[in]	read_only_mode	if true, then readonly mode checks are enforced.
@return DB_SUCCESS or error code */
dberr_t
Datafile::open_read_write(bool read_only_mode)
{
	bool	success = false;
	ut_ad(m_handle == OS_FILE_CLOSED);

	/* This function can be called for file objects that do not need
	to be opened, which is the case when the m_filepath is NULL */
	if (m_filepath == NULL) {
		return(DB_ERROR);
	}

	set_open_flags(OS_FILE_OPEN);
	m_handle = os_file_create_simple_no_error_handling(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_FILE_READ_WRITE, read_only_mode, &success);

	if (!success) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile for read-write: '"
			<< m_filepath << "'";
		return(DB_CANNOT_OPEN_FILE);
	}

	m_exists = true;

	return(DB_SUCCESS);
}


/** Close a data file.
@return DB_SUCCESS or error code */

dberr_t
Datafile::close()
{
	if (m_handle != OS_FILE_CLOSED) {
		ibool	success = os_file_close(m_handle);
		ut_a(success);

		m_handle = OS_FILE_CLOSED;
	}

	return(DB_SUCCESS);
}

/** Make physical filename from the Tablespace::m_path
plus the Datafile::m_name and store it in Datafile::m_filepath.
@param path	NULL or full path for this datafile.
@param suffix	File extension for this tablespace datafile. */
void
Datafile::make_filepath(
	const char*	path)
{
	ut_ad(m_name != NULL);

	free_filepath();
	m_filepath = fil_make_filepath(path, m_name, IBD, false);

	if (m_filepath != NULL) {
		set_filename();
	}
}

/** Make physical filename from the Tablespace::m_path
plus the Datafile::m_name and store it in Datafile::m_filepath.
@param path	NULL or full path for this datafile.
@param suffix	File extension for this tablespace datafile. */
void
Datafile::make_filepath_no_ext(
	const char*	path)
{
	ut_ad(m_name != NULL);

	free_filepath();
	m_filepath = fil_make_filepath(path, m_name, NO_EXT, false);

	set_filename();
}

/** Set the filepath by duplicating the filepath sent in. This is the
name of the file with its extension and absolute or relative path.
@param[in]	filepath	filepath to set */

void
Datafile::set_filepath(const char* filepath)
{
	free_filepath();
	m_filepath = static_cast<char*>(ut_malloc_nokey(strlen(filepath) + 1));
	::strcpy(m_filepath, filepath);
	set_filename();
}

/** Free the filepath buffer. */

void
Datafile::free_filepath()
{
	if (m_filepath != NULL) {
		ut_free(m_filepath);
		m_filepath = NULL;
		m_filename = NULL;
	}
}

/** Reads a few significant fields from the first page of the first
datafile.  The Datafile must already be open.
@param[in]	read_only_mode	if true, then readonly mode checks are enforced.
@return DB_SUCCESS or DB_IO_ERROR if page cannot be read */
dberr_t
Datafile::read_first_page(bool read_only_mode)
{
	if (m_handle == OS_FILE_CLOSED) {
		dberr_t err = open_or_create(read_only_mode);
		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	m_first_page_buf = static_cast<byte*>
		(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));

	/* Align the memory for a possible read from a raw device */

	m_first_page = static_cast<byte*>
		(ut_align(m_first_page_buf, UNIV_PAGE_SIZE));

	if (!os_file_read(m_handle, m_first_page, 0, UNIV_PAGE_SIZE)) {
			ib::error() << "Cannot read first page of '"
				<< m_filepath << "'";
			return(DB_IO_ERROR);
		}

	if (m_order == 0) {
		m_flags = fsp_header_get_flags(m_first_page);

		m_space_id = fsp_header_get_space_id(m_first_page);
	}

	return(DB_SUCCESS);
}

/** Free the first page from memory when it is no longer needed. */

void
Datafile::free_first_page()
{
	if (m_first_page_buf) {
		ut_free(m_first_page_buf);
		m_first_page_buf = NULL;
		m_first_page = NULL;
	}
}

/** Validates the datafile and checks that it conforms with the expected
space ID and flags.  The file should exist and be successfully opened
in order for this function to validate it.
@param[in]	space_id	The expected tablespace ID.
@param[in]	flags	The expected tablespace flags.
@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
m_is_valid is also set true on success, else false. */

dberr_t
Datafile::validate_to_dd(
	ulint	space_id,
	ulint	flags)
{
	dberr_t err;

	if (!is_open()) {
		return DB_ERROR;
	}

	/* Validate this single-table-tablespace with the data dictionary,
	but do not compare the DATA_DIR flag, in case the tablespace was
	remotely located. */
	err = validate_first_page();
	if (err != DB_SUCCESS) {
		return(err);
	}

	if (m_space_id == space_id
	    && ((m_flags & ~FSP_FLAGS_MASK_DATA_DIR)
		== (flags & ~FSP_FLAGS_MASK_DATA_DIR))) {
		/* Datafile matches the tablespace expected. */
		return(DB_SUCCESS);
	}

	/* else do not use this tablespace. */
	m_is_valid = false;

	ib::error() << "In file '" << m_filepath << "', tablespace id and"
		" flags are " << m_space_id << " and " << m_flags << ", but in"
		" the InnoDB data dictionary they are " << space_id << " and "
		<< flags << ". Have you moved InnoDB .ibd files around without"
		" using the commands DISCARD TABLESPACE and IMPORT TABLESPACE?"
		" " << TROUBLESHOOT_DATADICT_MSG;

	return(DB_ERROR);
}

/** Validates this datafile for the purpose of recovery.  The file should
exist and be successfully opened. We initially open it in read-only mode
because we just want to read the SpaceID.  However, if the first page is
corrupt and needs to be restored from the doublewrite buffer, we will
reopen it in write mode and ry to restore that page.
@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
m_is_valid is also set true on success, else false. */

dberr_t
Datafile::validate_for_recovery()
{
	dberr_t err;

	ut_ad(is_open());
	ut_ad(!srv_read_only_mode);

	err = validate_first_page();

	switch (err) {
	case DB_SUCCESS:
	case DB_TABLESPACE_EXISTS:
		break;

	default:
		/* Re-open the file in read-write mode  Attempt to restore
		page 0 from doublewrite and read the space ID from a survey
		of the first few pages. */
		close();
		err = open_read_write(srv_read_only_mode);
		if (err != DB_SUCCESS) {
			ib::error() << "Datafile '" << m_name << "' could not"
				" be opened in read-write mode so that the"
				" doublewrite pages could be restored.";
			return(err);
		};

		err = find_space_id();
		if (err != DB_SUCCESS || m_space_id == 0) {
			ib::error() << "Datafile '" << m_name << "' is"
				" corrupted. Cannot determine the space ID from"
				" the first 64 pages.";
			return(err);
		}

		err = restore_from_doublewrite(0);
		if (err != DB_SUCCESS) {
			return(err);
		}

		/* Free the previously read first page and then re-validate. */
		free_first_page();
		err = validate_first_page();
	}

	return(err);
}

/** Checks the consistency of the first page of a datafile when the
tablespace is opened.  This occurs before the fil_space_t is created
so the Space ID found here must not already be open.
m_is_valid is set true on success, else false.
@param[out]	flush_lsn	contents of FIL_PAGE_FILE_FLUSH_LSN
(only valid for the first file of the system tablespace)
@retval DB_SUCCESS on if the datafile is valid
@retval DB_CORRUPTION if the datafile is not readable
@retval DB_TABLESPACE_EXISTS if there is a duplicate space_id */

dberr_t
Datafile::validate_first_page(lsn_t* flush_lsn)
{
	m_is_valid = true;
	const char* error_txt = NULL;
	char* prev_name;
	char* prev_filepath;

	if ((m_first_page == NULL)
	    && (read_first_page(srv_read_only_mode) != DB_SUCCESS)) {
		error_txt = "Cannot read first page";
	} else {
		ut_ad(m_first_page_buf);
		ut_ad(m_first_page);

		if (flush_lsn) {
			*flush_lsn = mach_read_from_8(
				m_first_page + FIL_PAGE_FILE_FLUSH_LSN);
		}
	}

	/* Check if the whole page is blank. */
	if (error_txt == NULL && !m_space_id && !m_flags) {
		ulint		nonzero_bytes	= UNIV_PAGE_SIZE;
		const byte*	b		= m_first_page;

		while (!*b && --nonzero_bytes) {
			b++;
		}

		if (!nonzero_bytes) {
			error_txt = "Header page consists of zero bytes";
		}
	}

	const page_size_t	page_size(m_flags);

	if (error_txt != NULL) {
		/* skip the next few tests */
	} else if (!fsp_flags_is_valid(m_flags)) {
		/* Tablespace flags must be valid. */
		error_txt = "Tablespace flags are invalid";
	} else if (univ_page_size.logical() != page_size.logical()) {
		/* Page size must be univ_page_size. */
		ib::error() << "Data file '" << m_name << "' uses page size "
			<< page_size.logical() << ", but the innodb_page_size"
			" start-up parameter is "
			<< univ_page_size.logical();
		free_first_page();
		return(DB_ERROR);
	} else if (page_get_page_no(m_first_page) != 0) {
		/* First page must be number 0 */
		error_txt = "Header page contains inconsistent data";
	} else if (m_space_id == ULINT_UNDEFINED) {
		/* The space_id can be most anything, except -1. */
		error_txt = "A bad Space ID was found";
	} else if (buf_page_is_corrupted(
			false, m_first_page, page_size,
			fsp_is_checksum_disabled(m_space_id))) {
		/* Look for checksum and other corruptions. */
		error_txt = "Checksum mismatch";
	}

	if (error_txt != NULL) {
		ib::error() << error_txt << " in tablespace: " << m_name
			<< ", Datafile: " << m_filepath << ", Space ID:"
			<< m_space_id  << ", Flags: " << m_flags << ". "
			<< TROUBLESHOOT_DATADICT_MSG;
		m_is_valid = false;
		free_first_page();
		return(DB_CORRUPTION);
	} else if (fil_space_read_name_and_filepath(
			   m_space_id, &prev_name, &prev_filepath)) {
		/* Make sure the space_id has not already been opened. */
		ib::error() << "Attempted to open a previously opened"
			" tablespace. Previous tablespace" << prev_name
			<< " at filepath: " << prev_filepath << " uses"
			" space ID: " << m_space_id << ". Cannot open"
			" tablespace " << m_name << " at filepath: "
			<< m_filepath << " which uses the same space ID.";

		ut_free(prev_name);
		ut_free(prev_filepath);

		m_is_valid = false;
		free_first_page();
		return(is_predefined_tablespace(m_space_id)
		       ? DB_CORRUPTION
		       : DB_TABLESPACE_EXISTS);
	}

	return(DB_SUCCESS);
}

/** Determine the space id of the given file descriptor by reading a few
pages from the beginning of the .ibd file.
@return DB_SUCCESS if space id was successfully identified, else DB_ERROR. */

dberr_t
Datafile::find_space_id()
{
	bool		st;
	os_offset_t	file_size;

	ut_ad(m_handle != OS_FILE_CLOSED);

	file_size = os_file_get_size(m_handle);

	if (file_size == (os_offset_t) -1) {
		ib::error() << "Could not get file size of datafile '"
			<< m_name << "'";
		return(DB_CORRUPTION);
	}

	/* Assuming a page size, read the space_id from each page and store it
	in a map.  Find out which space_id is agreed on by majority of the
	pages.  Choose that space_id. */
	for (ulint page_size = UNIV_ZIP_SIZE_MIN;
	     page_size <= UNIV_PAGE_SIZE_MAX; page_size <<= 1) {

		/* map[space_id] = count of pages */
		typedef std::map<
			ulint,
			ulint,
			std::less<ulint>,
			ut_allocator<std::pair<const ulint, ulint > > >
			verify_t;

		verify_t	verify;

		ulint		page_count = 64;
		ulint		valid_pages = 0;

		/* Adjust the number of pages to analyze based on file size */
		while ((page_count * page_size) > file_size) {
			--page_count;
		}

		ib::info() << "Page size:" << page_size
			<< ". Pages to analyze:" << page_count;

		byte* buf = static_cast<byte*>(ut_malloc_nokey(2*page_size));
		byte* page = static_cast<byte*>(ut_align(buf, page_size));

		for (ulint j = 0; j < page_count; ++j) {

			st = os_file_read(m_handle, page, (j* page_size), page_size);

			if (!st) {
				ib::info() << "READ FAIL: page_no:" << j;
				continue;
			}

			bool	noncompressed_ok = false;

			/* For noncompressed pages, the page size must be
			equal to univ_page_size.physical(). */
			if (page_size == univ_page_size.physical()) {
				noncompressed_ok = !buf_page_is_corrupted(
					false, page, univ_page_size, false);
			}

			const page_size_t	compr_page_size(
				page_size, univ_page_size.logical(), true);
			bool			compressed_ok;

			compressed_ok = !buf_page_is_corrupted(
				false, page, compr_page_size, false);

			if (noncompressed_ok || compressed_ok) {

				ulint	space_id = mach_read_from_4(page
					+ FIL_PAGE_SPACE_ID);

				if (space_id > 0) {
					ib::info() << "VALID: space:"
						<< space_id << " page_no:" << j
						<< " page_size:" << page_size;
					verify[space_id]++;
					++valid_pages;
				}
			}
		}

		ut_free(buf);
		ib::info() << "Page size: " << page_size
			<< ". Possible space_id count:" << verify.size();

		const ulint	pages_corrupted = 3;
		for (ulint missed = 0; missed <= pages_corrupted; ++missed) {

			for (verify_t::const_iterator it = verify.begin();
			     it != verify.end();
			     ++it) {

				ib::info() << "space_id:" << it->first
					<< ", Number of pages matched: "
					<< it->second << "/" << valid_pages
					<< " (" << page_size << ")";

				if (it->second == (valid_pages - missed)) {
					ib::info() << "Chosen space:"
						<< it->first;

					m_space_id = it->first;
					return(DB_SUCCESS);
				}
			}

		}
	}

	return(DB_CORRUPTION);
}


/** Finds a given page of the given space id from the double write buffer
and copies it to the corresponding .ibd file.
@param[in]	page_no		Page number to restore
@return DB_SUCCESS if page was restored from doublewrite, else DB_ERROR */

dberr_t
Datafile::restore_from_doublewrite(
	ulint	restore_page_no)
{
	/* Find if double write buffer contains page_no of given space id. */
	const byte*	page = recv_sys->dblwr.find_page(
		m_space_id, restore_page_no);

	if (page == NULL) {
		/* If the first page of the given user tablespace is not there
		in the doublewrite buffer, then the recovery is going to fail
		now. Hence this is treated as an error. */
		ib::error() << "Corrupted page "
			<< page_id_t(m_space_id, restore_page_no)
			<< " of datafile '" << m_name << "' could not be"
			" found in the doublewrite buffer.";

		return(DB_CORRUPTION);
	}

	const ulint		flags = mach_read_from_4(FSP_HEADER_OFFSET
							 + FSP_SPACE_FLAGS
							 + page);
	const page_size_t	page_size(flags);

	ut_a(page_get_page_no(page) == restore_page_no);

	ib::info() << "Restoring page "
		<< page_id_t(m_space_id, restore_page_no)
		<< " of datafile '" << m_name << "' from the doublewrite"
		<< " buffer. Writing " << page_size.physical() << " bytes into"
		<< " file '" << m_filepath << "'";

	if (!os_file_write(m_filepath, m_handle, page, 0,
			   page_size.physical())) {
		return(DB_CORRUPTION);
	}

	return(DB_SUCCESS);
}

/** Opens a handle to the file linked to in an InnoDB Symbolic Link file
in read-only mode so that it can be validated.
@param[in]	strict	whether to issue error messages
@return DB_SUCCESS if remote linked tablespace file is found and opened. */

dberr_t
RemoteDatafile::open_read_only(bool strict)
{
	ut_ad(m_filepath == NULL);

	read_link_file(name(), &m_link_filepath, &m_filepath);

	if (m_filepath == NULL) {
		/* There is no remote file */
		return(DB_ERROR);
	}

	dberr_t err = Datafile::open_read_only(strict);

	if (err != DB_SUCCESS && strict) {
		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib::error() << "A link file was found named '"
			<< m_link_filepath << "' but the linked tablespace '"
			<< m_filepath << "' could not be opened read-only.";
	}

	return(err);
}

/** Opens a handle to the file linked to in an InnoDB Symbolic Link file
in read-write mode so that it can be restored from doublewrite and validated.
@param[in]	read_only_mode	if true, then readonly mode checks are enforced.
@return DB_SUCCESS if remote linked tablespace file is found and opened. */

dberr_t
RemoteDatafile::open_read_write(bool read_only_mode)
{
	if (m_filepath == NULL) {
		read_link_file(name(), &m_link_filepath, &m_filepath);

		if (m_filepath == NULL) {
			/* There is no remote file */
			return(DB_ERROR);
		}
	}

	dberr_t err = Datafile::open_read_write(read_only_mode);

	if (err != DB_SUCCESS) {
		/* The following call prints an error message */
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "A link file was found named '"
			<< m_link_filepath << "' but the linked data file '"
			<< m_filepath << "' could not be opened for writing.";
	}

	return(err);
}

/** Release the resources. */

void
RemoteDatafile::shutdown()
{
	Datafile::shutdown();

	if (m_link_filepath != 0) {
		ut_free(m_link_filepath);
		m_link_filepath = 0;
	}
}

/** Creates a new InnoDB Symbolic Link (ISL) file.  It is always created
under the 'datadir' of MySQL. The datadir is the directory of a
running mysqld program. We can refer to it by simply using the path ".".
@param[in] name Tablespace Name
@param[in] filepath Remote filepath of tablespace datafile
@return DB_SUCCESS or error code */

dberr_t
RemoteDatafile::create_link_file(
	const char*	name,
	const char*	filepath)
{
	os_file_t	file;
	bool		success;
	dberr_t		err = DB_SUCCESS;
	char*		link_filepath = NULL;
	char*		prev_filepath = NULL;

	read_link_file(name, &link_filepath, &prev_filepath);

	ut_ad(!srv_read_only_mode);

	if (prev_filepath) {
		/* Truncate will call this with an existing
		link file which contains the same filepath. */
		bool same = !strcmp(prev_filepath, filepath);
		ut_free(prev_filepath);
		if (same) {
			ut_free(link_filepath);
			return(DB_SUCCESS);
		}
	}

	if (link_filepath == NULL) {
		return(DB_ERROR);
	}

	file = os_file_create_simple_no_error_handling(
		innodb_data_file_key, link_filepath,
		OS_FILE_CREATE, OS_FILE_READ_WRITE,
		srv_read_only_mode, &success);

	if (!success) {
		/* The following call will print an error message */
		ulint	error = os_file_get_last_error(true);
		ib::error() << "Cannot create file " << link_filepath << ".";

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib::error() << "The link file: " << filepath
				<< " already exists.";
			err = DB_TABLESPACE_EXISTS;

		} else if (error == OS_FILE_DISK_FULL) {
			err = DB_OUT_OF_FILE_SPACE;

		} else {
			err = DB_ERROR;
		}

		/* file is not open, no need to close it. */
		ut_free(link_filepath);
		return(err);
	}

	if (!os_file_write(link_filepath, file, filepath,
			   0, ::strlen(filepath))) {
		err = DB_ERROR;
	}

	/* Close the file, we only need it at startup */
	os_file_close(file);

	ut_free(link_filepath);

	return(err);
}

/** Deletes an InnoDB Symbolic Link (ISL) file.
@param[in] name Tablespace name */

void
RemoteDatafile::delete_link_file(
	const char*	name)
{
	char* link_filepath = fil_make_filepath(NULL, name, ISL, false);

	if (link_filepath != NULL) {
		os_file_delete_if_exists(innodb_data_file_key, link_filepath, NULL);

		ut_free(link_filepath);
	}
}

/** Reads an InnoDB Symbolic Link (ISL) file.
It is always created under the 'datadir' of MySQL.  The name is of the
form {databasename}/{tablename}. and the isl file is expected to be in a
'{databasename}' directory called '{tablename}.isl'. The caller must free
the memory of the null-terminated path returned if it is not null.
@param[in] name  The tablespace name
@param[out] link_filepath Filepath of the ISL file
@param[out] ibd_filepath Filepath of the IBD file read from the ISL file */

void
RemoteDatafile::read_link_file(
	const char*	name,
	char**		link_filepath,
	char**		ibd_filepath)
{
	char*		filepath;
	FILE*		file = NULL;

	*link_filepath = NULL;
	*ibd_filepath = NULL;

	/* The .isl file is in the 'normal' tablespace location. */
	*link_filepath = fil_make_filepath(NULL, name, ISL, false);
	if (*link_filepath == NULL) {
		return;
	}

	file = fopen(*link_filepath, "r+b");
	if (file) {
		filepath = static_cast<char*>(
			ut_malloc_nokey(OS_FILE_MAX_PATH));

		os_file_read_string(file, filepath, OS_FILE_MAX_PATH);
		fclose(file);

		if (::strlen(filepath)) {
			/* Trim whitespace from end of filepath */
			ulint lastch = ::strlen(filepath) - 1;
			while (lastch > 4 && filepath[lastch] <= 0x20) {
				filepath[lastch--] = 0x00;
			}
			os_normalize_path_for_win(filepath);
		}

		*ibd_filepath = filepath;
	}
}
