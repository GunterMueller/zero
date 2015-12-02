/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

/* -*- mode:C++; c-basic-offset:4 -*-
     Shore-MT -- Multi-threaded port of the SHORE storage manager

                       Copyright (c) 2007-2009
      Data Intensive Applications and Systems Labaratory (DIAS)
               Ecole Polytechnique Federale de Lausanne

                         All Rights Reserved.

   Permission to use, copy, modify and distribute this software and
   its documentation is hereby granted, provided that both the
   copyright notice and this permission notice appear in all copies of
   the software, derivative works or modified versions, and any
   portions thereof, and that both notices appear in supporting
   documentation.

   This code is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
   DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
   RESULTING FROM THE USE OF THIS SOFTWARE.
*/

/*<std-header orig-src='shore'>

 $Id: chkpt.cpp,v 1.81 2010/07/29 21:22:46 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the
software, derivative works or modified versions, and any portions
thereof, and that both notices appear in supporting documentation.

THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
"AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

This software was developed with support by the Advanced Research
Project Agency, ARPA order number 018 (formerly 8230), monitored by
the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
Further funding for this work was provided by DARPA through
Rome Research Laboratory Contract No. F30602-97-2-0247.

*/

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/


#define SM_SOURCE
#define CHKPT_C

#include "sm_base.h"
#include "chkpt_serial.h"
#include "chkpt.h"
#include "btree_logrec.h"       // Lock re-acquisition
#include "bf_tree.h"
#include "xct_dependent.h"
#include <new>
#include "sm.h"
#include "lock_raw.h"      // Lock information gathering
#include "w_okvl_inl.h"    // Lock information gathering
struct RawLock;            // Lock information gathering
#include "restart.h"
#include "vol.h"
#include <algorithm>

#include "stopwatch.h"

#ifdef EXPLICIT_TEMPLATE
template class w_auto_delete_array_t<lsn_t>;
template class w_auto_delete_array_t<tid_t>;
template class w_auto_delete_array_t<ss_m::xct_state_t>;
#endif


/*********************************************************************
 *
 *  class chkpt_thread_t
 *
 *  Checkpoint thread.
 *
 *********************************************************************/
class chkpt_thread_t : public smthread_t
{
public:
    NORET                chkpt_thread_t();
    NORET                ~chkpt_thread_t();

    virtual void        run();
    void                retire();
    void                awaken();
    bool                is_retired() {return _retire;}

private:
    bool                _retire;
    pthread_mutex_t     _retire_awaken_lock; // paired with _retire_awaken_cond
    pthread_cond_t      _retire_awaken_cond; // paried with _retire_awaken_lock
    bool                _kicked;

    // Simple counter to keep track total pending checkpoint requests
    unsigned int        chkpt_count;

    // disabled
    NORET                chkpt_thread_t(const chkpt_thread_t&);
    chkpt_thread_t&      operator=(const chkpt_thread_t&);
};


/*****************************************************
// Dead code, comment out just in case we need to re-visit it in the future

 // Not waiting on old transactions to finish

struct old_xct_tracker {
    struct dependent : public xct_dependent_t  {
        w_link_t _link;
        old_xct_tracker* _owner;

        dependent(xct_t* xd, old_xct_tracker* owner)
            : xct_dependent_t(xd), _owner(owner)
        {
            register_me();
        }

        virtual void xct_state_changed(ss_m::xct_state_t,
              ss_m::xct_state_t new_state)
        {
            if(new_state == ss_m::xct_ended)
            _owner->report_finished(xd());
        }
    };

    old_xct_tracker() : _list(W_LIST_ARG(dependent, _link), 0) , _count(0)
    {
        pthread_mutex_init(&_lock, 0);
        pthread_cond_init(&_cond, 0);
    }

    ~old_xct_tracker() {
        w_assert2(! _count);
        while(_list.pop());
    }

    void track(xct_t* xd) {
        dependent* d = new dependent(xd, this);
        pthread_mutex_lock(&_lock);
        _count++;
        _list.push(d);
        pthread_mutex_unlock(&_lock);
    }

    void wait_for_all() {
        pthread_mutex_lock(&_lock);
        while(_count)
            pthread_cond_wait(&_cond, &_lock);
        pthread_mutex_unlock(&_lock);
    }

    void report_finished(xct_t*) {
        pthread_mutex_lock(&_lock);
        if(! --_count)
            pthread_cond_signal(&_cond);
        pthread_mutex_unlock(&_lock);
    }

    pthread_mutex_t    _lock;
    pthread_cond_t     _cond;
    w_list_t<dependent, unsafe_list_dummy_lock_t> _list;
    long             _count;

};
*****************************************************/


