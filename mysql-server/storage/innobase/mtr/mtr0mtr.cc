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
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "fsp0sysspace.h"
#include "page0types.h"
#include "mtr0log.h"
#include "log0log.h"

#include "log0recv.h"

#ifdef UNIV_NONINL
#include "mtr0mtr.ic"
#endif /* UNIV_NONINL */

/** Iterate over a memo block in reverse. */
template <typename Functor>
struct Iterate {

	/** Release specific object */
	explicit Iterate(Functor& functor)
		:
		m_functor(functor)
	{
		/* Do nothing */
	}

	/** @return false if the functor returns false. */
	bool operator()(mtr_buf_t::block_t* block)
	{
		const mtr_memo_slot_t*	start =
			reinterpret_cast<const mtr_memo_slot_t*>(
				block->begin());

		mtr_memo_slot_t*	slot =
			reinterpret_cast<mtr_memo_slot_t*>(
				block->end());

		ut_ad(!(block->used() % sizeof(*slot)));

		while (slot-- != start) {

			if (!m_functor(slot)) {
				return(false);
			}
		}

		return(true);
	}

	Functor&	m_functor;
};

/** Find specific object */
struct Find {

	/** Constructor */
	Find(const void* object, ulint type)
		:
		m_slot(),
		m_type(type),
		m_object(object)
	{
		ut_a(object != NULL);
	}

	/** @return false if the object was found. */
	bool operator()(mtr_memo_slot_t* slot)
	{
		if (m_object == slot->object && m_type == slot->type) {
			m_slot = slot;
			return(false);
		}

		return(true);
	}

	/** Slot if found */
	mtr_memo_slot_t*m_slot;

	/** Type of the object to look for */
	ulint		m_type;

	/** The object instance to look for */
	const void*	m_object;
};

/** Release latches and decrement the buffer fix count.
@param slot	memo slot */
static
void
memo_slot_release(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_SX_FIX:
	case MTR_MEMO_PAGE_X_FIX: {

		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		buf_block_unfix(block);
		buf_page_release_latch(block, slot->type);
		break;
	}

	case MTR_MEMO_S_LOCK:
		rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

	case MTR_MEMO_SX_LOCK:
		rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

	case MTR_MEMO_X_LOCK:
		rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

#ifdef UNIV_DEBUG
	default:
		ut_ad(slot->type == MTR_MEMO_MODIFY);
#endif /* UNIV_DEBUG */
	}

	slot->object = NULL;
}

/** Unfix a page, do not release the latches on the page.
@param slot	memo slot */
static
void
memo_block_unfix(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_X_FIX:
	case MTR_MEMO_PAGE_SX_FIX: {
		buf_block_unfix(reinterpret_cast<buf_block_t*>(slot->object));
		break;
	}

	case MTR_MEMO_S_LOCK:
	case MTR_MEMO_X_LOCK:
	case MTR_MEMO_SX_LOCK:
		break;
#ifdef UNIV_DEBUG
	default:
#endif /* UNIV_DEBUG */
		break;
	}
}
/** Release latches represented by a slot.
@param slot	memo slot */
static
void
memo_latch_release(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_SX_FIX:
	case MTR_MEMO_PAGE_X_FIX: {
		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		memo_block_unfix(slot);

		buf_page_release_latch(block, slot->type);

		slot->object = NULL;
		break;
	}

	case MTR_MEMO_S_LOCK:
		rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

	case MTR_MEMO_X_LOCK:
		rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

	case MTR_MEMO_SX_LOCK:
		rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

#ifdef UNIV_DEBUG
	default:
		ut_ad(slot->type == MTR_MEMO_MODIFY);

		slot->object = NULL;
#endif /* UNIV_DEBUG */
	}
}

/** Release the latches acquired by the mini-transaction. */
struct ReleaseLatches {

	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {
			memo_latch_release(slot);
		}

		return(true);
	}
};

/** Release the latches and blocks acquired by the mini-transaction. */
struct ReleaseAll {
	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {
			memo_slot_release(slot);
		}

		return(true);
	}
};

/** Check that all slots have been handled. */
struct DebugCheck {
	/** @return true always. */
	bool operator()(const mtr_memo_slot_t* slot) const
	{
		ut_a(slot->object == NULL);
		return(true);
	}
};

