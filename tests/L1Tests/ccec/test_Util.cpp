/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2016 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/*
 * Dedicated L1 tests for the CEC logging utility - its configuration reader, verbosity
 * gates and buffer formatter.
 *
 * The configuration reader selects a fixed table entry by key prefix and never parses a
 * numeric value out of the file, so malformed-number scenarios are unreachable and are
 * deliberately absent.
 *
 * Both the hardcoded configuration file and the file-static log level are shared process
 * state, and the fixture restores BOTH: first the effective level the process had on
 * entry - recovered by probing CCEC_LOG, because cec_log_level has internal linkage and no
 * accessor - and then the file byte-for-byte, or its absence. That order is required,
 * because applying a level means writing the file. Restoring only one half leaks verbosity
 * into every later LibCCEC and DriverImpl case.
 *
 * The configuration path is fixed by production check_cec_log_status(), which hardcodes
 * fopen("/tmp/cec_log_enabled"), so this suite cannot be pointed at a private temporary
 * without a production change. A fixed name in a world-writable directory is the classic
 * unsafe-temporary shape, so one guard owns every access to it: the path is classified with
 * lstat and refused unless it is absent or a regular file this process owns; reads open
 * O_RDONLY|O_NOFOLLOW|O_CLOEXEC; writes go to a fresh O_EXCL|O_NOFOLLOW temporary in the
 * same directory, with the original mode, owner and group reapplied, and are rename()d over
 * the path so a planted link is replaced rather than written through.
 *
 * INTER-PROCESS CUSTODY. The path is fixed, so two copies of this suite on one host - or
 * this suite and anything else that writes it - contend for the same file, and
 * capture-then-restore is not atomic across processes: the second copy captures what the
 * first has already replaced and hands that back as though it were the original. An
 * advisory whole-file lock on a companion path, "/tmp/cec_log_enabled.testlock", is
 * therefore taken by the guard BEFORE it captures anything and released only after it has
 * restored, so the capture-mutate-restore window is exclusive. A lock this fixture cannot
 * obtain is a hard failure, not a warning: proceeding without it is precisely the race.
 *
 * RESTORATION IS NORMAL-FLOW ONLY - NO SIGNAL HANDLER. An earlier revision also armed the
 * restoration from handlers installed on SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV and
 * SIGABRT. That was wrong, and not marginally: the handler reached a function-local static
 * through instance(), mutated ordinary C++ object state, and could re-enter the very
 * rename()/unlink() sequence it interrupted - two racing writers over one shared temporary
 * path, from a context where almost nothing is defined. A crash mid-restore would corrupt
 * the file it was trying to protect. Restoration is now TearDown() plus one atexit hook,
 * both of which run on ordinary control flow; the residual exposure - a SIGKILL, or a
 * fatal signal - leaves the file as it stood, which is a stale value in a debug-logging
 * switch and is recoverable by hand, whereas an interrupted rename is not.
 *
 * cec_log_level is a plain int with internal linkage in Util.cpp: check_cec_log_status()
 * writes it, and CCEC_LOG() and dump_buffer() read it, with no lock, no atomic and no
 * accessor. The Bus reader and writer threads outlive every case in this binary, so a write
 * issued from the test thread is formally concurrent with their reads.
 *
 * WHY THAT IS NOT ANSWERED WITH fork(). An earlier revision ran every case body in a forked
 * child so the write happened where no Bus thread existed. fork() in a process that has
 * other threads leaves the child holding whatever locks those threads happened to own, and
 * the child then ran GoogleTest, stdio and libgcov - none of them async-signal-safe and all
 * of them allocator users. The isolation bought a formal data race and paid for it with a
 * deadlock that no amount of care inside the child could remove; a poll-and-kill deadline
 * around it converted the hang into a timeout, which is a failure report, not a fix.
 *
 * So the bodies run in this process, and the remaining exposure is stated rather than
 * engineered around: an int written by one thread while others read it. Both Bus threads
 * park on a condition variable when there is no CEC traffic and no case here generates any,
 * and every read is a single load whose two possible values are the old level and the new
 * one. What is left unfixed is in production, not here: cec_log_level should be atomic, or
 * Util.cpp should expose a seam for setting and reading it. That is a production change, so
 * it is reported as a BLOCKED gap rather than made - see the traceability report.
 *
 * The output assertions are unaffected by any of this: each matches a unique marker or an
 * ordered token sequence rather than the size of the capture, so output from another
 * writer reaching the redirected descriptor cannot change a verdict.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ccec/Util.hpp"

namespace {

const char *kLogConfigPath = "/tmp/cec_log_enabled";

struct LogLevelCase {
    const char *key;
    int value;
};

const LogLevelCase kLogLevels[] = {
    { "FATAL", LOG_FATAL },
    { "ERROR", LOG_ERROR },
    { "WARN", LOG_WARN },
    { "EXP", LOG_EXP },
    { "NOTICE", LOG_NOTICE },
    { "INFO", LOG_INFO },
    { "DEBUG", LOG_DEBUG },
    { "TRACE", LOG_TRACE }
};

/*
 * Self-contained stdout capture, at file-descriptor level.
 *
 * The code under test logs through printf/vprintf - C stdio writing to fd 1 - so redirecting the
 * C++ std::cout streambuf would capture nothing. GoogleTest does offer CaptureStdout() and
 * GetCapturedStdout(), and they work, but they live in ::testing::internal, which upstream
 * documents as not part of the public API; this suite builds against GoogleTest 1.14.0 locally and
 * v1.15.0 in CI, and it is the only place in the middleware suite that would reach into that
 * namespace. Doing it directly costs a few lines and removes the coupling: duplicate fd 1, point it
 * at an anonymous temporary, run the body, restore, read back.
 *
 * tmpfile() is used rather than a named path: it is unlinked as it is created, so there is nothing
 * for another process to substitute.
 */
