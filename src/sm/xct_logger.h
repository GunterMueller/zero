#ifndef XCT_LOGGER_H
#define XCT_LOGGER_H

#include "sm.h"
#include "xct.h"
#include "btree_page_h.h"
#include "logdef_gen.h"
#include "log_core.h"

class XctLogger
{
public:

    /*
     * This method replaces the old log "stubs" that were generated by a Perl
     * script logdef.pl. Two overloads are required because of the cumbersome
     * way in which PageLSNs are managed in Zero (see xct_t::give_logbuf).
     */
    // CS TODO we need a new page-lsn update mechanism!
    template <class Logrec, class... Args>
    static lsn_t log(const Args&... args)
    {
        xct_t* xd = smthread_t::xct();
        bool should_log = smlevel_0::log && smlevel_0::logging_enabled && xd;
        if (!should_log)  { return lsn_t::null; }

        logrec_t* logrec = _get_logbuf(xd);
        new (logrec) Logrec;
        logrec->init_header(Logrec::TYPE);
        logrec->init_xct_info();
        reinterpret_cast<Logrec*>(logrec)->construct(args...);
        w_assert1(logrec->valid_header());

        // REDO log records always pertain to a page and must therefore use log_p
        w_assert1(!logrec->is_redo());

        // If it's a log for piggy-backed SSX, we call log->insert without updating _last_log
        // because this is a single log independent from other logs in outer transaction.
        if (xd->is_piggy_backed_single_log_sys_xct()) {
            w_assert1(logrec->is_single_sys_xct());
            lsn_t lsn;
            W_COERCE( ss_m::log->insert(*logrec, &lsn) );
            w_assert1(lsn != lsn_t::null);
            DBGOUT3(<< " SSX logged: " << logrec->type() << "\n new_lsn= " << lsn);
            return lsn;
        }

        lsn_t lsn;
        logrec->set_xid_prev(xd->tid(), xd->last_lsn());
        W_COERCE(ss_m::log->insert(*logrec, &lsn));
        W_COERCE(xd->update_last_logrec(logrec, lsn));

        return lsn;
    }

    template <class Logrec, class PagePtr, class... Args>
    static lsn_t log_p(PagePtr p, const Args&... args)
    {
        xct_t* xd = smthread_t::xct();
        bool should_log = smlevel_0::log && smlevel_0::logging_enabled && xd;
        if (!should_log)  { return lsn_t::null; }

        logrec_t* logrec = _get_logbuf(xd);
        new (logrec) Logrec;
        logrec->init_header(Logrec::TYPE);
        logrec->init_xct_info();
        logrec->init_page_info(p);
        reinterpret_cast<Logrec*>(logrec)->construct(p, args...);
        w_assert1(logrec->valid_header());

        if (p->tag() != t_btree_p || p->root() == p->pid()) {
            logrec->set_root_page();
        }

        // set page LSN chain
        logrec->set_page_prev_lsn(p->get_page_lsn());

        // If it's a log for piggy-backed SSX, we call log->insert without updating _last_log
        // because this is a single log independent from other logs in outer transaction.
        if (xd->is_piggy_backed_single_log_sys_xct()) {
            w_assert1(logrec->is_single_sys_xct());
            lsn_t lsn;
            W_COERCE( ss_m::log->insert(*logrec, &lsn) );
            w_assert1(lsn != lsn_t::null);
            _update_page_lsns(p, lsn);
            DBGOUT3(<< " SSX logged: " << logrec->type() << "\n new_lsn= " << lsn);
            return lsn;
        }


        lsn_t lsn;
        logrec->set_xid_prev(xd->tid(), xd->last_lsn());
        W_COERCE(ss_m::log->insert(*logrec, &lsn));
        W_COERCE(xd->update_last_logrec(logrec, lsn));
        _update_page_lsns(p, lsn);

        return lsn;
    }

