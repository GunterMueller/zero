
set(sm_STAT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/alloc_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/alloc_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bf_hashtable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bf_tree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bf_tree_cleaner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btcursor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_defrag.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_grow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_search.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_split.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_impl_verify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_logrec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/btree_page_h.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chkpt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/eventlog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fixable_page_h.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/generic_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/latch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/latches.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_dump.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_lil.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_raw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_x.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_carray.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_consumer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_storage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_lsn_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logarchiver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logarchive_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logarchive_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logarchive_scanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mem_mgmt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logrec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_evictioner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_cleaner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_cleaner_decoupled.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/partition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/restart.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/restore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/smindex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/smstats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/smthread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/stnode_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/xct.cpp
)

add_library(sm STATIC ${sm_STAT_SRCS})
target_link_libraries(sm
    common
    boost_program_options
    boost_system
    boost_filesystem
    )

# generate a library that performs swizzling but no pin_cnt increments/decrements
add_library(sm_swizzling_nopincntincdec STATIC ${sm_STAT_SRCS})
target_link_libraries(sm_swizzling_nopincntincdec fc sthread common numa)
# make sure we don't override any previous value of COMPILE_DEFINITIONS
get_target_property (SM_SWIZZLING_NOPINCNTINCDEC_COMPILEDEF sm_swizzling_nopincntincdec COMPILE_DEFINITIONS)
set(SM_SWIZZLING_NOPINCNTINCDEC_COMPILEDEF "NO_PINCNT_INCDEC")
set_target_properties (sm_swizzling_nopincntincdec
    PROPERTIES COMPILE_DEFINITIONS "${SM_SWIZZLING_NOPINCNTINCDEC_COMPILEDEF}"
    EXCLUDE_FROM_ALL ON
)

# generate a library that simulates no swizzling
add_library(sm_noswizzling STATIC ${sm_STAT_SRCS})
target_link_libraries(sm_noswizzling fc sthread common numa)
# make sure we don't override any previous value of COMPILE_DEFINITIONS
get_target_property (SM_NOSWIZZLING_COMPILEDEF sm_noswizzling COMPILE_DEFINITIONS)
set(SM_NOSWIZZLING_COMPILEDEF "SIMULATE_NO_SWIZZLING")
set_target_properties (sm_noswizzling
    PROPERTIES COMPILE_DEFINITIONS "${SM_NOSWIZZLING_COMPILEDEF}"
    EXCLUDE_FROM_ALL ON
)

# generate a library that simulates an in-memory DB
add_library(sm_mainmemory STATIC ${sm_STAT_SRCS})
target_link_libraries(sm_mainmemory fc sthread common numa)
# make sure we don't override any previous value of COMPILE_DEFINITIONS
get_target_property (SM_MAINMEMORY_COMPILEDEF sm_mainmemory COMPILE_DEFINITIONS)
set(SM_MAINMEMORY_COMPILEDEF "SIMULATE_MAINMEMORYDB")
set_target_properties (sm_mainmemory
    PROPERTIES COMPILE_DEFINITIONS "${SM_MAINMEMORY_COMPILEDEF}"
    EXCLUDE_FROM_ALL ON
)