class StdoutCapture {
public:
    StdoutCapture()
        : savedStdout_(-1)
        , sink_(::tmpfile())
    {
        if (sink_ == nullptr) {
            return;
        }

        ::fflush(stdout);
        savedStdout_ = ::dup(STDOUT_FILENO);
        if (savedStdout_ < 0) {
            ::fclose(sink_);
            sink_ = nullptr;
            return;
        }

        if (::dup2(::fileno(sink_), STDOUT_FILENO) < 0) {
            ::close(savedStdout_);
            savedStdout_ = -1;
            ::fclose(sink_);
            sink_ = nullptr;
        }
    }

    StdoutCapture(const StdoutCapture &) = delete;
    StdoutCapture &operator=(const StdoutCapture &) = delete;

    ~StdoutCapture() {
        restore();
        if (sink_ != nullptr) {
            ::fclose(sink_);
        }
    }

    bool isValid() const { return sink_ != nullptr && savedStdout_ >= 0; }

    // Restores the real stdout first, so nothing written afterwards is swallowed, then returns
    // everything the body wrote.
    std::string read() {
        restore();
        if (sink_ == nullptr) {
            return std::string();
        }

        ::fflush(sink_);
        ::rewind(sink_);

        std::string captured;
        char buffer[512];
        size_t bytesRead = 0;
        while ((bytesRead = ::fread(buffer, 1, sizeof(buffer), sink_)) > 0) {
            captured.append(buffer, bytesRead);
        }
        return captured;
    }

private:
    void restore() {
        if (savedStdout_ >= 0) {
            ::fflush(stdout);
            ::dup2(savedStdout_, STDOUT_FILENO);
            ::close(savedStdout_);
            savedStdout_ = -1;
        }
    }

    int savedStdout_;
    FILE *sink_;
};

template <typename Callable>
std::string captureStdout(Callable body) {
    StdoutCapture capture;
    if (!capture.isValid()) {
        // Redirection could not be established. Run the body anyway so the test still exercises
        // the code, and return nothing so the content assertions fail loudly rather than silently
        // passing against an empty string that was never compared.
        body();
        return std::string();
    }

    try {
        body();
    } catch (...) {
        (void)capture.read();
        throw;
    }

    return capture.read();
}

/*
 * Guard over the production log-configuration file.
 *
 * One primitive owns every access to the path: capture, write, remove and restore. That
 * is deliberate - four hand-rolled variants of "save the file and hope to put it back"
 * is exactly how a fixture ends up leaving a machine modified - and it is what makes the
 * no-follow, atomic-replace and signal-safe properties described in the file header hold
 * for every code path rather than for the ones somebody remembered.
 *
 * The guard is also the holder of the inter-process lock over the path, so the exclusive
 * window and the capture it protects cannot get out of step: activate() takes the lock
 * before it reads a byte, and restoreAndDeactivate() releases it after it has written the
 * original back.  Nothing here runs from a signal handler - see RESTORATION IS NORMAL-FLOW
 * ONLY in this file's header for why that was removed.
 */