/*********************************************************************
 *
 *  chkpt_m::chkpt_m()
 *
 *  Constructor for Checkpoint Manager.
 *
 *********************************************************************/
NORET
chkpt_m::chkpt_m()
    : _chkpt_thread(0), _chkpt_count(0)
{
    _chkpt_last = ss_m::log->master_lsn();
    if(_chkpt_last == lsn_t::null) {
        _chkpt_last = lsn_t(1,0);
    }
}

/*********************************************************************
 *
 *  chkpt_m::~chkpt_m()
 *
 *  Destructor. If a thread is spawned, tell it to exit.
 *
 *********************************************************************/
NORET
chkpt_m::~chkpt_m()
{
    if (_chkpt_thread)
    {
        retire_chkpt_thread();
    }

    // Destruct the chkpt_m thresd (itself), if it is a simulated crash,
    // the system might be in the middle of taking a checkpoint, and
    // the chkpt_m thread has the chekpt chkpt_serial_m mutex and won't have
    // a chance to release the mutex
    // We cannot blindly release the mutex because it will get into infinite wait
    // in such case, we might see debug assert (core dump) from
    //     ~w_pthread_lock_t() { w_assert1(!_holder); pthread_mutex_destroy(&_mutex);}
}


/*********************************************************************
 *
 *  chkpt_m::spawn_chkpt_thread()
 *
 *  Fork the checkpoint thread.
 *  Caller should spawn the chkpt thread immediatelly after
 *  the chkpt_m has been created.
 *
 *********************************************************************/
void
chkpt_m::spawn_chkpt_thread()
{
    w_assert1(_chkpt_thread == 0);
    if (ss_m::log)
    {
        // Create thread (1) to take checkpoints
        _chkpt_thread = new chkpt_thread_t;
        if (! _chkpt_thread)
            W_FATAL(eOUTOFMEMORY);
        W_COERCE(_chkpt_thread->fork());
    }
}



/*********************************************************************
 *
 *  chkpt_m::retire_chkpt_thread()
 *
 *  Kill the checkpoint thread.
 *
 *  Called from:
 *      chkpt_m destructor
 *      ss_m::_destruct_once() - shutdown storage manager, signal chkpt thread
 *                                            to retire before destroy chkpt_m
 *
 *********************************************************************/
void
chkpt_m::retire_chkpt_thread()
{
    if (ss_m::log)
    {
        w_assert1(_chkpt_thread);

        // Notify the checkpoint thread to retire itself
        _chkpt_thread->retire();
        W_COERCE( _chkpt_thread->join() ); // wait for it to end
        delete _chkpt_thread;
        _chkpt_thread = 0;
    }
}

/*********************************************************************
*
*  chkpt_m::wakeup_and_take()
*
*  Issue an asynch checkpoint request
*
*********************************************************************/
void
chkpt_m::wakeup_and_take()
{
    if(ss_m::log && _chkpt_thread)
    {
        INC_TSTAT(log_chkpt_wake);
        _chkpt_thread->awaken();
    }
}

/*********************************************************************
*
*  chkpt_m::synch_take()
*
*  Issue an synch checkpoint request
*
*********************************************************************/
void chkpt_m::synch_take()
{
    take(t_chkpt_sync);
    return;
}


