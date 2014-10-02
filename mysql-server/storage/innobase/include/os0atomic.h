/*****************************************************************************
Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file include/os0atomic.h
Macros for using atomics

Created 2012-09-23 Sunny Bains (Split from os0sync.h)
*******************************************************/

#ifndef os0atomic_h
#define os0atomic_h

#include "univ.i"

/**********************************************************//**
Atomic compare-and-swap and increment for InnoDB. */

#if defined(HAVE_IB_GCC_ATOMIC_BUILTINS)

# define HAVE_ATOMIC_BUILTINS

# ifdef HAVE_IB_GCC_ATOMIC_BUILTINS_64
#  define HAVE_ATOMIC_BUILTINS_64
# endif

/**********************************************************//**
Returns old value,  ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_val_compare_and_swap_ulint(ptr, old_val, new_val) \
	__sync_val_compare_and_swap(ptr, old_val, new_val)

/**********************************************************//**
Returns true if swapped, ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_compare_and_swap(ptr, old_val, new_val) \
	__sync_bool_compare_and_swap(ptr, old_val, new_val)

# define os_compare_and_swap_ulint(ptr, old_val, new_val) \
	os_compare_and_swap(ptr, old_val, new_val)

# define os_compare_and_swap_lint(ptr, old_val, new_val) \
	os_compare_and_swap(ptr, old_val, new_val)

#  define os_compare_and_swap_uint32(ptr, old_val, new_val) \
	os_compare_and_swap(ptr, old_val, new_val)

# ifdef HAVE_IB_ATOMIC_PTHREAD_T_GCC
#  define os_compare_and_swap_thread_id(ptr, old_val, new_val) \
	os_compare_and_swap(ptr, old_val, new_val)
#  define INNODB_RW_LOCKS_USE_ATOMICS
#  define IB_ATOMICS_STARTUP_MSG \
	"Mutexes and rw_locks use GCC atomic builtins"
# else /* HAVE_IB_ATOMIC_PTHREAD_T_GCC */
#  define IB_ATOMICS_STARTUP_MSG \
	"Mutexes use GCC atomic builtins, rw_locks do not"
# endif /* HAVE_IB_ATOMIC_PTHREAD_T_GCC */

/**********************************************************//**
Returns the resulting value, ptr is pointer to target, amount is the
amount of increment. */

# define os_atomic_increment(ptr, amount) \
	__sync_add_and_fetch(ptr, amount)

# define os_atomic_increment_lint(ptr, amount) \
	os_atomic_increment(ptr, amount)

# define os_atomic_increment_ulint(ptr, amount) \
	os_atomic_increment(ptr, amount)

# define os_atomic_increment_uint32(ptr, amount ) \
	os_atomic_increment(ptr, amount)

# define os_atomic_increment_uint64(ptr, amount) \
	os_atomic_increment(ptr, amount)

/* Returns the resulting value, ptr is pointer to target, amount is the
amount to decrement. */

# define os_atomic_decrement(ptr, amount) \
	__sync_sub_and_fetch(ptr, amount)

# define os_atomic_decrement_lint(ptr, amount) \
	os_atomic_decrement(ptr, amount)

# define os_atomic_decrement_ulint(ptr, amount) \
	os_atomic_decrement(ptr, amount)

# define os_atomic_decrement_uint32(ptr, amount) \
	os_atomic_decrement(ptr, amount)

# define os_atomic_decrement_uint64(ptr, amount) \
	os_atomic_decrement(ptr, amount)

/**********************************************************//**
Returns the old value of *ptr, atomically sets *ptr to new_val */

# define os_atomic_test_and_set_ulint(ptr, new_val) \
	__sync_lock_test_and_set(ptr, new_val)

#elif defined(HAVE_IB_SOLARIS_ATOMICS)

# define HAVE_ATOMIC_BUILTINS
# define HAVE_ATOMIC_BUILTINS_64

/* If not compiling with GCC or GCC doesn't support the atomic
intrinsics and running on Solaris >= 10 use Solaris atomics */

# include <atomic.h>