class LogConfigGuard {
public:
    static LogConfigGuard &instance() {
        static LogConfigGuard guard;
        return guard;
    }

    /*
     * Capture the current state of the path and arm the restoration paths. Returns false
     * when the path is something this fixture must not touch; lastError() then explains
     * what was found. No mutation of any kind has happened at that point.
     */
    bool activate() {
        if (active_) {
            return true;
        }
        error_.clear();

        /* EXCLUSIVE FIRST, CAPTURE SECOND.  The path is fixed by production code, so a
         * second writer is possible; capturing before locking would capture whatever that
         * writer had already put there and hand it back as the original.
         *
         * A `return false` anywhere below this point leaves the lock held and active_ false.
         * That is deliberate rather than leaked: GoogleTest runs TearDown() even when a
         * fatal assertion aborts SetUp(), and restoreAndDeactivate() releases the lock on
         * the !active_ path for exactly this case; the atexit hook is the backstop. */
        if (!acquireLock()) {
            return false;
        }
        savedLength_ = 0;
        savedExisted_ = false;
        savedMode_ = 0600;
        savedUid_ = static_cast<uid_t>(-1);
        savedGid_ = static_cast<gid_t>(-1);

        struct stat status;
        if (lstat(kLogConfigPath, &status) == 0) {
            if (!S_ISREG(status.st_mode)) {
                error_ = std::string(kLogConfigPath)
                    + " exists and is not a regular file (mode "
                    + std::to_string(static_cast<unsigned long>(status.st_mode))
                    + "); refusing to modify it, because writing through it would alter"
                      " whatever it refers to";
                return false;
            }
            if (status.st_uid != geteuid() && geteuid() != 0) {
                error_ = std::string(kLogConfigPath) + " is owned by uid "
                    + std::to_string(static_cast<unsigned long>(status.st_uid))
                    + " and this process is not that user; refusing to modify it";
                return false;
            }

            const int fd = open(kLogConfigPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                error_ = std::string("could not read ") + kLogConfigPath + ": "
                    + std::strerror(errno);
                return false;
            }
            ssize_t got = 0;
            size_t total = 0;
            while (total < sizeof(saved_)
                   && (got = read(fd, saved_ + total, sizeof(saved_) - total)) > 0) {
                total += static_cast<size_t>(got);
            }
            const bool readFailed = (got < 0);
            /* A file larger than the static buffer could not be reproduced byte-for-byte
             * from a signal handler, and restoring a truncated copy would be worse than not
             * touching it at all, so it is refused outright. The production configuration is
             * a single short line, so this is a "something unexpected is here" signal. */
            char overflow = '\0';
            const bool tooLong = (total == sizeof(saved_)) && (read(fd, &overflow, 1) > 0);
            close(fd);
            if (readFailed) {
                error_ = std::string("could not read ") + kLogConfigPath;
                return false;
            }
            if (tooLong) {
                error_ = std::string(kLogConfigPath) + " is larger than "
                    + std::to_string(sizeof(saved_))
                    + " bytes, so it could not be restored faithfully; refusing to modify it";
                return false;
            }
            savedLength_ = total;
            savedExisted_ = true;
            savedMode_ = status.st_mode & 07777;
            savedUid_ = status.st_uid;
            savedGid_ = status.st_gid;
        } else if (errno != ENOENT) {
            error_ = std::string("could not inspect ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }

        buildTempPath();
        registerAtExitOnce();
        active_ = true;
        return true;
    }

    /* Write one configuration value, atomically and without following a link. */
    bool write(const char *data, size_t length) {
        return atomicWrite(data, length, savedMode_, savedUid_, savedGid_) == 0;
    }

    bool write(const std::string &contents) {
        return write(contents.data(), contents.size());
    }

    /* Remove the file. unlink() never follows, so a link would be removed, not its target. */
    bool remove() {
        return unlink(kLogConfigPath) == 0 || errno == ENOENT;
    }

    bool existed() const { return savedExisted_; }
    bool active() const { return active_; }
    const std::string &lastError() const { return error_; }

    /* Put the path back exactly as it was found, then release the exclusive window. */
    bool restoreAndDeactivate() {
        if (!active_) {
            releaseLock();     /* activate() may have failed after locking */
            return true;
        }
        const bool restored = (restoreQuietly() == 0);
        active_ = false;
        releaseLock();
        return restored;
    }

private:
    LogConfigGuard()
        : active_(false)
        , savedExisted_(false)
        , savedLength_(0)
        , savedMode_(0600)
        , savedUid_(static_cast<uid_t>(-1))
        , savedGid_(static_cast<gid_t>(-1))
        , atExitRegistered_(false)
        , lockFd_(-1) {
        saved_[0] = '\0';
        tempPath_[0] = '\0';
        lockPath_[0] = '\0';
    }

    LogConfigGuard(const LogConfigGuard &);
    LogConfigGuard &operator=(const LogConfigGuard &);

    /* Allocation-free: only open/write/fchmod/fchown/rename/unlink/close. */
    int atomicWrite(const char *data, size_t length, mode_t mode, uid_t uid, gid_t gid) {
        if (tempPath_[0] == '\0') {
            return -1;
        }
        unlink(tempPath_);
        const int fd = open(tempPath_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            return -1;
        }
        size_t written = 0;
        while (written < length) {
            const ssize_t chunk = ::write(fd, data + written, length - written);
            if (chunk <= 0) {
                if (chunk < 0 && errno == EINTR) {
                    continue;
                }
                close(fd);
                unlink(tempPath_);
                return -1;
            }
            written += static_cast<size_t>(chunk);
        }
        /* Hand back the metadata the file was found with. The two calls are NOT
         * equivalent, so they are not treated equivalently:
         *
         *   fchmod - this descriptor was just created by this process with O_EXCL, so
         *            this process owns the file and fchmod to any mode must succeed. A
         *            failure here means the restored file carries permissions the host
         *            did not have before, which is exactly the silent host mutation the
         *            restoration exists to prevent, so it fails the write.
         *   fchown - genuinely best-effort. A non-root process cannot give a file away,
         *            so EPERM is the normal, expected outcome whenever the config file
         *            was owned by somebody else, and failing on it would abort every
         *            unprivileged run for a condition the test cannot influence.
         */
        if (fchmod(fd, mode) != 0) {
            close(fd);
            unlink(tempPath_);
            return -1;
        }
        if (uid != static_cast<uid_t>(-1)) {
            (void)fchown(fd, uid, gid);
        }
        if (close(fd) != 0) {
            unlink(tempPath_);
            return -1;
        }
        if (rename(tempPath_, kLogConfigPath) != 0) {
            unlink(tempPath_);
            return -1;
        }
        return 0;
    }

    /* The one restoration primitive, used by TearDown and by the atexit hook alike. */
    int restoreQuietly() {
        if (!savedExisted_) {
            return (unlink(kLogConfigPath) == 0 || errno == ENOENT) ? 0 : -1;
        }
        return atomicWrite(saved_, savedLength_, savedMode_, savedUid_, savedGid_);
    }

    void buildTempPath() {
        /* Built once, into a static buffer, so no restoration path has to format it. */
        const long pid = static_cast<long>(getpid());
        std::snprintf(tempPath_, sizeof(tempPath_), "%s.test.%ld.tmp", kLogConfigPath, pid);
    }

    void registerAtExitOnce() {
        if (atExitRegistered_) {
            return;
        }
        std::atexit(&LogConfigGuard::atExitRestore);
        atExitRegistered_ = true;
    }

    /* Last-chance restoration on ORDINARY process exit.  atexit handlers run on return
     * from main() and on exit(), which is normal control flow - unlike a signal handler,
     * this may safely touch the guard's state and the filesystem.  It exists for the case
     * where a fatal GoogleTest assertion unwinds past TearDown. */
    static void atExitRestore() {
        LogConfigGuard &guard = instance();
        if (guard.active_) {
            (void)guard.restoreQuietly();
            guard.active_ = false;
        }
        guard.releaseLock();
    }

    /*
     * Advisory whole-file lock over a COMPANION path, not over the configuration file
     * itself.  Locking the file would mean holding a descriptor to it across the
     * rename() that replaces it, at which point the lock is on the replaced inode and no
     * longer excludes anybody.  A separate, never-renamed lock file keeps the lock and the
     * thing it protects independent.
     *
     * flock() rather than lockf()/fcntl(): the lock is wanted for the lifetime of the
     * descriptor with no byte ranges, it is released automatically if this process dies,
     * and it is not silently dropped when some other close() of the same file happens.
     * LOCK_NB with a bounded retry, because blocking for ever inside SetUp() would turn a
     * contended host into a suite that hangs instead of a suite that reports.
     */
    bool acquireLock() {
        if (lockFd_ >= 0) {
            return true;
        }
        std::snprintf(lockPath_, sizeof(lockPath_), "%s.testlock", kLogConfigPath);

        const int fd = open(lockPath_, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            error_ = std::string("could not open the custody lock ") + lockPath_ + ": "
                + std::strerror(errno)
                + " (a symlink at that path is refused, which is what O_NOFOLLOW reports"
                  " as ELOOP)";
            return false;
        }

        /* 5 s in 10 ms steps.  The critical section is a handful of small writes, so any
         * genuine holder clears it in milliseconds; anything longer is a stuck process and
         * is worth failing on rather than waiting out. */
        const int kAttempts = 500;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                lockFd_ = fd;
                return true;
            }
            if (errno != EWOULDBLOCK) {
                error_ = std::string("could not lock ") + lockPath_ + ": "
                    + std::strerror(errno);
                close(fd);
                return false;
            }
            struct timespec pause;
            pause.tv_sec = 0;
            pause.tv_nsec = 10 * 1000 * 1000L;
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
                /* finish the remaining interval nanosleep wrote back */
            }
        }
        error_ = std::string("another process has held ") + lockPath_ + " for over 5 s, so "
            + kLogConfigPath + " is not this suite's to modify.  Two copies of run_L1Tests "
            "on one host contend for that fixed path; run them one at a time.";
        close(fd);
        return false;
    }

    void releaseLock() {
        if (lockFd_ < 0) {
            return;
        }
        /* The lock file is deliberately NOT unlinked: another waiter may already hold a
         * descriptor to it, and removing it would let a third process create a new inode
         * and lock that instead - two "exclusive" holders of different files. */
        (void)flock(lockFd_, LOCK_UN);
        close(lockFd_);
        lockFd_ = -1;
    }

    bool active_;
    bool savedExisted_;
    size_t savedLength_;
    mode_t savedMode_;
    uid_t savedUid_;
    gid_t savedGid_;
    bool atExitRegistered_;
    int lockFd_;
    std::string error_;
    char saved_[4096];
    char tempPath_[256];
    char lockPath_[288];
};