/** Release a resource acquired by the mini-transaction. */
struct ReleaseBlocks {
	/** Release specific object */
	ReleaseBlocks(lsn_t start_lsn, lsn_t end_lsn)
		:
		m_end_lsn(end_lsn),
		m_start_lsn(start_lsn)
	{
		/* Do nothing */
	}

	/** Add the modified page to the buffer flush list. */
	void add_dirty_page_to_flush_list(mtr_memo_slot_t* slot) const
	{
		ut_ad(m_end_lsn > 0);
		ut_ad(m_start_lsn > 0);

		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		buf_flush_note_modification(block, m_start_lsn, m_end_lsn);
	}

	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {

			if (slot->type == MTR_MEMO_PAGE_X_FIX
			    || slot->type == MTR_MEMO_PAGE_SX_FIX) {

				add_dirty_page_to_flush_list(slot);

			} else if (slot->type == MTR_MEMO_BUF_FIX) {

				buf_block_t*	block;
				block = reinterpret_cast<buf_block_t*>(
					slot->object);
				if (block->made_dirty_with_no_latch) {
					add_dirty_page_to_flush_list(slot);
					block->made_dirty_with_no_latch = false;
				}
			}
		}

		return(true);
	}

	/** Mini-transaction REDO start LSN */
	lsn_t		m_end_lsn;

	/** Mini-transaction REDO end LSN */
	lsn_t		m_start_lsn;
};

class mtr_t::Command {
public:
	/** Constructor.
	Takes ownership of the mtr->m_impl, is responsible for deleting it.
	@param[in,out]	mtr	mini-transaction */
	explicit Command(mtr_t* mtr)
		:
		m_locks_released()
	{
		init(mtr);
	}

	void init(mtr_t* mtr)
	{
		m_impl = &mtr->m_impl;
		m_sync = mtr->m_sync;
	}

	/** Destructor */
	~Command()
	{
		ut_ad(m_impl == 0);
	}

	/** Write the redo log record, add dirty pages to the flush list and
	release the resources. */
	void execute();

	/** Release the blocks used in this mini-transaction. */
	void release_blocks();

	/** Release the latches acquired by the mini-transaction. */
	void release_latches();

	/** Release both the latches and blocks used in the mini-transaction. */
	void release_all();

	/** Release the resources */
	void release_resources();

	/** Append the redo log records to the redo log buffer.
	@param[in]	len	number of bytes to write */
	void finish_write(ulint len);

private:
	/** Prepare to write the mini-transaction log to the redo log buffer.
	@return number of bytes to write in finish_write() */
	ulint prepare_write();

	/** true if it is a sync mini-transaction. */
	bool			m_sync;

	/** The mini-transaction state. */
	mtr_t::Impl*		m_impl;

	/** Set to 1 after the user thread releases the latches. The log
	writer thread must wait for this to be set to 1. */
	volatile ulint		m_locks_released;

	/** Start lsn of the possible log entry for this mtr */
	lsn_t			m_start_lsn;

	/** End lsn of the possible log entry for this mtr */
	lsn_t			m_end_lsn;
};

/** Check if a mini-transaction is dirtying a clean page.
@return true if the mtr is dirtying a clean page. */

bool
mtr_t::is_block_dirtied(const buf_block_t* block)
{
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.buf_fix_count > 0);

	/* It is OK to read oldest_modification because no
	other thread can be performing a write of it and it
	is only during write that the value is reset to 0. */
	return(block->page.oldest_modification == 0);
}

/** Write the block contents to the REDO log */
struct mtr_write_log_t {
	/** Number of bytes to write */
	mutable ulint	m_len;

	/** Constructor */
	explicit mtr_write_log_t(ulint len) : m_len (len) {}

	/** Append a block to the redo log buffer.
	@return whether the appending should continue */
	bool operator()(const mtr_buf_t::block_t* block) const
	{
		ut_ad(m_len > 0);

		ulint	len = ut_min(m_len, block->used());

		log_write_low(block->begin(), len);
		m_len -= len;
		return(m_len > 0);
	}
};

/** Append records to the system-wide redo log buffer.
@param[in]	log	redo log records */

void
mtr_write_log(
	const mtr_buf_t*	log)
{
	const ulint	len = log->size();
	mtr_write_log_t	write_log(len);

	DBUG_PRINT("ib_log",
		   (ULINTPF "extra bytes written at " LSN_PF,
		    len, log_sys->lsn));

	log_reserve_and_open(len);
	log->for_each_block(write_log);
	log_close();
}