/*********************************************************************
*
*  chkpt_m::backward_scan_log(lock_heap)
*
*  Scans the log backwards, starting from _lsn until the t_chkpt_begin log record
*  corresponding to the latest completed checkpoint.
*
*********************************************************************/
void chkpt_m::backward_scan_log(const lsn_t master_lsn,
                                const lsn_t begin_lsn,
                                chkpt_t& new_chkpt,
                                const bool acquire_locks)
{
    w_assert0(begin_lsn >= master_lsn);
    new_chkpt.init(begin_lsn);

    if (master_lsn == lsn_t::null) {
        // The only possibility that we have a NULL as master lsn is due to a brand new
        // start (empty) of the engine, in such case, nothing to scan.

        // LL: we have to guarantee that this is the first chkpt ever being taken.
        // In this case, the chkpt begin lsn must be the very first one.
        w_assert0(begin_lsn == lsn_t(1,0));
        DBGOUT1( << "NULL master_lsn, nothing to scan");
        return;
    }

    DBGOUT1(<<"backward_scan_log("<<begin_lsn<<", "<<master_lsn<<")");

    log_i         scan(*log, begin_lsn, false); // false == backward scan
    logrec_t      r;
    lsn_t         lsn;   // LSN of the retrieved log record

    bool insideChkpt = false;
    bool scan_done = false;
    lsn_t chkpt_begin = master_lsn;

    while (scan.xct_next(lsn, r) && !scan_done)
    {
        if (r.is_skip() || r.type() == logrec_t::t_comment) {
            continue;
        }

        if (!r.tid().is_null()) {
            if (r.tid() > new_chkpt.get_highest_tid()) {
                new_chkpt.set_highest_tid(r.tid());
            }

            if (r.xid_prev().is_null()) {
                // We won't see this xct again -- delete it
                new_chkpt.delete_xct(r.tid());
            }
            else {
                new_chkpt.mark_xct_active(r.tid(), lsn, lsn);
                if (acquire_locks) {
                    _acquire_lock(r, new_chkpt);
                }
            }
        }

        if (r.is_page_update()) {
            w_assert0(r.is_redo() && !r.null_pid());
            new_chkpt.mark_page_dirty(r.pid(), lsn, lsn, r.stid());

            if (r.is_multi_page()) {
                w_assert0(r.pid2() != 0);
                new_chkpt.mark_page_dirty(r.pid2(), lsn, lsn, r.stid());
            }
        }

        switch (r.type())
        {
            case logrec_t::t_chkpt_begin:
                if (insideChkpt) {
                    // Signal to stop backward log scan loop now
                    w_assert1(master_lsn <= lsn);
                    scan_done = true;
                }
                break;

            case logrec_t::t_chkpt_bf_tab:
                if (insideChkpt) {
                    const chkpt_bf_tab_t* dp = (chkpt_bf_tab_t*) r.data();
                    for (uint i = 0; i < dp->count; i++) {
                        w_assert0(dp->brec[i].pid != 0);
                        new_chkpt.mark_page_dirty(dp->brec[i].pid, dp->brec[i].page_lsn,
                                dp->brec[i].rec_lsn, dp->brec[i].store);
                    }
                }
                break;


            case logrec_t::t_chkpt_xct_lock:
                if (insideChkpt && acquire_locks) {
                    const chkpt_xct_lock_t* dp = (chkpt_xct_lock_t*) r.data();
                    if (new_chkpt.is_xct_active(dp->tid)) {
                        for (uint i = 0; i < dp->count; i++) {
                            new_chkpt.add_lock(dp->tid,
                                    dp->xrec[i].lock_mode, dp->xrec[i].lock_hash);
                        }
                    }
                }
                break;

            case logrec_t::t_chkpt_xct_tab:
                if (insideChkpt) {
                    const chkpt_xct_tab_t* dp = (chkpt_xct_tab_t*) r.data();
                    for (size_t i = 0; i < dp->count; ++i) {
                        tid_t tid = dp->xrec[i].tid;
                        w_assert1(!tid.is_null());
                        new_chkpt.mark_xct_active(tid, dp->xrec[i].first_lsn,
                                dp->xrec[i].last_lsn);
                    }
                }
                break;


            case logrec_t::t_chkpt_end:
                // checkpoints should not run concurrently
                w_assert0(!insideChkpt);
                chkpt_begin = *((lsn_t*) r.data());
                w_assert0(master_lsn > chkpt_begin);
                insideChkpt = true;
                break;

            // CS TODO: why do we need this? Isn't it related to 2PC?
            case logrec_t::t_xct_freeing_space:
            case logrec_t::t_xct_end:
            case logrec_t::t_xct_abort:
                new_chkpt.mark_xct_ended(r.tid());
                break;

            case logrec_t::t_xct_end_group:
                {
                    // CS TODO: is this type of group commit still used?
                    w_assert0(false);
                    const xct_list_t* list = (xct_list_t*) r.data();
                    uint listlen = list->count;
                    for(uint i=0; i<listlen; i++) {
                        tid_t tid = list->xrec[i].tid;
                        new_chkpt.mark_xct_ended(tid);
                    }
                }
                break;

            case logrec_t::t_page_write:
                {
                    PageID pid = *((PageID*) r.data());
                    uint32_t count = *((uint32_t*) r.data() + sizeof(PageID));
                    PageID end = pid + count;

                    while (pid < end) {
                        new_chkpt.mark_page_clean(pid, lsn);
                        pid++;
                    }
                }
                break;

            case logrec_t::t_add_backup:
                {
                    const char* dev = (const char*)(r.data_ssx());
                    new_chkpt.add_backup(dev);
                }
                break;

            case logrec_t::t_chkpt_backup_tab:
                if (insideChkpt) {
                    // CS TODO
                }
                break;

            case logrec_t::t_restore_begin:
            case logrec_t::t_restore_end:
            case logrec_t::t_restore_segment:
            case logrec_t::t_chkpt_restore_tab:
                // CS TODO - IMPLEMENT!
                break;

            default:
                break;

        } //switch
    } //while

    w_assert0(scan_done);
    w_assert0(!chkpt_begin.is_null());
    w_assert0(chkpt_begin >= master_lsn);

    new_chkpt.cleanup();
}