/**********************************************************//**
Returns old value, ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_val_compare_and_swap_ulint(ptr, old_val, new_val) \
	(atomic_cas_ulong(ptr, old_val, new_val) == old_val)

/**********************************************************//**
Returns true if swapped, ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_compare_and_swap_uint32(ptr, old_val, new_val) \
	(atomic_cas_32(ptr, old_val, new_val) == old_val)

# define os_compare_and_swap_ulint(ptr, old_val, new_val) \
	(atomic_cas_ulong(ptr, old_val, new_val) == old_val)

# define os_compare_and_swap_lint(ptr, old_val, new_val) \
	((lint) atomic_cas_ulong((ulong_t*) ptr, old_val, new_val) == old_val)

# ifdef HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS
#  if SIZEOF_PTHREAD_T == 4
#   define os_compare_and_swap_thread_id(ptr, old_val, new_val) \
	((pthread_t) atomic_cas_32(ptr, old_val, new_val) == old_val)
#  elif SIZEOF_PTHREAD_T == 8
#   define os_compare_and_swap_thread_id(ptr, old_val, new_val) \
	((pthread_t) atomic_cas_64(ptr, old_val, new_val) == old_val)
#  else
#   error "SIZEOF_PTHREAD_T != 4 or 8"
#  endif /* SIZEOF_PTHREAD_T CHECK */
#  define INNODB_RW_LOCKS_USE_ATOMICS
#  define IB_ATOMICS_STARTUP_MSG \
	"Mutexes and rw_locks use Solaris atomic functions"
# else /* HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS */
#  define IB_ATOMICS_STARTUP_MSG \
	"Mutexes use Solaris atomic functions, rw_locks do not"
# endif /* HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS */

/**********************************************************//**
Returns the resulting value, ptr is pointer to target, amount is the
amount of increment. */

# define os_atomic_increment_lint(ptr, amount) \
	os_atomic_increment_ulint((ulong_t*) ptr, amount)

# define os_atomic_increment_ulint(ptr, amount) \
	atomic_add_long_nv(ptr, amount)

# define os_atomic_increment_uint32(ptr, amount) \
	atomic_add_32_nv(ptr, amount)

# define os_atomic_increment_uint64(ptr, amount) \
	atomic_add_64_nv(ptr, amount)

/* Returns the resulting value, ptr is pointer to target, amount is the
amount to decrement. */

# define os_atomic_decrement_lint(ptr, amount) \
	os_atomic_increment_ulint((ulong_t*) ptr, -(amount))

# define os_atomic_decrement_ulint(ptr, amount) \
	os_atomic_increment_ulint(ptr, -(amount))

# define os_atomic_decrement_uint32(ptr, amount) \
	os_atomic_increment_uint32(ptr, -(amount))

# define os_atomic_decrement_uint64(ptr, amount) \
	os_atomic_increment_uint64(ptr, -(amount))

/**********************************************************//**
Returns the old value of *ptr, atomically sets *ptr to new_val */

# define os_atomic_test_and_set_ulint(ptr, new_val) \
	atomic_swap_ulong(ptr, new_val)

#elif defined(HAVE_WINDOWS_ATOMICS)

# define HAVE_ATOMIC_BUILTINS

# ifdef _WIN64
#  define HAVE_ATOMIC_BUILTINS_64
# endif /* _WIN64 */

/**********************************************************//**
Atomic compare and exchange of signed integers (both 32 and 64 bit).
@return value found before the exchange.
If it is not equal to old_value the exchange did not happen. */
UNIV_INLINE
lint
win_cmp_and_xchg_lint(
/*==================*/
	volatile lint*	ptr,		/*!< in/out: source/destination */
	lint		new_val,	/*!< in: exchange value */
	lint		old_val);	/*!< in: value to compare to */

/**********************************************************//**
Atomic addition of signed integers.
@return Initial value of the variable pointed to by ptr */
UNIV_INLINE
lint
win_xchg_and_add(
/*=============*/
	volatile lint*	ptr,	/*!< in/out: address of destination */
	lint		val);	/*!< in: number to be added */

/**********************************************************//**
Atomic compare and exchange of unsigned integers.
@return value found before the exchange.
If it is not equal to old_value the exchange did not happen. */
UNIV_INLINE
ulint
win_cmp_and_xchg_ulint(
/*===================*/
	volatile ulint*	ptr,		/*!< in/out: source/destination */
	ulint		new_val,	/*!< in: exchange value */
	ulint		old_val);	/*!< in: value to compare to */

