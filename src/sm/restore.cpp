#include "w_defines.h"

#define SM_SOURCE

#include "restore.h"
#include "logarchiver.h"
#include "vol.h"
#include "sm_options.h"
#include "backup_reader.h"

RestoreBitmap::RestoreBitmap(size_t size)
    : bits(size, false) // initialize all bits to false
{
}

RestoreBitmap::~RestoreBitmap()
{
}

bool RestoreBitmap::get(unsigned i)
{
    spinlock_read_critical_section cs(&mutex);
    return bits.at(i);
}

void RestoreBitmap::set(unsigned i)
{
    spinlock_write_critical_section cs(&mutex);
    bits.at(i) = true;
}

void RestoreBitmap::serialize(char* buf, size_t from, size_t to)
{
    spinlock_read_critical_section cs(&mutex);
    w_assert0(from < to);
    w_assert0(to <= bits.size());

    /*
     * vector<bool> does not provide access to the internal bitmap structure,
     * so we stick with this non-efficient loop mechanism. If this becomes a
     * performance concern, the options are:
     * 1) Implement our own bitmap
     * 2) Reuse a bitmap with serialization support (e.g., Boost)
     * 3) In C++11, there is a data() method that returns the underlying array.
     *    Maybe that would work here.
     */
    size_t byte = 0, j = 0;
    for (size_t i = from; i < to; i++) {
        // set bit j on current byte
        // C++ guarantees that "true" expands to the integer 1
        buf[byte] |= (bits[i] << j++);

        // wrap around next byte
        if (j == 8) {
            j = 0;
            byte++;
        }
    }
}

void RestoreBitmap::deserialize(char* buf, size_t from, size_t to)
{
    spinlock_write_critical_section cs(&mutex);
    w_assert0(from < to);
    w_assert0(to <= bits.size());

    size_t byte = 0, j = 0;
    for (size_t i = from; i < to; i++) {
        // set if bit on position j is a one
        bits[i] = buf[byte] & (1 << j++);

        // wrap around next byte
        if (j == 8) {
            j = 0;
            byte++;
        }
    }
}

void RestoreBitmap::getBoundaries(size_t& lowestFalse, size_t& highestTrue)
{
    spinlock_read_critical_section cs(&mutex);

    lowestFalse = 0;
    highestTrue = 0;
    bool allTrueSoFar = true;
    for (size_t i = 0; i < bits.size(); i++) {
        if (bits[i]) highestTrue = i;
        if (allTrueSoFar && bits[i]) {
            lowestFalse = i + 1;
        }
        else {
            allTrueSoFar = false;
        }
    }
    w_assert0(lowestFalse <= bits.size());
    w_assert0(highestTrue < bits.size());
}

RestoreScheduler::RestoreScheduler(const sm_options& options,
        RestoreMgr* restore)
    : restore(restore)
{
    w_assert0(restore);
    numPages = restore->getNumPages();
    firstNotRestored = restore->getFirstDataPid();
    trySinglePass =
        options.get_bool_option("sm_restore_sched_singlepass", true);
}

RestoreScheduler::~RestoreScheduler()
{
}

void RestoreScheduler::enqueue(const shpid_t& pid)
{
    spinlock_write_critical_section cs(&mutex);
    queue.push(pid);
}

shpid_t RestoreScheduler::next()
{
    spinlock_write_critical_section cs(&mutex);

    shpid_t next = firstNotRestored;
    if (queue.size() > 0) {
        next = queue.front();
        queue.pop();
    }
    else if (trySinglePass) {
        // if queue is empty, find the first not-yet-restored PID
        while (next <= numPages && restore->isRestored(next)) {
            // if next pid is already restored, then the whole segment is
            next = next + restore->getSegmentSize();
        }
        firstNotRestored = next;
    }
    else {
        next = shpid_t(0); // no next pid for now
    }

    /*
     * CS: there is no guarantee (from the scheduler) that next is indeed not
     * restored yet, because we do not control the bitmap from here.  The only
     * guarantee is that only one thread will be executing the restore loop (or
     * that multiple threads will restore disjunct segment sets). In the
     * current implementation, it turns out that the scheduler is only invoked
     * from the restore loop, which means that a page returned by the scheduler
     * is guaranteed to not be restored when is is picked up by the restore
     * loop, but that may change in the future.
     */
    return next;
}