LogConfigGuard &logConfig() {
    return LogConfigGuard::instance();
}

bool writeLogConfig(const std::string &contents) {
    return logConfig().write(contents);
}

const int kUnknownLogLevel = -1;

// cec_log_level is file-static in Util.cpp with no accessor, so the effective level can
// only be observed: CCEC_LOG emits when its level is <= the configured level, so the
// highest level that still emits IS the configured level.
int probeEffectiveLogLevel() {
    for (int level = LOG_MAX - 1; level >= 0; --level) {
        const std::string marker = "UTIL_PROBE_LEVEL_" + std::to_string(level);
        const std::string output = captureStdout([level, &marker]() {
            CCEC_LOG(level, "%s", marker.c_str());
        });
        if (output.find(marker) != std::string::npos) {
            return level;
        }
    }
    return kUnknownLogLevel;
}

// Whether a configuration file is present at the fixed path. lstat, so a symlink standing
// there is not reported as the file.
bool logConfigExists() {
    struct stat linkStatus;
    return lstat(kLogConfigPath, &linkStatus) == 0 && S_ISREG(linkStatus.st_mode) != 0;
}

} // namespace

class UtilTest : public ::testing::Test {
protected:
    void SetUp() override {
        /* Take the inter-process lock, capture the configuration file and arm the atexit
         * restoration before the first write. A refusal here means either that another
         * process holds the path or that it holds something this fixture must not modify;
         * both are hard failures rather than a licence to proceed. */
        ASSERT_TRUE(logConfig().activate()) << logConfig().lastError();

        /* Read the effective level BEFORE anything writes it, because it is what TearDown
         * has to put back. cec_log_level has internal linkage and no accessor, so CCEC_LOG
         * is the only observation available. */
        entryLogLevel_ = probeEffectiveLogLevel();
        ASSERT_NE(kUnknownLogLevel, entryLogLevel_)
            << "no CCEC_LOG level produced output, so the level this case must restore could "
            << "not be observed; proceeding would leave the process at whatever verbosity "
            << "the case happens to set last.";

        /* Every case in this fixture must start from the same effective level. Each case
         * restores it in TearDown, so a mismatch here means a restoration did not hold -
         * either a case left the level moved, or another process rewrote the shared
         * configuration file despite the lock. */
        if (firstObservedLogLevel_ == kUnknownLogLevel) {
            firstObservedLogLevel_ = entryLogLevel_;
        }
        EXPECT_EQ(firstObservedLogLevel_, entryLogLevel_)
            << "the effective CEC log level changed between UtilTest cases, so either a case "
            << "did not restore it or something outside this fixture rewrote "
            << kLogConfigPath << " while the suite was running.";
    }