/** Start a mini-transaction.
@param sync		true if it is a synchronous mini-transaction
@param read_only	true if read only mini-transaction */

void
mtr_t::start(bool sync, bool read_only)
{
	UNIV_MEM_INVALID(this, sizeof(*this));

	UNIV_MEM_INVALID(&m_impl, sizeof(m_impl));

	m_sync =  sync;

	m_commit_lsn = 0;

	new(&m_impl.m_log) mtr_buf_t();
	new(&m_impl.m_memo) mtr_buf_t();

	m_impl.m_mtr = this;
	m_impl.m_log_mode = MTR_LOG_ALL;
	m_impl.m_inside_ibuf = false;
	m_impl.m_modifications = false;
	m_impl.m_made_dirty = false;
	m_impl.m_n_log_recs = 0;
	m_impl.m_state = MTR_STATE_ACTIVE;
	m_impl.m_named_space = TRX_SYS_SPACE;

	ut_d(m_impl.m_magic_n = MTR_MAGIC_N);
}

/** Release the resources */

void
mtr_t::Command::release_resources()
{
	ut_ad(m_impl->m_magic_n == MTR_MAGIC_N);

	/* Currently only used in commit */
	ut_ad(m_impl->m_state == MTR_STATE_COMMITTING);

#ifdef UNIV_DEBUG
	DebugCheck		release;
	Iterate<DebugCheck>	iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);
#endif /* UNIV_DEBUG */

	/* Reset the mtr buffers */
	m_impl->m_log.erase();

	m_impl->m_memo.erase();

	m_impl->m_state = MTR_STATE_COMMITTED;

	m_impl = 0;
}

/** Commit a mini-transaction. */

void
mtr_t::commit()
{
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	m_impl.m_state = MTR_STATE_COMMITTING;

	/* This is a dirty read, for debugging. */
	ut_ad(!recv_no_log_write);

	Command	cmd(this);

	if (m_impl.m_modifications
	    && (m_impl.m_n_log_recs > 0
		|| m_impl.m_log_mode == MTR_LOG_NO_REDO)) {

		ut_ad(!srv_read_only_mode
		      || m_impl.m_log_mode == MTR_LOG_NO_REDO);

		cmd.execute();
	} else {
		cmd.release_all();
		cmd.release_resources();
	}
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
MLOG_FILE_NAME records and a MLOG_CHECKPOINT marker.
The caller must invoke log_mutex_enter() and log_mutex_exit().
This is to be used at log_checkpoint().
@param[in]	checkpoint_lsn	the LSN of the log checkpoint  */

void
mtr_t::commit_checkpoint(lsn_t checkpoint_lsn)
{
	ut_ad(log_mutex_own());
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(get_log_mode() == MTR_LOG_ALL);
	ut_ad(!m_impl.m_made_dirty);
	ut_ad(m_impl.m_memo.size() == 0);
	ut_ad(!srv_read_only_mode);
	ut_d(m_impl.m_state = MTR_STATE_COMMITTING);

	/* This is a dirty read, for debugging. */
	ut_ad(!recv_no_log_write);

	switch (m_impl.m_n_log_recs) {
	case 0:
		break;
	case 1:
		*m_impl.m_log.front()->begin() |= MLOG_SINGLE_REC_FLAG;
		break;
	default:
		mlog_catenate_ulint(
			&m_impl.m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
	}

	byte*	ptr = m_impl.m_log.push<byte*>(SIZE_OF_MLOG_CHECKPOINT);
#if SIZE_OF_MLOG_CHECKPOINT != 9
# error SIZE_OF_MLOG_CHECKPOINT != 9
#endif
	*ptr = MLOG_CHECKPOINT;
	mach_write_to_8(ptr + 1, checkpoint_lsn);

	Command	cmd(this);
	cmd.finish_write(m_impl.m_log.size());
	cmd.release_resources();

	DBUG_PRINT("ib_log",
		   ("MLOG_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
		    checkpoint_lsn, log_sys->lsn));
}

#ifdef UNIV_DEBUG
/** Check the tablespace associated with the mini-transaction
(needed for generating a MLOG_FILE_NAME record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */

bool
mtr_t::is_named_space(ulint space) const
{
	switch (get_log_mode()) {
	case MTR_LOG_NONE:
	case MTR_LOG_NO_REDO:
		return(true);
	case MTR_LOG_ALL:
	case MTR_LOG_SHORT_INSERTS:
		return(m_impl.m_named_space == space
		       || is_predefined_tablespace(space));
	}

	ut_error;
	return(false);
}
#endif /* UNIV_DEBUG */

/** Release an object in the memo stack.
@return true if released */

bool
mtr_t::memo_release(const void* object, ulint type)
{
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_active());

	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!m_impl.m_modifications || type != MTR_MEMO_PAGE_X_FIX);

	Find		find(object, type);
	Iterate<Find>	iterator(find);

	if (!m_impl.m_memo.for_each_block_in_reverse(iterator)) {
		memo_slot_release(find.m_slot);
		return(true);
	}

	return(false);
}