    template <class Logrec, class PagePtr, class... Args>
    static lsn_t log_p(PagePtr p, PagePtr p2, const Args&... args)
    {
        xct_t* xd = smthread_t::xct();
        bool should_log = smlevel_0::log && smlevel_0::logging_enabled && xd;
        if (!should_log)  { return lsn_t::null; }

        logrec_t* logrec = _get_logbuf(xd);
        new (logrec) Logrec;
        logrec->init_header(Logrec::TYPE);
        logrec->init_xct_info();
        logrec->init_page_info(p);
        reinterpret_cast<Logrec*>(logrec)->construct(p, p2, args...);
        w_assert1(logrec->valid_header());

        if (p->tag() != t_btree_p || p->root() == p->pid()) {
            logrec->set_root_page();
        }
        if (p2->tag() != t_btree_p || p2->root() == p2->pid()) {
            logrec->set_root_page();
        }

        // set page LSN chain
        logrec->set_page_prev_lsn(p->get_page_lsn());
        // For multi-page log, also set LSN chain with a branch.
        w_assert1(logrec->is_multi_page());
        w_assert1(logrec->is_single_sys_xct());
        multi_page_log_t *multi = logrec->data_ssx_multi();
        w_assert1(multi->_page2_pid != 0);
        multi->_page2_prv = p2->get_page_lsn();

        // If it's a log for piggy-backed SSX, we call log->insert without updating _last_log
        // because this is a single log independent from other logs in outer transaction.
        if (xd->is_piggy_backed_single_log_sys_xct()) {
            w_assert1(logrec->is_single_sys_xct());
            lsn_t lsn;
            W_COERCE( ss_m::log->insert(*logrec, &lsn) );
            w_assert1(lsn != lsn_t::null);
            _update_page_lsns(p, lsn);
            _update_page_lsns(p2, lsn);
            DBGOUT3(<< " SSX logged: " << logrec->type() << "\n new_lsn= " << lsn);
            return lsn;
        }


        lsn_t lsn;
        logrec->set_xid_prev(xd->tid(), xd->last_lsn());
        W_COERCE(ss_m::log->insert(*logrec, &lsn));
        W_COERCE(xd->update_last_logrec(logrec, lsn));
        _update_page_lsns(p, lsn);
        _update_page_lsns(p2, lsn);

        return lsn;
    }

    /*
     * log_sys is used for system log records (e.g., checkpoints, clock
     * ticks, reads & writes, recovery events, debug stuff, stats, etc.)
     *
     * The difference to the other logging methods is that no xct or page
     * is involved and the logrec buffer is obtained with the 'new' operator.
     */
    template <class Logrec, class... Args>
    static lsn_t log_sys(const Args&... args)
    {
        // this should use TLS allocator, so it's fast
        // (see macro DEFINE_SM_ALLOC in allocator.h and logrec.cpp)
        logrec_t* logrec = new logrec_t;

        new (logrec) Logrec;
        logrec->init_header(Logrec::TYPE);
        logrec->init_xct_info();
        reinterpret_cast<Logrec*>(logrec)->construct(args...);
        w_assert1(logrec->valid_header());
        w_assert1(logrec_t::get_logrec_cat(Logrec::TYPE) == logrec_t::t_system);

        lsn_t lsn;
        W_COERCE(ss_m::log->insert(*logrec, &lsn));

        delete logrec;
        return lsn;
    }

    /*
     * log_page_chain is used for in-memory data structures that do not
     * maintain data directly in a page but must use chained log records
     * to keep track of their versions and dirty states. Used by
     * stnode_cache and alloc_cache
     */
    template <class Logrec, class... Args>
    static void log_page_chain(lsn_t& prev_page_lsn, const Args&... args)
    {
        // this should use TLS allocator, so it's fast
        // (see macro DEFINE_SM_ALLOC in allocator.h and logrec.cpp)
        logrec_t* logrec = new logrec_t;

        new (logrec) Logrec;
        logrec->init_header(Logrec::TYPE);
        logrec->init_xct_info();
        reinterpret_cast<Logrec*>(logrec)->construct(args...);
        w_assert1(logrec->valid_header());

        // CS TODO: all uses of log_page_chain are on a root page
        logrec->set_root_page();

        lsn_t lsn;
        logrec->set_page_prev_lsn(prev_page_lsn);
        W_COERCE(ss_m::log->insert(*logrec, &lsn));

        delete logrec;
        prev_page_lsn = lsn;
    }

    template <class PagePtr>
    static void _update_page_lsns(PagePtr page, lsn_t new_lsn)
    {
        page->update_page_lsn(new_lsn);
    }

    static logrec_t* _get_logbuf(xct_t* xd)
    {
        if (xd->is_piggy_backed_single_log_sys_xct()) {
            return smthread_t::get_logbuf2();
        }
        return smthread_t::get_logbuf();
    }
};

// CS TODO this is a temporary alias -- at some point the SM should have its
// own generic Logger template argument
using Logger = XctLogger;

#endif