/**********************************************************//**
Atomic compare and exchange of 32 bit unsigned integers.
@return value found before the exchange.
If it is not equal to old_value the exchange did not happen. */
UNIV_INLINE
DWORD
win_cmp_and_xchg_dword(
/*===================*/
	volatile DWORD*	ptr,		/*!< in/out: source/destination */
	DWORD		new_val,	/*!< in: exchange value */
	DWORD		old_val);	/*!< in: value to compare to */

/**********************************************************//**
Returns old value, ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_val_compare_and_swap_ulint(ptr, old_val, new_val) \
	win_cmp_and_xchg_ulint(ptr, new_val, old_val)

/**********************************************************//**
Returns true if swapped, ptr is pointer to target, old_val is value to
compare to, new_val is the value to swap in. */

# define os_compare_and_swap_lint(ptr, old_val, new_val) \
	(win_cmp_and_xchg_lint(ptr, new_val, old_val) == old_val)

# define os_compare_and_swap_ulint(ptr, old_val, new_val) \
	(win_cmp_and_xchg_ulint(ptr, new_val, old_val) == old_val)

# define os_compare_and_swap_uint32(ptr, old_val, new_val) \
	(InterlockedCompareExchange(ptr, new_val, old_val) == old_val)

/* windows thread objects can always be passed to windows atomic functions */
# define os_compare_and_swap_thread_id(ptr, old_val, new_val) \
	(win_cmp_and_xchg_dword(ptr, new_val, old_val) == old_val)

# define INNODB_RW_LOCKS_USE_ATOMICS
# define IB_ATOMICS_STARTUP_MSG \
	"Mutexes and rw_locks use Windows interlocked functions"

/**********************************************************//**
Returns the resulting value, ptr is pointer to target, amount is the
amount of increment. */

# define os_atomic_increment_lint(ptr, amount)			\
	(win_xchg_and_add(ptr, amount) + amount)

# define os_atomic_increment_ulint(ptr, amount)			\
	(static_cast<ulint>(win_xchg_and_add(			\
		reinterpret_cast<volatile lint*>(ptr),		\
		static_cast<lint>(amount)))			\
	+ static_cast<ulint>(amount))

# define os_atomic_increment_uint32(ptr, amount)		\
	(static_cast<ulint>(InterlockedExchangeAdd(		\
		reinterpret_cast<long*>(ptr),			\
		static_cast<long>(amount)))			\
	+ static_cast<ulint>(amount))

# define os_atomic_increment_uint64(ptr, amount)		\
	(static_cast<ib_uint64_t>(InterlockedExchangeAdd64(	\
		reinterpret_cast<LONGLONG*>(ptr),		\
		static_cast<LONGLONG>(amount)))			\
	+ static_cast<ib_uint64_t>(amount))

/**********************************************************//**
Returns the resulting value, ptr is pointer to target, amount is the
amount to decrement. There is no atomic substract function on Windows */

# define os_atomic_decrement_lint(ptr, amount)			\
	(win_xchg_and_add(ptr, -(static_cast<lint>(amount))) - amount)

# define os_atomic_decrement_ulint(ptr, amount)			\
	(static_cast<ulint>(win_xchg_and_add(			\
		reinterpret_cast<volatile lint*>(ptr),		\
		-(static_cast<lint>(amount))))			\
	- static_cast<ulint>(amount))

# define os_atomic_decrement_uint32(ptr, amount)		\
	(static_cast<ib_uint32_t>(InterlockedExchangeAdd(	\
		reinterpret_cast<long*>(ptr),			\
		-(static_cast<long>(amount))))			\
	- static_cast<ib_uint32_t>(amount))

# define os_atomic_decrement_uint64(ptr, amount)		\
	(static_cast<ib_uint64_t>(InterlockedExchangeAdd64(	\
		reinterpret_cast<LONGLONG*>(ptr),		\
		-(static_cast<LONGLONG>(amount))))		\
	- static_cast<ib_uint64_t>(amount))

/**********************************************************//**
Returns the old value of *ptr, atomically sets *ptr to new_val.
InterlockedExchange() operates on LONG, and the LONG will be
clobbered */

# define os_atomic_test_and_set_u32(ptr, new_val) \
	InterlockedExchange(ptr, new_val)