void RestoreScheduler::setSinglePass(bool singlePass)
{
    spinlock_write_critical_section cs(&mutex);
    trySinglePass = singlePass;
}

RestoreMgr::RestoreMgr(const sm_options& options,
        LogArchiver::ArchiveDirectory* archive, vol_t* volume, bool useBackup,
        bool takeBackup)
    : smthread_t(t_regular, "Restore Manager"),
    archive(archive), volume(volume), numRestoredPages(0),
    metadataRestored(false), useBackup(useBackup), takeBackup(takeBackup)
{
    w_assert0(archive);
    w_assert0(volume);

    segmentSize = options.get_int_option("sm_restore_segsize", 1024);
    if (segmentSize <= 0) {
        W_FATAL_MSG(fcINTERNAL,
                << "Restore segment size must be a positive number");
    }

    reuseRestoredBuffer =
        options.get_bool_option("sm_restore_reuse_buffer", true);

    DO_PTHREAD(pthread_mutex_init(&restoreCondMutex, NULL));
    DO_PTHREAD(pthread_cond_init(&restoreCond, NULL));

    /**
     * We assume that the given vol_t contains the valid metadata of the
     * volume. If the device is lost in/with a system failure -- meaning that
     * it cannot be properly mounted --, it should contain the metadata of the
     * backup volume. By "metadata", we mean at least the number of pages in
     * the volume, which is required to control restore progress. Note that
     * the number of "used" pages is of no value for restore, because pages
     * may get allocated and deallocated (possibly multiple times) during log
     * replay.
     */
    numPages = volume->num_pages();
    firstDataPid = volume->first_data_pageid();

    scheduler = new RestoreScheduler(options, this);
    bitmap = new RestoreBitmap(numPages);

    // Construct backup reader/buffer based on system options
    if (useBackup) {
        string backupImpl = options.get_string_option("sm_backup_kind",
                BackupOnDemandReader::IMPL_NAME);
        if (backupImpl == BackupOnDemandReader::IMPL_NAME) {
            backup = new BackupOnDemandReader(volume, segmentSize);
        }
        else if (backupImpl == BackupPrefetcher::IMPL_NAME) {
            int numSegments = options.get_int_option(
                    "sm_backup_prefetcher_segments", 3);
            w_assert0(numSegments > 0);
            backup = new BackupPrefetcher(volume, numSegments, segmentSize);
        }
        else {
            W_FATAL_MSG(eBADOPTION,
                    << "Invalid value for sm_backup_kind: " << backupImpl);
        }
    }
    else {
        /*
         * Even if we're not using a backup (i.e., backup-less restore), a
         * BackupReader object is still used for the restore workspace, which
         * is basically the buffer on which pages are restored.
         */
        backup = new DummyBackupReader(segmentSize);
    }
}

RestoreMgr::~RestoreMgr()
{
    delete bitmap;
    delete scheduler;
    delete backup;
}

inline unsigned RestoreMgr::getSegmentForPid(const shpid_t& pid)
{
    return (unsigned) std::max(pid, firstDataPid) / segmentSize;
}

inline shpid_t RestoreMgr::getPidForSegment(unsigned segment)
{
    return shpid_t(segment * segmentSize) + firstDataPid;
}

inline bool RestoreMgr::isRestored(const shpid_t& pid)
{
    if (pid < firstDataPid) {
        // first pages are metadata
        return metadataRestored;
    }

    unsigned seg = getSegmentForPid(pid);
    return bitmap->get(seg);
}

bool RestoreMgr::waitUntilRestored(const shpid_t& pid, size_t timeout_in_ms)
{
    DO_PTHREAD(pthread_mutex_lock(&restoreCondMutex));
    struct timespec timeout;
    while (!isRestored(pid)) {
        if (timeout_in_ms > 0) {
            sthread_t::timeout_to_timespec(timeout_in_ms, timeout);
            int code = pthread_cond_timedwait(&restoreCond, &restoreCondMutex,
                    &timeout);
            if (code == ETIMEDOUT) {
                return false;
            }
            DO_PTHREAD(code);
        }
        else {
            DO_PTHREAD(pthread_cond_wait(&restoreCond, &restoreCondMutex));
        }
    }
    DO_PTHREAD(pthread_mutex_unlock(&restoreCondMutex));

    return true;
}

