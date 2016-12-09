/*
 * (c) Copyright 2011-2013, Hewlett-Packard Development Company, LP
 */

#ifndef STNODE_PAGE_H
#define STNODE_PAGE_H

#include <vector>

#include "generic_page.h"
#include "latches.h"
#include "w_defines.h"
#include "sm_base.h"
#include "alloc_page.h"

/**
 * \brief Persistent structure representing metadata for a store.
 *
 * \details
  *
 * These are contained in \ref stnode_page's.
 */
struct stnode_t {
    /// also okay to initialize via memset
    stnode_t() : root(0)
    { }

    /**
     * Root page ID of the store; 0 if the store is not allocated and 1 for
     * the special store number 0, which is used to keep track of the last
     * extent allocated which was not assigned to a specific store.
     */
    PageID         root;      // +4 -> 4

    bool is_used() const  { return root != 0; }
};


/**
 * \brief Store-node page that contains one stnode_t for each
 * (possibly deleted or uncreated) store belonging to a given volume.
 */
class stnode_page : public generic_page_header {
public:

    /// max # \ref stnode_t's on a single stnode_page; thus, the
    /// maximum number of stores per volume
    static constexpr size_t max =
        (page_sz - sizeof(generic_page_header) - sizeof(extent_id_t))
        / sizeof(stnode_t);

    // Page ID used by the stnode page
    static const PageID stpid;

    stnode_t get(size_t index) const {
        w_assert1(index < max);
        return stnode[index];
    }

    void set_root(size_t index, PageID root) {
        w_assert1(index < max);
        stnode[index].root = root;
    }

    void set_last_extent(extent_id_t ext) { last_extent = ext; }

    extent_id_t get_last_extent() { return last_extent; }

    void format_empty() {
        memset(this, 0, sizeof(generic_page_header));
        pid = stnode_page::stpid;
        tag = t_stnode_p;

        last_extent = 0;
        memset(&stnode, 0, sizeof(stnode_t) * max);
    }


private:
    // ID of the lastly allocated extent
    extent_id_t last_extent;

    /// stnode[i] is the stnode_t for store # i of this volume
    stnode_t stnode[max];
};
BOOST_STATIC_ASSERT(sizeof(stnode_page) == generic_page_header::page_sz);

/**
 * \brief Store creation/destroy/query interface.
 *
 * \details
 * This object handles store create/destroy/query requests for one
 * volume.  99.99% of the requests are, of course, querying the root
 * page ID of indexes.  This object does a lightweight synchronization
 * (latch) to protect them from MT accesses.  However, this object
 * doesn't use locks because we don't need them.  If the store is
 * being destroyed, ss_m will check intent locks before calling this
 * object, so we are safe.
 *
 * This object and vol_t replace the "directory" thingies in original
 * Shore-MT with more efficiency and simplicity.
 */
class stnode_cache_t {
public:
    stnode_cache_t(bool virgin);

    /**
     * Returns the root page ID of the given store.
     * If that store isn't allocated, returns 0.
     * @param[in] store Store ID.
     */
    PageID get_root_pid(StoreID store) const;

    lsn_t get_root_elmsn(StoreID store) const;

    bool is_allocated(StoreID store) const;

    /// Make a copy of the entire stnode_t of the given store.
    stnode_t get_stnode(StoreID store) const;

    /// Returns the StoreID of all allocated stores in the volume.
    void get_used_stores(std::vector<StoreID>&) const;

    rc_t sx_create_store(PageID root_pid, StoreID& snum, bool redo = false) const;

    rc_t sx_append_extent(extent_id_t ext, bool redo = false) const;

    void dump(std::ostream& out) const;

    extent_id_t get_last_extent() const;

private:
    /// Returns the first StoreID that can be used for a new store in
    /// this volume or stnode_page::max if all available stores of
    /// this volume are already allocated.
    StoreID get_min_unused_stid(stnode_page* spage) const;

};

#endif // STNODE_PAGE_H
