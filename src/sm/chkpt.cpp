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

#include "sm_int_1.h"
#include "chkpt_serial.h"
#include "chkpt.h"
#include "logdef_gen.cpp"
#include "bf_tree.h"
#include "xct_dependent.h"
#include <new>
#include "sm.h"

#ifdef EXPLICIT_TEMPLATE
template class w_auto_delete_array_t<lsn_t>;
template class w_auto_delete_array_t<tid_t>;
template class w_auto_delete_array_t<smlevel_1::xct_state_t>;
#endif




/*********************************************************************
 *
 *  class chkpt_thread_t
 *
 *  Checkpoint thread. 
 *
 *********************************************************************/
class chkpt_thread_t : public smthread_t  {
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
    chkpt_thread_t&        operator=(const chkpt_thread_t&);
};


struct old_xct_tracker {
    struct dependent : public xct_dependent_t  {
        w_link_t _link;
        old_xct_tracker* _owner;

        dependent(xct_t* xd, old_xct_tracker* owner)
            : xct_dependent_t(xd), _owner(owner)
        {
            register_me();
        }
    
        virtual void xct_state_changed(smlevel_1::xct_state_t,
              smlevel_1::xct_state_t new_state)
        {
            if(new_state == smlevel_1::xct_ended) 
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
    if (_chkpt_thread) {
    retire_chkpt_thread();
    }
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
    if (smlevel_0::log)  {
        /* Create thread (1) to take checkpoints */
        _chkpt_thread = new chkpt_thread_t;
        if (! _chkpt_thread)  W_FATAL(eOUTOFMEMORY);
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
    if (log)  {
        w_assert1(_chkpt_thread);
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
    if(log && _chkpt_thread) {
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
    if(log)
    {
        // No need to acquire any mutex on checkpoint before calling the checkpoint function
        // The checkpoint function acqures the 'write' mutex internally
        take(smlevel_0::t_chkpt_sync);
    }
    return;
}


/*********************************************************************
 *
 *  chkpt_m::take()
 *
 *  This is the actual checkpoint function, which can be executed two different ways:
 *    1. Asynch request: does not block caller thread, i.e. user checkpoint
 *    2. Synch execution: blocks the caller thread
 *
 *  A checkpoint request does not mean a checkpoint will be executed:
 *    1. System is already in the middle of shutting down.  Note that shutdown process
 *        issues a synchronous checkpoint as part of shutdown
 *    2. No activities since previous checkpoint
 *          - 'end checkpoint' is the last log in the recovery log
 *          - previous checkpoint did not skip transaction or dirty page
 *    3. System started but recovery has not started yet
 *    4. In the middle of recovery, exceptions
 *          - at the end of Log Analysis phase - synchronous checkpoint
 *          - at the end of UNDO phase - synchronous checkpoint
 *
 *  Take a checkpoint. A Checkpoint consists of:
 *    1. Checkpoint Begin Log    (chkpt_begin)
 *    2. Checkpoint Device Table Log(s) (chkpt_dev_tab)     <-- this step is eliminated 
 *        -- all mounted devices
 *    3. Checkpoint Buffer Table Log(s)  (chkpt_bf_tab)
 *        -- dirty page entries in bf and their recovery lsn
 *    4. Checkpoint Transaction Table Log(s) (chkpt_xct_tab)
 *        -- active transactions and their first lsn
 *    5. Checkpoint End Log (chkpt_end)
 *
 *  Because a checkpoint can be executed either asynch or synch, it cannot return
 *  return code to caller.
 *  It brings down the server if catastrophically error occurred (i.e., not able to
 *  make the new master lsn durable, out-of-log space, etc.)
 *  If minor error occurrs (i.e. failed the initial validation), it generates a debug output, 
 *  and cancel the checkpoint operation silently.
 *********************************************************************/
void chkpt_m::take(chkpt_mode_t chkpt_mode)
{
    FUNC(chkpt_m::take);

    // It is possible multiple checkpoint requests arrived concurrently, at most 
    // one asynch checkpoint request at any time, but we might have multiple synch
    // checkpoiint requests.
    // This is okay because we will serialize the requests using a 'write' mutex and
    // not lose any of the checkpoint request, although some of the requests might
    // need to wait for a while (blocking)

    if (! log)   {
        /*
         *  recovery facilities disabled ... do nothing
         */
        return;
    }

    /*   
     * checkpoints are fuzzy
     * but must be serialized wrt each other.
     *
     * Acquire the 'write' mutex immediatelly to serialize concurrent checkpoint requests.
     *
     * NB: EVERYTHING BETWEEN HERE AND RELEASING THE MUTEX
     * MUST BE W_COERCE (not W_DO).
     */
    chkpt_serial_m::write_acquire();
    DBGOUT1(<<"BEGIN chkpt_m::take");

    // Update statistics
    INC_TSTAT(log_chkpt_cnt);

    // Note: current code in ss_m::_destruct_once() sets the shutting_down flag first, then
    // 1. For clean shutdown, it takes a synchronous checkpoint and then
    //    retires the checkpoint thread
    // 2. For dirty shutdown, it retires the checkpoint thread immediatelly (which might still be
    //    working on a checkpoint triggered by user).
    //
    //Checkpoint can be activated 2 ways:
    // 1. Calling 'wakeup_and_take' - asynch
    // 2. Calling 'chkpt_m::synch_take' - synch

    // Start the initial validation check for make sure the incoming checkpoint request is valid
    bool valid_chkpt = true;

    if (log && _chkpt_thread) {
        // If received a 'retire' message, return without doing anything
        if (true == _chkpt_thread->is_retired()) {
            DBGOUT1(<<"END chkpt_m::take - detected retire, skip checkpoint");
            valid_chkpt = false;
        }
    }

    if ((ss_m::shutting_down) && (smlevel_0::t_chkpt_async == chkpt_mode))
    {
        // No asynch checkpoint if we are shutting down    
        DBGOUT1(<<"END chkpt_m::take - detected shutdown, skip asynch checkpoint");
        valid_chkpt = false;
    }
    else if ((ss_m::shutting_down) && (smlevel_0::t_chkpt_sync == chkpt_mode))
    {
        // Middle of shutdown, allow synch checkpoint request
        DBGOUT1(<<"END chkpt_m::take - system shutdown, allow synch checkpoint");        
    }
    else
    {
        // Not in shutdown   
        if (before_recovery())
        {
            DBGOUT1(<<"END chkpt_m::take - before system startup/recovery, skip checkpoint");
            valid_chkpt = false;
        }
        if (in_recovery() && (smlevel_0::t_chkpt_sync != chkpt_mode))
        {
            DBGOUT1(<<"END chkpt_m::take - system in recovery, skip asynch checkpoint");            
            valid_chkpt = false;
        } 
        else if (in_recovery() && (smlevel_0::t_chkpt_sync == chkpt_mode))
        {
            if (!in_recovery_analysis() && !in_recovery_redo())
            {
                DBGOUT1(<<"END chkpt_m::take - system in recovery, allow synch checkpoint");
            }
            else
            {
                DBGOUT1(<<"END chkpt_m::take - system in REDO phase, disallow checkpoint");
                valid_chkpt = false;
            }
        }
        else
        {
            // We cannot be in recovery if we get here
            if (true == in_recovery())
            {
                DBGOUT1(<<"END chkpt_m::take - system should not be in Recovery, exist checkpoint");
                valid_chkpt = false;           
            }
        }
    }

    // Optimization idea: if the last log record in the recovery log is 'end checkpoint' then
    // no new activities since last completed checkpoint, should we skip this checkpoint request?
    // No, becasue even without new log record, there still might be buffer pool flush after
    // the previous completed checkpoint (note checkpoint is a non-blocking operation),
    // we should take a checkpoint just to be safe

    // Done with the checkpoint validation, should we continue?
    if (false == valid_chkpt)
    {
        // Failed the checkpoint validation, release the 'write' mutex and exist
        chkpt_serial_m::write_release();
        return;
    }

    // We are okay to proceed with the checkpoint process
    
    /*
     * Allocate a buffer for storing log records
     */
    w_auto_delete_t<logrec_t> logrec(new logrec_t);


#define LOG_INSERT(constructor_call, rlsn)            \
    do {                                              \
        new (logrec) constructor_call;                \
        W_COERCE( log->insert(*logrec, rlsn) );       \
        if(!log->consume_chkpt_reservation(logrec->length())) { \
            chkpt_serial_m::write_release();                    \
            W_FATAL(eOUTOFLOGSPACE);                            \
        }                                                       \
    } while(0)


/*****************************************************
// Dead code, comment out just in case we need to re-visit it in the future

 // No checking on available log space (Recovery milestone 1)
 // Not waiting on old transactions to finish and no buffle pool flush before checkpoint

 retry:
    

    // FRJ: We must somehow guarantee that the log always has space to
    // accept checkpoints.  We impose two constraints to this end:

    // 1. We cap the total space checkpoints are allowed to consume in
    //    any one log partition. This is a good idea anyway because
    //    checkpoint size is linear in the number of dirty buffer pool
    //    pages -- ~2MB per GB of dirty data -- and yet the utility of
    //    checkpoints drops off quickly as the dirty page count
    //    increases -- log analysis and recovery must start at the lsn
    //    of the oldest dirty page regardless of how recent the
    //    checkpoint was.

    // 2. No checkpoint may depend on more than /max_openlog-1/ log
    //    partitions. In other words, every checkpoint completion must
    //    leave at least one log partition available.

    // We use these two constraints, together with log reservations,
    // to guarantee the ability to reclaim log space if the log
    // becomes full. The log maintains, on our behalf, a reservation
    // big enough for two maximally-sized checkpoints (ie the dirty
    // page table lists every page in the buffer pool). Every time we
    // reclaim a log segment this reservation is topped up atomically.

    
    // if current partition is max_openlog then the oldest lsn we can
    // tolerate is 2.0. We must flush all pages dirtied before that
    // time and must wait until all transactions with an earlier
    // start_lsn have ended (at worst they will abort if the log fills
    // up before they can commit).

    //  TODO: use smlevel_0::log_warn_callback to notify the VAS in
    // case old transactions are't currently active for some reason.

    // Also, remember the current checkpoint count so we can see
    // whether we get raced...

    // #warning "TODO use log_warn_callback in case old transactions aren't logging right now"
    long curr_pnum = log->curr_lsn().file();
    long too_old_pnum = std::max(0l, curr_pnum - max_openlog+1);
    if(!log->verify_chkpt_reservation()) {
    // Yikes! The log can't guarantee that we'll be able to
    // complete any checkpoint after this one, so we must reclaim
    // space even if the log doesn't seem to be full.
    
        too_old_pnum = log->global_min_lsn().file();
        if(too_old_pnum == curr_pnum) {
            // how/why did they reserve so much log space???
            W_FATAL(eOUTOFLOGSPACE);
        }
    }

    // We cannot proceed if any transaction has a too-low start_lsn;
    // wait for them to complete before continuing.
       
    // WARNING: we have to wake any old transactions which are waiting
    // on locks, or we risk deadlocks where the lock holder waits on a
    // full log while the old transaction waits on the lock.
    lsn_t oldest_valid_lsn = log_m::first_lsn(too_old_pnum+1);
    old_xct_tracker tracker;
    { 
    xct_i it(true); // do acquire the xlist_mutex...
    while(xct_t* xd=it.next()) {
        lsn_t const &flsn = xd->first_lsn();
        if(flsn.valid() && flsn < oldest_valid_lsn) {
            // poison the transaction and add it to the list...
            xd->force_nonblocking();
            tracker.track(xd);
        }
    }
    }

    // release the chkpt_serial to do expensive stuff

    // We'll record the current checkpoint count so we can detect
    // whether we get raced during the gap.
    long chkpt_stamp = _chkpt_count;
    chkpt_serial_m::write_release();

    
    // clear out all too-old pages
    W_COERCE(bf->force_until_lsn(oldest_valid_lsn.data()));

    // hopefully the page cleaning took long enough that the old
    // transactions all ended...
    tracker.wait_for_all();

    // raced?
    chkpt_serial_m::write_acquire();
    if(_chkpt_count != chkpt_stamp)
    goto retry;
*****************************************************/


    // Finally, we're ready to start the actual checkpoint!
    uint16_t total_page_count = 0;
    uint16_t total_txn_count = 0;

    // Write a Checkpoint Begin Log and record its lsn in master
    lsn_t master;

    // The original chkpt code was using the LSN when the device was mounted
    // as the begin checkpoint LSN
    //     LOG_INSERT(chkpt_begin_log(io->GetLastMountLSN()), &master);   
    // In the new implementation, use _curr_lsn as the begin checkpoint LSN
    // while _curr_lsn is the lsn of the next-to-be-inserted log record LSN
    LOG_INSERT(chkpt_begin_log(log->curr_lsn()), &master);

    /*
     *  Checkpoint the buffer pool dirty page table, and record
     *  minimum of the recovery lsn of all dirty pages.
     *
     *  We could do this (very slow) operation before grabbing the
     *  checkpoint mutex because all pages can do is get younger by
     *  being flushed; no page can become older than the min_rec_lsn
     *  we record here ... however, we have to serialize checkpoints
     *  because, although they are fuzzy, they cannot intermingle.
     *  One must complete before another starts. Recovery relies
     *  on it.  Either everyone uses wakeup_and_take or they (dismount,
     *  mount, etc) wait on this.
     *
     *  The srv_log does wakeup_and_take() whenever a new partition is
     *  opened, and it might be that a checkpoint is spanning a 
     *  partition.
     */
     
    // Initialize the min_rec_lsn, this is to store the minimum lsn for this checkpoint
    lsn_t min_rec_lsn = lsndata_max;
    {
        bf_idx     bfsz = bf->get_block_cnt();

        // One log record per block, max is set to make chkpt_bf_tab_t fit in logrec_t::data_sz
        const     uint32_t chunk = chkpt_bf_tab_t::max;

        w_auto_delete_array_t<lpid_t> pid(new lpid_t[chunk]);
        w_auto_delete_array_t<lsn_t> rec_lsn(new lsn_t[chunk]);
        w_auto_delete_array_t<lsn_t> page_lsn(new lsn_t[chunk]);
        w_assert1(pid && rec_lsn && page_lsn);

        for (bf_idx i = 1; i < bfsz; )  
        {
            // Loop over all buffer pages, one block at a time
            uint32_t count = chunk;

            // Have the minimum rec_lsn of the bunch
            // returned iff it's less than the value passed in
            bf->get_rec_lsn(i, count, pid.get(), rec_lsn.get(), page_lsn.get(), min_rec_lsn,
                            master, log->curr_lsn());
            if (count)  
            {
                total_page_count += count;

                // write all the information into a 'chkpt_bf_tab_log' (chkpt_bf_tab_t) log record 
                LOG_INSERT(chkpt_bf_tab_log(count, pid, rec_lsn, page_lsn), 0);
            }
        }
        //fprintf(stderr, "Checkpoint found %d dirty pages\n", total_page_count);
    }


/*****************************************************
//Dead code, comment out just in case we need to re-visit it in the future

// Eliminate logging mount/vol related stuff in checkpoint (Recovery milestone 1)
// Is it safe to eliminate this set of logging?  We won't have volume and device information
// in the checkpoint, which is used in lpid_t (volume number + store number + page number)

    // Checkpoint the dev mount table
    {

        // Log the mount table in "max loggable size" chunks.
        // XXX casts due to enums
        const int chunk = (int)max_vols > (int)chkpt_dev_tab_t::max 
            ? (int)chkpt_dev_tab_t::max : (int)max_vols;
        int dev_cnt = io->num_vols();

        int    i;
        char        **devs;
        devs = new char *[chunk];
        if (!devs)
            W_FATAL(fcOUTOFMEMORY);
        for (i = 0; i < chunk; i++) {
            devs[i] = new char[max_devname+1];
            if (!devs[i])
                W_FATAL(fcOUTOFMEMORY);
        }
        vid_t        *vids;
        vids = new vid_t[chunk];
        if (!vids)
            W_FATAL(fcOUTOFMEMORY);

        for (i = 0; i < dev_cnt; i += chunk)  {
            
            int ret;
            W_COERCE( io->get_vols(i, MIN(dev_cnt - i, chunk),
                          devs, vids, ret));
            if (ret)  {

                // Write a Checkpoint Device Table Log
                // XXX The bogus 'const char **' cast is for visual c++
                LOG_INSERT(chkpt_dev_tab_log(ret, (const char **) devs, vids), 0);
            }
        }
        delete [] vids;
        for (i = 0; i < chunk; i++)
            delete [] devs[i];
        delete [] devs;
    }
*****************************************************/

    // Checkpoint is not a blocking operation, do not locking the transaction table
    // Note that transaction table list could change underneath the checkpoint
    //
    // W_COERCE(xct_t::acquire_xlist_mutex());

    // Because it is a descending list, the newest transaction goes to the
    // beginning of the list, new transactions arrived after we started 
    // scanning will not be recorded in the checkpoint
    
    /*
     *  Checkpoint the transaction table, and record
     *  minimum of first_lsn of all transactions.
     */
    lsn_t min_xct_lsn = lsn_t::max;
    {
        // For each transaction, record transaction ID,
        // lastLSN (the LSN of the most recent log record for the transaction)
        // 'abort' flags (transaction state), and undo nxt (for rollback)
        
        const int    chunk = chkpt_xct_tab_t::max;

        // tid is increasing, youngest tid has the largest value
        tid_t        youngest = xct_t::youngest_tid();  
        
        w_auto_delete_array_t<tid_t> tid(new tid_t[chunk]);
        w_auto_delete_array_t<xct_state_t> state(new xct_state_t[chunk]);
        w_auto_delete_array_t<lsn_t> last_lsn(new lsn_t[chunk]);
        w_auto_delete_array_t<lsn_t> undo_nxt(new lsn_t[chunk]);

        xct_i x(false); // false -> do not acquire the mutex when accessing the transaction table

        // Not 'const' becasue we are acquring a traditional latch on the object
        xct_t* xd = 0;  
        do 
        {
            int i = 0;
            while (i < chunk && (xd = x.next()))  
            {
                // Loop over all transactions and record only
                // xcts that dirtied something.
                // Skip those that have ended but not yet
                // been destroyed.
                // Both active and aborted transaction will be
                // recorded in checkpoiont

                // Acquire a traditional read latch to prevent reading transit data
                // A write latch is issued from the xct_t::change_state function which
                // is used in txn commit and abort operation
                // We don't want to read transaction data when state is changing
                // It is possible the transaction got committed or aborted after we 
                // gather log data for the transaction, so the commit or abort information
                // will be in the logs after the checkpoint, it is okay

                w_rc_t latch_rc = xd->latch().latch_acquire(LATCH_SH, WAIT_FOREVER);

                if (latch_rc.is_error())
                {
                    // Unable to the read acquire latch, cannot continue, raise an internal error
                    DBGOUT2 (<< "Error when acquiring LATCH_SH for checkpoint transaction object. xd->tid = "
                             << xd->tid() << ", rc = " << latch_rc);

                    // To be a good citizen, release the 'write' mutex before raise error
                    chkpt_serial_m::write_release();

                    W_FATAL_MSG(fcINTERNAL, << "unable to latch a transaction object");
                    return;
                }

                if( xd->state() == xct_t::xct_ended) 
                {
                   xd->latch().latch_release();                
                   continue;
                }

                // Transaction table is implemented in a descend list sorted by tid
                // therefore the newest transaction goes to the beginning of the list
                // When scanning, the newest transaction comes first
                
                if (xd->first_lsn().valid())  
                {
                    tid[i] = xd->tid();

                    // A transaction state can be xct_t::xct_aborting if
                    // 1. A normal aborting transaction
                    // 2. Doom transaction - all transactions are marked as 
                    //     aborting (doomed) by Log Analysys phase, UNDO phase 
                    //     identifies the true doomed transactions.
                    // A checkpoint will be taken at the end of Log Analysis phase
                    // therefore record all transaction state as is.
                    //
                    state[i] = xd->state();

                    assert(lsn_t::null!= xd->last_lsn());
                    last_lsn[i] = xd->last_lsn();  // most recent LSN

                    // 'set_undo_nxt' is initiallized to NULL,
                    // 1. Set in Log_Analysis phase in Recovery for UNDO phase,
                    // 2. Set in xct_t::_flush_logbuf, set to last_lan if undoable,
                    //     set to last_lsn if not a compensation transaction,
                    //     if a compensation record, set to transaction's 
                    //     last log record's undo_next
                    // 'undo_nxt' is used in UNDO phase in Recovery, 
                    // transaction abort/rollback, also somehow in log truncation
                    //                 
                    undo_nxt[i] = xd->undo_nxt();

                    assert(lsn_t::null!= xd->first_lsn());                    
                    if (min_xct_lsn > xd->first_lsn())
                        min_xct_lsn = xd->first_lsn();

                    ++i;

                    ++total_txn_count;
                }

                // Release the traditional read latch before go get the next txn
                xd->latch().latch_release();
            }

            // It is possible we don't have any transaction when doing the checkpoint,
            // in such case, we will write out 1 log record, this is because we want to 
            // record the youndgest xct (with the largest tid)
            {
                // Filled up one log record, write a Transaction Table Log out
                // before processing more transactions

                LOG_INSERT(chkpt_xct_tab_log(youngest, i, tid, state,
                                   last_lsn, undo_nxt), 0);
            }
        } while (xd);
    }

    // Non-blocking checkpoint, we never acquired a mutex on the list
    //
    // xct_t::release_xlist_mutex();

    
    /*
     *  Make sure that min_rec_lsn and min_xct_lsn are valid
     *  master: lsn from the 'begin checkpoint' log record
     *  min_xct_lsn: minimum of first_lsn of all recorded transactions, both active and aborting
     *  min_rec_lsn: minimum lsn of all buffer pool dirty or in_doubt pages
     *
     *  If min_*_lsn > master, it could be one ot 2 things:
     *    1. Nothing happened, therefore the min_*_lsn are still set to lsn_t::max
     *    2. All activities occurred after 'begin checkpoint'
     *  Recovery would have to start from the master LSN in any case.
     */
    if (min_rec_lsn > master)
        min_rec_lsn = master;
    if (min_xct_lsn > master)
        min_xct_lsn = master;

    // Finish up the checkpoint
    // 1. If io->GetLastMountLSN() > master, this is not supposed to happen
    // 2. If in the process of shutting down, we can get here only 
    //     if it is a checkpoint issued by shutdown process
    // If conditions are not met, return without writing the Checkpoint End log, so the recovery
    // will ignore this checkpoint.

    if ((io->GetLastMountLSN() <= master) &&
        ((false == ss_m::shutting_down) ||           // Not in shutdown
        ((ss_m::shutting_down) &&
        (smlevel_0::t_chkpt_sync == chkpt_mode))))    // In shutdown and synch checkpoint
    {
        // Write the Checkpoint End Log with:
        //  1. master: LSN from begin checkpoint
        //  2. min_rec_lsn: minimum lsn of all buffer pool dirty or in_doubt pages

        LOG_INSERT(chkpt_end_log (master, min_rec_lsn), 0);
        DBGOUT1(<<"chkpt_m::take - total dirty page count = " << total_page_count << ", total txn count = " << total_txn_count);

        // Sync the log
        // In checkpoint operation, flush the recovery log to harden the log records
        // We either flush the log or flush the buffer pool, but not both        
        W_COERCE( log->flush_all() );

        // Make the new master lsn durable
        // This is the step to record the last completed checkpoint in the known location
        // in each log file (partition), so the Recovery process can find the 
        // latest completed checkpoint.
        // It reocrds the begin checkpoint LSN (master) and the 
        // minimum LSN (std::min(min_rec_lsn, min_xct_lsn))
        // Error in this step will bring down the server
        log->set_master(master, min_rec_lsn, min_xct_lsn);

        // Scavenge some log
        W_COERCE( log->scavenge(min_rec_lsn, min_xct_lsn) );

    }
    else
    {
        DBGOUT1(<<"END chkpt_m::take - skip checkpoint end log because system is shutting down");
    }

    // Release the 'write' mutex so the next checkpoint request can come in
    chkpt_serial_m::write_release();
    DBGOUT1(<<"END chkpt_m::take");

    return;
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
                assert(0 < chkpt_count);            
                DBG(<<"Found pending checkpoint request count: " << chkpt_count);
                chkpt_count = 0;
            }

            // Reset the flag before we continue to execute the checkpoint
            _kicked = false;
        }

        assert(smlevel_1::chkpt);

        // If a retire request arrived, exit immediatelly without checkpoint
        if(_retire)
            break;

        // No need to acquire checkpoint mutex before calling the checkpoint operation
        smlevel_1::chkpt->take(smlevel_0::t_chkpt_async);
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