#else
# define IB_ATOMICS_STARTUP_MSG \
	"Mutexes uses sys mutexes and rw_locks use InnoDB's own implementation"
#endif

#ifdef HAVE_ATOMIC_BUILTINS
# define os_atomic_inc_ulint(m,v,d)	os_atomic_increment_ulint(v, d)
# define os_atomic_dec_ulint(m,v,d)	os_atomic_decrement_ulint(v, d)
# ifdef _WIN32
#  define TAS(l, n)			os_atomic_test_and_set_u32((l), (n))
#else
#  define TAS(l, n)			os_atomic_test_and_set_ulint((l), (n))
# endif /* _WIN32 */
# define CAS(l, o, n)		os_val_compare_and_swap_ulint((l), (o), (n))
#else
# define os_atomic_inc_ulint(m,v,d)	os_atomic_inc_ulint_func(m, v, d)
# define os_atomic_dec_ulint(m,v,d)	os_atomic_dec_ulint_func(m, v, d)
#endif /* HAVE_ATOMIC_BUILTINS */

/**********************************************************//**
Following macros are used to update specified counter atomically
if HAVE_ATOMIC_BUILTINS defined. Otherwise, use mutex passed in
for synchronization */
#ifdef HAVE_ATOMIC_BUILTINS
#define os_increment_counter_by_amount(mutex, counter, amount)	\
	(void) os_atomic_increment_ulint(&counter, amount)

#define os_decrement_counter_by_amount(mutex, counter, amount)	\
	(void) os_atomic_increment_ulint(&counter, (-((lint) amount)))
#else
#define os_increment_counter_by_amount(mutex, counter, amount)	\
	do {							\
		mutex_enter(&(mutex));				\
		(counter) += (amount);				\
		mutex_exit(&(mutex));				\
	} while (0)

#define os_decrement_counter_by_amount(mutex, counter, amount)	\
	do {							\
		ut_a(counter >= amount);			\
		mutex_enter(&(mutex));				\
		(counter) -= (amount);				\
		mutex_exit(&(mutex));				\
	} while (0)
#endif  /* HAVE_ATOMIC_BUILTINS */

#define os_inc_counter(mutex, counter)				\
	os_increment_counter_by_amount(mutex, counter, 1)

#define os_dec_counter(mutex, counter)				\
	do {							\
		os_decrement_counter_by_amount(mutex, counter, 1);\
	} while (0);

/** barrier definitions for memory ordering */
#ifdef HAVE_IB_GCC_ATOMIC_THREAD_FENCE
# define HAVE_MEMORY_BARRIER
# define os_rmb	__atomic_thread_fence(__ATOMIC_ACQUIRE)
# define os_wmb	__atomic_thread_fence(__ATOMIC_RELEASE)
# define IB_MEMORY_BARRIER_STARTUP_MSG \
	"GCC builtin __atomic_thread_fence() is used for memory barrier"

#elif defined(HAVE_IB_GCC_SYNC_SYNCHRONISE)
# define HAVE_MEMORY_BARRIER
# define os_rmb	__sync_synchronize()
# define os_wmb	__sync_synchronize()
# define IB_MEMORY_BARRIER_STARTUP_MSG \
	"GCC builtin __sync_synchronize() is used for memory barrier"

#elif defined(HAVE_IB_MACHINE_BARRIER_SOLARIS)
# define HAVE_MEMORY_BARRIER
# include <mbarrier.h>
# define os_rmb	__machine_r_barrier()
# define os_wmb	__machine_w_barrier()
# define IB_MEMORY_BARRIER_STARTUP_MSG \
	"Solaris memory ordering functions are used for memory barrier"

#elif defined(HAVE_WINDOWS_MM_FENCE) && defined(_WIN64)
# define HAVE_MEMORY_BARRIER
# include <mmintrin.h>
# define os_rmb	_mm_lfence()
# define os_wmb	_mm_sfence()
# define IB_MEMORY_BARRIER_STARTUP_MSG \
	"_mm_lfence() and _mm_sfence() are used for memory barrier"

#else
# define os_rmb
# define os_wmb
# define IB_MEMORY_BARRIER_STARTUP_MSG \
	"Memory barrier is not used"
#endif

#ifndef UNIV_NONINL
#include "os0atomic.ic"
#endif /* UNIV_NOINL */

#endif /* !os0atomic_h */