    void TearDown() override {
        /* THE ORDER MATTERS AND IS THE WHOLE POINT.  Applying a level means WRITING the
         * configuration file, so the level has to go back first and the file's original
         * bytes second - the reverse order would restore the file and then overwrite it. */
        restoreEntryLogLevel();

        /* Hand the file back byte-for-byte (or remove it again), disarm the atexit hook and
         * release the inter-process lock. */
        EXPECT_TRUE(logConfig().restoreAndDeactivate())
            << "failed to restore " << kLogConfigPath;

        /* Prove the restoration rather than assume it: the level observed on the way out
         * must be the level observed on the way in. */
        EXPECT_EQ(entryLogLevel_, probeEffectiveLogLevel())
            << "this case left the process-wide CEC log level at a different value than it "
            << "found, which would leak verbosity into every later LibCCEC and DriverImpl "
            << "case in this binary";
    }

    /*
     * Runs one case body against the INFO baseline every case starts from.
     *
     * The name is kept because every case reads `RunWithBaseline([this]{ ... })` and the
     * shape of the cases is unchanged; what it no longer does is fork.  See WHY THAT IS NOT
     * ANSWERED WITH fork() in this file's header: the body executes in this process, and the
     * process-wide level it moves is put back by TearDown() above.
     */
    void RunWithBaseline(const std::function<void()> &body) {
        ASSERT_TRUE(logConfig().active())
            << "the configuration-file guard is not armed, so nothing may write "
            << kLogConfigPath;

        applyLogConfig("INFO\n");
        body();
    }