void chkpt_t::init(lsn_t begin_lsn)
{
    this->begin_lsn = begin_lsn;
    buf_tab.clear();
    xct_tab.clear();
    bkp_path.clear();
}

void chkpt_t::mark_page_dirty(PageID pid, lsn_t page_lsn, lsn_t rec_lsn,
        StoreID store)
{
    // operator[] adds an empty dirty entry if key is not found
    buf_tab_entry_t e = buf_tab[pid];
    if (page_lsn > e.page_lsn) { e.page_lsn = page_lsn; }
    if (rec_lsn < e.rec_lsn) { e.rec_lsn = rec_lsn; }
    // CS TODO: why do we need the store?
    e.store = store;
}

void chkpt_t::mark_page_clean(PageID pid, lsn_t lsn)
{
    if(buf_tab.count(pid) > 0) {
        if(buf_tab[pid].page_lsn < lsn) {
            buf_tab[pid].page_lsn = lsn;
            buf_tab[pid].dirty = false;
        }
    }
    else {
        // Page does not exist in bf_table, we add it with
        // dummy values and mark as !dirty
        buf_tab[pid].store = 0;
        buf_tab[pid].rec_lsn = lsn_t::null;
        buf_tab[pid].page_lsn = lsn;
        buf_tab[pid].dirty = false;
    }
}

bool chkpt_t::is_xct_active(tid_t tid) const
{
    xct_tab_t::const_iterator iter = xct_tab.find(tid);
    if (iter == xct_tab.end()) {
        return false;
    }
    return iter->second.state;
}

void chkpt_t::mark_xct_active(tid_t tid, lsn_t first_lsn, lsn_t last_lsn)
{
    // operator[] adds an empty active entry if key is not found
    xct_tab_entry_t e = xct_tab[tid];
    if (last_lsn > e.last_lsn) { e.last_lsn = last_lsn; }
    if (first_lsn < e.first_lsn) { e.first_lsn = first_lsn; }
}

void chkpt_t::mark_xct_ended(tid_t tid)
{
    xct_tab[tid].state = xct_t::xct_ended;
}

void chkpt_t::delete_xct(tid_t tid)
{
    xct_tab.erase(tid);
}

void chkpt_t::add_backup(const char* path)
{
    bkp_path = path;
}

void chkpt_t::add_lock(tid_t tid, okvl_mode mode, uint32_t hash)
{
    if (!is_xct_active(tid)) { return; }
    lock_info_t entry;
    entry.lock_mode = mode;
    entry.lock_hash = hash;
    xct_tab[tid].locks.push_back(entry);
}

void chkpt_t::cleanup()
{
    // Remove non-dirty pages
    for(buf_tab_t::iterator it  = buf_tab.begin();
                            it != buf_tab.end(); ) {
        if(it->second.dirty == false) {
            buf_tab.erase(it++);
        }
        else {
            ++it;
        }
    }

    // Remove finished transactions.
    for(xct_tab_t::iterator it  = xct_tab.begin();
                            it != xct_tab.end(); ) {
        if(it->second.state == xct_t::xct_ended) {
            xct_tab.erase(it++);      //erase xct
        }
        else {
            ++it;
        }
    }
}

lsn_t chkpt_t::get_min_xct_lsn() const
{
    lsn_t min_xct_lsn = lsn_t::max;
    for(xct_tab_t::const_iterator it = xct_tab.begin();
            it != xct_tab.end(); ++it)
    {
        if(it->second.state != xct_t::xct_ended
                && min_xct_lsn > it->second.first_lsn)
        {
            min_xct_lsn = it->second.first_lsn;
        }
    }
    if (min_xct_lsn == lsn_t::max) { return lsn_t::null; }
    return min_xct_lsn;
}

lsn_t chkpt_t::get_min_rec_lsn() const
{
    lsn_t min_rec_lsn = lsn_t::max;
    for(buf_tab_t::const_iterator it = buf_tab.begin();
            it != buf_tab.end(); ++it)
    {
        if(it->second.dirty && min_rec_lsn > it->second.rec_lsn) {
            min_rec_lsn = it->second.rec_lsn;
        }
    }
    if (min_rec_lsn == lsn_t::max) { return lsn_t::null; }
    return min_rec_lsn;
}

