/*
 * Copyright (c) 2008-2012 Niels Provos, Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "event2/event-config.h"

#ifndef _EVENT_DISABLE_THREAD_SUPPORT

#include "event2/thread.h"

#include <stdlib.h>
#include <string.h>

#include "log-internal.h"
#include "mm-internal.h"
#include "util-internal.h"
#include "evthread-internal.h"

#ifdef EVTHREAD_EXPOSE_STRUCTS
#define GLOBAL
#else
#define GLOBAL static
#endif

/* globals */
GLOBAL int _evthread_lock_debugging_enabled = 0;
GLOBAL struct evthread_lock_callbacks _evthread_lock_fns = {
	0, 0, NULL, NULL, NULL, NULL
};
GLOBAL unsigned long (*_evthread_id_fn)(void) = NULL;
GLOBAL struct evthread_condition_callbacks _evthread_cond_fns = {
	0, NULL, NULL, NULL, NULL
};

/* Used for debugging */
static struct evthread_lock_callbacks _original_lock_fns = {
	0, 0, NULL, NULL, NULL, NULL
};
static struct evthread_condition_callbacks _original_cond_fns = {
	0, NULL, NULL, NULL, NULL
};

void
evthread_set_id_callback(unsigned long (*id_fn)(void))
{
	_evthread_id_fn = id_fn;
}

int
evthread_set_lock_callbacks(const struct evthread_lock_callbacks *cbs)
{
	//在非调试情况下，实际使用的是_evthread_lock_fns变量的线程锁函数指针成员。
	//在调试情况下实际使用的是_original_lock_fns变量
	struct evthread_lock_callbacks *target =
	    _evthread_lock_debugging_enabled
	    ? &_original_lock_fns : &_evthread_lock_fns;

	if (!cbs) { //参数为NULL，取消线程锁功能 
		if (target->alloc)
			event_warnx("Trying to disable lock functions after "
			    "they have been set up will probaby not work.");
		memset(target, 0, sizeof(_evthread_lock_fns)); //target 清0了
		return 0;
	}

	 //一旦设置就不能修改 
	if (target->alloc) {
		/* Uh oh; we already had locking callbacks set up.*/
		if (target->lock_api_version == cbs->lock_api_version &&
			target->supported_locktypes == cbs->supported_locktypes &&
			target->alloc == cbs->alloc &&
			target->free == cbs->free &&
			target->lock == cbs->lock &&
			target->unlock == cbs->unlock) {
			/* no change -- allow this. */
			return 0;
		}
		event_warnx("Can't change lock callbacks once they have been "
		    "initialized.");
		return -1;
	}

	//这个四个函数指针都不为NULL时才能成功定制。因为这四个函数是配套使用的 
	if (cbs->alloc && cbs->free && cbs->lock && cbs->unlock) {
		memcpy(target, cbs, sizeof(_evthread_lock_fns));
		return event_global_setup_locks_(1);
	} else {
		return -1;
	}
}

int
evthread_set_condition_callbacks(const struct evthread_condition_callbacks *cbs)
{
	struct evthread_condition_callbacks *target =
	    _evthread_lock_debugging_enabled
	    ? &_original_cond_fns : &_evthread_cond_fns;

	if (!cbs) {
		if (target->alloc_condition)
			event_warnx("Trying to disable condition functions "
			    "after they have been set up will probaby not "
			    "work.");
		memset(target, 0, sizeof(_evthread_cond_fns));
		return 0;
	}
	if (target->alloc_condition) {
		/* Uh oh; we already had condition callbacks set up.*/
		if (target->condition_api_version == cbs->condition_api_version &&
			target->alloc_condition == cbs->alloc_condition &&
			target->free_condition == cbs->free_condition &&
			target->signal_condition == cbs->signal_condition &&
			target->wait_condition == cbs->wait_condition) {
			/* no change -- allow this. */
			return 0;
		}
		event_warnx("Can't change condition callbacks once they "
		    "have been initialized.");
		return -1;
	}
	if (cbs->alloc_condition && cbs->free_condition &&
	    cbs->signal_condition && cbs->wait_condition) {
		memcpy(target, cbs, sizeof(_evthread_cond_fns));
	}
	if (_evthread_lock_debugging_enabled) {
		_evthread_cond_fns.alloc_condition = cbs->alloc_condition;
		_evthread_cond_fns.free_condition = cbs->free_condition;
		_evthread_cond_fns.signal_condition = cbs->signal_condition;
	}
	return 0;
}

struct debug_lock {
	unsigned locktype; //锁的类型 
	unsigned long held_by; //这个锁是被哪个线程所拥有  
	/* XXXX if we ever use read-write locks, we will need a separate
	 * lock to protect count. */
	int count; //这个锁的加锁次数
	void *lock;  //锁类型，在pthreads下为pthread_mutex_t*类型 
};