    /*
     * Put cec_log_level back to the value the case found, through the only route production
     * offers: write the matching key into the configuration file and let
     * check_cec_log_status() read it.  There is no setter.
     */
    void restoreEntryLogLevel() {
        if (entryLogLevel_ == kUnknownLogLevel) {
            return;
        }
        if (probeEffectiveLogLevel() == entryLogLevel_) {
            return;   /* the case did not move it; nothing to write */
        }
        for (size_t i = 0; i < sizeof(kLogLevels) / sizeof(kLogLevels[0]); ++i) {
            if (kLogLevels[i].value != entryLogLevel_) {
                continue;
            }
            const std::string contents = std::string(kLogLevels[i].key) + "\n";
            EXPECT_TRUE(writeLogConfig(contents))
                << "could not write " << kLogConfigPath << " to restore log level "
                << entryLogLevel_;
            EXPECT_NO_THROW(check_cec_log_status());
            return;
        }
        ADD_FAILURE() << "the entry log level " << entryLogLevel_ << " has no key in the "
                      << "production level table, so it cannot be restored through "
                      << "check_cec_log_status(); the table and LOG_MAX have diverged.";
    }

    void applyLogConfig(const std::string &contents) {
        EXPECT_TRUE(writeLogConfig(contents));
        EXPECT_NO_THROW(check_cec_log_status());
    }

    /* Removing the configuration file goes through the same guard as every other access,
     * so no test reaches the path with a call that could follow a link. */
    void removeLogConfig() {
        EXPECT_TRUE(logConfig().remove()) << "failed to remove " << kLogConfigPath;
    }

    std::string captureLog(int level, const std::string &marker) {
        return captureStdout([level, &marker]() {
            CCEC_LOG(level, "%s", marker.c_str());
        });
    }

    std::string captureDump(unsigned char *buffer, int length) {
        return captureStdout([buffer, length]() {
            dump_buffer(buffer, length);
        });
    }

private:
    /* The effective level this process had when the case started, which it must still have
     * when the case ends. */
    int entryLogLevel_ = kUnknownLogLevel;

    /* Shared by every case in the fixture: the effective level observed before the first
     * case ran, which every later case must also observe. */
    static int firstObservedLogLevel_;
};

int UtilTest::firstObservedLogLevel_ = kUnknownLogLevel;