/** Prepare to write the mini-transaction log to the redo log buffer.
@return number of bytes to write in finish_write() */

ulint
mtr_t::Command::prepare_write()
{
	switch (m_impl->m_log_mode) {
	case MTR_LOG_SHORT_INSERTS:
		ut_ad(0);
		/* fall through (write no redo log) */
	case MTR_LOG_NO_REDO:
	case MTR_LOG_NONE:
		ut_ad(m_impl->m_log.size() == 0);
		log_mutex_enter();
		m_end_lsn = m_start_lsn = log_sys->lsn;
		return(0);
	case MTR_LOG_ALL:
		break;
	}

	ulint	len	= m_impl->m_log.size();
	ulint	n_recs	= m_impl->m_n_log_recs;
	ut_ad(len > 0);
	ut_ad(n_recs > 0);

	if (len > log_sys->buf_size / 2) {
		log_buffer_extend((len + 1) * 2);
	}

	fil_space_t*	space
		= is_predefined_tablespace(m_impl->m_named_space)
		? NULL
		: fil_names_write(m_impl->m_named_space, m_impl->m_mtr);

	ut_ad(m_impl->m_n_log_recs >= n_recs);

	log_mutex_enter();

	if (space != NULL && fil_names_dirty(space)) {
		/* This mini-transaction was the first one to modify
		the tablespace since the latest checkpoint. Do include
		the MLOG_FILE_NAME record that was appended to m_log
		by fil_names_write().  In all other cases, we will use
		the old m_log.size() (omitting the MLOG_FILE_NAME)
		when copying the log to the global redo log buffer. */
		ut_ad(m_impl->m_n_log_recs > n_recs);
		mlog_catenate_ulint(
			&m_impl->m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
		len = m_impl->m_log.size();
	} else {
		/* This was not the first time of dirtying the
		tablespace since the latest checkpoint. Thus, we
		should not append any MLOG_FILE_NAME record.

		If fil_names_write() returned space!=NULL, it would
		have appended a MLOG_FILE_NAME record. We must copy
		the m_impl->m_log only up to the start of that
		MLOG_FILE_NAME record, not including the record. */

		ut_ad(space == NULL
		      ? (n_recs == m_impl->m_n_log_recs)
		      : (n_recs < m_impl->m_n_log_recs));
		ut_ad(space == NULL
		      ? (len == m_impl->m_log.size())
		      : (len < m_impl->m_log.size()));

		if (n_recs <= 1) {
			ut_ad(n_recs == 1);

			/* Flag the single log record as the
			only record in this mini-transaction. */
			*m_impl->m_log.front()->begin()
				|= MLOG_SINGLE_REC_FLAG;
		} else {
			/* Because this mini-transaction comprises
			multiple log records, append MLOG_MULTI_REC_END
			at the end. */

			if (space != NULL) {
				/* Replace the first byte of the
				to-be-ignored MLOG_FILE_NAME log
				record with MLOG_MULTI_REC_END. */
				byte* tail = m_impl->m_log.at<byte*>(len++);
				ut_ad(*tail == MLOG_FILE_NAME);
				*tail = MLOG_MULTI_REC_END;
				ut_ad(len < m_impl->m_log.size());
			} else {
				/* Append MLOG_MULTI_REC_END. */
				mlog_catenate_ulint(
					&m_impl->m_log, MLOG_MULTI_REC_END,
					MLOG_1BYTE);
				len++;
				ut_ad(len == m_impl->m_log.size());
			}
		}
	}

	ut_ad(len <= m_impl->m_log.size());
	return(len);
}

/** Append the redo log records to the redo log buffer
@param[in] len	number of bytes to write */

void
mtr_t::Command::finish_write(
	ulint	len)
{
	ut_ad(m_impl->m_log_mode == MTR_LOG_ALL);
	ut_ad(log_mutex_own());
	ut_ad(m_impl->m_log.size() >= len);
	ut_ad(len > 0);

	if (m_impl->m_log.is_small()) {
		const mtr_buf_t::block_t*	front = m_impl->m_log.front();
		ut_ad(len <= front->used());

		m_end_lsn = log_reserve_and_write_fast(
			front->begin(), len, &m_start_lsn);

		if (m_end_lsn > 0) {
			return;
		}
	}

	/* Open the database log for log_write_low */
	m_start_lsn = log_reserve_and_open(len);

	mtr_write_log_t	write_log(len);
	m_impl->m_log.for_each_block(write_log);

	m_end_lsn = log_close();
}

/** Release the latches and blocks acquired by this mini-transaction */

void
mtr_t::Command::release_all()
{
	ReleaseAll release;
	Iterate<ReleaseAll> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);

	/* Note that we have released the latches. */
	m_locks_released = 1;
}