static void *
debug_lock_alloc(unsigned locktype)
{
	struct debug_lock *result = mm_malloc(sizeof(struct debug_lock));
	if (!result)
		return NULL;

	//用户设置过自己的线程锁函数
	if (_original_lock_fns.alloc) {
		//用用户定制的线程锁函数分配一个线程锁 
		if (!(result->lock = _original_lock_fns.alloc(
				locktype|EVTHREAD_LOCKTYPE_RECURSIVE))) {
			mm_free(result);
			return NULL;
		}
	} else {
		result->lock = NULL;
	}
	result->locktype = locktype;
	result->count = 0;
	result->held_by = 0;
	return result;
}

static void
debug_lock_free(void *lock_, unsigned locktype)
{
	struct debug_lock *lock = lock_;
	EVUTIL_ASSERT(lock->count == 0);
	EVUTIL_ASSERT(locktype == lock->locktype);
	if (_original_lock_fns.free) {
		_original_lock_fns.free(lock->lock,
		    lock->locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
	}
	lock->lock = NULL;
	lock->count = -100;
	mm_free(lock);
}

static void
evthread_debug_lock_mark_locked(unsigned mode, struct debug_lock *lock)
{
	//如果锁是没有bug，并且不是递归锁，那么锁住一次后，是不允许再锁的，也就不会进入evthread_debug_lock_mark_locked
	//如果有bug，那么evthread_debug_lock_mark_locked可以记录这种情况
	//增加锁的加锁次数.解锁时会减一 
	++lock->count;
	if (!(lock->locktype & EVTHREAD_LOCKTYPE_RECURSIVE))
		EVUTIL_ASSERT(lock->count == 1);
	if (_evthread_id_fn) {
		unsigned long me;

		//获取线程ID 
		me = _evthread_id_fn();
		if (lock->count > 1)
			EVUTIL_ASSERT(lock->held_by == me);
		lock->held_by = me;  //记录这个锁是被哪个线程所拥有 
	}
}

static int
debug_lock_lock(unsigned mode, void *lock_)
{
	struct debug_lock *lock = lock_;
	int res = 0;
	if (lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
		EVUTIL_ASSERT(mode & (EVTHREAD_READ|EVTHREAD_WRITE));
	else
		EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
	if (_original_lock_fns.lock)
		res = _original_lock_fns.lock(mode, lock->lock);

	//lock 成功返回0，失败返回非0
	if (!res) {
		//记录这个锁的使用情况
		evthread_debug_lock_mark_locked(mode, lock);
	}
	return res;
}

static void
evthread_debug_lock_mark_unlocked(unsigned mode, struct debug_lock *lock)
{
	if (lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
		EVUTIL_ASSERT(mode & (EVTHREAD_READ|EVTHREAD_WRITE));
	else
		EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
	if (_evthread_id_fn) {
		//检测锁的拥有者是否为要解锁的线程 
		EVUTIL_ASSERT(lock->held_by == _evthread_id_fn());
		if (lock->count == 1)
			lock->held_by = 0;
	}
	--lock->count; //减少被加锁次数 
	EVUTIL_ASSERT(lock->count >= 0);
}

static int
debug_lock_unlock(unsigned mode, void *lock_)
{
	struct debug_lock *lock = lock_;
	int res = 0;
	
	//先检测
	evthread_debug_lock_mark_unlocked(mode, lock);
	if (_original_lock_fns.unlock)
		res = _original_lock_fns.unlock(mode, lock->lock);
	return res;
}

static int
debug_cond_wait(void *_cond, void *_lock, const struct timeval *tv)
{
	int r;
	struct debug_lock *lock = _lock;
	EVUTIL_ASSERT(lock);
	EVLOCK_ASSERT_LOCKED(_lock);
	evthread_debug_lock_mark_unlocked(0, lock);
	r = _original_cond_fns.wait_condition(_cond, lock->lock, tv);
	evthread_debug_lock_mark_locked(0, lock);
	return r;
}

void
evthread_enable_lock_debuging(void)
{
	struct evthread_lock_callbacks cbs = {
		EVTHREAD_LOCK_API_VERSION,
		EVTHREAD_LOCKTYPE_RECURSIVE,
		debug_lock_alloc,
		debug_lock_free,
		debug_lock_lock,
		debug_lock_unlock
	};
	if (_evthread_lock_debugging_enabled)
		return;

	//把当前用户定制的锁操作复制到_original_lock_fns结构体变量中
	memcpy(&_original_lock_fns, &_evthread_lock_fns,
	    sizeof(struct evthread_lock_callbacks));

	//将当前的锁操作设置成调试锁操作。但调试锁操作函数内部  
    //还是使用_original_lock_fns的锁操作函数，_original_lock_fns是用户定制锁的副本  
	memcpy(&_evthread_lock_fns, &cbs,
	    sizeof(struct evthread_lock_callbacks));

	memcpy(&_original_cond_fns, &_evthread_cond_fns,
	    sizeof(struct evthread_condition_callbacks));
	_evthread_cond_fns.wait_condition = debug_cond_wait;
	_evthread_lock_debugging_enabled = 1;

	/* XXX return value should get checked. */
	event_global_setup_locks_(0);
}

int
_evthread_is_debug_lock_held(void *lock_)
{
	struct debug_lock *lock = lock_;
	if (! lock->count)
		return 0;
	if (_evthread_id_fn) {
		unsigned long me = _evthread_id_fn();
		if (lock->held_by != me)
			return 0;
	}
	return 1;
}

void *
_evthread_debug_get_real_lock(void *lock_)
{
	struct debug_lock *lock = lock_;
	return lock->lock;
}

void *
evthread_setup_global_lock_(void *lock_, unsigned locktype, int enable_locks)
{
	/* there are four cases here:
	   1) we're turning on debugging; locking is not on.
	   2) we're turning on debugging; locking is on.
	   3) we're turning on locking; debugging is not on.
	   4) we're turning on locking; debugging is on. */

	if (!enable_locks && _original_lock_fns.alloc == NULL) {
		/* Case 1: allocate a debug lock. */
		EVUTIL_ASSERT(lock_ == NULL);
		return debug_lock_alloc(locktype);
	} else if (!enable_locks && _original_lock_fns.alloc != NULL) {
		/* Case 2: wrap the lock in a debug lock. */
		struct debug_lock *lock;
		EVUTIL_ASSERT(lock_ != NULL);

		if (!(locktype & EVTHREAD_LOCKTYPE_RECURSIVE)) {
			/* We can't wrap it: We need a recursive lock */
			_original_lock_fns.free(lock_, locktype);
			return debug_lock_alloc(locktype);
		}
		lock = mm_malloc(sizeof(struct debug_lock));
		if (!lock) {
			_original_lock_fns.free(lock_, locktype);
			return NULL;
		}
		lock->lock = lock_;
		lock->locktype = locktype;
		lock->count = 0;
		lock->held_by = 0;
		return lock;
	} else if (enable_locks && ! _evthread_lock_debugging_enabled) {
		/* Case 3: allocate a regular lock */
		EVUTIL_ASSERT(lock_ == NULL);
		return _evthread_lock_fns.alloc(locktype);
	} else {
		/* Case 4: Fill in a debug lock with a real lock */
		struct debug_lock *lock = lock_;
		EVUTIL_ASSERT(enable_locks &&
		              _evthread_lock_debugging_enabled);
		EVUTIL_ASSERT(lock->locktype == locktype);
		EVUTIL_ASSERT(lock->lock == NULL);
		lock->lock = _original_lock_fns.alloc(
			locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
		if (!lock->lock) {
			lock->count = -200;
			mm_free(lock);
			return NULL;
		}
		return lock;
	}
}


#ifndef EVTHREAD_EXPOSE_STRUCTS
unsigned long
_evthreadimpl_get_id()
{
	return _evthread_id_fn ? _evthread_id_fn() : 1;
}
void *
_evthreadimpl_lock_alloc(unsigned locktype)
{
	return _evthread_lock_fns.alloc ?
	    _evthread_lock_fns.alloc(locktype) : NULL;
}
void
_evthreadimpl_lock_free(void *lock, unsigned locktype)
{
	if (_evthread_lock_fns.free)
		_evthread_lock_fns.free(lock, locktype);
}
int
_evthreadimpl_lock_lock(unsigned mode, void *lock)
{
	if (_evthread_lock_fns.lock)
		return _evthread_lock_fns.lock(mode, lock);
	else
		return 0;
}
int
_evthreadimpl_lock_unlock(unsigned mode, void *lock)
{
	if (_evthread_lock_fns.unlock)
		return _evthread_lock_fns.unlock(mode, lock);
	else
		return 0;
}
void *
_evthreadimpl_cond_alloc(unsigned condtype)
{
	return _evthread_cond_fns.alloc_condition ?
	    _evthread_cond_fns.alloc_condition(condtype) : NULL;
}
void
_evthreadimpl_cond_free(void *cond)
{
	if (_evthread_cond_fns.free_condition)
		_evthread_cond_fns.free_condition(cond);
}
int
_evthreadimpl_cond_signal(void *cond, int broadcast)
{
	if (_evthread_cond_fns.signal_condition)
		return _evthread_cond_fns.signal_condition(cond, broadcast);
	else
		return 0;
}
int
_evthreadimpl_cond_wait(void *cond, void *lock, const struct timeval *tv)
{
	if (_evthread_cond_fns.wait_condition)
		return _evthread_cond_fns.wait_condition(cond, lock, tv);
	else
		return 0;
}
int
_evthreadimpl_is_lock_debugging_enabled(void)
{
	return _evthread_lock_debugging_enabled;
}

int
_evthreadimpl_locking_enabled(void)
{
	return _evthread_lock_fns.lock != NULL;
}
#endif

#endif