// Traceability: #gap-mw-util - section 6.2 rank 33, P2 -
// check_cec_log_status().
TEST_F(UtilTest, MissingConfigFileLeavesLogLevelUnchanged) {
    RunWithBaseline([this]() {
        applyLogConfig("DEBUG\n");
        removeLogConfig();
        EXPECT_FALSE(logConfigExists());

        std::string readerOutput;
        EXPECT_NO_THROW(readerOutput = captureStdout([]() {
            check_cec_log_status();
        }));
        EXPECT_NE(readerOutput.find("cec_log_enabled"), std::string::npos);

        unsigned char probe[] = { 0xA5, 0x5A, 0xA5 };
        const std::string dumpOutput = captureDump(probe, 3);
        EXPECT_NE(dumpOutput.find("A5 5A A5"), std::string::npos);
    });
}

TEST_F(UtilTest, EveryRecognisedLevelMapsToItsNumericSetting) {
    RunWithBaseline([this]() {
        unsigned char dumpProbe[] = { 0xD3, 0x7B };

        for (const LogLevelCase &levelCase : kLogLevels) {
            SCOPED_TRACE(levelCase.key);
            applyLogConfig(std::string(levelCase.key) + "\n");

            const std::string thresholdMarker =
                std::string("UTIL_THRESHOLD_") + levelCase.key;
            const std::string thresholdOutput =
                captureLog(levelCase.value, thresholdMarker);
            EXPECT_NE(thresholdOutput.find(thresholdMarker), std::string::npos);

            if (levelCase.value + 1 < LOG_MAX) {
                const std::string aboveMarker =
                    std::string("UTIL_ABOVE_") + levelCase.key;
                const std::string aboveOutput =
                    captureLog(levelCase.value + 1, aboveMarker);
                EXPECT_EQ(aboveOutput.find(aboveMarker), std::string::npos);
            }

            const std::string dumpOutput = captureDump(dumpProbe, 2);
            if (levelCase.value >= LOG_DEBUG) {
                EXPECT_NE(dumpOutput.find("D3 7B"), std::string::npos);
            } else {
                EXPECT_EQ(dumpOutput.find("D3 7B"), std::string::npos);
            }
        }
    });
}

TEST_F(UtilTest, EmptyConfigFileLeavesLogLevelUnchanged) {
    RunWithBaseline([this]() {
        applyLogConfig("INFO\n");
        applyLogConfig("");

        const std::string infoMarker = "UTIL_EMPTY_FILE_INFO";
        const std::string infoOutput = captureLog(LOG_INFO, infoMarker);
        EXPECT_NE(infoOutput.find(infoMarker), std::string::npos);

        const std::string debugMarker = "UTIL_EMPTY_FILE_DEBUG";
        const std::string debugOutput = captureLog(LOG_DEBUG, debugMarker);
        EXPECT_EQ(debugOutput.find(debugMarker), std::string::npos);
    });
}

TEST_F(UtilTest, UnrecognisedLevelLeavesLogLevelUnchanged) {
    RunWithBaseline([this]() {
        applyLogConfig("TRACE\n");
        applyLogConfig("NOT_A_LEVEL\n");

        const std::string marker = "UTIL_UNRECOGNISED_RETAINS_TRACE";
        const std::string output = captureLog(LOG_TRACE, marker);
        EXPECT_NE(output.find(marker), std::string::npos);
    });
}

TEST_F(UtilTest, ShortPrefixDoesNotMatchRecognisedLevel) {
    RunWithBaseline([this]() {
        applyLogConfig("INFO\n");
        applyLogConfig("DEBU\n");

        const std::string infoMarker = "UTIL_SHORT_PREFIX_INFO";
        const std::string infoOutput = captureLog(LOG_INFO, infoMarker);
        EXPECT_NE(infoOutput.find(infoMarker), std::string::npos);

        const std::string debugMarker = "UTIL_SHORT_PREFIX_DEBUG";
        const std::string debugOutput = captureLog(LOG_DEBUG, debugMarker);
        EXPECT_EQ(debugOutput.find(debugMarker), std::string::npos);
    });
}

TEST_F(UtilTest, RecognisedLevelWithTrailingContentIsAccepted) {
    RunWithBaseline([this]() {
        applyLogConfig("DEBUG extra trailing text\n");

        unsigned char probe[] = { 0xC3, 0x5E };
        const std::string output = captureDump(probe, 2);
        EXPECT_NE(output.find("C3 5E"), std::string::npos);
    });
}