/*********************************************************************
*
*  chkpt_m::_analysis_acquire_lock_log(r, xd, lock_heap)
*
*  Helper function to process lock re-acquisition based on the log record
*  called by analysis_pass_backward only
*
*  System is not opened during Log Analysis phase
*
*********************************************************************/
void chkpt_m::_acquire_lock(logrec_t& r,            // In: log record
                                         chkpt_t& new_chkpt)
{
    w_assert1(new_chkpt.is_xct_active(r.tid()));
    w_assert1(!r.is_single_sys_xct());
    w_assert1(!r.is_multi_page());
    w_assert1(!r.is_cpsn());
    w_assert1(!r.null_pid());
    w_assert1(r.is_page_update());

    switch (r.type())
    {
        case logrec_t::t_btree_insert:
        case logrec_t::t_btree_insert_nonghost:
            {
                btree_insert_t* dp = (btree_insert_t*) r.data();

                w_keystr_t key;
                key.construct_from_keystr(dp->data, dp->klen);

                okvl_mode mode = btree_impl::create_part_okvl(okvl_mode::X, key);
                lockid_t lid (r.stid(), (const unsigned char*) key.buffer_as_keystr(),
                        key.get_length_as_keystr());

                new_chkpt.add_lock(r.tid(), mode, lid.hash());
            }
            break;
        case logrec_t::t_btree_update:
            {
                btree_update_t* dp = (btree_update_t*) r.data();

                w_keystr_t key;
                key.construct_from_keystr(dp->_data, dp->_klen);

                okvl_mode mode = btree_impl::create_part_okvl(okvl_mode::X, key);
                lockid_t lid (r.stid(), (const unsigned char*) key.buffer_as_keystr(),
                        key.get_length_as_keystr());

                new_chkpt.add_lock(r.tid(), mode, lid.hash());
            }
            break;
        case logrec_t::t_btree_overwrite:
            {
                btree_overwrite_t* dp = (btree_overwrite_t*) r.data();

                w_keystr_t key;
                key.construct_from_keystr(dp->_data, dp->_klen);

                okvl_mode mode = btree_impl::create_part_okvl(okvl_mode::X, key);
                lockid_t lid (r.stid(), (const unsigned char*) key.buffer_as_keystr(),
                        key.get_length_as_keystr());

                new_chkpt.add_lock(r.tid(), mode, lid.hash());
            }
            break;
        case logrec_t::t_btree_ghost_mark:
            {
                btree_ghost_t* dp = (btree_ghost_t*) r.data();
                for (size_t i = 0; i < dp->cnt; ++i) {
                    w_keystr_t key (dp->get_key(i));

                    okvl_mode mode = btree_impl::create_part_okvl(okvl_mode::X, key);
                    lockid_t lid (r.stid(), (const unsigned char*) key.buffer_as_keystr(),
                            key.get_length_as_keystr());

                    new_chkpt.add_lock(r.tid(), mode, lid.hash());
                }
            }
            break;
        case logrec_t::t_btree_ghost_reserve:
            {
                btree_ghost_reserve_t* dp = (btree_ghost_reserve_t*) r.data();

                w_keystr_t key;
                key.construct_from_keystr(dp->data, dp->klen);

                okvl_mode mode = btree_impl::create_part_okvl(okvl_mode::X, key);
                lockid_t lid (r.stid(), (const unsigned char*) key.buffer_as_keystr(),
                        key.get_length_as_keystr());

                new_chkpt.add_lock(r.tid(), mode, lid.hash());
            }
            break;
        default:
            w_assert0(r.type() == logrec_t::t_page_img_format);
            break;
    }

    return;
}