void RestoreMgr::setSinglePass(bool singlePass)
{
    scheduler->setSinglePass(singlePass);
}

bool RestoreMgr::requestRestore(const shpid_t& pid, generic_page* addr)
{
    if (pid < firstDataPid || pid >= numPages) {
        return false;
    }

    scheduler->enqueue(pid);

    if (addr && reuseRestoredBuffer) {
        spinlock_write_critical_section cs(&requestMutex);

        /*
         * CS: Once mutex is held, check if page hasn't already been restored.
         * This avoids a race condition in which the segment gets restored
         * after the caller invoked isRestored() but before it places its
         * request. Since the restore loop also acquires this mutex before
         * copying restored page contents into the buffered addresses,
         * we guarantee that the segment cannot go from not-restored to
         * restored inside this critical section.
         */
        if (!isRestored(pid)) {
            // only one request for each page ID at a time
            // (buffer pool logic is responsible for ensuring this)
            w_assert1(bufferedRequests.find(pid) == bufferedRequests.end());

            bufferedRequests[pid] = addr;
            return true;
        }
    }
    return false;
}

void RestoreMgr::restoreMetadata()
{
    if (metadataRestored) {
        return;
    }

    LogArchiver::ArchiveScanner logScan(archive);

    // open scan on pages [0,1) to get all store operation log records
    LogArchiver::ArchiveScanner::RunMerger* merger =
        logScan.open(lpid_t(volume->vid(), 0), lpid_t(volume->vid(), 1),
                lsn_t::null);

    // TODO: is redo of store operations idempotent?
    // should we use a specific LSN as starting point?

    logrec_t* lr;
    while (merger->next(lr)) {
        lr->redo(NULL);
    }

    // TODO: my guess is that we don't have to write back the restored
    // metadata pages because there are only two possibilites:
    // 1) system eventually crashes -- metadata restored in restart from
    // checkpoint and log analysis
    //    (In theory, all metadata could be considered "server state" and be
    //    maintained exclusively in the log with checkpoints)
    // 2) device is eventually unmounted (e.g., during clean shutdown) --
    // pages get written back

    metadataRestored = true;

    // send signal to waiting threads (acquire mutex to avoid lost signal)
    DO_PTHREAD(pthread_mutex_lock(&restoreCondMutex));
    DO_PTHREAD(pthread_cond_broadcast(&restoreCond));
    DO_PTHREAD(pthread_mutex_unlock(&restoreCondMutex));
}

void RestoreMgr::restoreLoop()
{
    // wait until volume is actually marked as failed
    while(!takeBackup && !volume->is_failed()) {
        usleep(1000); // 1 ms
    }

    restoreMetadata();

    LogArchiver::ArchiveScanner logScan(archive);

    while (numRestoredPages < numPages) {
        shpid_t requested = scheduler->next();
        if (isRestored(requested)) {
            continue;
        }
        if (requested == shpid_t(0)) {
            // no page available for now
            usleep(2000); // 2 ms
            continue;
        }
        w_assert0(requested >= firstDataPid);

        unsigned segment = getSegmentForPid(requested);
        shpid_t firstPage = getPidForSegment(segment);

        lpid_t start = lpid_t(volume->vid(), firstPage);
        lpid_t end = lpid_t(volume->vid(), firstPage + segmentSize);

        lsn_t lsn = lsn_t::null;
        char* workspace = backup->fix(segment);

        if (useBackup) {
            // Log archiver is queried with lowest the LSN in the segment, to
            // guarantee that all required log records will be retrieved in one
            // query. If one page is particularly old (an outlier), then the
            // query will fetch too many log records which won't be replayed.
            // We need to think of optimizations for this scenario, e.g.,
            // performing multiple queries for subsets of pages of the same
            // segment.
            generic_page* page = (generic_page*) workspace;
            for (int i = 0; i < segmentSize; i++, page++) {
                // If checksum does not match, we consider it a virgin page
                if (page->checksum != page->calculate_checksum()) {
                    continue;
                }
                if (lsn == lsn_t::null || page->lsn < lsn) {
                    lsn = page->lsn;
                }
            }
        }

        LogArchiver::ArchiveScanner::RunMerger* merger =
            logScan.open(start, end, lsn);

        generic_page* page = (generic_page*) workspace;
        fixable_page_h fixable;
        shpid_t current = firstPage;
        shpid_t prevPage = 0;
        size_t redone = 0;

        DBG(<< "Restoring segment " << segment << " page " << current);

        logrec_t* lr;
        while (merger->next(lr)) {
            DBGOUT4(<< "Would restore " << *lr);

            lpid_t lrpid = lr->construct_pid();
            w_assert1(lrpid.vol() == volume->vid());
            w_assert1(lrpid.page >= firstPage);
            w_assert1(lrpid.page >= prevPage);
            w_assert1(lrpid.page < firstPage + segmentSize);

            while (lrpid.page > current) {
                current++;
                page++;
                DBG(<< "Restoring page " << current);
            }

            if (!fixable.is_fixed() || fixable.pid().page != lrpid.page) {
                // PID is manually set for virgin pages
                // this guarantees correct redo of multi-page logrecs
                if (page->pid != lrpid) {
                    page->pid = lrpid;
                }
                fixable.setup_for_restore(page);
            }

            lr->redo(&fixable);
            fixable.update_initial_and_last_lsn(lr->lsn_ck());
            fixable.update_clsn(lr->lsn_ck());

            // Restored pages are always written out with proper checksum.
            // If restore thread is actually generating a new backup, this
            // is a requirement.
            page->checksum = page->calculate_checksum();
            w_assert1(page->pid == lrpid);
            w_assert1(page->checksum == page->calculate_checksum());

            prevPage = lrpid.page;
            redone++;
        }

        // current should point to the first not-restored page (excl. bound)
        if (redone > 0) { // i.e., something was restored
            current++;
        }

        // in the last segment, we may write less than the segment size
        finishSegment(workspace, segment, current - firstPage);

        backup->unfix(segment);
    }

    DBG(<< "Restore thread finished! " << numRestoredPages
            << " pages restored");
}