/** Release the latches acquired by this mini-transaction */

void
mtr_t::Command::release_latches()
{
	ReleaseLatches release;
	Iterate<ReleaseLatches> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);

	/* Note that we have released the latches. */
	m_locks_released = 1;
}

/** Release the blocks used in this mini-transaction */

void
mtr_t::Command::release_blocks()
{
	ReleaseBlocks release(m_start_lsn, m_end_lsn);
	Iterate<ReleaseBlocks> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);
}

/** Write the redo log record, add dirty pages to the flush list and release
the resources. */

void
mtr_t::Command::execute()
{
	ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);

	if (const ulint len = prepare_write()) {
		finish_write(len);
	}

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_enter();
	}

	/* It is now safe to release the log mutex because the
	flush_order mutex will ensure that we are the first one
	to insert into the flush list. */
	log_mutex_exit();

	m_impl->m_mtr->m_commit_lsn = m_end_lsn;

	release_blocks();

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_exit();
	}

	release_latches();

	release_resources();
}

#ifdef UNIV_DEBUG
/** Check if memo contains the given item.
@return	true if contains */

bool
mtr_t::memo_contains(
	mtr_buf_t*	memo,
	const void*	object,
	ulint		type)
{
	Find		find(object, type);
	Iterate<Find>	iterator(find);

	return(!memo->for_each_block_in_reverse(iterator));
}

/** Check if memo contains the given page.
@param memo		info
@param ptr		record
@param type		type of
@return	true if contains */

bool
mtr_t::memo_contains_page(mtr_buf_t* memo, const byte* ptr, ulint type)
{
	return(memo_contains(memo, buf_block_align(ptr), type));
}

/** Debug check for flags */
struct FlaggedCheck {
	FlaggedCheck(const void* ptr, ulint flags)
		:
		m_ptr(ptr),
		m_flags(flags)
	{
		// Do nothing
	}

	bool operator()(const mtr_memo_slot_t* slot) const
	{
		if (m_ptr == slot->object && (m_flags & slot->type)) {
			return(false);
		}

		return(true);
	}

	const void*	m_ptr;
	ulint		m_flags;
};

/** Check if memo contains the given item.
@param object		object to search
@param flags		specify types of object (can be ORred) of
			MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */

bool
mtr_t::memo_contains_flagged(const void* ptr, ulint flags) const
{
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_committing() || is_active());

	FlaggedCheck		check(ptr, flags);
	Iterate<FlaggedCheck>	iterator(check);

	return(!m_impl.m_memo.for_each_block_in_reverse(iterator));
}

/** Check if memo contains the given page.
@param ptr		buffer frame
@param flags		specify types of object with OR of
			MTR_MEMO_PAGE_S_FIX... values
@return true if contains */

bool
mtr_t::memo_contains_page_flagged(
	const byte*	ptr,
	ulint		flags) const
{
	return(memo_contains_flagged(buf_block_align(ptr), flags));
}

/** Print info of an mtr handle. */

void
mtr_t::print() const
{
	ib_logf(IB_LOG_LEVEL_INFO,
		"Mini-transaction handle: memo size %lu bytes"
		" log size %lu bytes",
		m_impl.m_memo.size(), get_log()->size());
}

#endif /* UNIV_DEBUG */