void chkpt_m::take(chkpt_mode_t chkpt_mode)
{
    if (t_chkpt_async == chkpt_mode) {
        DBGOUT1(<< "Checkpoint request: asynch");
    }
    else {
        DBGOUT1(<< "Checkpoint request: synch");
    }

    if (!ss_m::log) {
        return; // recovery facilities disabled ... do nothing
    }

    /*
     * Checkpoints are fuzzy but must be serialized wrt each other.
     *
     * Acquire the 'write' mutex immediatelly to serialize concurrent checkpoint requests.
     *
     * NB: EVERYTHING BETWEEN HERE AND RELEASING THE MUTEX
     * MUST BE W_COERCE (not W_DO).
     *
     * The W_COERCE is like W_DO(x), but instead of returning in the error
     * case, it fails catastrophically.
     * It is used in checkpoint function because
     *   Checkpoint has no means to return error information
     */
    chkpt_serial_m::write_acquire();
    DBGOUT1(<<"BEGIN chkpt_m::take");

    INC_TSTAT(log_chkpt_cnt);

    // Start the initial validation check for make sure the incoming checkpoint request is valid
    bool valid_chkpt = true;
    {
        if (ss_m::log && _chkpt_thread)
        {
            // Log is on and chkpt thread is available, but the chkpt thread has received
            // a 'retire' message (signal to shutdown), return without doing anything
            if (true == _chkpt_thread->is_retired())
            {
                DBGOUT1(<<"END chkpt_m::take - detected retire, skip checkpoint");
                valid_chkpt = false;
            }
        }
        else if ((ss_m::shutting_down) && (t_chkpt_async == chkpt_mode))
        {
            // No asynch checkpoint if we are shutting down
            DBGOUT1(<<"END chkpt_m::take - detected shutdown, skip asynch checkpoint");
            valid_chkpt = false;
        }
        else if ((ss_m::shutting_down) && (t_chkpt_sync == chkpt_mode))
        {
            // Middle of shutdown, allow synch checkpoint request
            DBGOUT1(<<"PROCESS chkpt_m::take - system shutdown, allow synch checkpoint");
        }
        else
        {
            // Not in shutdown
            if (ss_m::before_recovery())
            {
                DBGOUT1(<<"END chkpt_m::take - before system startup/recovery, skip checkpoint");
                valid_chkpt = false;
            }
            else if (ss_m::ss_m::in_recovery() && (t_chkpt_sync != chkpt_mode))
            {
                // Asynch checkpoint
                if (false == ss_m::use_serial_restart())
                {
                    // System opened after Log Analysis phase, allow asynch checkpoint
                    // after Log Analysis phase
                    if (ss_m::ss_m::in_recovery_analysis())
                        valid_chkpt = false;
                }
                else
                {
                    // System is not opened during recovery
                    DBGOUT1(<<"END chkpt_m::take - system in recovery, skip asynch checkpoint");
                    valid_chkpt = false;
                }
            }
            else if (ss_m::in_recovery() && (t_chkpt_sync == chkpt_mode))
            {
                // Synch checkpoint
                if (false == ss_m::use_serial_restart())
                {
                    // System opened after Log Analysis phase, accept system checkpoint anytime
                    DBGOUT1(<<"PROCESS chkpt_m::take - system in recovery, allow synch checkpoint");
                }
                else
                {
                    // System is not opened during recovery
                    if (ss_m::in_recovery_analysis() || ss_m::in_recovery_undo())
                    {
                        DBGOUT1(<<"PROCESS chkpt_m::take - system in recovery, allow synch checkpoint");
                    }
                    else
                    {
                        DBGOUT1(<<"END chkpt_m::take - system in REDO phase, disallow checkpoint");
                        valid_chkpt = false;
                    }
                }
            }
            else
            {
                // We cannot be in recovery if we get here
                if (true == ss_m::in_recovery())
                {
                    DBGOUT1(<<"END chkpt_m::take - system should not be in Recovery, exist checkpoint");
                    valid_chkpt = false;
                }
                else
                {
                    if (t_chkpt_sync == chkpt_mode)
                    {
                        DBGOUT1(<<"PROCESS chkpt_m::take - allow synch/internal checkpoint");
                    }
                    else
                    {
                        DBGOUT1(<<"PROCESS chkpt_m::take - allow asynch/user checkpoint");
                    }
                }
            }
        }
    }

     // Done with the checkpoint validation, should we continue?
    if (!valid_chkpt) {
        // Failed the checkpoint validation, release the 'write' mutex and exist
        chkpt_serial_m::write_release();
        return;
    }

    // Verifies if there is enough space to insert the maximum lenght of a chkpt
    // in the log.
    if(!ss_m::log->verify_chkpt_reservation()) {
        chkpt_serial_m::write_release();
        W_FATAL(eOUTOFLOGSPACE);
    }

    // Allocate a buffer for storing log records
    w_auto_delete_t<logrec_t> logrec(new logrec_t);

#define LOG_INSERT(constructor_call, rlsn)            \
    do {                                              \
        new (logrec) constructor_call;                \
        W_COERCE( ss_m::log->insert(*logrec, rlsn) );       \
        if(!ss_m::log->consume_chkpt_reservation(logrec->length())) { \
            chkpt_serial_m::write_release();                    \
            W_FATAL(eOUTOFLOGSPACE);                            \
        }                                                       \
    } while(0)

    const lsn_t curr_lsn = ss_m::log->curr_lsn();
    lsn_t begin_lsn = lsn_t::null;

    // Insert chkpt_begin log record.
    LOG_INSERT(chkpt_begin_log(lsn_t::null), &begin_lsn);
    W_COERCE(ss_m::log->flush_all() );

    w_assert1(curr_lsn.data() <= begin_lsn.data());

    // Backward scan from begin_lsn to first completed chkpt (may not be master).
    lsn_t master_lsn = ss_m::log->master_lsn();
    chkpt_t new_chkpt;

    backward_scan_log(master_lsn, begin_lsn, new_chkpt, true);

    //============================ WRITE LOG RECORDS ===========================

    uint chunk;

    // Serialize bkp_tab
    vector<string> backup_paths;
    backup_paths.push_back(new_chkpt.bkp_path);
    LOG_INSERT(chkpt_backup_tab_log(backup_paths.size(),
                (const string*)(&backup_paths[0])), 0);

    //LOG_INSERT(chkpt_restore_tab_log(vol->vid()), 0);

    // Serialize buf_tab
    chunk = chkpt_bf_tab_t::max;
    vector<PageID> pid;
    vector<StoreID> store;
    vector<lsn_t> rec_lsn;
    vector<lsn_t> page_lsn;
    for(buf_tab_t::const_iterator it = new_chkpt.buf_tab.begin();
            it != new_chkpt.buf_tab.end(); ++it)
    {
        DBGOUT1(<<"pid[]="<<it->first<< " , " <<
                  "store[]="<<it->second.store<< " , " <<
                  "rec_lsn[]="<<it->second.rec_lsn<< " , " <<
                  "page_lsn[]="<<it->second.page_lsn);
        pid.push_back(it->first);
        store.push_back(it->second.store);
        rec_lsn.push_back(it->second.rec_lsn);
        page_lsn.push_back(it->second.page_lsn);
         if(pid.size()==chunk || &*it==&*new_chkpt.buf_tab.rbegin()) {
            LOG_INSERT(chkpt_bf_tab_log(pid.size(), (const PageID*)(&pid[0]),
                                                    (const StoreID*)(&store[0]),
                                                    (const lsn_t*)(&rec_lsn[0]),
                                                    (const lsn_t*)(&page_lsn[0])), 0);
            pid.clear();
            store.clear();
            rec_lsn.clear();
            page_lsn.clear();
         }
    }

    chunk = chkpt_xct_tab_t::max;
    vector<tid_t> tid;
    vector<smlevel_0::xct_state_t> state;
    vector<lsn_t> last_lsn;
    vector<lsn_t> first_lsn;
    vector<okvl_mode> lock_mode;
    vector<uint32_t> lock_hash;
    for(xct_tab_t::const_iterator it=new_chkpt.xct_tab.begin();
            it != new_chkpt.xct_tab.end(); ++it) {
        DBGOUT1(<<"tid[]="<<it->first<<" , " <<
                  "state[]="<<it->second.state<< " , " <<
                  "last_lsn[]="<<it->second.last_lsn<<" , " <<
                  "first_lsn[]="<<it->second.first_lsn);

        tid.push_back(it->first);
        state.push_back(it->second.state);
        last_lsn.push_back(it->second.last_lsn);
        first_lsn.push_back(it->second.first_lsn);
        if(tid.size()==chunk || &*it==&*new_chkpt.xct_tab.rbegin()) {
            LOG_INSERT(chkpt_xct_tab_log(new_chkpt.get_highest_tid(), tid.size(),
                                        (const tid_t*)(&tid[0]),
                                        (const smlevel_0::xct_state_t*)(&state[0]),
                                        (const lsn_t*)(&last_lsn[0]),
                                        (const lsn_t*)(&first_lsn[0])), 0);
            tid.clear();
            state.clear();
            last_lsn.clear();
            first_lsn.clear();
        }

        // gather lock table
        for(list<lock_info_t>::const_iterator jt = it->second.locks.begin();
                jt != it->second.locks.end(); ++jt)
        {
            DBGOUT1(<<"    lock_mode[]="<<jt->lock_mode<<" , lock_hash[]="<<jt->lock_hash);
            lock_mode.push_back(jt->lock_mode);
            lock_hash.push_back(jt->lock_hash);
            if(lock_mode.size() == chunk) {
                LOG_INSERT(chkpt_xct_lock_log(it->first,
                                      lock_mode.size(),
                                      (const okvl_mode*)(&lock_mode[0]),
                                      (const uint32_t*)(&lock_hash[0])), 0);
                lock_mode.clear();
                lock_hash.clear();
            }
        }
        if(lock_mode.size() > 0) {
            LOG_INSERT(chkpt_xct_lock_log(it->first,
                        lock_mode.size(),
                        (const okvl_mode*)(&lock_mode[0]),
                        (const uint32_t*)(&lock_hash[0])), 0);
            lock_mode.clear();
            lock_hash.clear();
        }
    }

    // In case the transaction table was empty, we insert a xct_tab_log anyway,
    // because we want to save the highest tid.
    if(new_chkpt.xct_tab.size() == 0) {
        LOG_INSERT(chkpt_xct_tab_log(new_chkpt.get_highest_tid(), tid.size(),
                                        (const tid_t*)(&tid[0]),
                                        (const smlevel_0::xct_state_t*)(&state[0]),
                                        (const lsn_t*)(&last_lsn[0]),
                                        (const lsn_t*)(&first_lsn[0])), 0);
    }
    //==========================================================================

    if (ss_m::shutting_down && !ss_m::shutdown_clean) // Dirty shutdown (simulated crash)
    {
        DBGOUT1(<<"chkpt_m::take ABORTED due to dirty shutdown, "
                << ", dirty page count = " << pid.size()
                << ", total txn count = " << tid.size());
    }
    else
    {
        LOG_INSERT(chkpt_end_log (new_chkpt.get_begin_lsn(),
                                  new_chkpt.get_min_rec_lsn(),
                                  new_chkpt.get_min_xct_lsn()), 0);

        W_COERCE(ss_m::log->flush_all() );
        DBGOUT1(<<"Setting master_lsn to " << new_chkpt.get_begin_lsn());
        ss_m::log->set_master(new_chkpt.get_begin_lsn(),
                              new_chkpt.get_min_rec_lsn(),
                              new_chkpt.get_min_xct_lsn());
    }

    DBGOUT1(<<"Exiting take()");

    // Release the 'write' mutex so the next checkpoint request can come in
    chkpt_serial_m::write_release();
}