void RestoreMgr::finishSegment(char* workspace, unsigned segment, size_t count)
{
    shpid_t firstPage = getPidForSegment(segment);

    if (count > 0) {
        w_assert1((int) count <= segmentSize);
        // write pages back to replacement device (or backup)
        if (takeBackup) {
            W_COERCE(volume->write_backup(firstPage, count, workspace));
        }
        else {
            W_COERCE(volume->write_many_pages(firstPage, (generic_page*) workspace,
                        count, true /* ignoreRestore */));
        }
        DBG(<< "Wrote out " << count << " pages of segment " << segment);
    }

    // taking a backup should not generate log records, so pass the redo flag
    markSegmentRestored(workspace, segment, takeBackup /* redo */);

    // send signal to waiting threads (acquire mutex to avoid lost signal)
    DO_PTHREAD(pthread_mutex_lock(&restoreCondMutex));
    DO_PTHREAD(pthread_cond_broadcast(&restoreCond));
    DO_PTHREAD(pthread_mutex_unlock(&restoreCondMutex));
}

void RestoreMgr::markSegmentRestored(char* workspace, unsigned segment, bool redo)
{
    spinlock_write_critical_section cs(&requestMutex);

    /*
     * Now that the segment is restored, copy it into the buffer pool frame of
     * each matching request. Acquire the mutex for that to avoid race
     * condition in which segment gets restored after caller checks but before
     * its request is placed.
     */
    if (reuseRestoredBuffer && workspace && !redo) {
        shpid_t firstPage = getPidForSegment(segment);

        for (int i = 0; i < segmentSize; i++) {
            map<shpid_t, generic_page*>::iterator pos =
                bufferedRequests.find(firstPage + i);

            if (pos != bufferedRequests.end()) {
                char* wpage = workspace + (sizeof(generic_page) * i);

                memcpy(pos->second, wpage, sizeof(generic_page));
                bufferedRequests.erase(pos);
            }
        }
    }

    // Mark whole segment as restored, even if no page was actually replayed
    // (i.e., segment contains only unused pages)
    numRestoredPages += segmentSize;
    if (numRestoredPages >= numPages) {
        numRestoredPages = numPages;
    }

    if (!redo) {
        sys_xct_section_t ssx(true);
        log_restore_segment(volume->vid(), segment);
        ssx.end_sys_xct(RCOK);
    }

    bitmap->set(segment);
}

void RestoreMgr::run()
{
    // for now, restore thread only runs restore loop
    restoreLoop();
}