TEST_F(UtilTest, DumpBufferEmitsAtDebugAndTraceLevels) {
    RunWithBaseline([this]() {
        const char *enablingLevels[] = { "DEBUG", "TRACE" };
        unsigned char probe[] = { 0x00, 0x0F, 0xA6, 0xFF };

        for (const char *level : enablingLevels) {
            SCOPED_TRACE(level);
            applyLogConfig(std::string(level) + "\n");

            const std::string output = captureDump(probe, 4);
            EXPECT_NE(output.find("00"), std::string::npos);
            EXPECT_NE(output.find("0F"), std::string::npos);
            EXPECT_NE(output.find("A6"), std::string::npos);
            EXPECT_NE(output.find("FF"), std::string::npos);
        }
    });
}

TEST_F(UtilTest, DumpBufferIsSilentBelowDebug) {
    RunWithBaseline([this]() {
        unsigned char probe[] = { 0xA5, 0x5A, 0xA5 };

        for (const LogLevelCase &levelCase : kLogLevels) {
            if (levelCase.value >= LOG_DEBUG) {
                continue;
            }

            SCOPED_TRACE(levelCase.key);
            applyLogConfig(std::string(levelCase.key) + "\n");
            const std::string output = captureDump(probe, 3);
            EXPECT_EQ(output.find("A5 5A A5"), std::string::npos);
        }
    });
}

TEST_F(UtilTest, DumpBufferWithZeroLengthEmitsNothing) {
    RunWithBaseline([this]() {
        applyLogConfig("DEBUG\n");

        unsigned char probe[] = { 0xA5, 0x5A, 0xA5 };
        const std::string output = captureDump(probe, 0);
        EXPECT_EQ(output.find("A5 5A A5"), std::string::npos);
    });
}

TEST_F(UtilTest, DumpBufferEmitsMaximumLengthBuffer) {
    RunWithBaseline([this]() {
        applyLogConfig("DEBUG\n");

        unsigned char buffer[256];
        for (unsigned int index = 0; index < sizeof(buffer); ++index) {
            buffer[index] = static_cast<unsigned char>(index);
        }

        /*
         * Assert the whole dump as one ordered token sequence rather than asserting the size
         * of the capture. It is the stronger statement about dump_buffer - every byte, in
         * order, in the documented "%02X " form - and unlike a size comparison it stays
         * deterministic if any other writer reaches the redirected descriptor.
         */
        std::string expectedSequence;
        expectedSequence.reserve(sizeof(buffer) * 3);
        for (unsigned int value = 0; value < sizeof(buffer); ++value) {
            char expectedToken[4];
            std::snprintf(expectedToken, sizeof(expectedToken), "%02X ",
                          static_cast<unsigned int>(buffer[value]));
            expectedSequence += expectedToken;
        }
        ASSERT_EQ(sizeof(buffer) * 3, expectedSequence.size());

        const std::string output = captureDump(buffer, sizeof(buffer));
        EXPECT_NE(output.find(expectedSequence), std::string::npos)
            << "dump_buffer did not emit all " << sizeof(buffer)
            << " bytes as an ordered uppercase token sequence";
    });
}

TEST_F(UtilTest, LogEmitsAtOrBelowConfiguredLevel) {
    RunWithBaseline([this]() {
        applyLogConfig("INFO\n");

        const std::string lowerMarker = "UTIL_LOG_LOWER_LEVEL";
        const std::string lowerOutput = captureLog(LOG_ERROR, lowerMarker);
        EXPECT_NE(lowerOutput.find(lowerMarker), std::string::npos);

        const std::string thresholdMarker = "UTIL_LOG_THRESHOLD_LEVEL";
        const std::string thresholdOutput = captureLog(LOG_INFO, thresholdMarker);
        EXPECT_NE(thresholdOutput.find(thresholdMarker), std::string::npos);
    });
}

TEST_F(UtilTest, LogSuppressesLevelsAboveConfiguredLevel) {
    RunWithBaseline([this]() {
        applyLogConfig("INFO\n");

        const std::string aboveMarker = "UTIL_LOG_ABOVE_LEVEL";
        const std::string aboveOutput = captureLog(LOG_DEBUG, aboveMarker);
        EXPECT_EQ(aboveOutput.find(aboveMarker), std::string::npos);

        const std::string sentinelMarker = "UTIL_LOG_SENTINEL_LEVEL";
        const std::string sentinelOutput = captureLog(LOG_MAX, sentinelMarker);
        EXPECT_EQ(sentinelOutput.find(sentinelMarker), std::string::npos);
    });
}