/*********************************************************************
 *
 *  chkpt_thread_t::chkpt_thread_t()
 *
 *  Construct a Checkpoint Thread. Priority level is t_time_critical
 *  so that checkpoints are done as fast as it could be done. Most of
 *  the time, however, the checkpoint thread is blocked waiting for
 *  a go-ahead signal to take a checkpoint.
 *
 *********************************************************************/
chkpt_thread_t::chkpt_thread_t()
    : smthread_t(t_time_critical, "chkpt", WAIT_NOT_USED),
    _retire(false), _kicked(false), chkpt_count(0)
{
    rename("chkpt_thread");            // for debugging
    DO_PTHREAD(pthread_mutex_init(&_retire_awaken_lock, NULL));
    DO_PTHREAD(pthread_cond_init(&_retire_awaken_cond, NULL));
}


/*********************************************************************
 *
 *  chkpt_thread_t::~chkpt_thread_t()
 *
 *  Destroy a checkpoint thread.
 *
 *********************************************************************/
chkpt_thread_t::~chkpt_thread_t()
{
    /* empty */
}



/*********************************************************************
 *
 *  chkpt_thread_t::run()
 *
 *  Body of checkpoint thread. Repeatedly:
 *    1. wait for signal to activate
 *    2. if retire intention registered, then quit
 *    3. write all buffer pages dirtied before the n-1 checkpoint
 *    4. if toggle off then take a checkpoint
 *    5. flip the toggle
 *    6. goto 1
 *
 *  Essentially, the thread will take one checkpoint for every two
 *  wakeups.
 *
 *********************************************************************/
void
chkpt_thread_t::run()
{
    CmpXctLockTids	 lock_cmp;
    XctLockHeap      dummy_heap(lock_cmp);

    while(! _retire)
    {
        {
            // Enter mutex first
            CRITICAL_SECTION(cs, _retire_awaken_lock);
            while(!_kicked  && !_retire)
            {
                // Unlock the mutex and wait on _retire_awaken_cond (checkpoint request)
                DO_PTHREAD(pthread_cond_wait(&_retire_awaken_cond, &_retire_awaken_lock));

                // On return, we own the mutex again
            }
            // On a busy system it is possible (but rare) to have more than one pending
            // checkpoint requests, we will only execute checkpoint once in such case
            if (true == _kicked)
            {
                w_assert1(0 < chkpt_count);
                DBG(<<"Found pending checkpoint request count: " << chkpt_count);
                chkpt_count = 0;
            }

            // Reset the flag before we continue to execute the checkpoint
            _kicked = false;
        }

        w_assert1(ss_m::chkpt);

        // If a retire request arrived, exit immediatelly without checkpoint
        if(_retire)
            break;

        // No need to acquire checkpoint mutex before calling the checkpoint operation
        // Asynch checkpoint should never record lock information

        ss_m::chkpt->take(chkpt_m::t_chkpt_async);
    }
}


/*********************************************************************
 *
 *  chkpt_thread_t::retire()
 *
 *  Register an intention to retire and activate the thread.
 *  The thread will exit when it wakes up and checks the retire
 *  flag.
 *  If the thread is in the middle of executing a checkpoint, it won't check the
 *  retire flag until after the checkpoint finished execution.
 *
 *********************************************************************/
void
chkpt_thread_t::retire()
{
     CRITICAL_SECTION(cs, _retire_awaken_lock);
     _retire = true;
     DO_PTHREAD(pthread_cond_signal(&_retire_awaken_cond));
}


/*********************************************************************
 *
 *  chkpt_thread_t::awaken()
 *
 *  Signal an asynch checkpoint request arrived.
 *
 *********************************************************************/
void
chkpt_thread_t::awaken()
{
    CRITICAL_SECTION(cs, _retire_awaken_lock);
    _kicked = true;
    ++chkpt_count;
    DO_PTHREAD(pthread_cond_signal(&_retire_awaken_cond));
}
