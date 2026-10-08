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

/**
* @defgroup hdmicec
* @{
* @defgroup ccec
* @{
**/


/**
 * @file test_DriverAidl.cpp
 *
 * @brief L1 tests for the AIDL HDMI CEC back-end, the runtime back-end selection and the
 *        halcompat compatibility rule that selection rests on.
 *
 * Compatibility and preflight are tested by direct call, closed and probe-forced OPENED states
 * on local DriverAidlImpl instances, and back-end-specific behaviour in fixtures that assert the
 * resolved back-end in SetUp. Cases leave the shared driver open; the log level moves only via
 * ScopedCecLogLevel.
 *
 * Unreachable by construction for this era-0, major-1 client: (1) older-same-major, as its
 * same-major servers are exactly 1000-1999 and 999 is cross-major ((3020, 3000) reaches the arm
 * instead), and (2) halcompat's era >= 1 branch.
 *
 * @see DriverAidlImpl
 * @see Driver::getInstance()
 * @see AIDL_HAL_MIGRATION_NOTES.md
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ccec/CECFrame.hpp"
#include "ccec/Connection.hpp"
#include "ccec/Driver.hpp"
#include "ccec/Exception.hpp"
#include "ccec/FrameListener.hpp"
#include "ccec/LibCCEC.hpp"
#include "ccec/OpCode.hpp"
#include "ccec/Operands.hpp"
#include "ccec/Util.hpp"

/* Not an installed header, so reached by relative path; needed to name DriverImpl in the
 * dynamic_casts that identify the back-end this process resolved. */
#include "../../../ccec/src/DriverImpl.hpp"

/* Not installed either; needed for isBinderPreflightOk(), the AIDL half of those dynamic_casts,
 * and local closed instances that reach the state guards without touching the shared driver. */
#include "../../../ccec/src/DriverAidlImpl.hpp"

#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>
#include <utils/StrongPointer.h>

/* Used only inside DriverAidlSessionTest bodies, through selfOrNull(), which never creates a
 * ProcessState and so never opens a binder driver that is absent. */
#include <binder/ProcessState.h>

/* Quoted as DriverAidlImpl.cpp spells it; it lives under HALIF_PREFIX/common/current, the
 * third include root configure resolves for it. */
#include "halcompat.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "fake_hdmi_cec_aidl_service.h"
#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::SetArgPointee;

/** @brief Short alias for the generated AIDL package, spelled as DriverAidlImpl.cpp spells it. */
namespace cechal = ::com::rdk::hal::hdmicec;

/** @brief Short alias for the halcompat.h compatibility helpers, as DriverAidlImpl.cpp has it. */
namespace halcompat = ::com::rdk::hal::halcompat;

CCEC_BEGIN_NAMESPACE

/**
 * @brief Test-only gateway to private DriverAidlImpl::isBinderPreflightOk(), which befriends it.
 *
 * Forwards its arguments unchanged, so the predicate's own defaults apply.
 * tests/L1Tests/test_main.cpp defines it token-identically, as the one-definition rule requires.
 */
struct BinderPreflightTestAccess {
    /**
     * @brief Calls DriverAidlImpl::isBinderPreflightOk() with @p args, forwarded unchanged.
     *
     * @param [in] args - The predicate's leading arguments, in its parameter order.
     *
     * @return bool - The predicate's verdict.
     */
    template <typename... Args>
    static bool isBinderPreflightOk(Args &&...args) {
        return DriverAidlImpl::isBinderPreflightOk(std::forward<Args>(args)...);
    }
};

CCEC_END_NAMESPACE

/* AIDL contract: >16-byte frames refused, not truncated; ACK sense inverted; open() polls for and
 * registers one PLAYBACK_DEVICE address, read back from the HAL; physical address fixed 1.0.0.0. */

/* Fixture manifest: per-fixture table and counts in AIDL_HAL_MIGRATION_NOTES.md. AIDL-only fixtures
 * need invocation B; invocation-A filter: -DriverAidlSessionTest.*:DriverAidlTransmitTest.* */
namespace {

/**
 * @brief The AIDL sendMessage 16-byte limit, restated because production's
 *        AIDL_MAX_MESSAGE_LENGTH has internal linkage.
 *
 * @see DriverAidlImpl::write()
 */
constexpr size_t kAidlMaxMessageLength = 16;

/** @brief One byte past the AIDL contract and still within the legacy HAL's 20. */
constexpr size_t kJustOverAidlLimit = 17;

/** @brief The legacy HAL specification's maximum, the far end of the disputed range. */
constexpr size_t kLegacyMaxMessageLength = 20;

/**
 * @brief The selected-path log format, transcribed from SELECTED_BACK_END_LOG_FORMAT in Driver.cpp.
 *
 * Production's constant has internal linkage; the case using this drives the real logger with it.
 */
const char *const kSelectedBackEndLogFormat =
    "Driver::getInstance : HDMI CEC HAL back-end selected : %s\r\n";

/** @brief Back-end name the AIDL arm substitutes, transcribed from SELECTED_BACK_END_AIDL. */
const char *const kSelectedBackEndAidl   = "AIDL";

/** @brief Back-end name the legacy arm substitutes, transcribed from SELECTED_BACK_END_LEGACY. */
const char *const kSelectedBackEndLegacy = "legacy";

/**
 * @brief This client's compiled-against IHdmiCec version, as a constant of this translation unit.
 *
 * @warning IHdmiCec::VERSION has no out-of-line definition, so comparing it directly in an
 *          EXPECT_* or ASSERT_* macro (an odr-use) fails to link - use this copy.
 */
constexpr int32_t kClientInterfaceVersion = cechal::IHdmiCec::VERSION;

/** @brief The frozen IHdmiCec hash this client was compiled against, copied for symmetry. */
const char *const kFrozenInterfaceHash = cechal::IHdmiCec::HASHVALUE;

/** @brief The interface hash halcompat rejects as a failed hash RPC. */
const char *const kBrokenInterfaceHash = "-1";

/** @brief The hash a pre-freeze development server reports. */
const char *const kUnfrozenInterfaceHash = "notfrozen";

/**
 * @brief IHdmiCecController's compiled-against version, copied for the link reason
 *        kClientInterfaceVersion gives.
 *
 * @see kClientInterfaceVersion
 */
constexpr int32_t kControllerClientInterfaceVersion = cechal::IHdmiCecController::VERSION;

/** @brief IHdmiCecController's frozen hash, copied as kFrozenInterfaceHash is. */
const char *const kControllerFrozenInterfaceHash = cechal::IHdmiCecController::HASHVALUE;

/** @brief A version no interface in this snapshot reports, installed to make a fake diverge. */
constexpr int32_t kDivergentReportedVersion = 4242;

/** @brief A well-formed 40-hex-digit hash that is not the frozen one, for the same purpose. */
const char *const kDivergentReportedHash = "0000000000000000000000000000000000000000";

/** @brief A server version inside this client's era and major, newer than it: must be accepted. */
constexpr int32_t kNewerCompatibleVersion = 1010;

/** @brief The largest era-0 major-1 encoding, the far end of the accepted range. */
constexpr int32_t kNewestCompatibleVersion = 1999;

/** @brief The next major generation up: rejected by the cross-major conjunct. */
constexpr int32_t kCrossMajorVersion = 2000;

/** @brief Era 1: rejected because this client's era is 0. */
constexpr int32_t kCrossEraVersion = 100000;

/** @brief The generator default a pre-freeze server reports: era 0, major 0, so cross-major. */
constexpr int32_t kUnfrozenGeneratorVersion = 1;

/**
 * @brief Client half (era 0, major 3, minor 2) of a pair reaching the older-same-major arm,
 *        which unreachable path 1 in the file block shows this client cannot reach.
 *
 * @see kOlderSameMajorServer
 */
constexpr int32_t kOlderSameMajorClient = 3020;

/** @brief Server half of that pair: era 0, major 3, minor 0, so older within the same major. */
constexpr int32_t kOlderSameMajorServer = 3000;

/* Compile-time invariants. Each pins a value a case depends on, and its message says what to
 * re-derive if it fires. The version table is pinned through constexpr detail::isCompatible. */

static_assert(cechal::IHdmiCec::VERSION == 1000,
    "The compiled-against AIDL interface version is no longer 1000. Every version case in this "
    "file was derived for era 0, major 1 - and so were the two unreachable-by-construction "
    "conclusions recorded in the file block. Recompute both: decode the new VERSION with "
    "halcompat.h:93-96, re-derive which server values satisfy era(server) == era(client) and "
    "major(server) == major(client), and rebuild the accept/reject table below from that. In "
    "particular, if the new era is 1 or greater then the era >= 1 branch at halcompat.h:110-111 "
    "becomes reachable and must be covered rather than recorded as unreachable.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, cechal::IHdmiCec::VERSION),
    "A server reporting exactly this client's version is no longer compatible. That is the "
    "identity case; if it fails, the era rules at halcompat.h:108-115 changed fundamentally and "
    "CompatibleWhenServerReportsThisClientsVersion must be revisited before anything else.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kNewerCompatibleVersion),
    "A newer server within the same era and major is no longer accepted. Do not 'fix' this by "
    "asserting rejection: accepting a newer additive server is the documented rule "
    "(halcompat.h:100-103), and a test that treated 'not exactly our version' as incompatible "
    "would encode a rule that does not exist. If the rule genuinely changed, update "
    "CompatibleWhenServerReportsNewerVersionInSameMajor and this assertion together.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kNewestCompatibleVersion),
    "1999 - the largest era-0 major-1 encoding - is no longer accepted, so the accepted range is "
    "no longer the whole of [1000, 1999]. Unreachable path (1) in the file block is derived from "
    "that range being closed at the top; re-derive it before changing any case.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kCrossMajorVersion),
    "A server in the next major generation is now accepted. In era 0 a major bump is breaking "
    "(halcompat.h:60-62), so this would mean the era-0 rule was relaxed; if that is intended, "
    "IncompatibleWhenServerReportsDifferentMajor must be inverted and the file block's account of "
    "which conjunct rejects 999 rewritten, since that case also relies on the cross-major "
    "conjunct.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kCrossEraVersion),
    "An era-1 server now satisfies this era-0 client. Re-read halcompat.h:110-115: for that to "
    "happen either the era equality conjunct went away or this client's era changed. If the "
    "client moved to era 1 or later, the VERSION assertion above fires too and the whole table "
    "is rebuilt from there.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kUnfrozenGeneratorVersion),
    "The generator's default version 1 now satisfies a released client. That is the pre-freeze "
    "development server the rules exist to reject (halcompat.h:105-106); if it is now accepted, "
    "IncompatibleWhenServerReportsUnfrozenGeneratorVersion must be inverted deliberately and not "
    "by accident.");

static_assert(!halcompat::detail::isCompatible(kOlderSameMajorClient, kOlderSameMajorServer),
    "The genuine older-same-major proof no longer holds, and it is the only one this file has. "
    "(3020, 3000) is era 0 == era 0, major 3 == major 3, and 3000 >= 3020 false, so the ordering "
    "conjunct at halcompat.h:114 is what rejects it - which is exactly the arm being pinned. Note "
    "that halcompat.h:128's own static_assert(!isCompatible(3000, 2000), \"era0 older rejected\") "
    "is mislabelled: major(3000) is 3 and major(2000) is 2, so that pair is rejected by the "
    "cross-major conjunct and merely re-covers a case already covered. Do not substitute it for "
    "this one. And do not look for an equivalent pair for this client - unreachable path 1 in "
    "the file block proves none exists.");

static_assert(kDivergentReportedVersion != cechal::IHdmiCec::VERSION
                  && kDivergentReportedVersion != cechal::IHdmiCecController::VERSION,
    "kDivergentReportedVersion now equals a compiled-in interface version. The fake-metadata "
    "cases install it precisely because it differs: each fake's version getter traces only when "
    "the value it reports differs from its own compiled-in constant, so an equal value would "
    "drive the quiet arm while the cases still passed - the exact false green the divergence "
    "trace exists to prevent. Pick another value; nothing else depends on which.");

static_assert(static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0) == 0
              && static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1) == 1
              && static_cast<int32_t>(cechal::SendMessageStatus::BUSY) == 2,
    "The SendMessageStatus enumerators were renumbered by a snapshot regeneration. Stop here: the "
    "ACK sense is inverted between directed and broadcast messages, so a renumbering that went "
    "unnoticed would silently swap 'acknowledged' for 'rejected' on one of the two - the exact "
    "defect the whole status-translation matrix below exists to prevent. Re-read the enumerator "
    "documentation, then re-derive every arm of DriverAidlImpl::write()'s status translation and "
    "every case in DriverAidlTransmitTest from it.");

static_assert(CECFrame::MAX_LENGTH == 128,
    "CECFrame's capacity changed, and it is one of the three numbers the frame-size difference "
    "rests on - the other two being the AIDL contract's 16 and the legacy HAL specification's 20. "
    "If the capacity dropped below 20 the legacy arm of FrameOfTwentyBytes... can no longer be "
    "constructed at all; if it dropped below 17 the disputed range vanishes and the difference "
    "stops being observable. Re-derive the three frame sizes before touching a case.");

static_assert(kAidlMaxMessageLength < kJustOverAidlLimit
              && kJustOverAidlLimit <= kLegacyMaxMessageLength
              && kLegacyMaxMessageLength <= CECFrame::MAX_LENGTH,
    "The three frame sizes no longer straddle the authority conflict they were chosen to "
    "straddle: 16 must be accepted by both back-ends, 17 must sit strictly above the AIDL limit "
    "and at or below the legacy one, and 20 must still fit in a CECFrame. Fix the constants, not "
    "this assertion - and if AIDL_MAX_MESSAGE_LENGTH in ccec/src/DriverAidlImpl.cpp changed, "
    "kAidlMaxMessageLength here is the transcription that must follow it.");

static_assert(REPORT_PHYSICAL_ADDRESS == 0x84,
    "REPORT_PHYSICAL_ADDRESS is no longer 0x84. It is the selector for the CEC CTS 9-3-3 arm in "
    "DriverAidlImpl::write() (mirroring DriverImpl.cpp:279-284), which is the one rejected "
    "broadcast opcode that must raise CECNoAckException while every other rejected broadcast "
    "returns normally. Update the CTS-arm case and its negative control together, or the two "
    "cases will be asserting the same opcode and the distinction will be untested.");

/**
 * @brief Captures fd 1 into an anonymous tmpfile(), for asserting on CCEC_LOG output.
 *
 * CCEC_LOG writes through printf, so redirection is at descriptor level, as in test_Util.cpp.
 *
 * @warning Redirection is process-wide while an instance lives; keep its scope narrow.
 * @see isValid()
 */
class StdoutCapture {
public:
    /**
     * @brief Redirects fd 1 into an anonymous temporary file, unwinding in place on failure.
     *
     * @post isValid() reports whether the redirection is in force; on failure nothing is
     *       redirected and no descriptor is retained.
     */
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

    /** @brief Not copyable: two instances would each own one process-wide redirection. */
    StdoutCapture(const StdoutCapture &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    StdoutCapture &operator=(const StdoutCapture &) = delete;

    /** @brief Restores the real stdout and releases the temporary; idempotent with read(). */
    ~StdoutCapture() {
        restore();
        if (sink_ != nullptr) {
            ::fclose(sink_);
        }
    }

    /**
     * @brief Reports whether the redirection was established.
     *
     * @return bool - true when fd 1 is redirected and the original descriptor is held.
     */
    bool isValid() const { return sink_ != nullptr && savedStdout_ >= 0; }

    /**
     * @brief Ends the capture and returns everything written while it was in force.
     *
     * @return std::string - The captured bytes, empty when construction failed or nothing was
     *                       written. Safe to call more than once.
     */
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
    /** @brief Puts fd 1 back and releases the saved descriptor, at most once. */
    void restore() {
        if (savedStdout_ >= 0) {
            ::fflush(stdout);
            ::dup2(savedStdout_, STDOUT_FILENO);
            ::close(savedStdout_);
            savedStdout_ = -1;
        }
    }

    /** @brief Duplicate of the original fd 1, or -1 when no redirection is in force. */
    int savedStdout_;

    /** @brief The anonymous temporary receiving the captured output, or nullptr on failure. */
    FILE *sink_;
};

/**
 * @brief Local IHdmiCecDefault double reporting chosen metadata to halcompat::isCompatible.
 *
 * isCompatible reads getInterfaceHash() and getInterfaceVersion() by virtual dispatch, so no
 * Bn* base, registration or onTransact is needed. The stock default reports an empty hash.
 *
 * @warning Not derived from BnHdmiCec, whose remote onTransact would report compiled-in metadata.
 * @see frozenDoubleReportingVersion()
 * @see doubleReportingHash()
 */
class MetadataDouble : public cechal::IHdmiCecDefault {
public:
    /**
     * @brief Constructs a double that will report exactly the metadata given.
     *
     * @param [in] hash    - Interface hash to report.
     * @param [in] version - Interface version to report, in halcompat's positional encoding.
     */
    MetadataDouble(std::string hash, int32_t version)
        : hash_(std::move(hash))
        , version_(version)
    {
    }

    /**
     * @brief Answers the interface-hash query halcompat::isCompatible issues.
     *
     * @return std::string - The hash this double was constructed with, unmodified.
     */
    std::string getInterfaceHash() override { return hash_; }

    /**
     * @brief Answers the interface-version query halcompat::isCompatible issues.
     *
     * @return int32_t - The version this double was constructed with, unmodified.
     */
    int32_t getInterfaceVersion() override { return version_; }

private:
    /** @brief Hash reported to every caller of getInterfaceHash(). */
    std::string hash_;

    /** @brief Version reported to every caller of getInterfaceVersion(). */
    int32_t version_;
};

/**
 * @brief Builds a double reporting the generated frozen hash and a chosen version.
 *
 * @param [in] version - Interface version the double reports, in halcompat's encoding.
 * @return android::sp<cechal::IHdmiCec> - A local double, never null.
 */
::android::sp<cechal::IHdmiCec> frozenDoubleReportingVersion(int32_t version) {
    return ::android::sp<MetadataDouble>::make(std::string(kFrozenInterfaceHash), version);
}

/**
 * @brief Builds a double reporting a chosen hash and this client's own version.
 *
 * @param [in] hash - Interface hash the double reports. Must not be null.
 * @return android::sp<cechal::IHdmiCec> - A local double, never null.
 */
::android::sp<cechal::IHdmiCec> doubleReportingHash(const char *hash) {
    return ::android::sp<MetadataDouble>::make(std::string(hash), kClientInterfaceVersion);
}

/**
 * @brief The directory temporary nodes are created in: TMPDIR when valid, otherwise /tmp.
 *
 * TMPDIR is used only when it is absolute and names an existing, writable directory.
 *
 * @return std::string - An absolute directory path without a trailing separator, or "/" when
 *                       TMPDIR names the filesystem root.
 */
std::string temporaryDirectory() {
    const char *const configured = ::getenv("TMPDIR");

    if (configured != nullptr && configured[0] == '/') {
        struct stat info;

        if (::stat(configured, &info) == 0 && S_ISDIR(info.st_mode)
            && ::access(configured, W_OK | X_OK) == 0) {
            std::string directory(configured);

            while (directory.size() > 1u && directory[directory.size() - 1u] == '/') {
                directory.erase(directory.size() - 1u);
            }
            return directory;
        }
    }

    return std::string("/tmp");
}

/**
 * @brief A named, openable non-binder file, unlinked by its own destructor.
 *
 * Serves the preflight case that declines a path which opens but is a regular file, not a
 * character device; the predicate opens a path itself, so an anonymous tmpfile() cannot serve.
 */
class TemporaryNode {
public:
    /** @brief Creates the node; isValid() reports whether it succeeded. */
    TemporaryNode() {
        const std::string pattern = temporaryDirectory() + "/blitzy_cec_aidl_preflight_XXXXXX";

        std::vector<char> mutablePattern(pattern.begin(), pattern.end());
        mutablePattern.push_back('\0');

        const int fd = ::mkstemp(mutablePattern.data());

        if (fd < 0) {
            return;
        }
        ::close(fd);
        path_.assign(mutablePattern.data());
    }

    /** @brief Not copyable: two owners would each unlink the one node. */
    TemporaryNode(const TemporaryNode &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    TemporaryNode &operator=(const TemporaryNode &) = delete;

    /** @brief Unlinks the node on whichever exit path the case takes; never throws. */
    ~TemporaryNode() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    /**
     * @brief Reports whether the node was created.
     *
     * @return bool - true when a node exists at path() and will be unlinked by the destructor,
     *                false when mkstemp() failed, in which case path() is empty.
     */
    bool isValid() const { return !path_.empty(); }

    /**
     * @brief The node's path, for handing to the predicate under test.
     *
     * @return const std::string& - The absolute path, or an empty string when creation failed.
     */
    const std::string &path() const { return path_; }

private:
    /** @brief Path of the created node, empty when creation failed. */
    std::string path_;
};

/**
 * @brief Raises the CEC log level for a scope under custody of production's log-configuration
 *        file, then restores the file and the level and verifies both.
 *
 * The only seam is check_cec_log_status() and its fixed path, so the file is locked, validated,
 * replaced atomically and restored byte for byte, under the lock test_Util.cpp also takes.
 *
 * @warning Not reentrant or thread safe; construct one at a time, on the test thread.
 * @see restoreAndVerify()
 */
class ScopedCecLogLevel {
public:
    /**
     * @brief Takes custody of the configuration path and raises the level to @p levelName.
     *
     * @param [in] levelName - A name from production's level table, e.g. "DEBUG"; a null, empty
     *                         or over-long name is refused.
     * @post isRaised() reports whether the level moved; on refusal failureReason() says why and
     *       the path is as found, unless an unproved rollback kept custody for the destructor.
     */
    explicit ScopedCecLogLevel(const char *levelName)
        : raised_(false)
        , restorePending_(false)
        , restored_(false)
        , existed_(false)
        , lockFd_(-1)
        , savedLength_(0)
        , savedMode_(0600)
        , savedUid_(static_cast<uid_t>(-1))
        , savedGid_(static_cast<gid_t>(-1))
        , entryLevel_(kUnobservableLevel)
        , restoreErrno_(0)
    {
        saved_[0] = '\0';

        /* Formatted first, so a name that does not fit is refused before anything is locked. */
        char line[64];

        if (levelName == nullptr || levelName[0] == '\0') {
            failureReason_ = "no level name was given, so there is no level to raise to";
            return;
        }

        const int formatted = std::snprintf(line, sizeof(line), "%s\n", levelName);

        if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(line)) {
            failureReason_ = std::string("the level name [") + levelName
                + "] does not fit the " + std::to_string(sizeof(line))
                + "-byte configuration line this guard writes";
            return;
        }

        if (!buildCustodyPaths()) {
            failureReason_ = std::string("the lock and temporary paths derived from ")
                + kLogConfigPath + " do not fit their buffers, so custody cannot be taken";
            return;
        }

        /* Lock before capture, so another writer's content is never taken as the original. */
        if (!acquireLock()) {
            return;
        }

        if (!captureCurrentState()) {
            releaseLock();
            return;
        }

        /* Observe the level to restore inside the custody window, rather than derive it from the
         * file: check_cec_log_status() keeps the level when the file is absent or unrecognised. */
        entryLevel_ = probeEffectiveLevel();

        if (entryLevel_ == kUnobservableLevel) {
            failureReason_ = std::string("no CCEC_LOG level produced output, so the effective"
                                         " level this guard would have to restore could not be"
                                         " observed; refusing to raise it, because a raise"
                                         " whose restoration cannot be PROVED would leave the"
                                         " verbosity of every later case in this binary"
                                         " unknown");
            releaseLock();
            return;
        }

        registerAtExitOnce();
        sActive_ = this;

        if (atomicWrite(line, static_cast<size_t>(formatted)) != 0) {
            /* rename() alone publishes and is all-or-nothing, so the path is as found. */
            failureReason_ = std::string("could not replace ") + kLogConfigPath
                + " atomically: " + std::strerror(errno)
                + " (the original is untouched and the level was not raised)";
            sActive_ = nullptr;
            releaseLock();
            return;
        }

        if (!pathHolds(line, static_cast<size_t>(formatted))) {
            /* A writer outside the lock replaced the path: put the original back and record
             * whether that succeeded, since a failed rollback is the worse outcome. */
            restorePending_ = true;

            failureReason_ = std::string(kLogConfigPath)
                + " does not hold the level this guard wrote, so a writer outside the custody"
                  " lock replaced it; the level was not raised and ";

            const char *const publicationFailure = restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure == nullptr && verifyRestoredState(verificationDetail)) {
                /* Rolled back and verified by restoreAndVerify()'s checks: custody can go. */
                restorePending_ = false;
                failureReason_ += "the original has been restored and verified";
                sActive_ = nullptr;
                releaseLock();
                return;
            }

            /* Rollback unproved: keep the lock and the atexit backstop armed, so the destructor
             * retries and reports; restorePending_ stays true. */
            failureReason_ += (publicationFailure != nullptr)
                ? std::string("THE ORIGINAL COULD NOT BE RESTORED EITHER: ")
                      + publicationFailure + " (errno " + std::to_string(restoreErrno_) + ": "
                      + std::strerror(restoreErrno_) + ")"
                : std::string("THE ROLLBACK COULD NOT BE VERIFIED: ") + verificationDetail;
            failureReason_ += ". The custody lock is deliberately still held and the atexit"
                              " backstop still armed, so the destructor retries and reports;"
                              " until one of them succeeds this host may carry state this run"
                              " left behind";
            return;
        }

        check_cec_log_status();
        raised_ = true;
    }

    /**
     * @brief Restores the path and the process-wide level, verifies both, and releases custody.
     *
     * The route a case must take. Under the still-held lock it publishes the entry level and the
     * original, re-reads the bytes (or confirms absence) and re-probes the effective level.
     *
     * @param [out] detail - Empty on success; otherwise which check failed and what was observed.
     * @return bool - True when both are proved back or there was nothing to take back; false
     *         means the host may be altered and the calling case must fail.
     * @post Idempotent; on true isRaised() is false and the destructor does nothing.
     */
    bool restoreAndVerify(std::string &detail)
    {
        detail.clear();

        if (restored_ || (!raised_ && !restorePending_)) {
            /* Nothing published, or already proved back. restorePending_ is tested as well, so a
             * constructor that published but never raised still reaches the work below. */
            return true;
        }

        /* 1. Publication, and 2's precondition: the reader is driven with the bytes in place. */
        const char *const publicationFailure = restoreLevelAndPath();

        if (publicationFailure != nullptr) {
            detail = std::string("could not put ") + kLogConfigPath + " back: "
                + publicationFailure + " (errno " + std::to_string(restoreErrno_) + ": "
                + std::strerror(restoreErrno_) + "). The custody lock is deliberately still"
                  " held and the atexit backstop still armed, so the destructor will try again;"
                  " until one of them succeeds this host carries this run's raised log level";
            return false;
        }

        if (!verifyRestoredState(detail)) {
            return false;
        }

        /* Proved: clear the active pointer first, so the atexit backstop then finds nothing. */
        raised_ = false;
        restorePending_ = false;
        restored_ = true;
        if (sActive_ == this) {
            sActive_ = nullptr;
        }
        releaseLock();
        return true;
    }

private:
    /**
     * @brief Proves the path content (or absence) and the effective level are as captured.
     *
     * Shared by restoreAndVerify() and the constructor's rollback, so there is one verification.
     *
     * @param [out] detail - Empty when both hold; otherwise which failed and what was observed.
     * @return bool - True when both match what this guard captured when it took custody.
     * @pre The custody lock is held and restoreLevelAndPath() has just reported success.
     */
    bool verifyRestoredState(std::string &detail)
    {
        /* 1. Content where the file existed, absence where it did not. */
        if (existed_) {
            if (!pathHolds(saved_, savedLength_)) {
                detail = std::string(kLogConfigPath)
                    + " does not hold the bytes this guard captured, although the replace"
                      " reported success, so a writer outside the custody lock has replaced it"
                      " since. The host's own configuration is NOT back, and what stands at"
                      " that path is not this run's to correct";
                return false;
            }
        } else if (!pathIsAbsent()) {
            detail = std::string(kLogConfigPath)
                + " is still present, although this guard created it and has just removed it,"
                  " so either the removal did not take effect or another writer has recreated"
                  " it. A file the host did not have is left behind, and every later process"
                  " reading it will take this run's level for the host's";
            return false;
        }

        /* 2. The level itself - the check a restored file cannot stand in for. */
        const int observedLevel = probeEffectiveLevel();

        if (observedLevel != entryLevel_) {
            detail = std::string("the process-wide CEC log level is ")
                + std::to_string(observedLevel) + " but was " + std::to_string(entryLevel_)
                + " when this guard took custody, so " + kLogConfigPath
                + " was put back without the level following it. Every later case in this"
                  " binary would run at the wrong verbosity, and a case asserting on a"
                  " suppressed line would pass or fail for a reason that is not its own";
            return false;
        }

        return true;
    }

public:

    /**
     * @brief Best-effort backstop restoration for an exit that skipped restoreAndVerify().
     *
     * Restores through the same primitive and verifies, reporting failure on stdout because a
     * destructor cannot fail a test; cases must not rely on it.
     */
    ~ScopedCecLogLevel()
    {
        /* Both states mean something published is not proved back: raised_ is a skipped
         * restoreAndVerify(), restorePending_ the constructor's unproved rollback. */
        if (raised_ || restorePending_) {
            const char *const publicationFailure = restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure != nullptr) {
                reportBackstopFailure(publicationFailure);
            } else if (!verifyRestoredState(verificationDetail)) {
                /* Published but not proved: reported, as a destructor cannot fail a test. */
                reportBackstopFailure("the restoration could not be verified");
            }
            raised_ = false;
            restorePending_ = false;
        }
        if (sActive_ == this) {
            sActive_ = nullptr;
        }
        /* Released unconditionally and last, so no stale lock refuses later cases or runs. */
        releaseLock();
    }

    /**
     * @brief Whether the level was really moved to the name this guard was given.
     *
     * @return bool - true while a raise this guard performed is in force and proved, false when
     *                the raise was refused or has been proved taken back.
     */
    bool isRaised() const { return raised_; }

    /**
     * @brief Why isRaised() is false, in a form a failure message can stream.
     *
     * @return const std::string& - The specific refusal, or empty while isRaised() is true.
     */
    const std::string &failureReason() const { return failureReason_; }

    /**
     * @brief The effective level found at custody, which restoreAndVerify() compares against.
     *
     * @return int - A level in `0 .. LOG_MAX - 1`, or unobservableLevel() when it could not be
     *         observed, in which case the guard refused to raise.
     */
    int entryLevel() const { return entryLevel_; }

    /**
     * @brief Observes the process-wide effective level, so a case can check it directly.
     *
     * @return int - The observed level, or unobservableLevel() when nothing was observed.
     *
     * @warning Emits to stdout; call it outside any StdoutCapture scope.
     */
    static int observeEffectiveLevel() { return probeEffectiveLevel(); }

    /**
     * @brief The sentinel observeEffectiveLevel() returns when no level emitted at all.
     *
     * @return int - The unobservable-level sentinel, distinct from every real level.
     */
    static int unobservableLevel() { return kUnobservableLevel; }

    /** @brief Not copyable: custody of the shared path belongs to one instance at a time. */
    ScopedCecLogLevel(const ScopedCecLogLevel &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    ScopedCecLogLevel &operator=(const ScopedCecLogLevel &) = delete;

private:
    /** @brief The path production's check_cec_log_status() hardcodes, not this file's choice. */
    static constexpr const char *kLogConfigPath = "/tmp/cec_log_enabled";

    /** @brief Lock-path suffix, appended to kLogConfigPath exactly as test_Util.cpp appends it. */
    static constexpr const char *kLockSuffix = ".testlock";

    /**
     * @brief Production's level-name table, transcribed in order so the index is the level.
     *
     * Restoration must name the level it wants back, as check_cec_log_status() ignores a line
     * matching no entry.
     */
    static constexpr const char *kLevelNames[] = {
        "FATAL", "ERROR", "WARN", "EXP", "NOTICE", "INFO", "DEBUG", "TRACE"
    };

    static_assert(sizeof(kLevelNames) / sizeof(kLevelNames[0]) == LOG_MAX,
                  "the transcribed level table and production's LOG_MAX have diverged, so a"
                  " level this guard restores to could name nothing production recognises");

    /**
     * @brief The name production's reader matches for @p level, or nullptr if it has none.
     *
     * @param [in] level - A level value, not necessarily in range.
     * @return const char* - The table entry, or nullptr when @p level is outside
     *         `0 .. LOG_MAX - 1` and therefore cannot be asked for by name at all.
     */
    static const char *levelName(int level) {
        if (level < 0 || level >= LOG_MAX) {
            return nullptr;
        }
        return kLevelNames[level];
    }

    /** @brief probeEffectiveLevel()'s result when nothing emitted; outside 0 .. LOG_MAX - 1. */
    static constexpr int kUnobservableLevel = -1;

    /**
     * @brief Observes the effective level, for which production offers no getter.
     *
     * Emits a pid-tagged marker at each level from LOG_MAX - 1 down, each into its own
     * StdoutCapture; the highest level that emits is the configured one.
     *
     * @return int - The observed level in `0 .. LOG_MAX - 1`, or kUnobservableLevel when no
     *         level emitted or stdout could not be captured.
     */
    static int probeEffectiveLevel() {
        for (int level = LOG_MAX - 1; level >= 0; level--) {
            char marker[64];

            const int formatted = std::snprintf(marker, sizeof(marker),
                                                "AIDL_LEVEL_PROBE_%d_%ld", level,
                                                static_cast<long>(::getpid()));

            if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(marker)) {
                return kUnobservableLevel;
            }

            std::string emitted;
            {
                StdoutCapture capture;

                if (!capture.isValid()) {
                    return kUnobservableLevel;
                }

                CCEC_LOG(level, "%s", marker);
                emitted = capture.read();
            }

            if (emitted.find(marker) != std::string::npos) {
                return level;
            }
        }

        return kUnobservableLevel;
    }

    /** @brief Retry budget for the custody lock: 500 attempts at 10 ms, so 5 s in total. */
    static constexpr int kLockAttempts = 500;

    /** @brief Interval between lock attempts, in nanoseconds. */
    static constexpr long kLockRetryNs = 10L * 1000L * 1000L;

    /** @brief The lock and temporary paths, pre-formatted so restoration never allocates. */
    static inline char sLockPath_[288] = { '\0' };
    static inline char sTempPath_[288] = { '\0' };

    /** @brief The instance currently holding custody, for the atexit backstop. */
    static inline ScopedCecLogLevel *sActive_ = nullptr;

    /** @brief Whether the backstop has been registered; registration happens exactly once. */
    static inline bool sAtExitRegistered_ = false;

    /**
     * @brief Formats the lock and temporary paths from kLogConfigPath, once per process.
     *
     * @return bool - Whether both custody paths are usable
     * @retval true  - Both hold a complete, non-truncated string.
     * @retval false - Formatting overflowed one of the buffers. Both are left empty, so
     *                 no later step can act on a half-formatted path.
     */
    static bool buildCustodyPaths() {
        if (sLockPath_[0] != '\0' && sTempPath_[0] != '\0') {
            return true;
        }

        const int lockLength = std::snprintf(sLockPath_, sizeof(sLockPath_), "%s%s",
                                             kLogConfigPath, kLockSuffix);
        const int tempLength = std::snprintf(sTempPath_, sizeof(sTempPath_), "%s.aidltest.%ld.tmp",
                                             kLogConfigPath, static_cast<long>(::getpid()));

        if (lockLength <= 0 || static_cast<size_t>(lockLength) >= sizeof(sLockPath_)
            || tempLength <= 0 || static_cast<size_t>(tempLength) >= sizeof(sTempPath_)) {
            sLockPath_[0] = '\0';
            sTempPath_[0] = '\0';
            return false;
        }
        return true;
    }

    /**
     * @brief Takes the exclusive flock() on the companion lock path, retrying for a bounded time.
     *
     * @return bool - Whether custody was taken
     * @retval true  - The lock is held and `lockFd_` carries the locked descriptor.
     * @retval false - failureReason() carries why: either the open refused the path, or
     *                 the bounded retry expired against another holder.
     */
    bool acquireLock() {
        const int fd = ::open(sLockPath_, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);

        if (fd < 0) {
            failureReason_ = std::string("could not open the custody lock ") + sLockPath_ + ": "
                + std::strerror(errno)
                + " (a symlink at that path is refused, which is what O_NOFOLLOW reports as"
                  " ELOOP)";
            return false;
        }

        for (int attempt = 0; attempt < kLockAttempts; attempt++) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                lockFd_ = fd;
                return true;
            }
            if (errno != EWOULDBLOCK) {
                failureReason_ = std::string("could not lock ") + sLockPath_ + ": "
                    + std::strerror(errno);
                ::close(fd);
                return false;
            }

            struct timespec pause;
            pause.tv_sec = 0;
            pause.tv_nsec = kLockRetryNs;
            while (::nanosleep(&pause, &pause) != 0 && errno == EINTR) {
                /* finish the remaining interval nanosleep wrote back */
            }
        }

        failureReason_ = std::string("another holder has kept ") + sLockPath_ + " for over "
            + std::to_string((kLockAttempts * (kLockRetryNs / 1000000L)) / 1000L)
            + " s, so " + kLogConfigPath + " is not this case's to modify. The holder is a"
              " second run_L1Tests on this host, this binary's own Util suite, or another guard"
              " still alive in this process - all three contend for that production-fixed path,"
              " and the first is fixed by running the copies one at a time";
        ::close(fd);
        return false;
    }

    /** @brief Releases the lock, if held; the lock file itself is never unlinked. */
    void releaseLock() {
        if (lockFd_ < 0) {
            return;
        }
        /* Never unlinked: a waiter may hold it, and a new inode would allow a second holder. */
        (void)::flock(lockFd_, LOCK_UN);
        ::close(lockFd_);
        lockFd_ = -1;
    }

    /**
     * @brief Classifies the configuration path and captures it, without following a link.
     *
     * Refuses anything but a regular file, and a file the effective user does not own unless
     * that user is root; confirms the opened inode is the one lstat() classified, and refuses a
     * file larger than the capture buffer.
     *
     * @return bool - Whether the path was classified and captured
     * @retval true  - Either the original bytes, mode and ownership are captured, or the
     *                 path is legitimately absent and `existed_` records that.
     * @retval false - The path was refused; failureReason() carries which check refused it.
     */
    bool captureCurrentState() {
        struct stat linkStatus;

        if (::lstat(kLogConfigPath, &linkStatus) != 0) {
            if (errno == ENOENT) {
                existed_ = false;
                return true;
            }
            failureReason_ = std::string("could not inspect ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }

        if (!S_ISREG(linkStatus.st_mode)) {
            failureReason_ = std::string(kLogConfigPath)
                + " exists and is NOT a regular file (mode "
                + std::to_string(static_cast<unsigned long>(linkStatus.st_mode))
                + "), which at a fixed name in a world-writable directory is very likely a"
                  " planted link; refusing to modify it, because replacing or writing through"
                  " it would alter whatever it refers to";
            return false;
        }

        if (linkStatus.st_uid != ::geteuid() && ::geteuid() != 0) {
            failureReason_ = std::string(kLogConfigPath) + " is owned by uid "
                + std::to_string(static_cast<unsigned long>(linkStatus.st_uid))
                + " and this process is not that user; refusing to modify it";
            return false;
        }

        const int fd = ::open(kLogConfigPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

        if (fd < 0) {
            failureReason_ = std::string("could not read ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }

        struct stat openStatus;

        if (::fstat(fd, &openStatus) != 0 || openStatus.st_dev != linkStatus.st_dev
            || openStatus.st_ino != linkStatus.st_ino) {
            ::close(fd);
            failureReason_ = std::string(kLogConfigPath)
                + " was replaced between being classified and being opened, so what would be"
                  " captured is not what was validated; refusing to modify it";
            return false;
        }

        ssize_t got = 0;
        size_t total = 0;

        while (total < sizeof(saved_)
               && (got = ::read(fd, saved_ + total, sizeof(saved_) - total)) > 0) {
            total += static_cast<size_t>(got);
        }

        const bool readFailed = (got < 0);
        char overflow = '\0';
        const bool tooLong = (total == sizeof(saved_)) && (::read(fd, &overflow, 1) > 0);

        ::close(fd);

        if (readFailed) {
            failureReason_ = std::string("could not read ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }
        if (tooLong) {
            failureReason_ = std::string(kLogConfigPath) + " is larger than "
                + std::to_string(sizeof(saved_))
                + " bytes, so it could not be restored byte for byte; refusing to modify it";
            return false;
        }

        savedLength_ = total;
        savedMode_ = linkStatus.st_mode & 07777;
        savedUid_ = linkStatus.st_uid;
        savedGid_ = linkStatus.st_gid;
        existed_ = true;
        return true;
    }

    /**
     * @brief Publishes @p length bytes at the configuration path, atomically and link-safely.
     *
     * Writes an `O_EXCL|O_NOFOLLOW` temporary in the same directory and rename()s it over the
     * path, without allocating.
     *
     * @param [in] data   - First byte of the content; exactly @p length bytes are written.
     * @param [in] length - Number of bytes to publish.
     * @return int - Publication result
     * @retval 0  - The rename has completed and the path holds the new content.
     * @retval -1 - `errno` is set by the failing syscall, the temporary is unlinked, and
     *              the destination is left exactly as it was.
     */
    int atomicWrite(const char *data, size_t length) {
        if (sTempPath_[0] == '\0') {
            return -1;
        }

        (void)::unlink(sTempPath_);

        const int fd = ::open(sTempPath_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              0600);

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
                ::close(fd);
                (void)::unlink(sTempPath_);
                return -1;
            }
            written += static_cast<size_t>(chunk);
        }

        /* Hand back the found mode and owner. fchmod must succeed on this process's own O_EXCL
         * file; fchown is best effort, as EPERM is normal for an unprivileged process. */
        if (::fchmod(fd, savedMode_) != 0) {
            ::close(fd);
            (void)::unlink(sTempPath_);
            return -1;
        }
        if (savedUid_ != static_cast<uid_t>(-1)) {
            /* Taken into a named local, because a (void) cast on the call does not satisfy
             * glibc's warn_unused_result under -Wall; EPERM has no unprivileged remedy. */
            const int ownershipResult = ::fchown(fd, savedUid_, savedGid_);
            (void)ownershipResult;
        }
        if (::close(fd) != 0) {
            (void)::unlink(sTempPath_);
            return -1;
        }
        if (::rename(sTempPath_, kLogConfigPath) != 0) {
            (void)::unlink(sTempPath_);
            return -1;
        }
        return 0;
    }

    /**
     * @brief Whether the path currently holds exactly @p length bytes equal to @p data.
     *
     * Read `O_NOFOLLOW`, and one byte past @p length, into a buffer one byte larger than the
     * capture buffer, so both publications this guard performs can be verified.
     *
     * @param [in] data   - Expected content, compared byte for byte.
     * @param [in] length - Expected length in bytes.
     * @return bool - Whether the path holds exactly that content
     * @retval true  - The `O_NOFOLLOW` open and every read succeed, reaching end of file after
     *                 exactly @p length bytes, all equal to @p data.
     * @retval false - Any other content, a longer file, a link at the name, an unreadable path, or
     *                 a @p length larger than the capture buffer.
     */
    bool pathHolds(const char *data, size_t length) const {
        char actual[sizeof(saved_) + 1];

        if (length >= sizeof(actual)) {
            return false;
        }

        const int fd = ::open(kLogConfigPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

        if (fd < 0) {
            return false;
        }

        ssize_t got = 0;
        size_t total = 0;

        while (total < sizeof(actual)
               && (got = ::read(fd, actual + total, sizeof(actual) - total)) > 0) {
            total += static_cast<size_t>(got);
        }
        ::close(fd);

        return (got >= 0) && (total == length) && (std::memcmp(actual, data, length) == 0);
    }

    /**
     * @brief The one restoration primitive: the entry level back by name, then the file.
     *
     * The level goes first because the original need not name any level, and the reader is not
     * driven after the file is back. Allocation-free, since the atexit backstop calls it.
     *
     * @return const char* - nullptr on success, else a static description of the failed step,
     *         with restoreErrno_ holding the errno that step saw.
     */
    const char *restoreLevelAndPath() {
        restoreErrno_ = 0;

        /* 1. The level, by name, through production's own reader. */
        const char *const name = levelName(entryLevel_);

        if (name == nullptr) {
            return "the level observed when custody was taken has no name in production's own"
                   " table, so it cannot be asked for through check_cec_log_status() - the"
                   " transcribed table and LOG_MAX have diverged";
        }

        char line[64];
        const int formatted = std::snprintf(line, sizeof(line), "%s\n", name);

        if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(line)) {
            return "the entry level's name does not fit the configuration line this guard"
                   " writes, so the level could not be asked for";
        }

        if (atomicWrite(line, static_cast<size_t>(formatted)) != 0) {
            restoreErrno_ = errno;
            return "the entry level could not be published, so the raised level is still in"
                   " force process wide";
        }

        check_cec_log_status();

        /* 2. The file, without driving the reader again. */
        if (existed_) {
            if (atomicWrite(saved_, savedLength_) != 0) {
                restoreErrno_ = errno;
                return "the level went back but the captured original could not be written"
                       " back atomically, so the path still holds a line this guard wrote";
            }
            return nullptr;
        }

        if (::unlink(kLogConfigPath) != 0 && errno != ENOENT) {
            restoreErrno_ = errno;
            return "the level went back but the configuration file this guard created could"
                   " not be removed again, so a file the host did not have is left behind";
        }
        return nullptr;
    }

    /**
     * @brief Whether the configuration path is absent, classified with lstat() so a link counts.
     *
     * @return bool - Whether the path is empty
     * @retval true  - Nothing at all stands at the path.
     * @retval false - Something does, including a link whose target is missing.
     */
    static bool pathIsAbsent() {
        struct stat linkStatus;

        return ::lstat(kLogConfigPath, &linkStatus) != 0 && errno == ENOENT;
    }

    /**
     * @brief Reports a failed backstop restoration on stdout, without allocating.
     *
     * @param [in] failure - The static step description restoreLevelAndPath() returned.
     */
    void reportBackstopFailure(const char *failure) const {
        std::fflush(stdout);
        std::fprintf(stdout,
                     "\r\n[ScopedCecLogLevel] BACKSTOP RESTORATION FAILED for %s: %s (errno"
                     " %d: %s). The process-wide CEC log level was not proved to have returned"
                     " to the value this run found, and this line is the only report there is -"
                     " a destructor cannot fail a test. Delete or repair that path before"
                     " reading any later log in this run as the host's own.\r\n",
                     kLogConfigPath, failure, restoreErrno_, std::strerror(restoreErrno_));
        std::fflush(stdout);
    }

    /** @brief Registers the backstop exactly once per process. */
    void registerAtExitOnce() {
        if (sAtExitRegistered_) {
            return;
        }
        (void)std::atexit(&ScopedCecLogLevel::atExitRestore);
        sAtExitRegistered_ = true;
    }

    /**
     * @brief Last-chance restoration on ordinary process exit, and the second backstop.
     *
     * Acts only while an instance still holds custody and reports failures on stdout. No signal
     * handler is installed, since a signal mid-rename could race two writers over one temporary.
     */
    static void atExitRestore() {
        ScopedCecLogLevel *const active = sActive_;

        if (active == nullptr) {
            return;
        }
        /* Both states, as in the destructor: a skipped restoration or the constructor's unproved
         * rollback, which kept custody precisely so this retry could happen. */
        if (active->raised_ || active->restorePending_) {
            const char *const publicationFailure = active->restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure != nullptr) {
                active->reportBackstopFailure(publicationFailure);
            } else if (!active->verifyRestoredState(verificationDetail)) {
                active->reportBackstopFailure("the restoration could not be verified");
            }
            active->raised_ = false;
            active->restorePending_ = false;
        }
        sActive_ = nullptr;
        active->releaseLock();
    }

    bool raised_;

    /**
     * @brief Whether this guard has published something to the path that is not proved back.
     *
     * Independent of raised_, which a failed constructor read-back leaves false.
     */
    bool restorePending_;

    /** @brief Whether restoreAndVerify() has already restored, which makes it idempotent. */
    bool restored_;

    bool existed_;
    int lockFd_;
    size_t savedLength_;
    mode_t savedMode_;
    uid_t savedUid_;
    gid_t savedGid_;

    /** @brief The level observed at construction, to restore; kUnobservableLevel on refusal. */
    int entryLevel_;

    /** @brief errno captured at the failing restoration step, or 0 when none failed. */
    int restoreErrno_;

    /** @brief Why custody was refused or restoration failed; empty while nothing has failed. */
    std::string failureReason_;

    /** @brief The file's original bytes; a fixed buffer, since restoration must not allocate. */
    char saved_[4096];
};

/**
 * @brief Renders bytes exactly as DriverAidlImpl::EventListener::onMessageSent logs them.
 *
 * Lowercase hex, two digits per byte, no separator. Unlike production it never truncates,
 * which the short frames used here make irrelevant.
 *
 * @param [in] bytes Message bytes to render, in order.
 *
 * @return std::string - Two lowercase hex digits per byte, in order; empty for empty input.
 */
std::string lowercaseHexOf(const std::vector<uint8_t> &bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string rendered;

    rendered.reserve(bytes.size() * 2);
    for (size_t index = 0; index < bytes.size(); index++) {
        rendered.push_back(digits[(bytes[index] >> 4) & 0x0F]);
        rendered.push_back(digits[bytes[index] & 0x0F]);
    }

    return rendered;
}

// Synthetic binder probe: plain-function substitutes for the six kernel-facing operations of
// isBinderPreflightOk() and its pre-lookup re-verification, so their arms need no binder driver.

/**
 * @brief The descriptor the synthetic probe returns for a successful open.
 *
 * The synthetic probe only returns and records this number and performs no real descriptor
 * operation on it, so a real descriptor that shares the number is never touched.
 */
constexpr int kSyntheticBinderFd = 4242;

/**
 * @brief The descriptor the synthetic probe returns for the second open in one call.
 *
 * isServiceAvailable() opens the node twice, so a distinct value lets a case assert which
 * descriptor was pinged and which was released.
 */
constexpr int kSyntheticSecondBinderFd = 4343;

/**
 * @brief The identity of a well-formed binder node: a root-owned character device, mode 0600.
 *
 * Device, inode and rdev are arbitrary but fixed. Mode and owner are what the preflight
 * validates, so they are built from POSIX macros independently of the production constants.
 */
const DriverAidlImpl::BinderNodeIdentity kValidatedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    0u,
};

/**
 * @brief The validated node's identity with only @c inode changed: a substituted node.
 *
 * Changing one field means the case fails unless the inode is genuinely compared.
 */
const DriverAidlImpl::BinderNodeIdentity kSubstitutedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000b2b2ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    0u,
};

/**
 * @brief The validated node re-permissioned: device, inode and rdev unchanged, mode now 0666.
 *
 * Models a `chmod` after validation, which a `(device, inode, rdev)` comparison reports as
 * unchanged.
 */
const DriverAidlImpl::BinderNodeIdentity kRePermissionedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0666),
    0u,
};

/**
 * @brief The validated node re-owned: every field unchanged except the owner, now uid 1000.
 *
 * Models a `chown` after validation, which only an owner comparison can decline.
 */
const DriverAidlImpl::BinderNodeIdentity kReOwnedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    1000u,
};

/**
 * @brief A well-formed root-owned node published with mode 0666, as conformant platforms do.
 *
 * The preflight must accept it; declining it would refuse the AIDL path on every such platform.
 */
const DriverAidlImpl::BinderNodeIdentity kWorldWritableNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0666),
    0u,
};

/**
 * @brief Configuration and observations for the synthetic probe.
 *
 * The @c answer* members choose where the predicate stops; the @c *Calls and @c last* members
 * are what cases assert. The @c *Second members answer differently from the second call on,
 * because isServiceAvailable() pings and identifies twice; their flags default to false.
 */
struct SyntheticProbeState {
    /** @brief What openNode() returns: kSyntheticBinderFd, or negative to fail the open. */
    int answerOpen = kSyntheticBinderFd;
    /** @brief What readProtocolVersion() returns: 0 for success, -1 to fail the ioctl. */
    int answerProtocolRead = 0;
    /** @brief The protocol version readProtocolVersion() reports on success. */
    unsigned int answerProtocolVersion = 0u;
    /** @brief What pingContextManager() reports on its FIRST call. */
    bool answerPing = false;
    /** @brief Whether pingContextManager() answers differently from its second call onwards. */
    bool hasSecondPingAnswer = false;
    /** @brief What pingContextManager() reports from its second call onwards, when enabled. */
    bool answerPingSecond = false;
    /** @brief What identifyDescriptor() returns: 0 for success, -1 to fail the fstat. */
    int answerIdentifyDescriptor = 0;
    /** @brief The identity identifyDescriptor() reports on its FIRST successful call. */
    DriverAidlImpl::BinderNodeIdentity answerDescriptorIdentity = kValidatedNodeIdentity;
    /** @brief Whether identifyDescriptor() reports a different node from its second call onwards. */
    bool hasSecondDescriptorIdentity = false;
    /** @brief The identity identifyDescriptor() reports from its second call onwards, when enabled. */
    DriverAidlImpl::BinderNodeIdentity answerDescriptorIdentitySecond = kSubstitutedNodeIdentity;
    /**
     * @brief Whether identifyDescriptor() returns a different result from its second call onwards.
     *
     * Unlike hasSecondDescriptorIdentity, this makes the reopened descriptor fail to identify,
     * which is a separate re-verification arm.
     */
    bool hasSecondDescriptorIdentifyResult = false;
    /** @brief What identifyDescriptor() returns from its second call onwards, when enabled. */
    int answerIdentifyDescriptorSecond = 0;
    /** @brief What identifyPath() returns: 0 for success, -1 to fail the stat. */
    int answerIdentifyPath = 0;
    /** @brief The identity identifyPath() reports on success - the node the NAME resolves to. */
    DriverAidlImpl::BinderNodeIdentity answerPathIdentity = kValidatedNodeIdentity;

    /** @brief How many times openNode() was called. */
    int openCalls = 0;
    /** @brief How many times identifyDescriptor() was called. */
    int identifyDescriptorCalls = 0;
    /** @brief How many times identifyPath() was called. */
    int identifyPathCalls = 0;
    /** @brief How many times readProtocolVersion() was called. */
    int protocolReadCalls = 0;
    /** @brief How many times pingContextManager() was called. */
    int pingCalls = 0;
    /** @brief How many times closeNode() was called. */
    int closeCalls = 0;

    /** @brief The path openNode() was last given. */
    std::string lastOpenedPath;
    /** @brief The flags openNode() was last given. */
    int lastOpenFlags = 0;
    /** @brief The descriptor readProtocolVersion() was last given. */
    int lastProtocolFd = -1;
    /** @brief The descriptor pingContextManager() was last given. */
    int lastPingFd = -1;
    /** @brief The deadline pingContextManager() was last given, in milliseconds. */
    unsigned int lastPingTimeoutMs = 0u;
    /** @brief The descriptor closeNode() was last given. */
    int lastClosedFd = -1;
    /** @brief The descriptor identifyDescriptor() was last given. */
    int lastIdentifiedFd = -1;
    /** @brief The path identifyPath() was last given. */
    std::string lastIdentifiedPath;
    /** @brief Every descriptor closeNode() was given, in order, so pairing can be asserted. */
    std::vector<int> closedFds;
};

/** @brief The synthetic probe's configuration and observations; reset by resetSyntheticProbe(). */
SyntheticProbeState g_syntheticProbe;

/**
 * @brief Records the call and answers with the configured descriptor.
 *
 * A second successful open answers kSyntheticSecondBinderFd; a failing configuration fails both.
 *
 * @param [in] path  Driver node path requested; recorded, a null pointer as an empty string.
 * @param [in] flags Open flags requested; recorded, never acted on.
 *
 * @return int - SyntheticProbeState::answerOpen, or kSyntheticSecondBinderFd from the second call.
 */
int syntheticOpenNode(const char *path, int flags) {
    g_syntheticProbe.openCalls++;
    g_syntheticProbe.lastOpenedPath = (path != nullptr) ? std::string(path) : std::string();
    g_syntheticProbe.lastOpenFlags = flags;

    if (g_syntheticProbe.answerOpen < 0) {
        return g_syntheticProbe.answerOpen;
    }

    return (g_syntheticProbe.openCalls >= 2) ? kSyntheticSecondBinderFd
                                             : g_syntheticProbe.answerOpen;
}

/** @brief Records the call and reports the configured node identity, or fails the fstat. */
int syntheticIdentifyDescriptor(int fd, DriverAidlImpl::BinderNodeIdentity *out) {
    g_syntheticProbe.identifyDescriptorCalls++;
    g_syntheticProbe.lastIdentifiedFd = fd;

    if (g_syntheticProbe.answerIdentifyDescriptor != 0) {
        return g_syntheticProbe.answerIdentifyDescriptor;
    }

    // The second-call failure arm precedes the identity arm: a failed call reports no identity.
    if (g_syntheticProbe.hasSecondDescriptorIdentifyResult &&
        (g_syntheticProbe.identifyDescriptorCalls >= 2) &&
        (g_syntheticProbe.answerIdentifyDescriptorSecond != 0)) {
        return g_syntheticProbe.answerIdentifyDescriptorSecond;
    }

    if (out != nullptr) {
        const bool useSecond = g_syntheticProbe.hasSecondDescriptorIdentity &&
                               (g_syntheticProbe.identifyDescriptorCalls >= 2);

        *out = useSecond ? g_syntheticProbe.answerDescriptorIdentitySecond
                         : g_syntheticProbe.answerDescriptorIdentity;
    }

    return 0;
}

/** @brief Records the call and reports what the NAME resolves to, or fails the stat. */
int syntheticIdentifyPath(const char *path, DriverAidlImpl::BinderNodeIdentity *out) {
    g_syntheticProbe.identifyPathCalls++;
    g_syntheticProbe.lastIdentifiedPath = (path != nullptr) ? std::string(path) : std::string();

    if (g_syntheticProbe.answerIdentifyPath != 0) {
        return g_syntheticProbe.answerIdentifyPath;
    }

    if (out != nullptr) {
        *out = g_syntheticProbe.answerPathIdentity;
    }

    return 0;
}

/**
 * @brief Records the call and reports the configured protocol version, or fails the read.
 *
 * @param [in]  fd      Descriptor read through; recorded only.
 * @param [out] version Receives answerProtocolVersion; untouched on a failing read or when null.
 *
 * @return int - Read result
 * @retval 0  - The version was reported.
 * @retval -1 - The ioctl is configured to fail.
 */
int syntheticReadProtocolVersion(int fd, unsigned int *version) {
    g_syntheticProbe.protocolReadCalls++;
    g_syntheticProbe.lastProtocolFd = fd;

    if (g_syntheticProbe.answerProtocolRead == 0 && version != nullptr) {
        *version = g_syntheticProbe.answerProtocolVersion;
    }
    return g_syntheticProbe.answerProtocolRead;
}

/**
 * @brief Records the call and its deadline, then answers.
 *
 * With @c hasSecondPingAnswer set, calls from the second onwards answer @c answerPingSecond:
 * a context manager that answered the preflight and stopped answering before the lookup.
 *
 * @param [in] fd        Descriptor pinged through; recorded only.
 * @param [in] timeoutMs Deadline applied, in milliseconds; recorded so the bound is assertable.
 *
 * @return bool - The answer configured for this call.
 */
bool syntheticPingContextManager(int fd, unsigned int timeoutMs) {
    g_syntheticProbe.pingCalls++;
    g_syntheticProbe.lastPingFd = fd;
    g_syntheticProbe.lastPingTimeoutMs = timeoutMs;

    if (g_syntheticProbe.hasSecondPingAnswer && (g_syntheticProbe.pingCalls >= 2)) {
        return g_syntheticProbe.answerPingSecond;
    }

    return g_syntheticProbe.answerPing;
}

/**
 * @brief Records the released descriptor, in call order, and always succeeds.
 *
 * @param [in] fd Descriptor being released; recorded so a case can pair it with its open.
 *
 * @return int - Always 0; the predicate has no decision point for a failing close.
 */
int syntheticCloseNode(int fd) {
    g_syntheticProbe.closeCalls++;
    g_syntheticProbe.lastClosedFd = fd;
    g_syntheticProbe.closedFds.push_back(fd);
    return 0;
}

/**
 * @brief Clears every counter and installs one answer per decision point.
 *
 * Identity answers reset to a well-formed node, so each identity case overrides only the field
 * it is about.
 *
 * @param [in] answerOpen            - openNode()'s answer: kSyntheticBinderFd, or negative.
 * @param [in] answerProtocolRead    - readProtocolVersion()'s answer: 0 or -1.
 * @param [in] answerProtocolVersion - Protocol version reported on a successful read.
 * @param [in] answerPing            - pingContextManager()'s answer.
 */
void resetSyntheticProbe(int answerOpen,
                         int answerProtocolRead,
                         unsigned int answerProtocolVersion,
                         bool answerPing) {
    g_syntheticProbe = SyntheticProbeState();
    g_syntheticProbe.answerOpen = answerOpen;
    g_syntheticProbe.answerProtocolRead = answerProtocolRead;
    g_syntheticProbe.answerProtocolVersion = answerProtocolVersion;
    g_syntheticProbe.answerPing = answerPing;
}

/**
 * @brief The probe the cases hand to isBinderPreflightOk(), wired to the six functions above.
 *
 * @return DriverAidlImpl::BinderPreflightProbe - The six-function aggregate, by value.
 *
 * @warning Fill every member in the struct's order: the preflight's stages and the pre-lookup
 *          re-verification each use different members, calling one only once its stage is
 *          reached, so a case reaching an omitted member's stage calls a null function pointer.
 */
DriverAidlImpl::BinderPreflightProbe syntheticProbe() {
    DriverAidlImpl::BinderPreflightProbe probe = {
        syntheticOpenNode,
        syntheticIdentifyDescriptor,
        syntheticIdentifyPath,
        syntheticReadProtocolVersion,
        syntheticPingContextManager,
        syntheticCloseNode,
    };
    return probe;
}

/**
 * @brief A well-formed directed frame: initiator 4, destination 0, then an opcode.
 *
 * @param [in] opcode Opcode byte to follow the header.
 *
 * @return CECFrame - A two-byte frame whose destination nibble is 0, so the status
 *                    translation treats it as directed.
 */
CECFrame directedFrame(uint8_t opcode = GIVE_DEVICE_POWER_STATUS) {
    CECFrame frame;
    frame.append(static_cast<uint8_t>(0x40));
    frame.append(opcode);
    return frame;
}

/**
 * @brief A well-formed broadcast frame: destination nibble 0x0F, then an opcode.
 *
 * @param [in] opcode Opcode byte to follow the header.
 *
 * @return CECFrame - A two-byte frame whose destination nibble is 0x0F, so the status
 *                    translation treats it as a broadcast.
 */
CECFrame broadcastFrame(uint8_t opcode = REPORT_PHYSICAL_ADDRESS) {
    CECFrame frame;
    frame.append(static_cast<uint8_t>(0x4F));
    frame.append(opcode);
    return frame;
}

/**
 * @brief A directed frame padded with zero operands to exactly @p length bytes.
 *
 * Used for the frame-size boundary; the padding is decoded by nothing on either path.
 *
 * @param [in] length Total frame length in bytes; two or less yields directedFrame() unpadded.
 *
 * @return CECFrame - A well-formed directed frame of @p length bytes, or two when shorter.
 */
CECFrame frameOfLength(size_t length) {
    CECFrame frame = directedFrame();
    while (frame.length() < length) {
        frame.append(static_cast<uint8_t>(0x00));
    }
    return frame;
}

// Sink caller drift guard: a brace-, comment- and literal-aware scanner reads the real Sink
// plugin source, so the modelled addLogicalAddress call paths cannot silently go stale.

/** @brief One try block found by the scanner: its span, and the handlers that follow it. */
struct SourceTryBlock {
    /** @brief Offset of the try body's opening brace. */
    size_t bodyOpen = 0;
    /** @brief Offset of the try body's matching closing brace. */
    size_t bodyClose = 0;
    /** @brief The text inside each following catch clause's parentheses, in source order. */
    std::vector<std::string> handlers;
};

/** @brief One call site found by the scanner. */
struct SourceCallSite {
    /** @brief Offset of the first character of the matched call text. */
    size_t offset = 0;
    /** @brief One-based line number of the call, for diagnostics. */
    size_t line = 0;
    /** @brief Whether any enclosing block is a try block. */
    bool insideAnyTry = false;
    /** @brief Index into the model's try blocks for the innermost enclosing try, or -1. */
    int innermostTry = -1;
    /** @brief The code immediately before the call, whitespace collapsed, for receiver checks. */
    std::string precedingText;
};

/**
 * @brief A one-pass brace-, comment- and literal-aware model of a C++ source file.
 *
 * Records every try block with its handlers and every occurrence of one call text with its try
 * nesting; it interprets no types, templates or preprocessor conditionals.
 */
class CppSourceModel {
public:
    /**
     * @brief Walks @p text once, recording try blocks and occurrences of @p callNeedle
     *
     * @param [in] text       - Whole file contents.
     * @param [in] callNeedle - Call text to locate, matched only at an identifier boundary.
     *
     * @return bool - Whether the walk completed with a balanced block stack
     * @retval true  - The file parsed far enough for the model to be trusted.
     * @retval false - Braces or a catch parameter list did not balance; do not rely on the model.
     */
    bool scan(const std::string &text, const std::string &callNeedle) {
        tryBlocks_.clear();
        callSites_.clear();

        /** @brief One entry on the brace stack: whether it is a try body, and which one. */
        struct OpenBlock {
            /** @brief Whether this block is the body of a try. */
            bool isTry;
            /** @brief Index into tryBlocks_ when isTry, and -1 otherwise. */
            int tryIndex;
        };

        std::vector<OpenBlock> stack;
        std::vector<int> lastClosedTryAtDepth;
        size_t line = 1;
        const size_t length = text.size();

        for (size_t i = 0; i < length; ) {
            const char c = text[i];

            if (c == '\n') {
                line++;
                i++;
                continue;
            }

            // Line comment: everything to the newline is invisible to the model.
            if (c == '/' && (i + 1) < length && text[i + 1] == '/') {
                while (i < length && text[i] != '\n') {
                    i++;
                }
                continue;
            }

            // Block comment, counting the newlines inside it so line numbers stay right.
            if (c == '/' && (i + 1) < length && text[i + 1] == '*') {
                i += 2;
                while ((i + 1) < length && !(text[i] == '*' && text[i + 1] == '/')) {
                    if (text[i] == '\n') {
                        line++;
                    }
                    i++;
                }
                i = (i + 1 < length) ? i + 2 : length;
                continue;
            }

            // String and character literals, with backslash escapes honoured, so a brace or
            // a keyword inside one cannot reach the block stack.
            if (c == '"' || c == '\'') {
                const char quote = c;
                i++;
                while (i < length && text[i] != quote) {
                    if (text[i] == '\\' && (i + 1) < length) {
                        i++;
                    }
                    else if (text[i] == '\n') {
                        line++;
                    }
                    i++;
                }
                i = (i < length) ? i + 1 : length;
                continue;
            }

            if (c == '{') {
                const bool isTry = isPrecededByKeyword(text, i, "try");
                OpenBlock block = { isTry, -1 };

                if (isTry) {
                    SourceTryBlock record;
                    record.bodyOpen = i;
                    tryBlocks_.push_back(record);
                    block.tryIndex = static_cast<int>(tryBlocks_.size()) - 1;
                }
                stack.push_back(block);
                i++;
                continue;
            }

            if (c == '}') {
                if (stack.empty()) {
                    return false;
                }

                const OpenBlock block = stack.back();
                stack.pop_back();

                if (block.isTry) {
                    tryBlocks_[static_cast<size_t>(block.tryIndex)].bodyClose = i;

                    // Remember it at the depth it lived at, so the catch clauses that follow -
                    // which are scanned at that same depth - can be attached to it.
                    if (lastClosedTryAtDepth.size() <= stack.size()) {
                        lastClosedTryAtDepth.resize(stack.size() + 1, -1);
                    }
                    lastClosedTryAtDepth[stack.size()] = block.tryIndex;
                }
                i++;
                continue;
            }

            // A catch clause attaches to the try most recently closed at this depth; nothing else
            // can legally precede one.
            if (c == 'c' && matchesIdentifier(text, i, "catch")) {
                size_t cursor = skipBlanks(text, i + 5, line);

                if (cursor < length && text[cursor] == '(') {
                    const size_t parameterEnd = matchParenthesis(text, cursor);

                    if (parameterEnd == std::string::npos) {
                        return false;
                    }

                    if (stack.size() < lastClosedTryAtDepth.size()
                        && lastClosedTryAtDepth[stack.size()] >= 0) {
                        const size_t owner =
                            static_cast<size_t>(lastClosedTryAtDepth[stack.size()]);
                        tryBlocks_[owner].handlers.push_back(
                            collapseBlanks(text.substr(cursor + 1, parameterEnd - cursor - 1)));
                    }
                    i = parameterEnd + 1;
                    continue;
                }
            }

            if (c == callNeedle[0] && matchesIdentifier(text, i, callNeedle)) {
                SourceCallSite site;
                site.offset = i;
                site.line = line;

                for (size_t depth = 0; depth < stack.size(); depth++) {
                    if (stack[depth].isTry) {
                        site.insideAnyTry = true;
                        site.innermostTry = stack[depth].tryIndex;
                    }
                }

                const size_t contextStart = (i > 96u) ? (i - 96u) : 0u;
                site.precedingText = collapseBlanks(text.substr(contextStart, i - contextStart));
                callSites_.push_back(site);
                i += callNeedle.size();
                continue;
            }

            i++;
        }

        return stack.empty();
    }

    /**
     * @brief The call sites found, in source order.
     *
     * @return const std::vector<SourceCallSite>& - The call sites the most recent scan()
     *                                              recorded, in source order, to be relied on
     *                                              only when that scan() returned true. Empty
     *                                              before the first scan.
     */
    const std::vector<SourceCallSite> &callSites() const { return callSites_; }

    /**
     * @brief The try blocks found, in the order their bodies opened.
     *
     * @return const std::vector<SourceTryBlock>& - The try blocks the most recent scan()
     *                                              recorded, ordered by where their bodies
     *                                              opened, to be relied on only when that
     *                                              scan() returned true. Empty before the
     *                                              first scan.
     */
    const std::vector<SourceTryBlock> &tryBlocks() const { return tryBlocks_; }

private:
    /** @brief Whether @p keyword ends immediately before @p position, blanks aside. */
    static bool isPrecededByKeyword(const std::string &text, size_t position,
                                    const std::string &keyword) {
        size_t cursor = position;

        while (cursor > 0) {
            const char previous = text[cursor - 1];

            if (previous == ' ' || previous == '\t' || previous == '\n' || previous == '\r') {
                cursor--;
                continue;
            }

            // A trailing block comment between the keyword and the brace is skipped too.
            if (previous == '/' && cursor >= 2 && text[cursor - 2] == '*') {
                const size_t opening = text.rfind("/*", cursor - 2);
                if (opening == std::string::npos) {
                    return false;
                }
                cursor = opening;
                continue;
            }
            break;
        }

        if (cursor < keyword.size()) {
            return false;
        }

        if (text.compare(cursor - keyword.size(), keyword.size(), keyword) != 0) {
            return false;
        }

        const size_t before = cursor - keyword.size();
        return (before == 0) || !isIdentifierCharacter(text[before - 1]);
    }

    /** @brief Whether @p needle starts at @p position with no identifier character before it. */
    static bool matchesIdentifier(const std::string &text, size_t position,
                                  const std::string &needle) {
        if (text.compare(position, needle.size(), needle) != 0) {
            return false;
        }
        return (position == 0) || !isIdentifierCharacter(text[position - 1]);
    }

    /**
     * @brief Whether @p c can appear inside a C++ identifier.
     *
     * @param [in] c Character to classify.
     *
     * @return bool - True for a letter, a digit or an underscore; false for anything else,
     *                which is what makes a keyword match a whole token rather than a
     *                substring of a longer name.
     */
    static bool isIdentifierCharacter(char c) {
        return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9');
    }

    /**
     * @brief Advances past whitespace and comments, counting newlines into @p line.
     *
     * @param [in]     text     Source text being walked.
     * @param [in]     position Offset to start from.
     * @param [in,out] line     One-based line counter, advanced by every newline crossed so
     *                         that a later diagnostic can name the line.
     *
     * @return size_t - Offset of the first character that is neither whitespace nor part of
     *                  a comment, or text.size() if the text ends first.
     */
    static size_t skipBlanks(const std::string &text, size_t position, size_t &line) {
        const size_t length = text.size();

        while (position < length) {
            const char c = text[position];

            if (c == '\n') {
                line++;
                position++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                position++;
                continue;
            }
            if (c == '/' && (position + 1) < length && text[position + 1] == '/') {
                while (position < length && text[position] != '\n') {
                    position++;
                }
                continue;
            }
            if (c == '/' && (position + 1) < length && text[position + 1] == '*') {
                position += 2;
                while ((position + 1) < length
                       && !(text[position] == '*' && text[position + 1] == '/')) {
                    if (text[position] == '\n') {
                        line++;
                    }
                    position++;
                }
                position = (position + 1 < length) ? position + 2 : length;
                continue;
            }
            break;
        }
        return position;
    }

    /**
     * @brief Offset of the parenthesis matching the one at @p position, or npos.
     *
     * @param [in] text     Source text being walked.
     * @param [in] position Offset of the opening parenthesis.
     *
     * @return size_t - Offset of the matching closing parenthesis, or std::string::npos when
     *                  the parentheses do not balance before the text ends.
     */
    static size_t matchParenthesis(const std::string &text, size_t position) {
        int depth = 0;

        for (size_t i = position; i < text.size(); i++) {
            if (text[i] == '(') {
                depth++;
            }
            else if (text[i] == ')') {
                depth--;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    /**
     * @brief Collapses every run of whitespace to a single space.
     *
     * @param [in] text Text to normalize.
     *
     * @return std::string - @p text with every run of whitespace reduced to one space and no
     *                       leading blank, so that a receiver check compares shape rather
     *                       than the plugin's current formatting.
     */
    static std::string collapseBlanks(const std::string &text) {
        std::string collapsed;
        bool pendingBlank = false;

        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pendingBlank = !collapsed.empty();
                continue;
            }
            if (pendingBlank) {
                collapsed.push_back(' ');
                pendingBlank = false;
            }
            collapsed.push_back(c);
        }
        return collapsed;
    }

    /** @brief Try blocks recorded by the last scan(), ordered by where their bodies opened. */
    std::vector<SourceTryBlock> tryBlocks_;
    /** @brief Call sites recorded by the last scan(), in source order. */
    std::vector<SourceCallSite> callSites_;
};

/**
 * @brief The Sink caller source's location relative to a workspace root
 *
 * The plugin is a sibling component of this one, so every candidate below is this constant
 * appended to a root. Named once so the guard's diagnostics and its search agree.
 */
const char *const kSinkCallerRelativePath =
    "entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp";

/**
 * @brief The environment variable both CI workflows export for the pinned Sink checkout.
 *
 * A set value that cannot be read is a fault to report, not a condition to accommodate.
 */
const char *const kSinkCallerSourceVariable = "CEC_SINK_CALLER_SOURCE";

/** @brief Where the Sink caller source was found, or every path that was tried. */
struct SinkCallerSource {
    /** @brief Whether CEC_SINK_CALLER_SOURCE was set to a non-empty value. */
    bool variableSet = false;
    /** @brief The value of that variable, when it was set. */
    std::string variableValue;
    /** @brief Whether the file was located and read. */
    bool found = false;
    /** @brief The path that was read, when found. */
    std::string path;
    /** @brief Every path that was tried, in order, for the diagnostic. */
    std::vector<std::string> pathsTried;
    /** @brief The file contents, when found. */
    std::string text;
};

/**
 * @brief Reads a whole file in binary mode, with no newline translation.
 *
 * @param [in]  path     Path to read.
 * @param [out] contents Receives the bytes once read, even when empty; ignored when null.
 *
 * @return bool - Whether usable text was read
 * @retval true  - The file opened, read without error, and is not empty.
 * @retval false - The file could not be opened or read, or is empty and so has nothing to scan.
 */
bool readWholeFile(const std::string &path, std::string *contents) {
    std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary);

    if (!stream.is_open()) {
        return false;
    }

    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    if (stream.bad()) {
        return false;
    }

    if (contents != nullptr) {
        *contents = text;
    }
    return !text.empty();
}

/**
 * @brief The directory holding this process's executable.
 *
 * @return std::string - The directory part of `/proc/self/exe` without a trailing separator,
 *                       or empty when the link cannot be read or has no separator.
 */
std::string executableDirectory() {
    char resolved[4096];
    const ssize_t written = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);

    if (written <= 0) {
        return std::string();
    }
    resolved[written] = '\0';

    const std::string path(resolved);
    const size_t separator = path.rfind('/');

    return (separator == std::string::npos) ? std::string() : path.substr(0, separator);
}

/**
 * @brief Locates and reads the real Sink caller source.
 *
 * A set CEC_SINK_CALLER_SOURCE wins and is never replaced by a search, even when unreadable;
 * otherwise the sibling-checkout layout is tried from the working directory and from the
 * executable's directory.
 *
 * @return SinkCallerSource - Whether the variable was set and to what, every path tried in
 *                            order, and the text when one read; a miss still lists the paths.
 */
SinkCallerSource locateAndReadSinkCallerSource() {
    SinkCallerSource source;

    const char *const configured = ::getenv(kSinkCallerSourceVariable);

    if (configured != nullptr && configured[0] != '\0') {
        source.variableSet = true;
        source.variableValue = configured;
        source.pathsTried.push_back(source.variableValue);
        source.found = readWholeFile(source.variableValue, &source.text);

        if (source.found) {
            source.path = source.variableValue;
        }
        return source;
    }

    std::vector<std::string> roots;
    roots.push_back("../../..");

    const std::string exeDirectory = executableDirectory();
    if (!exeDirectory.empty()) {
        roots.push_back(exeDirectory + "/../../..");
        roots.push_back(exeDirectory + "/../../../..");
    }

    for (size_t i = 0; i < roots.size(); i++) {
        const std::string candidate = roots[i] + "/" + kSinkCallerRelativePath;
        source.pathsTried.push_back(candidate);

        if (readWholeFile(candidate, &source.text)) {
            source.found = true;
            source.path = candidate;
            break;
        }
    }
    return source;
}

/**
 * @brief Joins the paths a search tried, for a diagnostic that names all of them
 *
 * @param [in] paths Paths in the order they were tried.
 *
 * @return std::string - Each path bracketed and comma-separated, and empty for an empty
 *                       input. Bracketed rather than bare so that a path containing a space
 *                       still reads as one entry.
 */
std::string joinPaths(const std::vector<std::string> &paths) {
    std::string joined;

    for (size_t i = 0; i < paths.size(); i++) {
        if (i > 0) {
            joined += ", ";
        }
        joined += "[" + paths[i] + "]";
    }
    return joined;
}

// Receive-path observation: a frame is asserted where the middleware delivers it, a
// FrameListener on a Connection, through a deadline-bounded condition-variable wait.

/**
 * @brief Upper bound on a delivery that is expected to happen, in milliseconds.
 *
 * Generous because only a failing run reaches it; it matches the end-to-end cases in
 * ccec/test_Connection.cpp.
 */
constexpr int kFrameDeliveryTimeoutMs = 3000;

/**
 * @brief Window over which a delivery is expected not to happen, in milliseconds.
 *
 * Short, because every passing run waits out the whole window watching for a delivery that
 * must not happen. The stronger evidence is that the frame never surfaces after a later re-open.
 */
constexpr int kNonDeliveryWindowMs = 400;

/**
 * @brief The middleware's slow-synchronous-call threshold, in milliseconds, restated.
 *
 * Mirrors @c SLOW_HAL_CALL_WARN_MS in ccec/src/DriverAidlImpl.cpp, which has internal linkage.
 *
 * @warning Restating is safe here: if production raised its threshold above this value, the one
 *          case using it would fail loudly rather than silently stop exercising the warning arm.
 *
 * @see FakeHdmiCecController::setAddLogicalAddressesDelayMs()
 */
constexpr int kSlowHalCallWarnMs = 1000;

/**
 * @brief A FrameListener that records the bytes of every frame it is notified of
 *
 * FrameListener::notify is const, so the recording members are mutable - the same shape the
 * neighbouring suite's listener uses, and for the same reason.
 */
class RecordingFrameListener : public FrameListener {
public:
    /**
     * @brief Records @p frame's bytes and wakes anything waiting on the count
     *
     * @param [in] frame Frame the Bus reader is delivering. Copied out immediately, because
     *                  the frame does not outlive the notification.
     *
     * @note Runs on the Bus reader thread, so the copy is taken under @c mutex_ and the
     *       notification is issued after the lock is dropped.
     */
    void notify(const CECFrame &frame) const override {
        const uint8_t *buffer = nullptr;
        size_t length = 0;
        frame.getBuffer(&buffer, &length);

        std::vector<uint8_t> bytes;
        if (buffer != nullptr) {
            bytes.assign(buffer, buffer + length);
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            frames_.push_back(bytes);
        }
        condition_.notify_all();
    }

    /**
     * @brief Waits until at least @p expected frames have arrived, or the deadline passes
     *
     * @param [in] expected  - Number of frames to wait for.
     * @param [in] timeoutMs - Upper bound on the wait, in milliseconds.
     *
     * @return bool - Whether the count was reached within the bound.
     */
    bool waitForFrames(size_t expected, int timeoutMs) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this, expected]() { return frames_.size() >= expected; });
    }

    /**
     * @brief How many frames have been delivered so far
     *
     * @return size_t - The number of frames recorded at the moment of the call. A count
     *                  read while the reader thread is running is a snapshot, not a
     *                  settled total.
     */
    size_t frameCount() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return frames_.size();
    }

    /**
     * @brief The bytes of the frame at @p index.
     *
     * @param [in] index Zero-based delivery order.
     *
     * @return std::vector<uint8_t> - A copy of that frame's bytes, or empty when fewer than
     *                                @p index + 1 frames have arrived.
     */
    std::vector<uint8_t> frameAt(size_t index) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return (index < frames_.size()) ? frames_[index] : std::vector<uint8_t>();
    }

private:
    /** @brief Guards @c frames_ against the Bus reader thread and the case body at once. */
    mutable std::mutex mutex_;
    /** @brief Notifies waitForFrames() after each delivery, avoiding polling. */
    mutable std::condition_variable condition_;
    /** @brief The bytes of every frame delivered so far, in delivery order. */
    mutable std::vector<std::vector<uint8_t>> frames_;
};

/**
 * @brief A frame listener that parks the Bus reader inside notify() until release() is called.
 *
 * Holding the reader lets offered frames accumulate, the only way to reach the incoming queue's
 * refusal arm. State is @c mutable because notify() is @c const.
 *
 * @warning notify() blocks the Bus reader; call release() on every exit path, before asserting.
 *
 * @see RecordingFrameListener, DriverAidlImpl::offerReceivedFrame()
 */
class StallingFrameListener : public FrameListener {
public:
    /**
     * @brief Records arrival and notifies waiters, then blocks until release() is called.
     *
     * @param [in] frame Frame being delivered; not copied, as this listener only holds the thread.
     */
    void notify(const CECFrame &frame) const override {
        (void)frame;

        std::unique_lock<std::mutex> guard(mutex_);

        ++parkedCount_;
        condition_.notify_all();

        // Predicate rather than a bare wait, so a spurious wake-up cannot let the reader escape
        // early and quietly drain the queue this listener is holding full.
        condition_.wait(guard, [this] { return released_; });
    }

    /**
     * @brief Waits until the reader has entered notify() at least once
     *
     * @param [in] timeoutMs Milliseconds to wait before giving up
     *
     * @return bool - true once notify() has been entered at least once, false on timeout
     *
     * @note A false return means the delivery chain never reached this listener at all, which is
     *       a different failure from the queue not filling and must be reported as such.
     */
    bool waitUntilParked(int timeoutMs) const {
        std::unique_lock<std::mutex> guard(mutex_);

        return condition_.wait_for(guard, std::chrono::milliseconds(timeoutMs),
                                   [this] { return parkedCount_ > 0; });
    }

    /**
     * @brief Releases the parked reader, and every later notification, immediately
     *
     * @return None
     *
     * @post notify() returns at once from now on, so this is safe to call more than once and safe
     *       to call when nothing is parked.
     */
    void release() const {
        {
            std::lock_guard<std::mutex> guard(mutex_);

            released_ = true;
        }

        condition_.notify_all();
    }

private:
    /** @brief Guards the counter and flag below against the Bus reader thread and the case body. */
    mutable std::mutex mutex_;
    /** @brief Signals both directions: parked for the case body, released for the reader. */
    mutable std::condition_variable condition_;
    /** @brief Count of notify() entries; non-zero from the first one on, even after release(). */
    mutable size_t parkedCount_ = 0;
    /** @brief Set once by release(); notify() returns immediately once it is set. */
    mutable bool released_ = false;
};

/**
 * @brief The thread-name prefix libbinder gives threadpool threads on an Android build.
 *
 * ProcessState::makeBinderThreadName() composes "binder:PID_N" for each pool thread.
 *
 * @warning Not applied on this Linux port, so its absence says nothing about the pool; the pool
 *          is asserted through ProcessState::getThreadPoolMaxThreadCount() instead.
 */
const char *const kBinderThreadNamePrefix = "binder:";

/**
 * @brief Whether any thread of this process carries libbinder's threadpool name prefix.
 *
 * Corroboration only: it reads each /proc/self/task comm, and this port never applies the name,
 * so a false result is not evidence either way.
 *
 * @param [out] procIsReadable Set true once /proc/self/task opens, else false; ignored when null.
 *
 * @return bool - Whether a thread named "binder:*" exists in this process.
 * @retval true  - A threadpool thread is running and this build applies the name.
 * @retval false - No such thread is named, the ordinary result on this port.
 * @see kBinderThreadNamePrefix
 */
bool processHasABinderThread(bool *procIsReadable) {
    if (procIsReadable != nullptr) {
        *procIsReadable = false;
    }

    DIR *taskDirectory = ::opendir("/proc/self/task");

    if (taskDirectory == nullptr) {
        return false;
    }

    if (procIsReadable != nullptr) {
        *procIsReadable = true;
    }

    bool found = false;

    for (struct dirent *entry = ::readdir(taskDirectory); entry != nullptr && !found;
         entry = ::readdir(taskDirectory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        const std::string commPath = std::string("/proc/self/task/") + entry->d_name + "/comm";
        std::string comm;

        if (!readWholeFile(commPath, &comm)) {
            // A thread that exited between readdir and this open. Not an error, and not evidence.
            continue;
        }

        if (comm.compare(0, ::strlen(kBinderThreadNamePrefix), kBinderThreadNamePrefix) == 0) {
            found = true;
        }
    }

    ::closedir(taskDirectory);
    return found;
}

/**
 * @brief Shared state for a thread parked in DriverAidlImpl::read().
 *
 * Held behind a shared pointer so a reader that is never released can be abandoned safely.
 * Every member is guarded by @c mutex and every transition notifies @c signal.
 */
struct BlockedReaderState {
    /** @brief Guards every member below. */
    std::mutex mutex;
    /** @brief Notified on each transition, so a waiter wakes the instant one happens. */
    std::condition_variable signal;

    /** @brief Set once the thread is about to enter its first read(). */
    bool enteredRead = false;
    /** @brief How many frames read() has returned normally. */
    int framesRead = 0;
    /** @brief Set when read() was released by the InvalidStateException the sentinel produces. */
    bool releasedByInvalidState = false;
    /** @brief Set when read() raised something else, which would be a different defect. */
    bool releasedByOtherException = false;
    /** @brief Set when the thread body has finished, whatever released it. */
    bool finished = false;

    /**
     * @brief Waits until @p predicate holds, or the bound passes
     *
     * @param [in] timeoutMs  - Upper bound on the wait, in milliseconds.
     * @param [in] predicate  - Called under the lock; the wait ends when it returns true.
     *
     * @return bool - Whether the predicate became true within the bound.
     */
    template <typename Predicate>
    bool waitFor(int timeoutMs, Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate);
    }
};

/**
 * @brief A Connection with a listener attached, withdrawn and closed by the destructor.
 *
 * RAII, so a fatal assertion cannot leave the Bus holding a pointer to a destroyed listener.
 */
class ListeningConnection {
public:
    /**
     * @brief Opens a Connection on @p source and registers @p listener on it
     *
     * @param [in] source   - Logical address this connection filters for.
     * @param [in] name     - Connection name, used only in the middleware's own logs.
     * @param [in] listener - Listener to register; must outlive this object.
     */
    ListeningConnection(const LogicalAddress &source, const std::string &name,
                        FrameListener &listener)
        : connection_(source, true, name)
        , listener_(&listener)
    {
        connection_.addFrameListener(listener_);
    }

    /** @brief Not copyable: the registration is an identity the Bus holds a pointer to. */
    ListeningConnection(const ListeningConnection &) = delete;
    /** @brief Not assignable, for the same reason. */
    ListeningConnection &operator=(const ListeningConnection &) = delete;

    /** @brief Withdraws the listener and closes the connection, in that order. */
    ~ListeningConnection() {
        connection_.removeFrameListener(listener_);
        connection_.close();
    }

private:
    /** @brief The connection this object owns; closed by the destructor. */
    Connection connection_;
    /** @brief The registered listener, withdrawn by the destructor and not owned. */
    FrameListener *listener_;
};


} // namespace

/**
 * @brief Compatibility-rejection coverage, per branch, by direct call.
 *
 * Cases call halcompat::isCompatible<IHdmiCec>() or the rejection diagnostic against locally
 * constructed doubles and fakes; nothing is registered and the driver is never touched.
 *
 * @pre Runs under every invocation; needs no service, binder driver, back-end or HAL.
 */
class DriverAidlCompatibilityTest : public ::testing::Test {
};

/**
 * @brief MetadataDouble refuses every IHdmiCec method, so no case can pass on an answer it
 *        never asked for.
 * @pre Runs under every invocation; the double is local and halcompat is called directly.
 * @note Asserts that all seven interface methods return non-ok, onAsBinder() returns nullptr,
 *       and both metadata overrides report their installed values.
 * @see MetadataDouble
 */
TEST_F(DriverAidlCompatibilityTest, TheCompatibilityDoubleAnswersNoInterfaceMethod) {
    // Held by its concrete type: onAsBinder() is protected in IInterface and public only through
    // IHdmiCecDefault's override.
    const ::android::sp<MetadataDouble> doubleUnderTest =
        ::android::sp<MetadataDouble>::make(std::string(kFrozenInterfaceHash),
                                           kClientInterfaceVersion);
    ASSERT_TRUE(doubleUnderTest != nullptr);

    // Every interface method must refuse. The out-parameters are valid, initialised storage, and
    // only each method's returned status is asserted.
    cechal::State state = cechal::State::CLOSED;
    ::std::optional< ::com::rdk::hal::PropertyValue> property;
    ::std::vector<int32_t> addresses;
    ::android::sp<cechal::IHdmiCecController> controller;
    bool flag = false;

    EXPECT_FALSE(doubleUnderTest->getState(&state).isOk())
        << "the double answered getState. It must refuse every interface method, or a case could "
           "pass on an answer it never asked for";
    EXPECT_FALSE(doubleUnderTest->getProperty(cechal::Property::HAL_CEC_VERSION, &property).isOk())
        << "the double answered getProperty";
    EXPECT_FALSE(doubleUnderTest->getLogicalAddresses(&addresses).isOk())
        << "the double answered getLogicalAddresses";
    EXPECT_FALSE(doubleUnderTest->open(nullptr, &controller).isOk())
        << "the double answered open, which would let a compatibility case accidentally establish "
           "a session";
    EXPECT_FALSE(doubleUnderTest->close(nullptr, &flag).isOk())
        << "the double answered close";
    EXPECT_FALSE(doubleUnderTest->registerEventListener(nullptr, &flag).isOk())
        << "the double answered registerEventListener";
    EXPECT_FALSE(doubleUnderTest->unregisterEventListener(nullptr, &flag).isOk())
        << "the double answered unregisterEventListener";

    EXPECT_EQ(doubleUnderTest->onAsBinder(), nullptr)
        << "IHdmiCecDefault::onAsBinder no longer returns nullptr. That return is why the fake "
           "service derives from BnHdmiCec instead of from IHdmiCecDefault - an object that cannot "
           "produce a binder can never be published nor reached through a proxy - so if this "
           "changed, revisit FakeHdmiCecService, which records that reasoning";

    // And the two members that are overridden still answer, so the double is narrow but not inert.
    EXPECT_EQ(doubleUnderTest->getInterfaceVersion(), kClientInterfaceVersion)
        << "the version override is not in effect, so every version case below is measuring the "
           "stock default rather than the value it installed";
    EXPECT_EQ(doubleUnderTest->getInterfaceHash(), std::string(kFrozenInterfaceHash))
        << "the hash override is not in effect";
}

/**
 * @brief A null proxy is rejected before any metadata is read.
 * @pre Runs under every invocation; needs no service and no resolved back-end.
 * @note This is the arm selection takes whenever nothing is published under the production name.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServiceIsNull) {
    const ::android::sp<cechal::IHdmiCec> absent;

    ASSERT_TRUE(absent == nullptr) << "the fixture's own premise failed: a default-constructed "
                                     "sp<IHdmiCec> must be null for this case to test anything";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(absent))
        << "a null service proxy was accepted as compatible, so the selection would go on to "
           "open() and dereference nothing";
}

/**
 * @brief An empty interface hash is a failed hash query and is never read as agreement.
 * @pre Runs under every invocation. The subject is a stock IHdmiCecDefault, whose own hash is
 *      empty, so nothing here arranges the condition.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenInterfaceHashIsEmpty) {
    const ::android::sp<cechal::IHdmiCec> stockDefault =
        ::android::sp<cechal::IHdmiCecDefault>::make();

    ASSERT_TRUE(stockDefault != nullptr);
    ASSERT_TRUE(stockDefault->getInterfaceHash().empty())
        << "IHdmiCecDefault no longer returns an empty interface hash, so this case is no longer "
           "exercising the empty-hash arm; give it an explicit MetadataDouble(\"\", VERSION) "
           "instead and note that IHdmiCec.h:69-71 changed";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(stockDefault))
        << "a service reporting an empty interface hash was accepted; a failed hash query must "
           "never be read as agreement";
}

/**
 * @brief The "-1" spelling of a failed hash query is rejected while the version is compatible.
 * @pre Runs under every invocation.
 * @note "-1" is the hash the L1 harness installs for invocation C, whose factory-level fallback
 *       rests on this branch.
 * @see publishFakeForMode
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenInterfaceHashIsMinusOne) {
    const ::android::sp<cechal::IHdmiCec> broken = doubleReportingHash(kBrokenInterfaceHash);

    ASSERT_TRUE(broken != nullptr);
    ASSERT_EQ(broken->getInterfaceVersion(), kClientInterfaceVersion)
        << "the double must report a compatible version, so that a rejection can only have come "
           "from the hash";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(broken))
        << "a service reporting the \"-1\" interface hash was accepted as compatible, which would "
           "make invocation C select the AIDL back-end instead of falling back to legacy";
}

/**
 * @brief A "notfrozen" development server is rejected by the defaulted call and accepted
 *        only when opted into explicitly.
 * @pre Runs under every invocation. The defaulted call is the one production makes.
 * @note Both dispositions are asserted, so the rejection cannot be an unrecognised hash.
 * @see DriverAidlImpl::isServiceAvailable()
 */
TEST_F(DriverAidlCompatibilityTest, UnfrozenServerIsRejectedByDefaultAndAcceptedOnlyWhenOptedIn) {
    const ::android::sp<cechal::IHdmiCec> unfrozen = doubleReportingHash(kUnfrozenInterfaceHash);

    ASSERT_TRUE(unfrozen != nullptr);

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(unfrozen))
        << "an unfrozen development server was accepted by the defaulted call, which is the call "
           "the production back-end makes; a development image must be opted into explicitly";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(unfrozen, true))
        << "an unfrozen server was rejected even with allowUnfrozen=true, so the parameter no "
           "longer selects the behaviour it documents and the rejection above is happening for "
           "some other reason";
}

/**
 * @brief The real frozen hash with this client's own version is accepted.
 * @pre Runs under every invocation. Every version case is measured against this baseline.
 * @note The hash is asserted to be the frozen constant first, so the version comparison is reached.
 */
TEST_F(DriverAidlCompatibilityTest, CompatibleWhenServerReportsThisClientsVersion) {
    const ::android::sp<cechal::IHdmiCec> exact =
        frozenDoubleReportingVersion(kClientInterfaceVersion);

    ASSERT_TRUE(exact != nullptr);
    ASSERT_EQ(exact->getInterfaceHash(), std::string(kFrozenInterfaceHash))
        << "the double must report the real frozen hash, or it would be rejected by the hash arm "
           "and this case would pass without ever reaching the version comparison";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(exact))
        << "a server reporting this client's own interface version was rejected, so no service "
           "could ever be selected on any platform";
}

/**
 * @brief A newer server in the same era and major is accepted, at both ends of the range.
 * @pre Runs under every invocation.
 * @note Accepting 1010 and 1999 pins the range and guards against an "exact version only" rule
 *       that would reject a legitimately upgraded HAL.
 */
TEST_F(DriverAidlCompatibilityTest, CompatibleWhenServerReportsNewerVersionInSameMajor) {
    const ::android::sp<cechal::IHdmiCec> newer =
        frozenDoubleReportingVersion(kNewerCompatibleVersion);
    const ::android::sp<cechal::IHdmiCec> newest =
        frozenDoubleReportingVersion(kNewestCompatibleVersion);

    ASSERT_TRUE(newer != nullptr);
    ASSERT_TRUE(newest != nullptr);
    ASSERT_GT(kNewerCompatibleVersion, kClientInterfaceVersion)
        << "the 'newer' double must actually be newer than this client for the case to mean "
           "anything";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(newer))
        << "a newer server in the same era and major was rejected. The era-0 rule accepts it "
           "(halcompat.h:112-114); rejecting it would refuse an upgraded HAL that is in fact "
           "additive and compatible";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(newest))
        << "the top of this client's accepted range was rejected, so the accepted set is no "
           "longer the whole of [1000, 1999] that the file block's analysis relies on";
}

/**
 * @brief A next-major server is rejected by the major equality conjunct even though its
 *        version is numerically larger.
 * @pre Runs under every invocation.
 * @note The value is asserted greater than the client's first, so ordering alone would accept it.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsDifferentMajor) {
    const ::android::sp<cechal::IHdmiCec> crossMajor =
        frozenDoubleReportingVersion(kCrossMajorVersion);

    ASSERT_TRUE(crossMajor != nullptr);
    ASSERT_GT(kCrossMajorVersion, kClientInterfaceVersion)
        << "this case is only meaningful while the cross-major value is numerically greater than "
           "the client's, since that is what distinguishes the major conjunct from the ordering "
           "conjunct";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(crossMajor))
        << "a server from the next major generation was accepted. In era 0 a major bump is "
           "breaking, and it is larger numerically, so accepting it means the check degenerated "
           "to an ordering comparison";
}

/**
 * @brief A later-era server is rejected by the era equality conjunct.
 * @pre Runs under every invocation.
 * @note Distinct from the cross-major case: 100000 is era 1, major 0, a re-frozen interface
 *       rather than a broken one.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsDifferentEra) {
    const ::android::sp<cechal::IHdmiCec> crossEra =
        frozenDoubleReportingVersion(kCrossEraVersion);

    ASSERT_TRUE(crossEra != nullptr);

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(crossEra))
        << "an era-1 server satisfied this era-0 client. The two eras make different promises, so "
           "this must be rejected until the client itself is re-frozen into era 1 or later";
}

/**
 * @brief The generator's default version of 1 is rejected on major, not on ordering.
 * @pre Runs under every invocation.
 * @note Version 1 decodes to era 0, major 0, so the cross-major conjunct rejects it; no
 *       older-same-major value exists for this client.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsUnfrozenGeneratorVersion) {
    const ::android::sp<cechal::IHdmiCec> generatorDefault =
        frozenDoubleReportingVersion(kUnfrozenGeneratorVersion);

    ASSERT_TRUE(generatorDefault != nullptr);
    ASSERT_EQ(halcompat::detail::major(kUnfrozenGeneratorVersion), 0)
        << "version 1 no longer decodes to major 0, so the reason it is rejected has changed and "
           "this case's comment - and the file block's account of it - are now wrong";
    ASSERT_NE(halcompat::detail::major(kUnfrozenGeneratorVersion),
              halcompat::detail::major(kClientInterfaceVersion))
        << "version 1 now shares this client's major, which would make it the older-same-major "
           "case the file block proves cannot exist; re-derive unreachable path (1)";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(generatorDefault))
        << "a pre-freeze development server reporting the generator's default version was "
           "accepted as compatible";
}

/**
 * @brief The era-0 ordering conjunct rejects an older same-major server.
 * @pre Runs under every invocation; calls halcompat::detail::isCompatible directly with a
 *      client of 3020 and a server of 3000, since the conjunct is unreachable for this client.
 * @note Same era and same major are asserted first, so the rejection is attributable to
 *       ordering alone; the reversed pair is the positive control.
 */
TEST_F(DriverAidlCompatibilityTest, OlderSameMajorServerIsRejectedByTheOrderingRule) {
    ASSERT_EQ(halcompat::detail::era(kOlderSameMajorClient),
              halcompat::detail::era(kOlderSameMajorServer))
        << "the chosen pair no longer shares an era, so a rejection would come from the era "
           "conjunct and this case would stop testing the ordering conjunct";
    ASSERT_EQ(halcompat::detail::major(kOlderSameMajorClient),
              halcompat::detail::major(kOlderSameMajorServer))
        << "the chosen pair no longer shares a major, so this case has degenerated into the "
           "cross-major case - which is exactly the flaw in halcompat.h:128's own assertion";
    ASSERT_LT(kOlderSameMajorServer, kOlderSameMajorClient)
        << "the server must be older than the client for the ordering conjunct to be the one "
           "that fails";

    EXPECT_FALSE(halcompat::detail::isCompatible(kOlderSameMajorClient, kOlderSameMajorServer))
        << "an older server within the same era and major was accepted, so the ordering conjunct "
           "at halcompat.h:114 is no longer enforced";

    // Positive control on the same pair, reversed: a newer server in that major is accepted,
    // so the rejection above is attributable to the ordering and not to the pair itself.
    EXPECT_TRUE(halcompat::detail::isCompatible(kOlderSameMajorServer, kOlderSameMajorClient))
        << "the reversed pair was also rejected, so something other than ordering is rejecting "
           "both and the case above proves nothing about the ordering conjunct";
}

/**
 * @brief A metadata double whose first hash read fails and whose later reads succeed.
 *
 * Models the generated proxy's caching rule: a failed metadata read is stored as -1 and
 * retried, so the deciding read and a later reread can disagree.
 * @warning Not derived from BnHdmiCec, for the reason MetadataDouble's warning gives.
 */
class RecoveringMetadataDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief Reports the "-1" hash on the first read and the frozen digest thereafter. */
    std::string getInterfaceHash() override
    {
        ++hashReads_;
        return (hashReads_ == 1) ? std::string(kBrokenInterfaceHash)
                                 : std::string(kFrozenInterfaceHash);
    }

    /**
     * @brief Always this client's own version, so only the hash can ever reject.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }

    /**
     * @brief How many times getInterfaceHash() has been called on this double.
     *
     * @return int - The read count, which attributes a disagreement to the first-read failure.
     */
    int hashReads() const { return hashReads_; }

private:
    /** @brief Reads so far; the first is the one that reports a failed transaction. */
    int hashReads_ = 0;
};

/**
 * @brief Two compatibility calls on one service can disagree, because the deciding read and
 *        any later read are separate transactions.
 * @pre Runs under every invocation, against RecoveringMetadataDouble.
 * @note The first call rejects, the second accepts, and exactly two hash reads occur.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, ARecoveredMetadataReadMakesTwoCompatibilityCallsDisagree) {
    const ::android::sp<RecoveringMetadataDouble> recovering =
        ::android::sp<RecoveringMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = recovering;

    ASSERT_TRUE(service != nullptr);

    // The decision: the call whose answer actually selects the back-end.
    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the first compatibility call read a \"-1\" hash and accepted the service anyway, so the "
           "modelled transient failure would not have caused a fallback at all and nothing below "
           "this line would be testing the situation it claims to test";

    // The observation: a separate, later read of the same service, like the post-rejection
    // diagnostic read isServiceAvailable() makes.
    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the second compatibility call did not recover, so this double no longer models the "
           "proxy's retry-a-failed-read rule and cannot guard the causal wording it exists to "
           "guard";

    EXPECT_EQ(recovering->hashReads(), 2)
        << "the two compatibility calls did not perform exactly two hash reads, so the "
           "disagreement above came from something other than the modelled first-read failure";
}

/**
 * @brief A compatibility double whose metadata transaction fails outright.
 *
 * Models the snapshot read itself failing, as a dead or refusing service would over binder.
 */
class ThrowingMetadataDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief Throws, standing in for a failed metadata transaction. */
    std::string getInterfaceHash() override
    {
        throw std::runtime_error("simulated metadata transaction failure");
    }

    /**
     * @brief Always this client's own version; it is never reached, since the hash throws.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }
};

/**
 * @brief A double whose hash reads fail twice and succeed from the third read onward.
 *
 * Models delayed recovery, against which the emitter's single-snapshot rule is held.
 * @warning Not derived from BnHdmiCec, for the reason MetadataDouble's warning gives.
 */
class DelayedRecoveryMetadataDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief Reports the "-1" hash on the first two reads and the frozen digest thereafter. */
    std::string getInterfaceHash() override
    {
        ++hashReads_;
        return (hashReads_ <= 2) ? std::string(kBrokenInterfaceHash)
                                 : std::string(kFrozenInterfaceHash);
    }

    /**
     * @brief Always this client's own version, so only the hash can ever reject.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }

    /**
     * @brief How many times getInterfaceHash() has been called on this double.
     *
     * @return int - The read count, which pins the recovery to the third read.
     */
    int hashReads() const { return hashReads_; }

private:
    /** @brief Reads so far; the first two report a failed transaction and the rest recover. */
    int hashReads_ = 0;
};

/**
 * @brief Each observed hash category is described distinguishably, and no description makes
 *        a causal claim.
 * @pre Runs under every invocation; calls the production classifier directly, because the
 *      arm that emits this wording is unreachable without a binder transport.
 * @note Every phrase is asserted free of "because".
 * @see DriverAidlImpl::describeObservedInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheObservedHashDescriptionNamesEachCategoryWithoutClaimingCause) {
    const std::string empty;
    const std::string minusOne(kBrokenInterfaceHash);
    const std::string unfrozen(kUnfrozenInterfaceHash);
    const std::string frozen(kFrozenInterfaceHash);

    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(empty), ::testing::HasSubstr("FAILS"))
        << "an empty observed hash is not described as a failed transaction, so a reader cannot "
           "tell it apart from a value a working server reported";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(minusOne), ::testing::HasSubstr("FAILS"))
        << "the \"-1\" marker is not described as a failed transaction";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(unfrozen), ::testing::HasSubstr("UNFROZEN"))
        << "the \"notfrozen\" marker is not described as an unfrozen development build";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(frozen), ::testing::HasSubstr("frozen release digest"))
        << "a real frozen digest is not described as one";

    // The whole point of these phrases: none of them asserts what caused the rejection.
    for (const std::string &value : { empty, minusOne, unfrozen, frozen }) {
        const std::string phrase = DriverAidlImpl::describeObservedInterfaceHash(value);
        EXPECT_THAT(phrase, ::testing::Not(::testing::HasSubstr("because")))
            << "the description of [" << value << "] makes a causal claim, which no post-decision "
               "read is entitled to make";
    }
}

/**
 * @brief A snapshot that would itself be accepted is reported as acceptable, so the version
 *        rule can never be blamed for it.
 * @pre Runs under every invocation; calls the production predicate directly.
 * @note A newer same-era, same-major version is accepted too.
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
 */
TEST_F(DriverAidlCompatibilityTest, AnObservedSnapshotThatWouldBeAcceptedReportsChangedMetadata) {
    EXPECT_TRUE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                    std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kClientInterfaceVersion))
        << "a snapshot carrying the frozen digest and this client's own version was reported as "
           "not acceptable, so production would blame the version rule for metadata that is in "
           "fact perfectly compatible -- the exact false diagnosis this helper exists to prevent";

    // Newer within the same era and major is accepted by the real rule, so the observation
    // must be too. A helper that treated "not equal" as unacceptable would encode a false rule.
    EXPECT_TRUE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                    std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kNewerCompatibleVersion))
        << "a newer server within the same era and major was reported as not acceptable, which "
           "contradicts halcompat::detail::isCompatible()";
}

/**
 * @brief A bad hash or an incompatible version each makes an observed snapshot unacceptable
 *        on its own.
 * @pre Runs under every invocation; calls the production predicate directly.
 * @note Each half is varied with the other held compatible.
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
 */
TEST_F(DriverAidlCompatibilityTest, AnObservedSnapshotIsNotAcceptedForABadHashOrAnIncompatibleVersion) {
    // Each non-digest hash, with a compatible version, so only the hash can be the reason.
    for (const char *bad : { "", kBrokenInterfaceHash, kUnfrozenInterfaceHash }) {
        EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                         std::string(bad), kClientInterfaceVersion, kClientInterfaceVersion))
            << "the observed hash [" << bad << "] was reported acceptable even though halcompat "
               "rejects it, so the observation would contradict the decision it accompanies";
    }

    // A frozen digest with an incompatible version: the version half must do the rejecting,
    // and it is the real rule that decides, not a copy of it.
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kOlderSameMajorClient, kOlderSameMajorServer))
        << "an older server within the same major was reported acceptable, so the version half of "
           "the observation is not delegating to halcompat::detail::isCompatible()";
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kCrossMajorVersion))
        << "a cross-major server version was reported acceptable";
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kCrossEraVersion))
        << "a cross-era server version was reported acceptable";
}

// The production emitter, driven directly with its output captured, so the asserted wording
// is the wording DriverAidlImpl emits.

/**
 * @brief The production diagnostic names all three rejection rules and claims none of them.
 *
 * @pre Runs under every invocation, driving the production emitter directly.
 * @note The observed values are asserted to reach the log as well as the qualification.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, TheProductionDiagnosticNamesAllThreeRulesAndClaimsNoneOfThem) {
    // A genuine rejection: frozen hash, cross-major version. halcompat refuses it.
    const ::android::sp<cechal::IHdmiCec> service =
        frozenDoubleReportingVersion(kCrossMajorVersion);
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "this double was accepted, so it does not model a rejection and the diagnostic "
           "under test would never be reached in production for it";

    std::string emitted;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        emitted = capture.read();
    }

    EXPECT_THAT(emitted, ::testing::HasSubstr("REJECTED AS NOT COMPATIBLE"))
        << "the diagnostic did not report the rejection at all";
    EXPECT_THAT(emitted, ::testing::HasSubstr("WHICH ONE APPLIED IS NOT REPORTED HERE"))
        << "the diagnostic no longer declines to name the cause. That refusal is the whole "
           "correction: the metadata halcompat decided on cannot be recovered, so any claim "
           "about which rule applied is a guess presented as a finding";
    // All three possibilities are named as possibilities.
    EXPECT_THAT(emitted, ::testing::HasSubstr("unreadable interface hash"));
    EXPECT_THAT(emitted, ::testing::HasSubstr("UNFROZEN"));
    EXPECT_THAT(emitted, ::testing::HasSubstr("era and major"));
    // And the snapshot is labelled as evidence rather than as the reason.
    EXPECT_THAT(emitted, ::testing::HasSubstr("OBSERVED AFTER the decision"))
        << "the observed values are no longer labelled as post-decision, so a reader would "
           "take them for the values the decision used";
    EXPECT_THAT(emitted, ::testing::HasSubstr("halcompat::isCompatible(), the SOLE decision"))
        << "the diagnostic no longer states that halcompat owns the decision";
    // The observed values must reach the log, which CCEC_LOG truncates at 499 bytes.
    EXPECT_THAT(emitted, ::testing::HasSubstr("interface hash ["))
        << "the observed interface hash never reached the log -- check the 499-byte CCEC_LOG "
           "limit before assuming the value was not computed";
    EXPECT_THAT(emitted, ::testing::HasSubstr("server interface version 2000 against this client's 1000"))
        << "the observed and client versions never reached the log";
    EXPECT_THAT(emitted, ::testing::HasSubstr("legacy back-end is selected"))
        << "the outcome sentence never reached the log";
}

/**
 * @brief Recovered metadata is reported as changed metadata, never blamed on the version rule.
 *
 * @pre Runs under every invocation, driving the production emitter against
 *      RecoveringMetadataDouble.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, TheProductionDiagnosticBlamesChangedMetadataNotTheVersionRule) {
    const ::android::sp<RecoveringMetadataDouble> recovering =
        ::android::sp<RecoveringMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = recovering;

    // Read 1 is the decision, and it fails on a "-1" hash -- so a fallback happens.
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the deciding call accepted a \"-1\" hash, so no fallback would occur and this case "
           "would not be modelling a rejection at all";

    // The emitter's single snapshot is read 2, which recovers: one decision, then one
    // snapshot, exactly as production performs them.
    std::string emitted;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        emitted = capture.read();
    }
    ASSERT_EQ(recovering->hashReads(), 2)
        << "the emitter did not take exactly one snapshot after the deciding read. More than "
           "one read is the defect this design removed: a predicate result combined with "
           "separately retried values is what let compatible metadata be blamed on the "
           "version rule";

    EXPECT_THAT(emitted, ::testing::HasSubstr("WOULD itself have been accepted"))
        << "a snapshot that would have been accepted was not reported as such, so the reader "
           "is given no signal that the metadata changed after the decision";
    EXPECT_THAT(emitted, ::testing::HasSubstr("INTERMITTENT METADATA TRANSACTION"))
        << "the diagnostic did not point at an intermittent transaction, which is the only "
           "honest reading when the post-decision snapshot is itself compatible";
    EXPECT_THAT(emitted, ::testing::HasSubstr("frozen release digest"))
        << "the recovered hash was not described as a frozen digest, so the observation "
           "misreports what the server answered";
    // The regression guard: the version rule is named as one possibility, so the causal
    // claim is what must be absent.
    EXPECT_THAT(emitted, ::testing::Not(::testing::HasSubstr("because the interface version")))
        << "the diagnostic asserted a version cause for a server whose observed version is "
           "compatible, which is a statement the single-snapshot rule exists to make "
           "unconstructible";
    EXPECT_THAT(emitted, ::testing::HasSubstr("WHICH ONE APPLIED IS NOT REPORTED HERE"))
        << "the non-causal qualification was dropped on the recovery path";
}

/**
 * @brief A recovery that arrives later than the snapshot is still never blamed on the version.
 *
 * @pre Runs under every invocation, driving the production emitter against
 *      DelayedRecoveryMetadataDouble, whose hash recovers only on the third read.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, DelayedMetadataRecoveryOnAThirdReadCannotBeBlamedOnTheVersionRule) {
    const ::android::sp<DelayedRecoveryMetadataDouble> delayed =
        ::android::sp<DelayedRecoveryMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = delayed;

    // Decision on read 1: fails. Emitter snapshot on read 2: still fails. So the emitter sees
    // a broken hash and must say so -- reporting the hash, not the version.
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service));
    std::string firstEmission;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        firstEmission = capture.read();
    }
    ASSERT_EQ(delayed->hashReads(), 2)
        << "the decision and one snapshot should have consumed exactly two reads";
    EXPECT_THAT(firstEmission, ::testing::HasSubstr("FAILS"))
        << "a \"-1\" snapshot was not described as a failed metadata read";
    EXPECT_THAT(firstEmission, ::testing::Not(::testing::HasSubstr("WOULD itself have been accepted")))
        << "a snapshot that is still broken was reported as one that would have been accepted";

    // Second decision on read 3 recovers, so halcompat now accepts; that honest outcome is
    // asserted rather than worked around.
    const bool secondVerdict = halcompat::isCompatible<cechal::IHdmiCec>(service);
    ASSERT_TRUE(secondVerdict)
        << "the third read did not recover, so this double is not modelling DELAYED recovery";

    // And the recovered values, put through the emitter, are reported as changed metadata.
    std::string secondEmission;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        secondEmission = capture.read();
    }
    EXPECT_THAT(secondEmission, ::testing::HasSubstr("WOULD itself have been accepted"))
        << "recovered metadata was not reported as changed or recovered";
    EXPECT_THAT(secondEmission, ::testing::Not(::testing::HasSubstr("because the interface version")))
        << "a version cause was asserted for a compatible observation";
}

/**
 * @brief A failed observation costs a less specific message and cannot relabel the cause.
 *
 * @pre Runs under every invocation, driving the production emitter against
 *      ThrowingMetadataDouble.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, AFailedObservationEmitsTheFallbackMessageAndPreservesTheCause) {
    const ::android::sp<cechal::IHdmiCec> service =
        ::android::sp<ThrowingMetadataDouble>::make();

    // The emitter is static and cannot reach an instance's recorded cause; this asserts the
    // observable half of that guarantee.
    DriverAidlImpl backEnd;
    const std::string reasonBefore(backEnd.unavailabilityReason() != NULL
                                       ? backEnd.unavailabilityReason()
                                       : "");

    std::string emitted;
    {
        StdoutCapture capture;
        // Must not propagate: isServiceAvailable()'s own handler would relabel the rejection
        // as REASON_QUERY_FAILED.
        ASSERT_NO_THROW(
            DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec")));
        emitted = capture.read();
    }

    EXPECT_THAT(emitted, ::testing::HasSubstr("could not be read back afterwards"))
        << "a failed observation did not produce the reduced message";
    EXPECT_THAT(emitted, ::testing::HasSubstr("its recorded cause unchanged"))
        << "the reduced message no longer states that the recorded cause is unaffected, which "
           "is the property that stops a failed diagnostic being read as a different fault";
    EXPECT_THAT(emitted, ::testing::HasSubstr("REJECTED AS NOT COMPATIBLE"))
        << "the reduced message dropped the rejection itself and reports only the read failure";
    EXPECT_THAT(emitted, ::testing::Not(::testing::HasSubstr("OBSERVED AFTER the decision")))
        << "the detailed observation wording was emitted even though no snapshot was obtained";

    const std::string reasonAfter(backEnd.unavailabilityReason() != NULL
                                      ? backEnd.unavailabilityReason()
                                      : "");
    EXPECT_EQ(reasonBefore, reasonAfter)
        << "the emitter changed an instance's recorded unavailability reason. It is static and "
           "must be incapable of that; if this ever fails, the emitter has grown access to "
           "instance state and the preserve-cause guarantee is no longer structural";
}

// The four cases below test the fakes' own interface-metadata controls; none registers a
// fake, because registration needs a binder driver and these cases also run without one.
/**
 * @brief The fake service reports the interface metadata installed on it, and says when it
 *        diverges.
 * @pre Runs under every invocation, on a locally constructed, never registered fake.
 * @note Defaults are reported quietly; each installed value is reported and traced.
 * @see FakeHdmiCecService::setInterfaceVersion(), FakeHdmiCecService::setInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheServiceFakeReportsTheInterfaceMetadataInstalledOnIt) {
    const ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    ASSERT_TRUE(fake != nullptr);

    // Defaults first, under capture, so the quiet arm of both traces is pinned rather than
    // assumed. A fake that traced unconditionally would print here.
    int32_t defaultVersion = 0;
    std::string defaultHash;
    std::string defaultOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence traces cannot be read; "
               "failing rather than asserting against an empty capture";

        defaultVersion = fake->getInterfaceVersion();
        defaultHash = fake->getInterfaceHash();

        defaultOutput = capture.read();
    }

    EXPECT_EQ(defaultVersion, kClientInterfaceVersion)
        << "an unconfigured fake service did not report the compiled-in interface version, so "
           "every AIDL-selected invocation would be driving a service the middleware must "
           "refuse - and the compatible case would have no way to occur";
    EXPECT_EQ(defaultHash, std::string(kFrozenInterfaceHash))
        << "an unconfigured fake service did not report the compiled-in interface hash";
    EXPECT_THAT(defaultOutput, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a fake reporting its own compiled-in metadata still printed a divergence line. The "
           "trace is conditional precisely so that the one line a run does print marks the "
           "moment the fake was made to diverge";

    // The hash half. This is the control the L1 harness's incompatible mode uses, so the value
    // installed here is the value that mode installs.
    int32_t versionAfterHash = 0;
    std::string installedHash;
    std::string hashOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        fake->setInterfaceHash(std::string(kBrokenInterfaceHash));
        installedHash = fake->getInterfaceHash();
        versionAfterHash = fake->getInterfaceVersion();

        hashOutput = capture.read();
    }

    EXPECT_EQ(installedHash, std::string(kBrokenInterfaceHash))
        << "the fake service did not report the hash installed on it, so invocation C's "
           "present-but-incompatible arm could not be arranged at all";
    EXPECT_EQ(versionAfterHash, kClientInterfaceVersion)
        << "installing a hash disturbed the reported version. The two are separate members "
           "deliberately: a case that installs one must be able to rely on the other";
    EXPECT_THAT(hashOutput, ::testing::HasSubstr("setInterfaceHash] Hash set from"))
        << "the hash setter installed a value without tracing it, so a captured log would carry "
           "a divergence line below with no cause anywhere above it";
    EXPECT_THAT(hashOutput, ::testing::HasSubstr("getInterfaceHash] Reporting overridden hash"))
        << "the hash getter reported a divergent value without tracing it";

    // The version half: an installed version is reported without disturbing the hash, and both
    // its setter and its getter trace it.
    int32_t installedVersion = 0;
    std::string hashAfterVersion;
    std::string versionOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        fake->setInterfaceVersion(kDivergentReportedVersion);
        installedVersion = fake->getInterfaceVersion();
        hashAfterVersion = fake->getInterfaceHash();

        versionOutput = capture.read();
    }

    EXPECT_EQ(installedVersion, kDivergentReportedVersion)
        << "the fake service did not report the interface version installed on it. AAP 0.4.1 "
           "requires this control, and without it the service's version-divergence trace has no "
           "caller that can reach it";
    EXPECT_EQ(hashAfterVersion, std::string(kBrokenInterfaceHash))
        << "installing a version reverted or disturbed the hash installed earlier";
    EXPECT_THAT(versionOutput, ::testing::HasSubstr("setInterfaceVersion] Version set from"))
        << "the version setter installed a value without tracing it";
    // The value is spelled from the constant rather than as a literal, so changing the constant
    // cannot leave this expectation matching a number nothing installs any more.
    EXPECT_THAT(versionOutput,
                ::testing::HasSubstr("getInterfaceVersion] Reporting overridden version: "
                                     + std::to_string(kDivergentReportedVersion)))
        << "the service's version-divergence trace did not fire for a divergent version. That "
           "branch is the one this case exists to drive, and a getter that reported the "
           "installed value without tracing it would leave it unreached";
}

/**
 * @brief The fake service restores both metadata values on reset(), so an override cannot
 *        outlive the case that installed it.
 * @pre Runs under every invocation, on a locally constructed, never registered fake.
 * @note The harness registers one fake for the whole process, so a surviving value would
 *       decide every later case.
 * @see FakeHdmiCecService::reset()
 */
TEST_F(DriverAidlCompatibilityTest, TheServiceFakeRestoresBothMetadataValuesOnReset) {
    const ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    ASSERT_TRUE(fake != nullptr);

    fake->setInterfaceHash(std::string(kDivergentReportedHash));
    fake->setInterfaceVersion(kDivergentReportedVersion);

    // The premise: both values really did change. Without this the case could pass on a fake
    // whose setters never worked.
    ASSERT_EQ(fake->getInterfaceHash(), std::string(kDivergentReportedHash))
        << "the hash was not installed, so this case would assert restoration of a value that "
           "was never disturbed";
    ASSERT_EQ(fake->getInterfaceVersion(), kDivergentReportedVersion)
        << "the version was not installed, for the same reason";

    fake->reset();

    int32_t restoredVersion = 0;
    std::string restoredHash;
    std::string afterReset;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        restoredVersion = fake->getInterfaceVersion();
        restoredHash = fake->getInterfaceHash();

        afterReset = capture.read();
    }

    EXPECT_EQ(restoredHash, std::string(kFrozenInterfaceHash))
        << "reset() did not restore the compiled-in interface hash, so a case that made the "
           "service incompatible would leave every later case running against a service the "
           "middleware refuses";
    EXPECT_EQ(restoredVersion, kClientInterfaceVersion)
        << "reset() did not restore the compiled-in interface version. The hash and the version "
           "are restored by the same critical section, so one surviving means the pair has "
           "drifted apart";
    EXPECT_THAT(afterReset, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a getter still reported divergence after reset(), which means it is reporting a "
           "value reset() did not reach";
}

/**
 * @brief The fake controller reports the interface metadata installed on it, and says when it
 *        diverges.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @note The middleware checks only the service's metadata, so this case makes no
 *       compatibility claim.
 * @see FakeHdmiCecController::setInterfaceVersion(), FakeHdmiCecController::setInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeReportsTheInterfaceMetadataInstalledOnIt) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    int32_t defaultVersion = 0;
    std::string defaultHash;
    std::string defaultOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence traces cannot be read; "
               "failing rather than asserting against an empty capture";

        defaultVersion = controller->getInterfaceVersion();
        defaultHash = controller->getInterfaceHash();

        defaultOutput = capture.read();
    }

    EXPECT_EQ(defaultVersion, kControllerClientInterfaceVersion)
        << "an unconfigured fake controller did not report the compiled-in controller interface "
           "version";
    EXPECT_EQ(defaultHash, std::string(kControllerFrozenInterfaceHash))
        << "an unconfigured fake controller did not report the compiled-in controller interface "
           "hash";
    EXPECT_THAT(defaultOutput, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a controller reporting its own compiled-in metadata still printed a divergence line";

    int32_t installedVersion = 0;
    std::string installedHash;
    std::string divergentOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        controller->setInterfaceHash(std::string(kDivergentReportedHash));
        controller->setInterfaceVersion(kDivergentReportedVersion);

        installedHash = controller->getInterfaceHash();
        installedVersion = controller->getInterfaceVersion();

        divergentOutput = capture.read();
    }

    EXPECT_EQ(installedHash, std::string(kDivergentReportedHash))
        << "the fake controller did not report the hash installed on it. AAP 0.4.1 requires the "
           "fake to offer overridable metadata, and this class had no metadata setter at all";
    EXPECT_EQ(installedVersion, kDivergentReportedVersion)
        << "the fake controller did not report the version installed on it";
    EXPECT_THAT(divergentOutput, ::testing::HasSubstr("setInterfaceHash] Hash set from"))
        << "the controller's hash setter installed a value without tracing it";
    EXPECT_THAT(divergentOutput, ::testing::HasSubstr("setInterfaceVersion] Version set from"))
        << "the controller's version setter installed a value without tracing it";
    EXPECT_THAT(divergentOutput,
                ::testing::HasSubstr("FakeHdmiCecController::getInterfaceHash] Reporting "
                                     "overridden hash"))
        << "the controller's hash-divergence trace did not fire for a divergent hash. That "
           "branch is one of the two this case exists to drive";
    EXPECT_THAT(divergentOutput,
                ::testing::HasSubstr("FakeHdmiCecController::getInterfaceVersion] Reporting "
                                     "overridden version: "
                                     + std::to_string(kDivergentReportedVersion)))
        << "the controller's version-divergence trace did not fire for a divergent version. That "
           "branch is the other one, and the class-qualified prefix is asserted so that the "
           "service's identically worded line cannot satisfy this expectation";
}

/**
 * @brief The fake controller restores both metadata values on reset().
 *
 * @pre Runs under every invocation, on a locally constructed controller.
 * @note The service's reset() does not touch its controller, so a case configuring both must
 *       reset both.
 * @see FakeHdmiCecController::reset(), FakeHdmiCecService::reset()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeRestoresBothMetadataValuesOnReset) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    controller->setInterfaceHash(std::string(kDivergentReportedHash));
    controller->setInterfaceVersion(kDivergentReportedVersion);

    ASSERT_EQ(controller->getInterfaceHash(), std::string(kDivergentReportedHash))
        << "the hash was not installed, so this case would assert restoration of a value that "
           "was never disturbed";
    ASSERT_EQ(controller->getInterfaceVersion(), kDivergentReportedVersion)
        << "the version was not installed, for the same reason";

    controller->reset();

    int32_t restoredVersion = 0;
    std::string restoredHash;
    std::string afterReset;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        restoredVersion = controller->getInterfaceVersion();
        restoredHash = controller->getInterfaceHash();

        afterReset = capture.read();
    }

    EXPECT_EQ(restoredHash, std::string(kControllerFrozenInterfaceHash))
        << "reset() did not restore the controller's compiled-in interface hash";
    EXPECT_EQ(restoredVersion, kControllerClientInterfaceVersion)
        << "reset() did not restore the controller's compiled-in interface version, so an "
           "override would outlive the case that installed it";
    EXPECT_THAT(afterReset, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a getter still reported divergence after reset()";
}

/**
 * @brief By default the fake controller refuses an add IHdmiCecController forbids - a duplicate,
 *        an address outside 0..14 or a mixed request - and registers nothing for it.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @see FakeHdmiCecController::addLogicalAddresses()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeRefusesAnAddTheContractForbids) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    bool added = false;
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &added).isOk());
    EXPECT_TRUE(added) << "a free address inside 0..14 was refused";
    ASSERT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    const std::vector<std::vector<int32_t>> refused = { { 4 }, { -1 }, { 15 }, { 5, 15 }, { 6, 4 } };

    for (size_t i = 0; i < refused.size(); i++) {
        added = true;
        ASSERT_TRUE(controller->addLogicalAddresses(refused[i], &added).isOk()) << "request " << i;
        EXPECT_FALSE(added)
            << "request " << i << " reported success although the contract requires false for an "
               "already added or out-of-range address";
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }))
            << "refused request " << i << " changed the registrations";
        EXPECT_EQ(controller->getLastAddedLogicalAddresses(), refused[i]) << "request " << i;
    }

    added = false;
    ASSERT_TRUE(controller->addLogicalAddresses({ 0, 14 }, &added).isOk());
    EXPECT_TRUE(added) << "the range bounds 0 and 14 were refused";
    added = false;
    ASSERT_TRUE(controller->addLogicalAddresses({}, &added).isOk());
    EXPECT_TRUE(added) << "an empty request was refused";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4, 0, 14 }));
    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 8);
}

/**
 * @brief By default the fake controller refuses a removal IHdmiCecController forbids - an absent
 *        address, one outside 0..14 or a mixed request - and deregisters nothing for it.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @see FakeHdmiCecController::removeLogicalAddresses()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeRefusesARemovalTheContractForbids) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    bool result = false;
    ASSERT_TRUE(controller->addLogicalAddresses({ 4, 5 }, &result).isOk());
    ASSERT_TRUE(result);

    const std::vector<std::vector<int32_t>> refused = { { 6 }, { -1 }, { 15 }, { 4, 6 }, { 5, 15 } };

    for (size_t i = 0; i < refused.size(); i++) {
        result = true;
        ASSERT_TRUE(controller->removeLogicalAddresses(refused[i], &result).isOk()) << "request " << i;
        EXPECT_FALSE(result)
            << "request " << i << " reported success although the contract requires false for an "
               "address not added or out of range";
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4, 5 }))
            << "refused request " << i << " changed the registrations";
        EXPECT_EQ(controller->getLastRemovedLogicalAddresses(), refused[i]) << "request " << i;
    }

    result = false;
    ASSERT_TRUE(controller->removeLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_TRUE(result) << "removing a registered address was refused";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 5 }));

    result = true;
    ASSERT_TRUE(controller->removeLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_FALSE(result) << "removing an address a second time was accepted";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 5 }));
    EXPECT_EQ(controller->getRemoveLogicalAddressesCallCount(), 7);
}

/**
 * @brief Forced results and non-ok statuses still override the fake controller's validation, and
 *        reset() restores it.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @see FakeHdmiCecController::setAddLogicalAddressesResult(),
 *      FakeHdmiCecController::setRemoveLogicalAddressesResult()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeOverridesStillApplyAndResetRestoresValidation) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);
    bool result = true;

    controller->setAddLogicalAddressesResult(false);
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_FALSE(result) << "a forced false add was reported as true";
    EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty());

    controller->setAddLogicalAddressesResult(true);
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &result).isOk());
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_TRUE(result) << "a forced true add of a held address was reported as false";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    controller->setRemoveLogicalAddressesResult(false);
    ASSERT_TRUE(controller->removeLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_FALSE(result) << "a forced false removal was reported as true";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    controller->setRemoveLogicalAddressesResult(true);
    ASSERT_TRUE(controller->removeLogicalAddresses({ 9 }, &result).isOk());
    EXPECT_TRUE(result) << "a forced true removal of an absent address was reported as false";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    controller->reset();
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &result).isOk());
    ASSERT_TRUE(result);
    ASSERT_TRUE(controller->addLogicalAddresses({ 4 }, &result).isOk());
    EXPECT_FALSE(result) << "reset() left the forced add result installed";
    ASSERT_TRUE(controller->removeLogicalAddresses({ 9 }, &result).isOk());
    EXPECT_FALSE(result) << "reset() left the forced removal result installed";

    // A write would report true for these valid requests, so a false sentinel proves no write.
    controller->setAddLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    controller->setRemoveLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    result = false;
    const ::android::binder::Status addStatus = controller->addLogicalAddresses({ 5 }, &result);
    const ::android::binder::Status removeStatus = controller->removeLogicalAddresses({ 4 }, &result);

    EXPECT_EQ(addStatus.transactionError(), ::android::DEAD_OBJECT);
    EXPECT_EQ(removeStatus.transactionError(), ::android::DEAD_OBJECT);
    EXPECT_FALSE(result) << "a non-ok status still wrote the out-parameter";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }))
        << "a non-ok status still changed the registrations";
}

/**
 * @brief The fake controller fails a self-addressed one-byte allocation poll with the installed
 *        non-ok send status, records the poll and counts it only in the total.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @see FakeHdmiCecController::setSendMessageBinderStatus(), FakeHdmiCecController::sendMessage()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeFailsAnAllocationPollWithTheInstalledSendStatus) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    controller->setSendMessageBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    // A poll that is answered writes ACK_STATE_1, so BUSY surviving proves no write.
    cechal::SendMessageStatus reported = cechal::SendMessageStatus::BUSY;
    const ::android::binder::Status status = controller->sendMessage({ 0x44 }, &reported);

    EXPECT_FALSE(status.isOk()) << "the installed DEAD_OBJECT was ignored for an allocation poll";
    EXPECT_EQ(status.transactionError(), ::android::DEAD_OBJECT);
    EXPECT_EQ(reported, cechal::SendMessageStatus::BUSY) << "a failed poll still wrote the out-parameter";
    EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 1) << "the failed poll was not counted";
    EXPECT_EQ(controller->getSendMessageCallCount(), 0) << "a poll was counted as an application frame";
    EXPECT_TRUE(controller->getLastSentMessage().empty());
}

/**
 * @brief With an ok send status the fake controller answers a poll ACK_STATE_1 (free), and the
 *        total send count covers polls and application frames while the frame count covers frames.
 * @pre Runs under every invocation, on a controller constructed directly, never registered.
 * @see FakeHdmiCecController::getTotalSendMessageCallCount(), FakeHdmiCecController::getSendMessageCallCount()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeCountsPollsInTheTotalAndFramesInBothCounts) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    cechal::SendMessageStatus reported = cechal::SendMessageStatus::BUSY;
    ASSERT_TRUE(controller->sendMessage({ 0x88 }, &reported).isOk());
    EXPECT_EQ(reported, cechal::SendMessageStatus::ACK_STATE_1) << "an unoccupied poll did not answer free";
    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 1);
    EXPECT_EQ(controller->getSendMessageCallCount(), 0);

    // One byte but not self-addressed, so an application frame (a directed poll), then a full frame.
    const std::vector<std::vector<uint8_t>> frames = { { 0x40 }, { 0x40, 0x36 } };

    for (size_t i = 0; i < frames.size(); i++) {
        reported = cechal::SendMessageStatus::BUSY;
        ASSERT_TRUE(controller->sendMessage(frames[i], &reported).isOk()) << "frame " << i;
        EXPECT_EQ(reported, cechal::SendMessageStatus::ACK_STATE_0) << "frame " << i;
        EXPECT_EQ(controller->getLastSentMessage(), frames[i]) << "frame " << i;
    }

    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 3);
    EXPECT_EQ(controller->getSendMessageCallCount(), 2);
    EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 8 }));

    controller->reset();
    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 0) << "reset() did not clear the total";
    EXPECT_EQ(controller->getSendMessageCallCount(), 0);
}

/**
 * @brief The binder preflight's decision arms, driven by direct call.
 *
 * The driver path and context-manager timeout are parameters, so absent and non-binder nodes
 * are probed without touching the real /dev/binder. The preflight guards libbinder, which
 * aborts on a missing or mismatched driver and blocks when no servicemanager runs.
 * @note Every case supplies its own path and probe state, so each passes alone, in any order.
 */
class DriverAidlPreflightTest : public ::testing::Test {
};

/**
 * @brief An empty driver path is refused outright, before anything is opened.
 * @pre Runs under every invocation; the private static predicate is called through
 *      BinderPreflightTestAccess.
 * @note Refusing explicitly stops ::open("") being attempted and its errno misreported.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesAnEmptyDriverPath) {
    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(std::string()))
        << "the preflight accepted an empty driver path, so a deployment whose path variable "
           "resolved to nothing would be told the binder transport is usable";
}

/**
 * @brief A path with nothing behind it is declined because the open fails.
 * @pre Runs under every invocation; the path is asserted absent first.
 * @note This arm makes a host without kernel binder support fall back to legacy; a regression
 *       would abort the process inside libbinder.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesANonexistentDriverPath) {
    const std::string absent = "/dev/blitzy_cec_aidl_no_such_binder_node";

    ASSERT_NE(::access(absent.c_str(), F_OK), 0)
        << "the path chosen to be absent exists on this host, so this case is not exercising the "
           "cannot-open arm; choose another path";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(absent))
        << "the preflight accepted a driver path with nothing behind it. On a platform with no "
           "binder driver this is what stands between falling back to the legacy back-end and "
           "libbinder aborting the process during initialization";
}

/**
 * @brief An openable regular file, not a character device, is declined at the node-type check.
 * @pre Runs under every invocation, on a regular file TemporaryNode creates and unlinks.
 * @note The node is asserted readable and writable first, so the verdict is not the open's.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesAPathThatOpensButIsNotABinderDriver) {
    TemporaryNode node;

    ASSERT_TRUE(node.isValid()) << "a temporary node could not be created under ["
                                << temporaryDirectory()
                                << "], so this case cannot establish its precondition";
    ASSERT_EQ(::access(node.path().c_str(), R_OK | W_OK), 0)
        << "the temporary node is not readable and writable, so the preflight would decline it at "
           "the cannot-open arm instead of at the ioctl arm this case is about";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(node.path()))
        << "the preflight accepted a path that opens but does not speak the binder protocol. "
           "Opening a node is not evidence that it is a binder driver, and handing such a node to "
           "libbinder is the condition the pin treats as fatal";
}

/**
 * @brief The two-argument overload is callable and neither end of the timeout range changes
 *        the verdict for an absent node.
 * @pre Runs under every invocation, on an absent path.
 * @note The context-manager arm is not reached here; the synthetic-probe cases reach it.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, HonoursTheContextManagerTimeoutArgumentWithoutWaitingForAbsentNode) {
    const std::string absent = "/dev/blitzy_cec_aidl_no_such_binder_node";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(absent, 0u))
        << "a zero timeout was expected to mean 'do not wait at all' and to leave the verdict for "
           "an absent node unchanged";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     absent, DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS))
        << "the default timeout changed the verdict for an absent node, which would mean the node "
           "check is no longer short-circuiting ahead of the context-manager probe";
}

/**
 * @brief The default driver path and a non-zero default bound are the values the production
 *        and harness call sites probe.
 * @pre Runs under every invocation; asserts the constants, since the verdict is the host's.
 * @see DriverAidlImpl::isServiceAvailable()
 */
TEST_F(DriverAidlPreflightTest, DefaultDriverPathIsTheNodeLibbinderOpens) {
    EXPECT_STREQ(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, "/dev/binder")
        << "the default binder driver path changed. Both the production selection and the L1 "
           "harness call isBinderPreflightOk() with no arguments, so this constant is what they "
           "probe; if the platform genuinely moved the node, update this expectation and check "
           "that nothing else hard-codes the old spelling";

    EXPECT_GT(DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, 0u)
        << "the default context-manager timeout is zero, which means the production preflight no "
           "longer waits at all for handle 0 to resolve. A platform whose servicemanager starts "
           "concurrently with the middleware would then be misread as having none";
}

/**
 * @brief A node reporting a protocol version other than this build's is declined, and the
 *        mismatch alone decides it.
 * @pre Runs under every invocation, through the substituted probe, since no path can express
 *      this condition.
 * @note The ping would succeed and is asserted never called, so the version decides.
 * @see DriverAidlImpl::expectedBinderProtocolVersion()
 */
TEST_F(DriverAidlPreflightTest, DeclinesANodeWhoseProtocolVersionDiffersFromThisBuild) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();
    const unsigned int mismatched = expected + 1u;

    ASSERT_NE(mismatched, expected)
        << "the mismatched version this case constructs is equal to the expected one, so it is "
           "not exercising the mismatch arm at all";

    // The ping would succeed if it were reached, so a false verdict can only have come from
    // the version comparison.
    resetSyntheticProbe(kSyntheticBinderFd, 0, mismatched, true);

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                     syntheticProbe()))
        << "the preflight accepted a node reporting protocol " << mismatched << " while this build "
           "expects " << expected << ". libbinder requires equality, so every open would fail - and "
           "on the pinned stack a failed open is fatal rather than an error return";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol version was not read exactly once, so the verdict did not come from the "
           "version comparison";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 0)
        << "the context manager was probed although the protocol version did not match. The "
           "mismatch must decide the verdict on its own: a predicate that continued past it would "
           "hand a protocol-mismatched node to libbinder if the ping happened to succeed";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor opened for the probe was not released on the mismatch arm";
}

/**
 * @brief A node whose protocol matches but whose context manager never answers within the
 *        bound is declined, and the caller's own bound reaches the probe.
 * @pre Runs under every invocation, through the substituted probe with the version matching.
 * @note One ping on the opened descriptor carries the caller's 250 ms, then one close.
 */
TEST_F(DriverAidlPreflightTest, DeclinesAMatchingProtocolWhoseContextManagerNeverAnswers) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    const unsigned int chosenTimeoutMs = 250u;

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, chosenTimeoutMs, syntheticProbe()))
        << "the preflight accepted a node whose context manager did not answer. Reaching the "
           "service manager anyway is what blocks LibCCEC::init indefinitely, since obtaining an "
           "IServiceManager polls for handle 0 with no bound of its own";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once, so this verdict did not come from the "
           "context-manager arm";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, chosenTimeoutMs)
        << "the caller's deadline was not the one handed to the probe. The bound is the whole point "
           "of this arm: a predicate that substituted a deadline of its own would make the "
           "production timeout unreachable and untestable";
    EXPECT_EQ(g_syntheticProbe.lastPingFd, kSyntheticBinderFd)
        << "the context manager was probed on a descriptor other than the one that was opened and "
           "version-checked";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released after the context-manager probe";
}

/**
 * @brief An openable, root-owned character device that reports this build's protocol and
 *        answers on handle 0 is accepted, after all eight decision points pass in order.
 * @pre Runs under every invocation, through the substituted probe.
 * @note One O_RDWR open, one version read, one ping and exactly one close are asserted.
 */
TEST_F(DriverAidlPreflightTest, AcceptsANodeWhoseProtocolMatchesAndWhoseContextManagerAnswers) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    EXPECT_TRUE(BinderPreflightTestAccess::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                    syntheticProbe()))
        << "the preflight declined a node that opened, reported this build's protocol version ("
        << expected << ") and whose context manager answered. With this arm broken the AIDL "
           "back-end can never be selected on any platform, however well provisioned, and every "
           "rejection case in this fixture would still pass";

    EXPECT_EQ(g_syntheticProbe.openCalls, 1) << "the node was not opened exactly once";
    EXPECT_EQ(g_syntheticProbe.lastOpenedPath, std::string(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH))
        << "the preflight opened a path other than the one it was given";
    EXPECT_EQ(g_syntheticProbe.lastOpenFlags & O_RDWR, O_RDWR)
        << "the node was not opened for reading and writing both. A binder transaction needs "
           "both, so a "
           "read-only descriptor would pass the version check and then fail every transaction";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol version was not read exactly once on the accepting path";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed on the accepting path, so the verdict was reached "
           "without establishing that anything answers handle 0";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the probe descriptor was not released on the accepting path, which is the path that "
           "runs at every initialization on a binder-capable platform";
}

/**
 * @brief A bound above the ceiling is clamped to the ceiling before the probe is made.
 * @pre Runs under every invocation, through the substituted probe with the ping declining.
 * @note The probe records the bound it was handed, so the effective value is read, not timed.
 */
TEST_F(DriverAidlPreflightTest, ClampsAContextManagerTimeoutAboveTheCeilingToTheCeiling) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    const unsigned int requested = DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS * 6u;

    ASSERT_GT(requested, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the requested bound is not above the ceiling, so this case would exercise the "
           "pass-through arm rather than the clamp";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, requested, syntheticProbe()))
        << "the verdict changed for a node whose context manager does not answer, so this case is "
           "no longer reaching the context-manager arm at all";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once, so the recorded bound below did not "
           "come from this call";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the probe was handed " << g_syntheticProbe.lastPingTimeoutMs << " ms where the ceiling "
           "is " << DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS << " ms. An unclamped bound is "
           "time LibCCEC::init() spends blocked before it can fall back to the legacy back-end, "
           "which is the one thing the preflight exists to prevent";
}

/**
 * @brief A bound exactly at the ceiling is handed through unchanged.
 * @pre Runs under every invocation, through the substituted probe with the ping declining.
 * @note The boundary value is the only one at which `>` and `>=` differ.
 */
TEST_F(DriverAidlPreflightTest, PassesAContextManagerTimeoutAtTheCeilingThroughUnchanged) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS,
                     syntheticProbe()))
        << "the verdict changed for a node whose context manager does not answer";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "a bound exactly at the ceiling was altered on its way to the probe, so the clamp is "
           "using the wrong comparison and every request at the ceiling is being lowered";
}

/**
 * @brief The default bound is below the ceiling, so the clamp cannot fire on the production
 *        call path.
 * @pre Runs under every invocation; compares two constants and calls nothing.
 */
TEST_F(DriverAidlPreflightTest, TheDefaultContextManagerTimeoutSitsBelowTheCeiling) {
    EXPECT_LT(DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
              DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the default context-manager timeout ("
        << DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS << " ms) is not below the ceiling ("
        << DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS << " ms), so the production call path "
           "would take the clamp arm at every initialization and the default would be unreachable";
}

/**
 * @brief Each configured row (empty path, failed open, failed version read, version mismatch,
 *        declined ping, answered ping) releases exactly the descriptors it opened.
 * @pre Runs under every invocation, sweeping those six rows through one table driven by the
 *      substituted probe.
 * @note The rule is one close per successful open, so the empty-path and failed-open rows
 *       expect none.
 */
TEST_F(DriverAidlPreflightTest, EveryPreflightArmReleasesExactlyTheDescriptorsItOpened) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    /** @brief One row of the sweep: how the probe is configured, and what must follow. */
    struct Arm {
        /** @brief Which decision point this row stops at, quoted in every failure message. */
        const char *name;
        /** @brief Driver path handed to the predicate. */
        const char *path;
        /** @brief openNode()'s answer for this row. */
        int  answerOpen;
        /** @brief readProtocolVersion()'s answer for this row. */
        int  answerProtocolRead;
        /** @brief Protocol version reported on a successful read. */
        unsigned int answerProtocolVersion;
        /** @brief pingContextManager()'s answer for this row. */
        bool answerPing;
        /** @brief The verdict the predicate must reach. */
        bool expectedVerdict;
        /** @brief How many opens this row must perform. */
        int  expectedOpens;
        /** @brief How many closes must follow - one per successful open, and zero otherwise. */
        int  expectedCloses;
    };

    const Arm arms[] = {
        { "decision point 1: empty path",        "",  kSyntheticBinderFd,  0, expected,      true,  false, 0, 0 },
        { "decision point 2: open fails",        "/dev/binder", -1,        0, expected,      true,  false, 1, 0 },
        { "decision point 6: ioctl fails",       "/dev/binder", kSyntheticBinderFd, -1, expected, true, false, 1, 1 },
        { "decision point 7: protocol mismatch", "/dev/binder", kSyntheticBinderFd,  0, expected + 1u, true, false, 1, 1 },
        { "decision point 8: ping declines",     "/dev/binder", kSyntheticBinderFd,  0, expected,      false, false, 1, 1 },
        { "decision point 8: ping answers",      "/dev/binder", kSyntheticBinderFd,  0, expected,      true,  true,  1, 1 },
    };

    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        const Arm &arm = arms[i];

        resetSyntheticProbe(arm.answerOpen, arm.answerProtocolRead, arm.answerProtocolVersion,
                            arm.answerPing);

        const bool verdict = BinderPreflightTestAccess::isBinderPreflightOk(
            std::string(arm.path), DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
            syntheticProbe());

        EXPECT_EQ(verdict, arm.expectedVerdict)
            << "unexpected verdict on " << arm.name;
        EXPECT_EQ(g_syntheticProbe.openCalls, arm.expectedOpens)
            << "unexpected number of opens on " << arm.name;
        EXPECT_EQ(g_syntheticProbe.closeCalls, arm.expectedCloses)
            << "the descriptor accounting is wrong on " << arm.name << ": " << arm.expectedOpens
            << " successful open(s) were expected to be matched by " << arm.expectedCloses
            << " close(s), and " << g_syntheticProbe.closeCalls << " were made. A preflight that "
               "leaks a /dev/binder descriptor leaks a driver context for the life of the process; "
               "one that closes a negative descriptor is a bug of its own";

        if (arm.expectedCloses > 0) {
            EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
                << "the descriptor released on " << arm.name << " is not the one that was opened";
        }
    }
}

/**
 * @brief The POSIX node constants DriverAidlImpl restates match the system headers.
 * @pre Runs under every invocation; compares constants and calls nothing.
 * @note A drifted file-type mask would decline the AIDL path on every correctly provisioned
 *       platform.
 */
TEST_F(DriverAidlPreflightTest, TheRestatedPosixNodeConstantsMatchTheSystemHeaders) {
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK, static_cast<unsigned int>(S_IFMT))
        << "the restated file-type mask (0" << std::oct << DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK
        << ") differs from S_IFMT (0" << static_cast<unsigned int>(S_IFMT) << std::dec
        << "), so the preflight extracts the wrong bits from st_mode and its character-device "
           "test means nothing";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE, static_cast<unsigned int>(S_IFCHR))
        << "the restated character-device type (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE << ") differs from S_IFCHR (0"
        << static_cast<unsigned int>(S_IFCHR) << std::dec << "), so every real /dev/binder "
           "would be refused and the AIDL path could never be selected on any platform";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_REQUIRED_OWNER_UID, 0u)
        << "the required owner is no longer root. devtmpfs and binderfs both create the binder "
           "node as root, so anything else here either refuses every real platform or accepts a "
           "node an unprivileged process could have created";

    // The three permission constants, checked the same way: a wrong mask makes the
    // permissive-node diagnostic report the wrong bits, or nothing.
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              static_cast<unsigned int>(S_IRWXU | S_IRWXG | S_IRWXO))
        << "the restated permission mask (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK << ") differs from S_IRWXU|S_IRWXG|"
        << "S_IRWXO (0" << static_cast<unsigned int>(S_IRWXU | S_IRWXG | S_IRWXO) << std::dec
        << "), so the permission bits reported for a permissive binder node are the wrong bits";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE, static_cast<unsigned int>(S_IWGRP))
        << "the restated group-write bit (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE << ") differs from S_IWGRP (0"
        << static_cast<unsigned int>(S_IWGRP) << std::dec << "), so a group-writable binder node "
           "would go unreported";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE, static_cast<unsigned int>(S_IWOTH))
        << "the restated world-write bit (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE << ") differs from S_IWOTH (0"
        << static_cast<unsigned int>(S_IWOTH) << std::dec << "), so a world-writable binder node "
           "would go unreported";

    // The two write bits must be INSIDE the permission mask, or the line would announce a
    // condition and then print bits that cannot express it.
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE &
                  DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE)
        << "the group-write bit falls outside the permission mask the diagnostic prints";
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE &
                  DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE)
        << "the world-write bit falls outside the permission mask the diagnostic prints";

    // And neither may collide with the file-type bits, or the observation would fire on the
    // node's TYPE and the character-device check would be reading permission bits.
    EXPECT_EQ((DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE |
               DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE) &
                  DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              0u)
        << "the write bits overlap the file-type mask, so the two readings of st_mode - type and "
           "permissions - are no longer independent";
}

/**
 * @brief A node that opens but whose identity cannot be read is declined before the protocol
 *        ioctl.
 * @pre Runs under every invocation, through the substituted probe.
 * @note The identity is what the pre-lookup re-check compares against; the protocol read is
 *       asserted never attempted.
 */
TEST_F(DriverAidlPreflightTest, DeclinesANodeWhoseDescriptorCannotBeIdentified) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerIdentifyDescriptor = -1;

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a node it could not identify, so it certified a pathname with "
           "no evidence about what the name refers to and nothing to re-verify before libbinder "
           "opens the same name";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 1)
        << "the identity was not asked for exactly once";
    EXPECT_EQ(g_syntheticProbe.lastIdentifiedFd, kSyntheticBinderFd)
        << "the identity was asked of something other than the descriptor that was opened. Asking "
           "the PATH instead would defeat the purpose: the answer could change underneath the "
           "process, which is precisely the window this check closes";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node whose identity had already failed, so the "
           "identity arm no longer sits where it is documented to sit";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the identity-failure arm";
}

/**
 * @brief A node that is not a character device is declined.
 * @pre Runs under every invocation, through the substituted probe; only the file type differs
 *      from a good node.
 */
TEST_F(DriverAidlPreflightTest, DeclinesANodeThatIsNotACharacterDevice) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity.mode = static_cast<unsigned int>(S_IFREG | 0666);

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a REGULAR FILE as the binder driver. Every supported binder "
           "layout publishes a character device, so this node is not the driver, and a node an "
           "unprivileged process could create is exactly what must not be selected as the HAL";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node that is not a character device";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the file-type arm";
}

/**
 * @brief A character device not owned by root is declined.
 * @pre Runs under every invocation, through the substituted probe; only the owner differs
 *      from a good node.
 * @note devtmpfs and binderfs create the node as root, so another owner means an unprivileged
 *       process could have created or replaced it.
 */
TEST_F(DriverAidlPreflightTest, DeclinesACharacterDeviceThatIsNotOwnedByRoot) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity.uid = 1000u;

    ASSERT_EQ(g_syntheticProbe.answerDescriptorIdentity.mode &
                  DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE)
        << "this case is meant to isolate OWNERSHIP, and the node it configures is not a "
           "character device, so it would be refused one arm earlier";

    EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a binder node owned by an unprivileged user. Anything able to "
           "create that node is able to interpose on every CEC frame the middleware sends and "
           "receives, which is why ownership is refused rather than merely logged";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node that is not owned by root";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the ownership arm";
}

/**
 * @brief The validated descriptor and its identity are retained only when custody is
 *        requested; otherwise the descriptor is released.
 * @pre Runs under every invocation, through the substituted probe.
 * @note A retained descriptor pins the inode until libbinder reopens the same pathname.
 */
TEST_F(DriverAidlPreflightTest, RetainsTheValidatedDescriptorAndItsIdentityOnlyWhenCustodyIsRequested) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    int retained = 999;
    DriverAidlImpl::BinderNodeIdentity identity;

    memset(&identity, 0, sizeof(identity));

    ASSERT_TRUE(BinderPreflightTestAccess::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe(),
                    &retained, &identity))
        << "the preflight declined a well-formed node when custody was requested, so the custody "
           "parameters have changed the verdict rather than only what happens to the descriptor";

    EXPECT_EQ(retained, kSyntheticBinderFd)
        << "custody was requested and the validated descriptor was not handed over";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 0)
        << "the descriptor was released even though custody was requested. The caller then holds a "
           "number that no longer refers to anything, and the identity it is about to re-verify is "
           "no longer pinned - which is the whole of what custody buys";

    EXPECT_EQ(identity.device, kValidatedNodeIdentity.device)
        << "the retained identity does not carry the validated node's device";
    EXPECT_EQ(identity.inode, kValidatedNodeIdentity.inode)
        << "the retained identity does not carry the validated node's inode, which is the field a "
           "substitution changes";
    EXPECT_EQ(identity.rdev, kValidatedNodeIdentity.rdev)
        << "the retained identity does not carry the validated node's device numbers";
    EXPECT_EQ(identity.uid, kValidatedNodeIdentity.uid)
        << "the retained identity does not carry the validated node's owner";

    // The other half: no custody requested, so the descriptor is released and nothing is left open.
    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    ASSERT_TRUE(BinderPreflightTestAccess::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight declined the same well-formed node when custody was NOT requested";

    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was retained by a caller that asked for nothing. Every negative-arm case "
           "here, and the harness's own preflight assertion, would then leak a /dev/binder "
           "descriptor - a driver context nothing reclaims until the process exits";
    EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
        << "the descriptor released is not the one that was opened";
}

/**
 * @brief Every decline clears the custody slot to -1, so no caller sees a stale descriptor.
 * @pre Runs under every invocation, sweeping all eight decision points through the
 *      substituted probe.
 */
TEST_F(DriverAidlPreflightTest, ClearsTheCustodySlotOnEveryDeclineSoNoStaleDescriptorIsReported) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    /** @brief One decline row: the probe's answers and the decision point they stop at. */
    struct Decline {
        const char *name;
        const char *path;
        int  answerOpen;
        int  answerIdentifyDescriptor;
        unsigned int identityMode;
        unsigned int identityUid;
        int  answerProtocolRead;
        unsigned int answerProtocolVersion;
        bool answerPing;
    };

    const unsigned int goodMode = static_cast<unsigned int>(S_IFCHR | 0600);

    const Decline declines[] = {
        { "decision point 1: empty path",           "",            kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected,      true  },
        { "decision point 2: open fails",           "/dev/binder", -1,                  0, goodMode, 0u,    0,  expected,      true  },
        { "decision point 3: identity unreadable",  "/dev/binder", kSyntheticBinderFd, -1, goodMode, 0u,    0,  expected,      true  },
        { "decision point 4: not a character node", "/dev/binder", kSyntheticBinderFd,  0, static_cast<unsigned int>(S_IFREG | 0666), 0u, 0, expected, true },
        { "decision point 5: not owned by root",    "/dev/binder", kSyntheticBinderFd,  0, goodMode, 1000u, 0,  expected,      true  },
        { "decision point 6: ioctl fails",          "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    -1, expected,      true  },
        { "decision point 7: protocol mismatch",    "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected + 1u, true  },
        { "decision point 8: ping declines",        "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected,      false },
    };

    for (size_t i = 0; i < sizeof(declines) / sizeof(declines[0]); i++) {
        const Decline &decline = declines[i];

        resetSyntheticProbe(decline.answerOpen, decline.answerProtocolRead,
                            decline.answerProtocolVersion, decline.answerPing);
        g_syntheticProbe.answerIdentifyDescriptor = decline.answerIdentifyDescriptor;
        g_syntheticProbe.answerDescriptorIdentity.mode = decline.identityMode;
        g_syntheticProbe.answerDescriptorIdentity.uid = decline.identityUid;

        int retained = kSyntheticBinderFd;
        DriverAidlImpl::BinderNodeIdentity identity;

        memset(&identity, 0, sizeof(identity));

        EXPECT_FALSE(BinderPreflightTestAccess::isBinderPreflightOk(
                         std::string(decline.path),
                         DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe(),
                         &retained, &identity))
            << "unexpected verdict on " << decline.name;
        EXPECT_EQ(retained, -1)
            << "the custody slot still held " << retained << " after " << decline.name
            << ". A caller reading it would either release a descriptor it does not own or believe "
               "it holds a custody window that was never opened";
    }
}

// The pre-lookup re-verification cases drive isServiceAvailable() on a local instance and
// decline before the service lookup, so nothing reaches libbinder.
/**
 * @brief The service query declines when the name resolves to a different node between the
 *        check and the use.
 * @pre Runs under every invocation, through the substituted probe.
 * @note One path resolution, no second open and the retained descriptor's release locate the
 *       decline at the identity comparison.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheNameResolvesToADifferentNodeBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerPathIdentity = kSubstitutedNodeIdentity;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query proceeded to the service lookup after the driver node had been substituted "
           "between the preflight and the lookup";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "a decline recorded no reason, so the factory has nothing to name in its fallback line";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a substituted driver node was reported as a SERVICE problem. What failed is the "
           "transport, and naming the service would send an integrator looking in the wrong place";

    EXPECT_EQ(g_syntheticProbe.identifyPathCalls, 1)
        << "the pathname was not re-resolved exactly once immediately before the lookup";
    EXPECT_EQ(g_syntheticProbe.lastIdentifiedPath,
              std::string(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH))
        << "the re-resolution was performed on a different path from the one the preflight "
           "validated, so the comparison is not about the name libbinder will open";
    EXPECT_EQ(g_syntheticProbe.openCalls, 1)
        << "the node was reopened for the liveness check even though the identity comparison had "
           "already failed, so the decline is happening later than it should";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the retained descriptor was not released on the identity-mismatch decline";
    EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
        << "the descriptor released is not the retained one";
}

/**
 * @brief The service query declines when the path cannot be resolved between the check and
 *        the use.
 * @pre Runs under every invocation, through the substituted probe.
 * @note A failed re-resolution is refused, nonfatally, never treated as "unchanged".
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenThePathCannotBeResolvedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerIdentifyPath = -1;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "a pathname that could no longer be resolved was treated as unchanged, which is "
           "indistinguishable from not re-checking it at all";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "an unresolvable driver node was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.openCalls, 1)
        << "the node was reopened after the re-resolution had already failed";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the retained descriptor was not released when the re-resolution failed";
}

/**
 * @brief The service query declines at its first stage when the driver path is unusable, and
 *        does so before touching the node.
 * @pre Runs under every invocation, including on a host with a working binder driver.
 * @note The empty path is refused before the probe is called, so the probe counters at zero
 *       locate the decline at the first stage.
 * @see DriverAidlImpl::isServiceAvailable(), DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesAtThePreflightStageWhenTheDriverPathIsUnusable) {
    // A probe that would answer every question correctly is the control: the counters show
    // it was never consulted.
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(std::string(),
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "an unusable binder driver path did not decline. This is the arm a legacy-only SOC "
           "takes on every boot, and a true return here would send the factory on to a lookup "
           "against a node it never validated";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "the preflight declined and recorded no reason, so the selection helper has nothing to "
           "name in its fallback line and the platform condition becomes undiagnosable";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a preflight decline was not reported as a transport condition, so a legacy-only "
           "platform would be told something other than the truth about why it fell back";

    EXPECT_EQ(g_syntheticProbe.openCalls, 0)
        << "the node was opened although the path could never have been usable, so the decline is "
           "happening after stage one rather than at it - and the descriptor custody the later "
           "stages depend on was taken out for a path that was already refused";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 0)
        << "a descriptor was released although none should ever have been opened";
}

/**
 * @brief The service query declines when the descriptor reopened for the liveness check is
 *        not the validated node.
 * @pre Runs under every invocation, through the substituted probe.
 * @note The reopen is a second resolution of the name, so it is compared against the same
 *       validated identity.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheReopenedDescriptorIsNotTheValidatedNode) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kSubstitutedNodeIdentity;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the descriptor reopened for the liveness check was trusted without being identified "
           "against the node the preflight validated, so the second open is an unguarded second "
           "resolution of the same name";

    EXPECT_EQ(g_syntheticProbe.openCalls, 2)
        << "the liveness check did not open a second descriptor";
    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor that had already failed its identity "
           "comparison";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors - the retained one and the reopened one - were expected to be "
           "released, and " << g_syntheticProbe.closeCalls << " release(s) were made";
}

/**
 * @brief The service query declines when the descriptor reopened for the liveness check
 *        cannot be identified at all.
 * @pre Runs under every invocation, through a probe failing only its second identification.
 * @note A separate arm from the case above: an unidentifiable descriptor is refused, not
 *       trusted.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheReopenedDescriptorCannotBeIdentified) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentifyResult = true;
    g_syntheticProbe.answerIdentifyDescriptorSecond = -1;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query proceeded to the service lookup over a reopened descriptor whose identity "
           "could not be read, so nothing established that libbinder is about to open the node "
           "the preflight validated";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "an unidentifiable reopened descriptor was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified at all, so this case did not reach the arm "
           "it is about";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor whose identity had already failed to "
           "read; the ping is the LAST of the four re-verification points and must not run once "
           "an earlier one has declined";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made; a descriptor that failed its identity read is still a "
           "descriptor this check opened";
}

/**
 * @brief The service query declines, under the same bound, when the context manager stops
 *        answering between the preflight and the lookup.
 * @pre Runs under every invocation, through the substituted probe.
 * @note The re-ping is asserted to use the reopened descriptor, since the driver allows one
 *       transaction-buffer mapping per descriptor.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheContextManagerStopsAnsweringBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondPingAnswer = true;
    g_syntheticProbe.answerPingSecond = false;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query entered the service lookup with a context manager that had stopped "
           "answering, which is an unbounded wait inside libbinder rather than a fallback";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a context manager that stopped answering was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 2)
        << "the context manager was probed " << g_syntheticProbe.pingCalls
        << " time(s). The preflight's probe establishes liveness at CHECK time; the second probe "
           "is the one that establishes it at USE time, and without it the whole re-verification "
           "reduces to an identity comparison";
    EXPECT_EQ(g_syntheticProbe.lastPingFd, kSyntheticSecondBinderFd)
        << "the second ping was issued over the RETAINED descriptor. The binder driver permits "
           "one mapping per open descriptor for its lifetime, so that ping cannot succeed on any "
           "real platform and the AIDL path would be declined everywhere";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs,
              DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the second ping was not bounded by the same timeout as the first, so one of the two "
           "routes into the ping can exceed the ceiling";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made; the reopened descriptor is opened inside the check and must "
           "not outlive it";

    // Custody across the window: the reopened descriptor must be released before the
    // retained one, which pins the validated inode while the re-verification runs.
    ASSERT_EQ(g_syntheticProbe.closedFds.size(), 2u)
        << "the release sequence was not recorded as two entries, so the custody order cannot be "
           "checked at all";
    EXPECT_EQ(g_syntheticProbe.closedFds[0], kSyntheticSecondBinderFd)
        << "the first descriptor released was " << g_syntheticProbe.closedFds[0]
        << " where the descriptor reopened for the pre-lookup liveness check ("
        << kSyntheticSecondBinderFd
        << ") was expected. Releasing the RETAINED descriptor first means custody ended before "
           "the re-verification finished, which unpins the validated inode and lets a "
           "substitution at the same path be recycled onto the same inode number - the identity "
           "comparison would then report 'unchanged' for a node that had been replaced";
    EXPECT_EQ(g_syntheticProbe.closedFds[1], kSyntheticBinderFd)
        << "the descriptor released last was " << g_syntheticProbe.closedFds[1]
        << " where the retained, validated descriptor (" << kSyntheticBinderFd
        << ") was expected. It must outlive the entire re-verification and be released only when "
           "the query returns";
}

/**
 * @brief The service query declines when the validated node is re-permissioned between the
 *        check and the use.
 * @pre Runs under every invocation, through the substituted probe: same object, new mode.
 * @note The logged divergence mask names bit3, the mode, beside the declining verdict.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheValidatedNodeIsRePermissionedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kRePermissionedNodeIdentity;

    // The premise, asserted: same object, different mode, so the case cannot pass as a
    // substituted-node case.
    ASSERT_EQ(kRePermissionedNodeIdentity.device, kValidatedNodeIdentity.device);
    ASSERT_EQ(kRePermissionedNodeIdentity.inode, kValidatedNodeIdentity.inode);
    ASSERT_EQ(kRePermissionedNodeIdentity.rdev, kValidatedNodeIdentity.rdev);
    ASSERT_EQ(kRePermissionedNodeIdentity.uid, kValidatedNodeIdentity.uid);
    ASSERT_NE(kRePermissionedNodeIdentity.mode, kValidatedNodeIdentity.mode)
        << "this case is about the MODE and the two fixtures now agree on it, so it exercises "
           "nothing";

    DriverAidlImpl backEnd;
    bool verdict = true;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence diagnostic cannot be read; "
               "failing rather than asserting against an empty capture";

        verdict = backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                             DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                             syntheticProbe());

        captured = capture.read();
    }

    EXPECT_FALSE(verdict)
        << "the query proceeded to the service lookup after the validated binder node had been "
           "re-permissioned. The object is the same object, so nothing else in the path could "
           "have noticed: an attacker who cannot replace the node just widened access to it and "
           "the middleware certified the transport anyway";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "a decline recorded no reason, so the factory has nothing to name in its fallback line";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a re-permissioned driver node was reported as a SERVICE problem. What changed is the "
           "transport, and naming the service would send an integrator looking in the wrong place";

    EXPECT_THAT(captured, ::testing::HasSubstr("divergence mask 0x08"))
        << "the decline did not report a MODE divergence (bit3). Either the mode is not being "
           "compared and something else refused this node, or the diagnostic no longer says which "
           "attribute moved - and without that an integrator cannot tell a substituted node from "
           "a re-permissioned one. Captured: [" << captured << "]";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified, so this case did not reach the comparison "
           "it is about";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor whose identity comparison had "
           "already failed, so the decline is happening later than it should";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors - the retained one and the one reopened inside the check - were "
           "expected to be released, and " << g_syntheticProbe.closeCalls << " release(s) were "
           "made. A decline that leaks the fresh descriptor leaks a driver context for the life "
           "of the process";
}

/**
 * @brief The service query declines when the validated node is re-owned between the check
 *        and the use.
 * @pre Runs under every invocation, through the substituted probe; only the owner differs.
 * @note Bit4 of the logged divergence mask proves the uid was compared.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheValidatedNodeIsReOwnedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kReOwnedNodeIdentity;

    ASSERT_EQ(kReOwnedNodeIdentity.device, kValidatedNodeIdentity.device);
    ASSERT_EQ(kReOwnedNodeIdentity.inode, kValidatedNodeIdentity.inode);
    ASSERT_EQ(kReOwnedNodeIdentity.rdev, kValidatedNodeIdentity.rdev);
    ASSERT_EQ(kReOwnedNodeIdentity.mode, kValidatedNodeIdentity.mode);
    ASSERT_NE(kReOwnedNodeIdentity.uid, kValidatedNodeIdentity.uid)
        << "this case is about the OWNER and the two fixtures now agree on it, so it exercises "
           "nothing";

    DriverAidlImpl backEnd;
    bool verdict = true;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence diagnostic cannot be read";

        verdict = backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                             DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                             syntheticProbe());

        captured = capture.read();
    }

    EXPECT_FALSE(verdict)
        << "the query proceeded to the service lookup after ownership of the validated binder "
           "node had been transferred to an unprivileged user. The preflight refuses such a node "
           "when it SEES it; refusing it only at check time and not at use time leaves the "
           "refusal a formality";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a re-owned driver node was not reported as a transport condition";

    EXPECT_THAT(captured, ::testing::HasSubstr("divergence mask 0x10"))
        << "the decline did not report an OWNER divergence (bit4), so either the uid is not being "
           "compared or the diagnostic cannot distinguish it from the other four attributes. "
           "Captured: [" << captured << "]";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor that had already failed its identity "
           "comparison";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made";
}

/**
 * @brief Five agreeing identity attributes pass both comparisons, so the re-verification is
 *        not over-tightened.
 * @pre Runs under every invocation, through a probe that declines only the second ping.
 * @note Reaching that ping, two ping calls in all, proves both identity comparisons passed.
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryAcceptsAnUnchangedNodeOnAllFiveIdentityAttributes) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);

    // Every route to an identity answers with the same node, mode and owner included. This is
    // what a healthy platform looks like across the window.
    g_syntheticProbe.answerDescriptorIdentity = kValidatedNodeIdentity;
    g_syntheticProbe.answerPathIdentity = kValidatedNodeIdentity;
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kValidatedNodeIdentity;

    // The one thing that declines, and it is deliberately the last point of the four.
    g_syntheticProbe.hasSecondPingAnswer = true;
    g_syntheticProbe.answerPingSecond = false;

    DriverAidlImpl backEnd;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be redirected";

        EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                                DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                                syntheticProbe()))
            << "the query entered the service lookup although the second context-manager ping was "
               "refused; on this host that is an abort inside libbinder rather than a fallback";

        captured = capture.read();
    }

    EXPECT_EQ(g_syntheticProbe.identifyPathCalls, 1)
        << "the pathname was not re-resolved, so the first identity comparison never happened";
    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified, so the second identity comparison never "
           "happened";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 2)
        << "the second context-manager ping was never reached, which means one of the two identity "
           "comparisons REFUSED A NODE THAT HAD NOT CHANGED. That is the over-tightening failure: "
           "the comparison must assert that the five attributes did not MOVE, never that they hold "
           "particular values, or the AIDL path is declined on every correctly provisioned "
           "platform";

    EXPECT_THAT(captured, ::testing::Not(::testing::HasSubstr("divergence mask")))
        << "an identity divergence was reported for a node whose five attributes all agree, so "
           "the comparison is finding a difference that does not exist. Captured: ["
        << captured << "]";

    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made";
}

/**
 * @brief A group- or world-writable binder node is accepted, and its permission bits are
 *        reported.
 * @pre Runs under every invocation, through the substituted probe on a root-owned character
 *      device.
 * @note A regression guard: binder nodes are broadly accessible by design, so refusing one
 *       would decline the AIDL path on conformant platforms.
 */
TEST_F(DriverAidlPreflightTest, AcceptsAWorldWritableNodeAndReportsItsPermissionBits) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity = kWorldWritableNodeIdentity;

    // The premise: still a root-owned character device, so nothing ELSE in the preflight has
    // grounds to refuse it and the verdict below is about the permission bits alone.
    ASSERT_EQ(kWorldWritableNodeIdentity.mode & DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE)
        << "the node this case configures is not a character device, so it would be refused for a "
           "reason that has nothing to do with its permissions";
    ASSERT_EQ(kWorldWritableNodeIdentity.uid, DriverAidlImpl::BINDER_NODE_REQUIRED_OWNER_UID)
        << "the node this case configures is not owned by root, so it would be refused one arm "
           "earlier";
    ASSERT_NE(kWorldWritableNodeIdentity.mode & (DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE |
                                                 DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE),
              0u)
        << "the node this case configures is not writable beyond its owner, so there is nothing "
           "for the observation to report";

    bool verdict = false;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the permissive-node line cannot be read; "
               "failing rather than asserting against an empty capture";

        verdict = BinderPreflightTestAccess::isBinderPreflightOk(
            DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe());

        captured = capture.read();
    }

    EXPECT_TRUE(verdict)
        << "the preflight DECLINED a root-owned character device because its mode was permissive. "
           "A binder node must be openable by every binder client, so this refuses the AIDL path "
           "on conformant production platforms while passing in a root-only CI guest. Node "
           "permission restrictiveness is not a property this middleware can require: the "
           "enforceable controls are the character-device and root-owner checks plus the "
           "five-attribute comparison across the check-to-use window";

    EXPECT_THAT(captured, ::testing::HasSubstr("is writable beyond its owner, permission bits 0666"))
        << "the permissive node was accepted silently. The line is the whole of what the "
           "middleware can contribute here - it is what tells an integrator that, on a platform "
           "which ALSO leaves service registration unauthorized, the precondition of HAL "
           "impersonation is present. Captured: [" << captured << "]";

    // Exactly one line, and it does not turn into a decline on a second call: the observation is
    // stateless, so a caller cannot be surprised by a different verdict the next time round.
    const size_t firstHit = captured.find("is writable beyond its owner");
    ASSERT_NE(firstHit, std::string::npos);
    EXPECT_EQ(captured.find("is writable beyond its owner", firstHit + 1), std::string::npos)
        << "the permissive-node condition was reported more than once in a single preflight, so "
           "the observation has been placed somewhere that runs repeatedly";

    // And the accounting is the accounting of a POSITIVE verdict with no custody requested:
    // one open, one protocol read, one ping, and the descriptor released before return.
    EXPECT_EQ(g_syntheticProbe.openCalls, 1) << "the node was not opened exactly once";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol read was skipped, so the observation short-circuited the arms after it";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not pinged, so the observation short-circuited the arms after "
           "it";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on a positive verdict with no custody requested";
}

/**
 * @brief The permissive-node line is informational: at the WARN level it is suppressed and the
 *        verdict stays positive, so a healthy start on a standard 0666 node logs no warning.
 * @pre Runs under every invocation, through the substituted probe; the level is set to WARN with
 *      ScopedCecLogLevel and its restoration asserted.
 * @note The non-root-owner refusal, a WARN line, is the positive control that WARN still prints.
 */
TEST_F(DriverAidlPreflightTest, ThePermissiveNodeObservationLogsNoWarning) {
    bool acceptedVerdict = false;
    bool refusedVerdict = true;
    std::string accepted;
    std::string refused;
    bool levelWasSet = false;
    std::string levelRefusal;

    {
        // Set before the captures open, so the guard's own level probes stay out of them.
        ScopedCecLogLevel warnLevel("WARN");
        levelWasSet = warnLevel.isRaised();
        levelRefusal = warnLevel.failureReason();

        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be redirected, so the absence of the permissive-node line "
                   "cannot be told from an empty capture";

            resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                                true);
            g_syntheticProbe.answerDescriptorIdentity = kWorldWritableNodeIdentity;
            acceptedVerdict = BinderPreflightTestAccess::isBinderPreflightOk(
                DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe());
            accepted = capture.read();
        }

        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be redirected, so the positive control cannot be read";

            resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                                true);
            g_syntheticProbe.answerDescriptorIdentity = kWorldWritableNodeIdentity;
            g_syntheticProbe.answerDescriptorIdentity.uid = 1000u;
            refusedVerdict = BinderPreflightTestAccess::isBinderPreflightOk(
                DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe());
            refused = capture.read();
        }

        // Restoration is asserted under the custody lock; the destructor is only a backstop.
        std::string restoreDetail;
        ASSERT_TRUE(warnLevel.restoreAndVerify(restoreDetail)) << restoreDetail;
    }

    ASSERT_TRUE(levelWasSet)
        << "the middleware log level could not be set to WARN, so whether the permissive-node line "
           "is a warning cannot be observed. Reported reason: [" << levelRefusal << "]";

    EXPECT_TRUE(acceptedVerdict)
        << "the preflight declined a root-owned character device because its mode was permissive";
    EXPECT_EQ(accepted.find("is writable beyond its owner"), std::string::npos)
        << "the permissive-node line printed at the WARN level, so every healthy AIDL start on a "
           "standard 0666 binder node reports a warning. Captured: [" << accepted << "]";

    EXPECT_FALSE(refusedVerdict) << "the positive control's node, owned by uid 1000, was accepted";
    EXPECT_THAT(refused, ::testing::HasSubstr("is owned by uid 1000"))
        << "the non-root-owner refusal did not print at the WARN level, so the absence asserted "
           "above proves nothing about the permissive-node line's level. Captured: [" << refused << "]";
}

/**
 * @brief The resolved selection under invocation A: legacy, once, and stable thereafter.
 *
 * SetUp asserts that the resolved back-end is the legacy DriverImpl rather than adapting to
 * another. The selection is observed by dynamic_cast against the concrete types and by the
 * selected-path log line.
 * @pre Invocation A: the harness runs with CEC_TEST_AIDL_MODE `absent` or unset.
 * @note No case mutates the process-global driver, so TearDown has nothing to do.
 */
class DriverAidlSelectionTest : public ::testing::Test {
protected:
    /**
     * @brief Establishes that the resolved back-end is the legacy one, and caches it.
     *
     * @pre Invocation A: CEC_TEST_AIDL_MODE is `absent` or unset.
     * @post @c legacyBackEnd holds the resolved instance, non-null.
     */
    void SetUp() override {
        legacyBackEnd = dynamic_cast<DriverImpl *>(&Driver::getInstance());

        ASSERT_NE(legacyBackEnd, nullptr)
            << "this fixture requires the legacy back-end to be the resolved one, i.e. invocation "
               "A (CEC_TEST_AIDL_MODE=absent or unset), and the factory returned something else. "
               "The selection resolves once per process inside LibCCEC::init, so it cannot be "
               "changed from here; re-run with the invocation-A filter from the fixture manifest "
               "above instead";
    }

    /** @brief The resolved legacy back-end, cached by SetUp() for the case bodies. */
    DriverImpl *legacyBackEnd = nullptr;
};

/**
 * @brief With no AIDL service published, the factory selects the legacy back-end.
 * @pre Invocation A, established by the fixture's SetUp.
 * @note Casts in both directions make "exactly one of the two" an assertion.
 */
TEST_F(DriverAidlSelectionTest, AbsentServiceSelectsTheLegacyBackEnd) {
    Driver &resolved = Driver::getInstance();

    EXPECT_NE(dynamic_cast<DriverImpl *>(&resolved), nullptr)
        << "the resolved back-end is not the legacy one, although no AIDL service is registered";

    EXPECT_EQ(dynamic_cast<DriverAidlImpl *>(&resolved), nullptr)
        << "the resolved back-end is the AIDL one although no AIDL service is registered, so the "
           "selection is not resting on service availability at all";
}

/**
 * @brief The selected-path log line names the legacy back-end and survives the default
 *        log-level filter.
 * @pre Invocation A; drives the production logger with the transcribed contract format,
 *      since the factory's own line was logged before any test body ran.
 * @note The captured name must agree with what the casts established.
 */
TEST_F(DriverAidlSelectionTest, SelectedPathLogLineNamesTheLegacyBackEnd) {
    // The format is a contract, so its shape is asserted before it is used: exactly one
    // substitution, and the \r\n terminator every CCEC log line carries.
    const std::string format(kSelectedBackEndLogFormat);
    ASSERT_EQ(format.find("%s"), format.rfind("%s"))
        << "the transcribed selected-path format carries more than one substitution, so it no "
           "longer matches SELECTED_BACK_END_LOG_FORMAT in ccec/src/Driver.cpp, where the "
           "back-end name is the only part that varies";
    ASSERT_NE(format.find("%s"), std::string::npos)
        << "the transcribed selected-path format has no substitution at all, so it cannot name a "
           "back-end";
    ASSERT_GE(format.size(), 2u);
    EXPECT_EQ(format.substr(format.size() - 2), "\r\n")
        << "the selected-path format no longer ends in \\r\\n, which every CCEC_LOG line in this "
           "middleware carries and which the runner's line-oriented grep relies on";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so nothing about the emitted line can be "
               "established; failing rather than asserting against an empty capture";

        CCEC_LOG(LOG_INFO, kSelectedBackEndLogFormat, kSelectedBackEndLegacy);

        captured = capture.read();
    }

    const std::string expected =
        std::string("HDMI CEC HAL back-end selected : ") + kSelectedBackEndLegacy;

    EXPECT_NE(captured.find(expected), std::string::npos)
        << "the production logger did not emit the selected-path line naming the legacy back-end. "
           "Either the transcribed format at the top of this file has drifted from "
           "SELECTED_BACK_END_LOG_FORMAT and SELECTED_BACK_END_LEGACY in ccec/src/Driver.cpp, or "
           "the default CEC log level is now below LOG_INFO and the "
           "line is being filtered out - in which case every consumer of it, this suite included, "
           "is reading an empty log. Captured instead: [" << captured << "]";

    // The two back-end names must be distinguishable, or the line could not identify either.
    EXPECT_STRNE(kSelectedBackEndLegacy, kSelectedBackEndAidl)
        << "the two back-end names in the log contract are now identical, so the line cannot say "
           "which back-end was selected";
}

/**
 * @brief The factory hands back the same object on every call.
 * @pre Invocation A, though the property is independent of which back-end resolved.
 * @note A factory that re-decided per call would leave Bus's threads and LibCCEC talking to
 *       different drivers.
 * @see Driver::getInstance()
 */
TEST_F(DriverAidlSelectionTest, FactoryReturnsTheSameObjectOnEveryCall) {
    Driver *first = &Driver::getInstance();
    Driver *second = &Driver::getInstance();
    Driver *third = &Driver::getInstance();

    ASSERT_NE(first, nullptr);

    EXPECT_EQ(first, second) << "two consecutive calls to Driver::getInstance() returned different "
                               "objects, so the back-end is not a stable singleton";
    EXPECT_EQ(second, third) << "a third call returned yet another object";
    EXPECT_EQ(first, static_cast<Driver *>(legacyBackEnd))
        << "the object the factory returns is not the one SetUp resolved, so the identity "
           "established there does not describe what the rest of this suite is talking to";
}

/**
 * @brief Publishing a service after the selection has resolved does not change which back-end
 *        the factory holds.
 * @pre Invocation A; the binder preflight picks the form: strong (a service is published and
 *      asserted) where it passes, weak (singleton identity only) where it declines.
 * @warning A fake published here stays published for the rest of the process.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlSelectionTest, RegisteringAServiceMidProcessDoesNotChangeTheResolvedBackEnd) {
    Driver *before = &Driver::getInstance();
    ASSERT_EQ(dynamic_cast<DriverAidlImpl *>(before), nullptr)
        << "the AIDL back-end was already selected, so this case cannot demonstrate that a "
           "mid-process registration fails to switch to it";

    static ::android::sp<FakeHdmiCecService> latePublishedFake;

    // The environment's own answer decides the form, and it is asked exactly once so that
    // both the branch taken and every message below describe the same verdict.
    const bool canPublish = BinderPreflightTestAccess::isBinderPreflightOk();

    // Named once and reused in every message, so a failure states which form produced it
    // rather than leaving a reader of the log to work it out.
    const char *const form = canPublish
        ? "strong form (a service was published mid-process)"
        : "weak form (this host cannot publish a service, so nothing appeared for the selection "
          "to change to, and SC6(c) is not established by this run)";

    if (canPublish) {
        latePublishedFake = ::android::sp<FakeHdmiCecService>::make();
        ASSERT_TRUE(latePublishedFake != nullptr) << "the fake service could not be constructed";

        // Asserted, not reported: the preflight passed, so a refusal is a real environment
        // failure, and tolerating it would leave the strong form unreached.
        ASSERT_TRUE(registerFakeHdmiCecService(latePublishedFake))
            << "the binder preflight passed, so this host can publish a service, and the service "
               "manager refused this one anyway. Without a successful publication there is nothing "
               "for the selection to have been tempted by, so the assertions below would prove only "
               "that a singleton is a singleton - which "
               "DriverAidlSelectionTest.FactoryReturnsTheSameObjectOnEveryCall already proves. "
               "Fix the environment rather than weakening this case: check that servicemanager is "
               "running and that nothing else holds the name";

        std::cout << "[DriverAidlSelectionTest] " << form
                  << ": the fake was published while this process was running, and the selection "
                     "invariant is now asserted against a service that really is there"
                  << std::endl;
    } else {
        std::cout << "[DriverAidlSelectionTest] " << form
                  << ": the binder preflight declined, so no service was published and the "
                     "assertions below reduce to repeated-singleton identity. Re-run this case on a "
                     "binder-capable runner to obtain the SC6(c) evidence"
                  << std::endl;
    }

    Driver *after = &Driver::getInstance();

    EXPECT_EQ(after, before)
        << "the factory returned a different object after a service was registered, so the "
           "selection is being re-decided per call rather than held for the process. Form: " << form;

    EXPECT_NE(dynamic_cast<DriverImpl *>(after), nullptr)
        << "the resolved back-end is no longer the legacy one. A service that appears after "
           "initialization must NOT be adopted: half a session on each back-end is precisely the "
           "non-determinism resolving once is there to prevent. Form: " << form;

    EXPECT_EQ(dynamic_cast<DriverAidlImpl *>(after), nullptr)
        << "the AIDL back-end was adopted mid-process. Form: " << form;

    // No test property records the form, because the runner's JSON contract is another
    // file's; the form is on stdout and in every failure message.
}

/**
 * @brief The AIDL back-end's behaviour on locally constructed instances, under every invocation.
 *
 * Plain instances stay CLOSED, so the state guards, the writeAsync prelude and the unguarded
 * methods are reached with no HAL. Probes force the lifecycle state or inject in-process service
 * and controller doubles, so OPENED-state paths run without a live HAL. The legacy HAL mock is
 * reached only through Times(0) expectations, which TearDown clears.
 */
/* ---- Receive-queue ownership handoff: test-local helpers, all with internal linkage. ---- */

namespace {

/**
 * @brief A DriverAidlImpl whose incoming queue can be driven directly.
 *
 * Forces only the lifecycle field, so the queue arithmetic, the producer serialization and the
 * ownership report are reachable with no service, listener or threadpool. The destructor drains
 * the queue and returns the state to CLOSED on every exit path.
 */
class ReceiveQueueProbe : public DriverAidlImpl {
public:
    /** @brief Drains the queue and returns to CLOSED, so the base destructor never closes. */
    ~ReceiveQueueProbe() override {
        drain();
        markClosed();
    }

    /** @brief Puts the instance into the OPENED state the receive handoff requires. */
    void markOpened(void) { status = OPENED; }

    /** @brief Returns the instance to CLOSED, so no destructor takes the close path. */
    void markClosed(void) { status = CLOSED; }

    /**
     * @brief Puts the instance into CLOSING, the state close() sets before it offers its sentinel.
     *
     * Lets a case observe read() in the state that takes its FLUSH arm.
     */
    void markClosing(void) { status = CLOSING; }

    /**
     * @brief Offers one NULL close sentinel exactly as close() does, under the producer lock.
     *
     * Lets a case place the sentinel without leaving OPENED, or while it holds the instance lock.
     */
    void postCloseSentinel(void) {
        {CCEC_OSAL::AutoLock lock_(queueProducerMutex);
            rQueue.offer(0);
        }
    }

    /**
     * @brief Queues a frame as one accepted while OPENED and still queued when close() runs.
     *
     * @param [in] frame - Heap frame; ownership passes to the queue, and read()'s flush frees it.
     *
     * @pre The caller holds the instance lock, so the reader cannot consume this entry first.
     * @post The frame is queued behind whatever was already there.
     * @warning Takes queueProducerMutex, as every producer on this queue does.
     * @see postCloseSentinel()
     */
    void postFrameBehindSentinel(CECFrame *frame) {
        {CCEC_OSAL::AutoLock lock_(queueProducerMutex);
            rQueue.offer(frame);
        }
    }

    /**
     * @brief The instance lock read() takes to re-check the state, for a test to hold.
     *
     * Holding it parks the reader between its poll and its state re-check, which makes the
     * flush arm deterministic.
     *
     * @warning Take this before producerLock() or postCloseSentinel(), as close() nests them;
     *          never take it while holding producerLock().
     */
    Mutex &instanceLock(void) { return mutex; }

    /**
     * @brief Calls the production handoff and reports exactly what it reported.
     *
     * @param [in] frame - Frame to hand off; the queue owns it only if this returns true.
     * @return bool - DriverAidlImpl::offerReceivedFrame()'s result, uninterpreted.
     */
    bool offer(CECFrame *frame) { return offerReceivedFrame(frame); }

    /**
     * @brief The lock every producer on the incoming queue must take, for the test to hold.
     *
     * While a case holds it, a close() driven from another thread must not complete its offer.
     *
     * @return Mutex& - This instance's DriverAidlImpl::queueProducerMutex.
     * @warning Never take the instance mutex while holding this: close() nests them the other way.
     */
    Mutex &producerLock(void) { return queueProducerMutex; }

    /**
     * @brief Current occupancy of the incoming queue.
     *
     * @return size_t - Entries currently queued, sentinels included.
     */
    size_t occupancy(void) { return rQueue.size(); }

    /**
     * @brief Takes one entry exactly as the Bus reader does.
     *
     * @return CECFrame* - The frame taken, or NULL for a close() sentinel.
     * @pre occupancy() is non-zero: EventQueue::poll() blocks on an empty queue.
     */
    CECFrame *take(void) { return rQueue.poll(); }

    /**
     * @brief Empties the queue, counting frames and close sentinels apart and releasing frames.
     *
     * @param [out] frames    - Number of non-NULL entries taken; each one is released here.
     * @param [out] sentinels - Number of NULL entries, i.e. close() sentinels the queue accepted.
     *
     * @pre Nothing else is producing onto the queue.
     */
    void drainCounting(size_t &frames, size_t &sentinels) {
        std::vector<CECFrame *> taken;

        drainInto(taken, sentinels);

        frames = taken.size();

        for (size_t i = 0; i < taken.size(); i++) {
            delete taken[i];
        }
    }

    /**
     * @brief Empties the queue, retaining each frame's identity and counting sentinels apart.
     *
     * @param [out] frames    - Every non-NULL entry, in queue order; the caller now owns each.
     * @param [out] sentinels - Number of NULL entries, i.e. close() sentinels the queue accepted.
     *
     * @pre Nothing else is producing onto the queue.
     */
    void drainInto(std::vector<CECFrame *> &frames, size_t &sentinels) {
        sentinels = 0;

        while (occupancy() > 0) {
            CECFrame *entry = take();

            if (entry == NULL) {
                sentinels++;
            }
            else {
                frames.push_back(entry);
            }
        }
    }

    /**
     * @brief Releases everything still queued and reports how many entries were taken.
     *
     * Goes through drainCounting(), so take() stays the only EventQueue::poll() call site here.
     *
     * @return size_t - Frames and sentinels taken in total; every frame is released.
     */
    size_t drain(void) {
        size_t frames = 0;
        size_t sentinels = 0;

        drainCounting(frames, sentinels);

        return frames + sentinels;
    }
};

/**
 * @brief A sticky latch whose timed wait is bounded on a monotonic clock.
 *
 * Replaces CCEC_OSAL::ConditionVariable, whose timed wait runs on CLOCK_REALTIME and can hand
 * pthread_cond_timedwait an invalid tv_nsec. notify() sets the flag as well as waking waiters,
 * so a false return from wait() is a genuine timeout, never a missed signal.
 */
class MonotonicLatch {
public:
    /**
     * @brief Creates a clear latch.
     *
     * @post The latch is clear, so a wait begun now blocks until notify() or the bound.
     */
    MonotonicLatch(void) : signalled(false) {}

    /** @brief Sets the latch and wakes every waiter; idempotent and safe from any thread. */
    void notify(void) {
        {
            std::lock_guard<std::mutex> held(guard);
            signalled = true;
        }

        wakeUp.notify_all();
    }

    /**
     * @brief Waits for the latch for at most @p boundMs milliseconds of monotonic time.
     *
     * @param [in] boundMs - The bound in milliseconds, measured on std::chrono::steady_clock.
     * @return bool - true when the latch was set within the bound, false on a genuine timeout.
     */
    bool wait(long boundMs) {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);

        std::unique_lock<std::mutex> held(guard);

        return wakeUp.wait_until(held, deadline, [this]() { return signalled; });
    }

    /**
     * @brief Reports the latch's state without waiting at all.
     *
     * @return bool - Whether the latch has been set. Safe from any thread; the read is taken
     *                under the latch's own mutex.
     */
    bool isSignalled(void) {
        std::lock_guard<std::mutex> held(guard);

        return signalled;
    }

private:
    std::mutex              guard;
    std::condition_variable wakeUp;
    bool                    signalled;

    /** @brief Copy construction is disabled: declared private and never defined. */
    MonotonicLatch(const MonotonicLatch &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    MonotonicLatch & operator = (const MonotonicLatch &);
};

/**
 * @brief How long the overlap harness gives a worker to finish before abandoning it.
 *
 * Keeps the disposition path off an unbounded join, so a stuck worker yields a reported failure
 * rather than a hung suite. Empirical and generous, yet shorter than
 * PRODUCER_COMPLETION_TIMEOUT_MS because it is reached only after a failure was reported.
 */
const long WORKER_ABANDON_DEADLINE_MS = 2000;

/**
 * @brief One worker thread and its bounded join-or-detach disposal.
 *
 * dispose() waits a caller-supplied bound (WORKER_ABANDON_DEADLINE_MS from the overlap harness,
 * kStalledTransmitBoundMs from the stall harness), joining a body that returned and detaching one
 * that did not; after a detach the caller must retain everything the thread can reach.
 *
 * @warning Declare the owning harness outside the scope that holds the producer lock, so an
 *          early return releases the lock before the workers are disposed.
 */
class BoundedWorker {
public:
    /**
     * @brief Creates a worker holding no thread; start() is what launches one.
     */
    BoundedWorker(void) {}

    /**
     * @brief Last-resort disposition: detaches a still-joinable thread rather than joining it.
     *
     * Unreachable in this file's use, because the owning harness disposes every worker first.
     */
    ~BoundedWorker(void) {
        if (worker.joinable()) {
            worker.detach();
        }
    }

    /**
     * @brief Runs @p body on a new thread and sets the finished latch when it returns.
     *
     * @param [in] body - The worker body. Copied into the thread, so it must capture a
     *                    heap-resident state pointer and nothing on the test stack: an
     *                    abandoned worker outlives the case body.
     */
    template <typename Body>
    void start(Body body) {
        worker = std::thread([this, body]() {
            body();
            finished.notify();
        });
    }

    /**
     * @brief Bounded, monotonic wait for the body to return.
     *
     * @param [in] boundMs - The bound in milliseconds, on std::chrono::steady_clock.
     * @return bool - true when the body has returned, false on a genuine timeout.
     */
    bool finishedWithin(long boundMs) { return finished.wait(boundMs); }

    /**
     * @brief Reports whether the body has returned, without waiting.
     *
     * @return bool - Whether the body has returned. This is the body's own completion, not
     *                the thread's teardown, which is the distinction the bounded disposition
     *                rests on.
     */
    bool hasFinished(void) { return finished.isSignalled(); }

    /**
     * @brief Bounded disposition: join a finished worker, abandon one that is not.
     *
     * @param [in] boundMs - How long to wait for the body to return before abandoning.
     * @return bool - true when the worker was joined or holds no thread (never started or
     *                already disposed); false when it had to be detached, in which case the
     *                caller must not release anything the thread can reach.
     */
    bool dispose(long boundMs) {
        if (!worker.joinable()) {
            return true;
        }

        if (!finished.wait(boundMs)) {
            worker.detach();
            return false;
        }

        worker.join();

        return true;
    }

private:
    std::thread    worker;
    MonotonicLatch finished;

    /** @brief Copy construction is disabled: declared private and never defined. */
    BoundedWorker(const BoundedWorker &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    BoundedWorker & operator = (const BoundedWorker &);
};

/**
 * @brief Everything a concurrent producer-overlap worker can touch, allocated off the test stack.
 *
 * A detached worker may still be inside production close() or offerReceivedFrame() when the case
 * returns, so everything it reaches lives here, behind a pointer. One struct serves all three
 * concurrent cases; each leaves the fields it does not use at their initial values.
 */
struct QueueHandoffOverlapState {
    /** @brief Creates the state with no contending frame and every flag and counter clear. */
    QueueHandoffOverlapState(void)
        : contending(NULL)
        , contendingAccepted(false)
        , contendingRefusedByStateGuard(false)
        , contendingRaisedSomethingElse(false)
        , closeReturnedCleanly(false)
        , closeRaisedIoException(false)
        , closeRaisedSomethingElse(false)
        , producersAtRendezvous(0)
        , producersReleased(false)
        , rendezvousTimedOut(false)
    {}

    /** @brief The instance under test, whose queue and two locks are the shared state. */
    ReceiveQueueProbe probe;

    /** @brief Set by the receive worker immediately before it enters the production handoff. */
    MonotonicLatch receiveEntered;

    /** @brief Set by the close worker immediately before it enters production close(). */
    MonotonicLatch closeEntered;

    /**
     * @brief Every frame the case allocated, released by the harness once every worker joined.
     *
     * Released exactly once, after the queue is drained; deliberately retained if any worker was
     * abandoned.
     */
    std::vector<CECFrame *> allocated;

    /** @brief The frame the receive worker offers, owned by the case and decided by the race. */
    CECFrame *contending;

    /** @brief What the production handoff reported for @c contending. */
    bool contendingAccepted;

    /** @brief The handoff was refused by the OPENED-state guard rather than by occupancy. */
    bool contendingRefusedByStateGuard;

    /** @brief The handoff raised something that is neither of the two expected outcomes. */
    bool contendingRaisedSomethingElse;

    /** @brief close() returned without raising - which a proxyless instance must not do. */
    bool closeReturnedCleanly;

    /** @brief close() raised IOException, the expected report of the failed transaction. */
    bool closeRaisedIoException;

    /** @brief close() raised something else entirely. */
    bool closeRaisedSomethingElse;

    /**
     * @brief Counts the producers that have reached the stress case's rendezvous.
     *
     * The second to arrive releases the pair, so the overlap needs neither scheduling luck nor
     * the production lock under test.
     */
    std::atomic<unsigned int> producersAtRendezvous;

    /** @brief Flipped by the second producer to arrive; the first spins on it. */
    std::atomic<bool> producersReleased;

    /**
     * @brief Set when a producer gave up waiting for its partner at the rendezvous.
     *
     * Atomic because either worker can be the one to set it, and a plain bool written by two
     * threads is a data race however benign the values look.
     */
    std::atomic<bool> rendezvousTimedOut;

private:
    /** @brief Copy construction is disabled: declared private and never defined. */
    QueueHandoffOverlapState(const QueueHandoffOverlapState &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    QueueHandoffOverlapState & operator = (const QueueHandoffOverlapState &);
};

/**
 * @brief Owns the shared state and every worker, and disposes both without an unbounded wait.
 *
 * Nothing a worker can still reach is released while that worker may run: if any worker had to
 * be abandoned, the state and the workers are deliberately leaked. Every passing run joins its
 * workers and frees everything.
 */
class QueueHandoffOverlapHarness {
public:
    /**
     * @brief Allocates the shared state on the heap, off the test stack.
     * @post The shared state is live and reachable through operator*() and operator->().
     *       It outlives this harness whenever a worker had to be abandoned, which is the
     *       leak-by-design rule the class exists to enforce.
     */
    QueueHandoffOverlapHarness(void) : state(new QueueHandoffOverlapState()), abandoned(false) {}

    /**
     * @brief Disposes every worker, then frees frames, workers and state unless one was abandoned.
     */
    ~QueueHandoffOverlapHarness(void) {
        disposeWorkers();

        if (abandoned) {
            /* Leaked by design: a detached worker may still be inside production code using it. */
            return;
        }

        /* Workers are disposed, so drain the queue first (the probe's destructor then finds it
         * empty), then release every registered frame exactly once. */
        std::vector<CECFrame *> leftInQueue;
        size_t                  sentinelsLeftInQueue = 0;

        state->probe.drainInto(leftInQueue, sentinelsLeftInQueue);

        for (size_t i = 0; i < state->allocated.size(); i++) {
            delete state->allocated[i];
        }

        state->allocated.clear();

        for (size_t i = 0; i < workers.size(); i++) {
            delete workers[i];
        }

        delete state;
    }

    /** @brief The shared state, for the case body to read and to hand to its workers. */
    QueueHandoffOverlapState & operator *  (void) const { return *state; }

    /** @brief The shared state, for the case body to read and to hand to its workers. */
    QueueHandoffOverlapState * operator -> (void) const { return state; }

    /**
     * @brief Starts a worker the harness owns and disposes.
     *
     * @param [in] body - The worker body, which must capture the state pointer and nothing on
     *                    the test stack.
     * @return BoundedWorker& - The worker, for the case's own bounded completion observations.
     */
    template <typename Body>
    BoundedWorker &start(Body body) {
        BoundedWorker *worker = new BoundedWorker();

        workers.push_back(worker);
        worker->start(body);

        return *worker;
    }

    /**
     * @brief Disposes every worker boundedly; idempotent, and called again by the destructor.
     *
     * @return bool - true when every worker was joined; false when one had to be abandoned,
     *                which also arms the leak-by-design rule above.
     */
    bool disposeWorkers(void) {
        for (size_t i = 0; i < workers.size(); i++) {
            if (!workers[i]->dispose(WORKER_ABANDON_DEADLINE_MS)) {
                abandoned = true;
            }
        }

        return !abandoned;
    }

    /**
     * @brief Whether any worker had to be abandoned rather than joined, for a case to assert on.
     *
     * @return bool - true once a bounded disposition gave up on a worker; false on a clean run.
     */
    bool workerWasAbandoned(void) const { return abandoned; }

private:
    QueueHandoffOverlapState           *state;
    std::vector<BoundedWorker *>  workers;
    bool                          abandoned;

    /** @brief Copy construction is disabled: declared private and never defined. */
    QueueHandoffOverlapHarness(const QueueHandoffOverlapHarness &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    QueueHandoffOverlapHarness & operator = (const QueueHandoffOverlapHarness &);
};

/**
 * @brief How long a producer is watched for not completing while the test holds the lock.
 *
 * With the production lock in place the producer cannot complete however long the window; without
 * the acquisition nothing holds it, so it completes and the non-completion observation fails.
 */
const long PRODUCER_LOCK_OBSERVATION_MS = 400;

/**
 * @brief How long a released producer is given to finish before the case fails.
 *
 * Generous and empirical, so a timeout means the producer stopped making progress; bounded, so a
 * deadlock is reported rather than hanging the run.
 */
const long PRODUCER_COMPLETION_TIMEOUT_MS = 10000;

/**
 * @brief How many independent overlaps the stress case drives.
 *
 * The forbidden state is reachable only inside a narrow window, hit in roughly one attempt in
 * fifteen as measured here, so 256 attempts make a miss vanishingly unlikely.
 */
const size_t OVERLAP_STRESS_ITERATIONS = 256;

/** @brief The producers the stress case's rendezvous releases together. */
const unsigned int OVERLAP_RENDEZVOUS_PRODUCERS = 2;

/**
 * @brief How long a producer waits at the rendezvous for its partner before giving up.
 *
 * Reached only when the partner never ran, which is asserted as a harness failure.
 */
const long OVERLAP_RENDEZVOUS_BOUND_MS = 5000;

/**
 * @brief The widest deliberate offset between the two producers, in burnOffset() units.
 *
 * Varying the offset per iteration walks the producers' alignment across the race window, so the
 * sampled interleavings differ by construction rather than by luck.
 */
const unsigned int OVERLAP_OFFSET_SPREAD = 384;

/**
 * @brief Burns @p units of short, fixed busy work, without sleeping or yielding.
 *
 * A volatile store per unit cannot be elided by the optimiser.
 *
 * @param [in] units - How many increments to perform. Zero returns immediately.
 */
void burnOffset(unsigned int units) {
    volatile unsigned int sink = 0;

    for (unsigned int unit = 0; unit < units; unit++) {
        sink = sink + 1;
    }
}

/**
 * @brief Arrives at the two-producer rendezvous and returns once both producers are there.
 *
 * The second arriver releases the pair, so the release depends neither on the test thread's
 * scheduling nor on the production lock under test.
 *
 * @param [in,out] shared - The state carrying the arrival counter and the release flag.
 * @return bool - true when released; false when the bound expired and the partner never arrived.
 */
bool awaitOverlapRendezvous(QueueHandoffOverlapState *shared) {
    const unsigned int arrived =
        shared->producersAtRendezvous.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (arrived >= OVERLAP_RENDEZVOUS_PRODUCERS) {
        shared->producersReleased.store(true, std::memory_order_release);

        return true;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(OVERLAP_RENDEZVOUS_BOUND_MS);

    while (!shared->producersReleased.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }

    return true;
}

/**
 * @brief An IHdmiCec double whose close() reports whatever a case needs it to report.
 *
 * Reaches both close() failure shapes, a non-ok transaction and an ok one with a false result,
 * in direct, binder-free tests on an injected session. Derived from IHdmiCecDefault rather than
 * BnHdmiCec, so it can never be registered.
 *
 * @warning Only close() is overridden; every other method keeps UNKNOWN_TRANSACTION.
 * @see DriverAidlImpl::close()
 */
class ClosingServiceDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief The status the next close() transaction reports (default: ok). */
    ::android::binder::Status closeStatus = ::android::binder::Status::ok();
    /** @brief The boolean result the next close() writes out (default: success). */
    bool closeResult = true;
    /** @brief How many times close() has been called on this double. */
    unsigned int closeCalls = 0;
    /** @brief The controller reference the last close() was handed, for identity assertions. */
    ::android::sp<cechal::IHdmiCecController> lastControllerClosed;

    /** @brief Records the call and its controller, writes closeResult, returns closeStatus. */
    ::android::binder::Status close(const ::android::sp<cechal::IHdmiCecController> &hdmiCecController,
                                   bool *_aidl_return) override {
        closeCalls++;
        lastControllerClosed = hdmiCecController;

        if (_aidl_return != nullptr) {
            *_aidl_return = closeResult;
        }

        return closeStatus;
    }
};

/**
 * @brief An IHdmiCec double that reports whatever logical-address vector a case needs.
 *
 * The generated proxy hands getLogicalAddress() arbitrary 32-bit integers, so this double feeds
 * the out-of-contract arms (256, -1) to direct, binder-free tests through an injected service.
 * Derived from IHdmiCecDefault rather than BnHdmiCec.
 *
 * @warning Only getLogicalAddresses() is overridden; every other method keeps
 *          UNKNOWN_TRANSACTION.
 * @see DriverAidlImpl::getLogicalAddress()
 */
class AddressReportingDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief The vector the next getLogicalAddresses() writes out, verbatim. */
    std::vector<int32_t> addresses;
    /** @brief The status the next getLogicalAddresses() transaction reports (default: ok). */
    ::android::binder::Status readStatus = ::android::binder::Status::ok();
    /** @brief How many times getLogicalAddresses() has been called on this double. */
    unsigned int readCalls = 0;

    /** @brief Counts the call, writes @c addresses verbatim and returns readStatus. */
    ::android::binder::Status getLogicalAddresses(::std::vector<int32_t> *_aidl_return) override {
        readCalls++;

        if (_aidl_return != nullptr) {
            *_aidl_return = addresses;
        }

        return readStatus;
    }
};

/**
 * @brief An IHdmiCecController double whose sendMessage() reports any int32 a case needs.
 *
 * Holds the result as a raw integer cast on the way out, so a case can report a value outside
 * the three documented SendMessageStatus enumerators, as a parcel-reading proxy may deliver.
 *
 * @warning Only sendMessage() is overridden; the address-mutation methods keep
 *          UNKNOWN_TRANSACTION.
 * @see DriverAidlImpl::write()
 */
class TransmitResultDouble : public cechal::IHdmiCecControllerDefault {
public:
    /** @brief The value the next sendMessage() writes out, cast to the enum unchecked. */
    int32_t reportedStatus = static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0);
    /** @brief The status the next sendMessage() transaction reports (default: ok). */
    ::android::binder::Status transactionStatus = ::android::binder::Status::ok();
    /** @brief How many times sendMessage() has been called on this double. */
    unsigned int sendCalls = 0;
    /** @brief The bytes the last sendMessage() was handed, for marshalling assertions. */
    std::vector<uint8_t> lastMessage;

    /** @brief Records the call and message, writes reportedStatus, returns transactionStatus. */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> &message,
                                         cechal::SendMessageStatus *_aidl_return) override {
        sendCalls++;
        lastMessage = message;

        if (_aidl_return != nullptr) {
            *_aidl_return = static_cast<cechal::SendMessageStatus>(reportedStatus);
        }

        return transactionStatus;
    }
};

/**
 * @brief A DriverAidlImpl that can be placed in an OPENED session without a live HAL.
 *
 * Injects the state, service proxy and controller reference, the three things close() reads,
 * because driving the real open() would abort this process. The listener cannot be populated
 * here, so close()'s listener detach is covered by the session suite.
 *
 * @warning The destructor forces CLOSED, so a case's teardown never re-enters close().
 * @see DriverAidlImpl::close()
 * @see ClosingServiceDouble
 */
class SessionStateProbe : public DriverAidlImpl {
public:
    /** @brief Forces CLOSED, so the base destructor never re-enters close(). */
    ~SessionStateProbe() override { status = CLOSED; }

    /**
     * @brief Establishes the session state a successful open() would have left.
     *
     * @param [in] service    - The service proxy close() must call and must RETAIN.
     * @param [in] controller - The controller reference close() must hand over and CLEAR.
     */
    void injectOpenSession(const ::android::sp<cechal::IHdmiCec> &service,
                           const ::android::sp<cechal::IHdmiCecController> &controller) {
        hdmiCecService = service;
        hdmiCecController = controller;
        status = OPENED;
    }

    /**
     * @brief Attaches only the service proxy, leaving the instance CLOSED.
     *
     * getLogicalAddress() carries no state guard, so the proxy is its only precondition.
     *
     * @param [in] service - The service proxy getLogicalAddress() must call.
     */
    void injectServiceOnly(const ::android::sp<cechal::IHdmiCec> &service) { hdmiCecService = service; }

    /** @brief Seeds the local logical-address list, so "deliberately not cleared" is testable. */
    void seedLogicalAddress(const LogicalAddress &address) { logicalAddresses.push_back(address); }

    /** @brief Current lifecycle state, as close() left it. */
    int currentStatus(void) const { return status; }
    /**
     * @brief The CLOSED value of DriverAidlImpl's unnamed lifecycle enum.
     *
     * Read back through the class, so an assertion is tied to the definition, not a literal 0.
     */
    int closedState(void) const { return CLOSED; }
    /** @brief The OPENED value of the same unnamed enum, for the same reason. */
    int openedState(void) const { return OPENED; }
    /** @brief Whether a controller session reference is still held. */
    bool holdsController(void) const { return hdmiCecController != 0; }
    /** @brief Whether the top-level service proxy is still held, i.e. a reopen is attemptable. */
    bool holdsService(void) const { return hdmiCecService != 0; }
    /** @brief Whether a listener reference is still held. */
    bool holdsListener(void) const { return eventListener != 0; }
    /** @brief Size of the local logical-address list. */
    size_t logicalAddressCount(void) const { return logicalAddresses.size(); }
};

/**
 * @brief A SessionStateProbe that also exposes the enable-time address allocation.
 *
 * Drives DriverAidlImpl::registerDeviceLogicalAddress() on an injected session, because the real
 * open() cannot run on a driverless host, and exposes the protected candidate table.
 */
class AllocationProbe : public SessionStateProbe {
public:
    using DriverAidlImpl::logicalAddressCandidates;

    /** @brief Runs the allocation open() performs once the session is OPENED. */
    void registerAddress(void) { registerDeviceLogicalAddress(); }

    /** @brief The local logical-address list, in order. */
    std::vector<int> heldAddresses(void) const {
        std::vector<int> held;
        for (std::list<LogicalAddress>::const_iterator it = logicalAddresses.begin();
             it != logicalAddresses.end(); ++it) {
            held.push_back(it->toInt());
        }
        return held;
    }
};

/**
 * @brief A FakeHdmiCecController that refuses one chosen address and accepts every other.
 *
 * Reaches the allocation arm where the HAL declines a free candidate and the next one is tried.
 */
class DecliningControllerDouble : public FakeHdmiCecController {
public:
    /** @brief The address whose addLogicalAddresses() call reports false. */
    int32_t declinedAddress = LogicalAddress::PLAYBACK_DEVICE_1;
    /** @brief How many add calls this double refused. */
    unsigned int declinedCalls = 0;

    /** @brief Reports false for a one-element call naming declinedAddress; defers otherwise. */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                 bool *_aidl_return) override {
        if ((logicalAddresses.size() == 1) && (logicalAddresses[0] == declinedAddress)) {
            declinedCalls++;
            if (_aidl_return != nullptr) {
                *_aidl_return = false;
            }
            return ::android::binder::Status::ok();
        }
        return FakeHdmiCecController::addLogicalAddresses(logicalAddresses, _aidl_return);
    }
};

/**
 * @brief An IHdmiCecController that journals every address call in order and forwards each call.
 *
 * Forwards to the service's own FakeHdmiCecController, so FakeHdmiCecService::getLogicalAddresses()
 * keeps reporting exactly what the journalled calls left registered.
 */
class JournalingControllerDouble : public cechal::BnHdmiCecController {
public:
    /** @brief One journalled call: its operation, its vector and the registrations it left. */
    struct Entry {
        /** @brief "add" or "remove". */
        std::string operation;
        /** @brief The vector the adapter marshalled. */
        std::vector<int32_t> addresses;
        /** @brief The forwarded fake's registrations once the call returned. */
        std::vector<int32_t> registeredAfter;
    };

    /**
     * @brief Journals and forwards every address call to @p forwardTo.
     *
     * @param [in] forwardTo - The fake that answers, normally FakeHdmiCecService::getController().
     */
    explicit JournalingControllerDouble(const ::android::sp<FakeHdmiCecController> &forwardTo)
        : target(forwardTo) {}

    /** @brief Forwards the add, then journals it with the registrations it left. */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                 bool *_aidl_return) override {
        const ::android::binder::Status status = target->addLogicalAddresses(logicalAddresses, _aidl_return);
        journal.push_back({ "add", logicalAddresses, target->getRegisteredLogicalAddresses() });
        return status;
    }

    /** @brief Forwards the removal, then journals it with the registrations it left. */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                    bool *_aidl_return) override {
        const ::android::binder::Status status = target->removeLogicalAddresses(logicalAddresses, _aidl_return);
        journal.push_back({ "remove", logicalAddresses, target->getRegisteredLogicalAddresses() });
        return status;
    }

    /** @brief Forwards a transmit, allocation polls included, without journalling it. */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> &message,
                                         cechal::SendMessageStatus *_aidl_return) override {
        return target->sendMessage(message, _aidl_return);
    }

    /**
     * @brief Renders the calls from index @p first on as, for example, "remove{4} add{5}".
     *
     * @param [in] first - Index of the first journalled call to render.
     * @return std::string - The calls in order, space separated; empty when there are none.
     */
    std::string sequenceSince(size_t first) const {
        std::string rendered;
        for (size_t i = first; i < journal.size(); i++) {
            rendered += (rendered.empty() ? "" : " ") + journal[i].operation + "{";
            for (size_t j = 0; j < journal[i].addresses.size(); j++) {
                rendered += (j == 0 ? "" : ",") + std::to_string(journal[i].addresses[j]);
            }
            rendered += "}";
        }
        return rendered;
    }

    /** @brief The most addresses the fake held after any journalled call. */
    size_t mostRegisteredAtOnce(void) const {
        size_t most = 0;
        for (size_t i = 0; i < journal.size(); i++) {
            most = std::max(most, journal[i].registeredAfter.size());
        }
        return most;
    }

    /** @brief Every forwarded add and remove call, in call order. */
    std::vector<Entry> journal;

protected:
    /** @brief The fake every call is forwarded to. */
    const ::android::sp<FakeHdmiCecController> target;
};

/**
 * @brief A FakeHdmiCecController whose allocation poll of one address raises a non-CEC exception.
 *
 * Reaches the allocation arms where a poll fails with a standard or a non-standard exception.
 */
class RaisingPollControllerDouble : public FakeHdmiCecController {
public:
    /** @brief The address whose self-addressed one-byte poll raises instead of answering. */
    int32_t raisingAddress = LogicalAddress::PLAYBACK_DEVICE_1;
    /** @brief true to raise an int rather than std::runtime_error. */
    bool raisesNonStandard = false;
    /** @brief How many polls this double raised from. */
    unsigned int raisedPolls = 0;

    /** @brief Raises for the poll of raisingAddress; defers every other message to the fake. */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> &message,
                                         cechal::SendMessageStatus *_aidl_return) override {
        if ((message.size() == 1) &&
            (message[0] == static_cast<uint8_t>(((raisingAddress & 0x0F) << 4) | (raisingAddress & 0x0F)))) {
            raisedPolls++;
            if (raisesNonStandard) {
                throw raisingAddress;
            }
            throw std::runtime_error("injected allocation poll failure");
        }
        return FakeHdmiCecController::sendMessage(message, _aidl_return);
    }
};

/**
 * @brief A FakeHdmiCecController whose addLogicalAddresses() raises, before or after registering.
 *
 * Reaches the allocation arm where the add itself raises and a compensating removal is attempted.
 */
class RaisingAddControllerDouble : public FakeHdmiCecController {
public:
    /** @brief true to register through the fake and then raise; false to raise std::bad_alloc first. */
    bool registersBeforeRaising = false;
    /** @brief true to make the compensating removeLogicalAddresses() raise as well. */
    bool removalRaises = false;
    /** @brief false to defer every add to the fake, as a HAL that has recovered does. */
    bool addRaises = true;
    /** @brief How many add calls this double raised from. */
    unsigned int raisedAdds = 0;
    /** @brief How many removal calls this double raised from. */
    unsigned int raisedRemovals = 0;

    /**
     * @brief Raises std::bad_alloc, or registers through the fake and then raises std::runtime_error;
     *        defers to the fake while addRaises is false.
     */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                 bool *_aidl_return) override {
        if (!addRaises) {
            return FakeHdmiCecController::addLogicalAddresses(logicalAddresses, _aidl_return);
        }
        raisedAdds++;
        if (registersBeforeRaising) {
            FakeHdmiCecController::addLogicalAddresses(logicalAddresses, _aidl_return);
            throw std::runtime_error("injected failure after the registration took effect");
        }
        throw std::bad_alloc();
    }

    /** @brief Raises std::runtime_error when removalRaises is set; defers to the fake otherwise. */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                    bool *_aidl_return) override {
        if (removalRaises) {
            raisedRemovals++;
            throw std::runtime_error("injected compensating removal failure");
        }
        return FakeHdmiCecController::removeLogicalAddresses(logicalAddresses, _aidl_return);
    }
};

/**
 * @brief A JournalingControllerDouble whose next add or removal ends without a confirmed outcome.
 *
 * The call raises std::bad_alloc or returns FAILED_TRANSACTION, before or after the fake applies it,
 * or raises an int before it, and is journalled with "!" after its operation, so "remove!{4}" is a
 * call the adapter saw fail.
 */
class UnconfirmedOutcomeControllerDouble : public JournalingControllerDouble {
public:
    /** @brief How the next add or removal ends. */
    enum class Outcome {
        CONFIRMED,        /**< Forwarded and answered by the fake. */
        RAISES_UNAPPLIED, /**< Raises std::bad_alloc without reaching the fake. */
        RAISES_APPLIED,   /**< Reaches the fake, then raises std::bad_alloc. */
        FAILS_UNAPPLIED,  /**< Returns FAILED_TRANSACTION without reaching the fake. */
        FAILS_APPLIED,    /**< Reaches the fake, then returns FAILED_TRANSACTION. */
        RAISES_NON_STANDARD_UNAPPLIED /**< Raises an int, not a std::exception, without reaching the fake. */
    };

    using JournalingControllerDouble::JournalingControllerDouble;

    /** @brief How the next add ends; reverts to CONFIRMED once used. */
    Outcome nextAdd = Outcome::CONFIRMED;
    /** @brief How the next removal ends; reverts to CONFIRMED once used. */
    Outcome nextRemove = Outcome::CONFIRMED;

    /** @brief Journals the add, forwarding it unless nextAdd says otherwise, and ends it as nextAdd says. */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                 bool *_aidl_return) override {
        const Outcome outcome = nextAdd;
        nextAdd = Outcome::CONFIRMED;
        if (!applies(outcome)) {
            return endUnapplied("add", logicalAddresses, outcome);
        }
        return endApplied(outcome, JournalingControllerDouble::addLogicalAddresses(logicalAddresses, _aidl_return));
    }

    /** @brief Journals the removal, forwarding it unless nextRemove says otherwise, and ends it as nextRemove says. */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t> &logicalAddresses,
                                                    bool *_aidl_return) override {
        const Outcome outcome = nextRemove;
        nextRemove = Outcome::CONFIRMED;
        if (!applies(outcome)) {
            return endUnapplied("remove", logicalAddresses, outcome);
        }
        return endApplied(outcome, JournalingControllerDouble::removeLogicalAddresses(logicalAddresses, _aidl_return));
    }

private:
    /** @brief Whether @p outcome lets the call reach the fake. */
    static bool applies(Outcome outcome) {
        return (outcome != Outcome::RAISES_UNAPPLIED) && (outcome != Outcome::FAILS_UNAPPLIED) &&
               (outcome != Outcome::RAISES_NON_STANDARD_UNAPPLIED);
    }

    /** @brief Journals a call that never reached the fake, then raises or fails as @p outcome says. */
    ::android::binder::Status endUnapplied(const char *operation, const ::std::vector<int32_t> &addresses,
                                           Outcome outcome) {
        journal.push_back({ std::string(operation) + "!", addresses, target->getRegisteredLogicalAddresses() });
        if (outcome == Outcome::RAISES_UNAPPLIED) {
            throw std::bad_alloc();
        }
        if (outcome == Outcome::RAISES_NON_STANDARD_UNAPPLIED) {
            throw static_cast<int>(addresses.size());
        }
        return ::android::binder::Status::fromStatusT(::android::FAILED_TRANSACTION);
    }

    /** @brief Returns the fake's @p status, or marks the journalled call and raises or fails instead. */
    ::android::binder::Status endApplied(Outcome outcome, const ::android::binder::Status &status) {
        if (outcome == Outcome::CONFIRMED) {
            return status;
        }
        journal.back().operation += "!";
        if (outcome == Outcome::RAISES_APPLIED) {
            throw std::bad_alloc();
        }
        return ::android::binder::Status::fromStatusT(::android::FAILED_TRANSACTION);
    }
};

/**
 * @brief Asserts the HAL's registrations, the HAL-backed query and the local list after one step.
 *
 * @param [in] service    - The service whose own controller fake holds the registrations.
 * @param [in] probe      - The instance under test.
 * @param [in] registered - The registrations the fake must hold; never more than one.
 * @param [in] held       - The local list the probe must hold, which isValidLogicalAddress() must match.
 * @param [in] step       - Names the step in every failure message.
 */
void expectRegistrationState(const ::android::sp<FakeHdmiCecService> &service, AllocationProbe &probe,
                             const std::vector<int32_t> &registered, const std::vector<int> &held,
                             const std::string &step) {
    const std::vector<int32_t> actual = service->getController()->getRegisteredLogicalAddresses();
    const int addresses[] = { LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::AUDIO_SYSTEM,
                              LogicalAddress::PLAYBACK_DEVICE_2, LogicalAddress::PLAYBACK_DEVICE_3 };

    EXPECT_LE(actual.size(), 1u) << step << ": the HAL holds more than one address";
    EXPECT_EQ(actual, registered) << step << ": the HAL's registrations differ";
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), registered.empty() ? 0 : registered[0])
        << step << ": the HAL-backed query reports a different address";
    EXPECT_EQ(probe.heldAddresses(), held) << step << ": the local list differs";
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        EXPECT_EQ(probe.isValidLogicalAddress(LogicalAddress(addresses[i])),
                  std::find(held.begin(), held.end(), addresses[i]) != held.end())
            << step << ": isValidLogicalAddress(" << addresses[i] << ") disagrees with the local list";
    }
}

/** @brief Global operator new calls this thread has left before the injected failure; 0 when disarmed. */
thread_local unsigned long tlAllocationsUntilFailure = 0;

/** @brief Global operator new calls made on this thread while a ScopedAllocationFailure is in scope. */
thread_local unsigned long tlAllocationsCounted = 0;

/** @brief Whether a ScopedAllocationFailure is in scope on this thread. */
thread_local bool tlAllocationsWatched = false;

/** @brief Whether this thread's armed countdown has raised its one std::bad_alloc. */
thread_local bool tlAllocationFailureInjected = false;

/**
 * @brief Counts this thread's global operator new calls while in scope and, when armed, makes one raise.
 *
 * Only the constructing thread is affected, and only until destruction, so every other allocation in
 * this binary takes the default path of this file's replacement operator new.
 *
 * @see operator new(std::size_t)
 */
class ScopedAllocationFailure {
public:
    /**
     * @brief Starts counting and arms the countdown.
     *
     * @param [in] failingAllocation - 1-based index of the call that raises std::bad_alloc; 0 only counts.
     */
    explicit ScopedAllocationFailure(unsigned long failingAllocation) {
        tlAllocationsCounted = 0;
        tlAllocationFailureInjected = false;
        tlAllocationsUntilFailure = failingAllocation;
        tlAllocationsWatched = true;
    }

    /** @brief Stops counting and disarms the countdown. */
    ~ScopedAllocationFailure() {
        tlAllocationsWatched = false;
        tlAllocationsUntilFailure = 0;
    }

    /** @brief Not copyable: each scope owns the thread's one countdown. */
    ScopedAllocationFailure(const ScopedAllocationFailure &) = delete;
    /** @brief Not assignable, for the same reason. */
    ScopedAllocationFailure &operator=(const ScopedAllocationFailure &) = delete;

    /** @brief Calls counted so far, the raising one included. */
    unsigned long allocations(void) const { return tlAllocationsCounted; }

    /** @brief Whether the armed call was reached and raised. */
    bool injected(void) const { return tlAllocationFailureInjected; }
};

} // namespace

/**
 * @brief Replaces the global operator new for run_L1Tests: the default behaviour, plus the one
 *        std::bad_alloc a ScopedAllocationFailure on the calling thread arms.
 *
 * Unwatched, it is the default: malloc, retried through the installed new_handler, and
 * std::bad_alloc when there is none.
 *
 * @param [in] size - Bytes requested; 0 is served as 1.
 * @return void* - The allocation, never null.
 * @see ScopedAllocationFailure
 */
void *operator new(std::size_t size)
{
    if (tlAllocationsWatched) {
        tlAllocationsCounted++;
        if ((tlAllocationsUntilFailure != 0) && (--tlAllocationsUntilFailure == 0)) {
            tlAllocationFailureInjected = true;
            throw std::bad_alloc();
        }
    }

    if (size == 0) {
        size = 1;
    }

    for (;;) {
        void *block = std::malloc(size);
        if (block != nullptr) {
            return block;
        }

        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

/**
 * @brief Releases a block from the replacement operator new, as the default does.
 *
 * @param [in] block - The block, or null.
 */
__attribute__((noinline)) void operator delete(void *block) noexcept
{
    std::free(block);
}

/**
 * @brief The sized form compiled code calls; the size is not needed to release the block.
 *
 * @param [in] block - The block, or null.
 */
__attribute__((noinline)) void operator delete(void *block, std::size_t) noexcept
{
    std::free(block);
}

/**
 * @brief Fixture for the properties that hold on a locally constructed back-end, under every
 *        invocation and whichever back-end the process resolved to.
 *
 * Every case builds its own DriverAidlImpl or DriverImpl rather than using Driver::getInstance(),
 * so the resolved selection and the shared driver are never involved. Plain instances stay
 * CLOSED; probes force the lifecycle state or inject in-process service and controller doubles.
 *
 * @see DriverAidlSessionFixture for properties that need the shared driver's opened AIDL session.
 */
class DriverAidlLocalInstanceTest : public ::testing::Test {
protected:
    /**
     * @brief Clears any legacy HAL expectations left behind by a preceding case.
     *
     * @note Establishes no driver precondition, because none is needed - see the body.
     */
    void SetUp() override {
        mock = HdmiCecDriverMock::getInstance();
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        // No driver precondition is needed: every case drives a local instance and leaves the
        // process-global driver untouched, so TearDown restores nothing.
    }

    /**
     * @brief Verifies and clears legacy HAL expectations, so an unmet one is attributed to
     *        the case that set it.
     *
     * @note Restores no process-global state, because no case in this fixture disturbs any.
     */
    void TearDown() override {
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }
    }

    /**
     * @brief The legacy HAL mock, retained so expectations can be cleared around each case.
     *
     * @note Most cases assert that no legacy entry point is reached, a strict-mock property.
     */
    HdmiCecDriverMock *mock = nullptr;
};

/**
 * @brief writeAsync's frame prelude runs before its state guard on the AIDL back-end, so a closed
 *        driver handed an empty frame reports the decode failure rather than the invalid state.
 * @pre A locally constructed CLOSED DriverAidlImpl, under every invocation.
 * @note A well-formed frame on the same instance is stopped by the state guard: the positive
 *       control that makes this an ordering proof. No legacy HAL entry point is reached.
 */
TEST_F(DriverAidlLocalInstanceTest, FrameLoggingPrecedesStateGuardInWriteAsync) {
    ASSERT_NE(mock, nullptr);

    // The frame never survives the prelude, so nothing may reach the legacy HAL either -
    // and on this back-end nothing ever should, at any point.
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);

    DriverAidlImpl closedDriver;
    CECFrame emptyFrame;

    EXPECT_THROW({ closedDriver.writeAsync(emptyFrame); }, std::out_of_range)
        << "an empty frame on a closed AIDL driver did not raise std::out_of_range, so the frame "
           "prelude no longer runs ahead of the state guard";

    CECFrame wellFormed = directedFrame();
    EXPECT_THROW({ closedDriver.writeAsync(wellFormed); }, InvalidStateException)
        << "a well-formed frame on a closed AIDL driver did not raise InvalidStateException, so "
           "the state guard is no longer reached";
}

/**
 * @brief The same prelude-before-guard ordering holds on the legacy back-end, so the two are
 *        compared rather than assumed to agree.
 * @pre A locally constructed CLOSED DriverImpl, under every invocation.
 * @note The authorized writeAsync difference arises only once prelude and guard have both passed,
 *       so the failure modes before that point must match on both back-ends.
 */
TEST_F(DriverAidlLocalInstanceTest, WriteAsyncPreludeOrderIsIdenticalOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);

    DriverImpl closedLegacyDriver;
    CECFrame emptyFrame;

    EXPECT_THROW({ closedLegacyDriver.writeAsync(emptyFrame); }, std::out_of_range)
        << "the legacy back-end no longer raises std::out_of_range for an empty frame on a closed "
           "driver, so the two back-ends' prelude ordering has diverged";

    CECFrame wellFormed = directedFrame();
    EXPECT_THROW({ closedLegacyDriver.writeAsync(wellFormed); }, InvalidStateException)
        << "the legacy back-end no longer raises InvalidStateException for a well-formed frame on "
           "a closed driver";
}

/**
 * @brief Every state-guarded operation refuses a closed AIDL driver before touching any HAL.
 * @pre A locally constructed CLOSED DriverAidlImpl, under every invocation.
 * @note poll() has no guard of its own and reaches it through this back-end's write(), so it
 *       proves the internal call goes to the right place.
 * @warning A read() that got past the guard would block on the empty queue and hang the binary.
 */
TEST_F(DriverAidlLocalInstanceTest, EveryStateGuardedOperationRefusesAClosedDriver) {
    ASSERT_NE(mock, nullptr);

    // Not one legacy HAL entry point may be reached from the AIDL back-end, in any state.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _)).Times(0);

    DriverAidlImpl closedDriver;
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame = directedFrame();

    EXPECT_THROW({ closedDriver.write(frame); }, InvalidStateException)
        << "write() accepted a frame on a closed driver";
    EXPECT_THROW({ closedDriver.writeAsync(frame); }, InvalidStateException)
        << "writeAsync() accepted a frame on a closed driver";
    EXPECT_THROW({ closedDriver.addLogicalAddress(address); }, InvalidStateException)
        << "addLogicalAddress() claimed an address on a closed driver, so the driver's bookkeeping "
           "and the HAL's could diverge";
    EXPECT_THROW({ closedDriver.removeLogicalAddress(address); }, InvalidStateException)
        << "removeLogicalAddress() released an address on a closed driver";
    EXPECT_THROW({ closedDriver.poll(address, LogicalAddress(LogicalAddress::TV)); },
                 InvalidStateException)
        << "poll() transmitted on a closed driver, so it is no longer reaching the guard through "
           "this back-end's own write()";

    CECFrame received;
    EXPECT_THROW({ closedDriver.read(received); }, InvalidStateException)
        << "read() did not refuse a closed driver on entry. It must refuse before reaching the "
           "queue: with an empty queue EventQueue::poll blocks by contract, so a read that got "
           "past the guard here would hang the whole test binary rather than fail";
}

/**
 * @brief The address guards are state checks rather than address checks, so no logical address
 *        slips past them while closed.
 * @pre A locally constructed CLOSED DriverAidlImpl, under every invocation.
 * @note The sweep spans the TV, an interior address and broadcast/unregistered; neither add nor
 *       remove reaches the legacy HAL.
 */
TEST_F(DriverAidlLocalInstanceTest, AddressGuardsRejectEveryLogicalAddressWhileClosed) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _)).Times(0);

    DriverAidlImpl closedDriver;

    const int addresses[] = {
        LogicalAddress::TV,                // minimum
        LogicalAddress::PLAYBACK_DEVICE_1, // interior
        LogicalAddress::BROADCAST          // maximum / unregistered
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        const LogicalAddress address(addresses[i]);
        EXPECT_THROW({ closedDriver.addLogicalAddress(address); }, InvalidStateException)
            << "addLogicalAddress accepted address " << addresses[i] << " while closed";
        EXPECT_THROW({ closedDriver.removeLogicalAddress(address); }, InvalidStateException)
            << "removeLogicalAddress accepted address " << addresses[i] << " while closed";
    }
}

/**
 * @brief close() on an instance that was never opened returns silently, and does so twice.
 * @pre A locally constructed CLOSED DriverAidlImpl, under every invocation.
 * @note Silence matches the legacy back-end, whose throw is compiled out; idempotence matters
 *       because Bus and LibCCEC reach Driver::close() from three places.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseOnANeverOpenedDriverReturnsSilently) {
    ASSERT_NE(mock, nullptr);

    // No legacy close may be attempted from the AIDL back-end, and no AIDL close can be
    // either, since no session was ever held.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    DriverAidlImpl closedDriver;

    EXPECT_NO_THROW({ closedDriver.close(); })
        << "close() raised on a driver that was never opened. The legacy back-end returns "
           "silently - its throw is compiled out - so raising here is an unregistered difference";

    EXPECT_NO_THROW({ closedDriver.close(); })
        << "a second close() raised, so close is not idempotent; Bus and LibCCEC both reach "
           "Driver::close() and neither tracks whether another already did";
}

/**
 * @brief A failed close() still leaves the instance fully closed, on both failure shapes.
 * @pre A SessionStateProbe holding an injected session over a ClosingServiceDouble.
 * @note Asserted after the IOException: state CLOSED, controller released, service proxy
 *       retained, no listener, local address list untouched, and exactly one HAL close.
 */
TEST_F(DriverAidlLocalInstanceTest, AFailedCloseLeavesTheInstanceClosedWithNoSessionReferences) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end reaches no legacy entry point, on this path least of all.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    /** @brief One close() failure shape: its label, transaction status and boolean result. */
    struct FailureShape {
        const char *name;
        ::android::binder::Status status;
        bool result;
    };

    const FailureShape shapes[] = {
        // Transaction arrived and the HAL refused: ok status, false result.
        { "ok transaction reporting a false result", ::android::binder::Status::ok(), false },
        // Transport failure: the far side is gone, nothing was decided.
        { "non-ok transaction (DEAD_OBJECT)",
          ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT), true },
        // Refusal expressed as an exception code, which is what a live HAL sends.
        { "non-ok transaction (EX_ILLEGAL_STATE)",
          ::android::binder::Status::fromExceptionCode(::android::binder::Status::EX_ILLEGAL_STATE,
                                                       "no session is open"),
          true }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
        const ::android::sp<cechal::IHdmiCecController> controller =
            ::android::sp<cechal::IHdmiCecControllerDefault>::make();

        service->closeStatus = shapes[i].status;
        service->closeResult = shapes[i].result;

        {
            SessionStateProbe probe;

            probe.injectOpenSession(service, controller);
            probe.seedLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1));

            // Positive control: close() is silent on any state but OPENED, so a botched
            // injection would yield a green case that never reached the HAL.
            ASSERT_EQ(probe.currentStatus(), probe.openedState())
                << "the injected session state is not OPENED, so close() would return silently "
                   "and this case would assert nothing";

            EXPECT_THROW({ probe.close(); }, IOException)
                << "close() returned normally for the " << shapes[i].name
                << ". Both failure shapes must reach the caller as IOException, exactly as the "
                   "legacy back-end raises on a failed HdmiCecClose()";

            EXPECT_EQ(probe.currentStatus(), probe.closedState())
                << "the state is not CLOSED after the " << shapes[i].name
                << ". close() sets it before it raises precisely so that a caller which swallows "
                   "the exception - LibCCEC::term() and the destructor both do - still sees a "
                   "consistently closed object";

            EXPECT_FALSE(probe.holdsController())
                << "a controller session reference survived the " << shapes[i].name
                << ". The HAL may have dropped that session, so holding it is a reference to "
                   "something that no longer exists";

            EXPECT_TRUE(probe.holdsService())
                << "the service proxy was released by the " << shapes[i].name
                << ". It is the resolved-once selection result and holds no session state; "
                   "dropping it would make a reopen impossible without re-running the whole "
                   "preflight and lookup";

            EXPECT_FALSE(probe.holdsListener())
                << "a listener reference appeared from nowhere during the " << shapes[i].name;

            EXPECT_EQ(probe.logicalAddressCount(), 1u)
                << "the local logical-address list was cleared by the " << shapes[i].name
                << ". DriverImpl::close() does not clear it, so clearing here would be a fourth "
                   "observable difference between the back-ends";

            EXPECT_EQ(service->closeCalls, 1u)
                << "IHdmiCec::close() was called " << service->closeCalls << " times for the "
                << shapes[i].name << ", not once. close() must neither retry nor skip the call";

            EXPECT_EQ(service->lastControllerClosed.get(), controller.get())
                << "close() handed the HAL a controller reference that is not the session's";
        }

        // Destruction after a failed close must be inert: the state is already CLOSED, so the
        // destructor's close() returns silently rather than calling the HAL a second time.
        EXPECT_EQ(service->closeCalls, 1u)
            << "destruction after the " << shapes[i].name
            << " called IHdmiCec::close() again. The destructor closes only an OPENED instance, "
               "and a failed close leaves it CLOSED";
    }
}

/**
 * @brief A failed close() leaves an instance from which a reopen can be attempted, and the
 *        refusal such a reopen could meet is unambiguously a failure.
 * @pre A SessionStateProbe whose injected session's close() reports false.
 * @note The live reopen needs a binder driver and belongs to the session suite; this case asserts
 *       CLOSED with the service proxy retained, and that EX_ILLEGAL_STATE is a non-ok status.
 */
TEST_F(DriverAidlLocalInstanceTest, AFailedCloseLeavesTheInstanceReopenableOrCleanlyFailing) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
    const ::android::sp<cechal::IHdmiCecController> controller =
        ::android::sp<cechal::IHdmiCecControllerDefault>::make();

    service->closeResult = false;

    SessionStateProbe probe;

    probe.injectOpenSession(service, controller);

    ASSERT_THROW({ probe.close(); }, IOException);

    // Property 1: a reopen is attemptable. CLOSED is what open()'s own guard requires, and the
    // retained service proxy is what it calls; without either, open() could not even be tried.
    ASSERT_EQ(probe.currentStatus(), probe.closedState())
        << "a failed close left a state other than CLOSED, so open() would return silently "
           "instead of attempting a reopen - the object would be permanently unusable and "
           "would report nothing about why";
    ASSERT_TRUE(probe.holdsService())
        << "a failed close released the service proxy, so open() would raise IOException for "
           "the wrong reason - no proxy rather than a HAL-side session";

    // A second close is silent and reaches the HAL no further, so a caller retrying term()
    // cannot churn a session it no longer holds.
    EXPECT_NO_THROW({ probe.close(); })
        << "a second close() after a failed one raised. close() returns silently on any state "
           "other than OPENED, and Bus and LibCCEC both reach it without tracking each other";
    EXPECT_EQ(service->closeCalls, 1u)
        << "a second close() called the HAL again after the first one failed";

    // Property 2: open()'s only test is `!txn.isOk()`, so EX_ILLEGAL_STATE must never report ok,
    // or a HAL-side refusal would read as a successful reopen.
    const ::android::binder::Status refusal = ::android::binder::Status::fromExceptionCode(
        ::android::binder::Status::EX_ILLEGAL_STATE, "a session is already open");

    EXPECT_FALSE(refusal.isOk())
        << "EX_ILLEGAL_STATE now reports ok, so open()'s `!txn.isOk()` arm would not fire and a "
           "HAL-side refusal would be mistaken for a reopen";
    EXPECT_EQ(refusal.exceptionCode(), ::android::binder::Status::EX_ILLEGAL_STATE)
        << "the exception code does not survive Status::fromExceptionCode, so the diagnostic "
           "open() logs could not name the B2 condition";
}

/**
 * @brief A fresh instance holds no logical address, and isValidLogicalAddress answers without
 *        touching any HAL.
 * @pre A locally constructed DriverAidlImpl, under every invocation.
 * @note The whole address space is swept, because "holds nothing" is the claim.
 */
TEST_F(DriverAidlLocalInstanceTest, FreshInstanceHoldsNoLogicalAddressAndConsultsNoHal) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    const DriverAidlImpl freshDriver;

    for (int address = LogicalAddress::TV; address <= LogicalAddress::BROADCAST; address++) {
        EXPECT_FALSE(freshDriver.isValidLogicalAddress(LogicalAddress(address)))
            << "a freshly constructed AIDL back-end reported that it holds logical address "
            << address << ", although nothing has been added to it";
    }
}

/**
 * @brief open() without a cached service proxy raises IOException before anything touches
 *        libbinder, and without falling back to the legacy HAL.
 * @pre A locally constructed DriverAidlImpl, which never has a proxy.
 * @note The no-proxy check precedes the ProcessState threadpool call, so this runs without kernel
 *       binder support. The state is asserted unchanged, so a failed open cannot half-succeed.
 */
TEST_F(DriverAidlLocalInstanceTest, OpenWithoutAServiceProxyRaisesIoExceptionAndTouchesNoLegacyHal) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);
    EXPECT_CALL(*mock, HdmiCecSetRxCallback(_, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecSetTxCallback(_, _, _)).Times(0);

    DriverAidlImpl unresolvedDriver;

    EXPECT_THROW({ unresolvedDriver.open(); }, IOException)
        << "open() did not raise IOException on an instance holding no service proxy. If it "
           "instead reached ProcessState::self(), this process would have aborted rather than "
           "arrived here at all on a host without a binder driver";

    // Still closed, so the guards remain live - which is the proof that open() did not
    // half-succeed and leave the state OPENED with no session behind it.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ unresolvedDriver.write(frame); }, InvalidStateException)
        << "the instance behaves as though it were open after a failed open(), so open() moved the "
           "state before it had a session";
}

/**
 * @brief getLogicalAddress carries no state guard and answers 0 when it has no service to ask,
 *        whatever devType it is given.
 * @pre A locally constructed DriverAidlImpl holding no proxy, under every invocation.
 * @note Zero is the contract: LibCCEC::getLogicalAddress turns it into the InvalidStateException
 *       callers already handle for "no address".
 */
TEST_F(DriverAidlLocalInstanceTest, GetLogicalAddressReportsZeroWithoutAServiceAndIgnoresDevType) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    DriverAidlImpl unresolvedDriver;

    const int devTypes[] = { DeviceType::TV, DeviceType::TUNER, DeviceType::PLAYBACK_DEVICE };

    for (size_t i = 0; i < sizeof(devTypes) / sizeof(devTypes[0]); i++) {
        EXPECT_EQ(unresolvedDriver.getLogicalAddress(devTypes[i]), 0)
            << "getLogicalAddress reported a non-zero address for device type " << devTypes[i]
            << " although no AIDL service is held. Zero is what LibCCEC::getLogicalAddress turns "
               "into InvalidStateException, so any other value would report an address this "
               "device does not have";
    }
}

namespace {

/** @brief The physical address the AIDL back-end reports: 1.0.0.0, one nibble per byte. */
const unsigned int kFixedPhysicalAddress = 0x01000000u;

/** @brief The F.F.F.F value both plugins seed before the query, so an unwritten result shows. */
const unsigned int kPluginUnsetPhysicalAddress = 0x0F0F0F0Fu;

/** @brief Upper bound on a stalled transmit, so a failing case cannot hang the run. */
const long kStalledTransmitBoundMs = 10000;

/** @brief How long a physical-address query may take while a transmit is stalled. */
const long kPhysicalAddressAnswerBoundMs = 2000;

static_assert(DriverAidlImpl::FIXED_PHYSICAL_ADDRESS == kFixedPhysicalAddress,
              "DriverAidlImpl::FIXED_PHYSICAL_ADDRESS must encode 1.0.0.0 as 0x01000000, the "
              "nibble-per-byte form both plugins decode");

/**
 * @brief Renders a LibCCEC physical address as dotted text, decoding it as both plugins do.
 *
 * @param [in] value - The value getPhysicalAddress() wrote.
 *
 * @return std::string - The text PhysicalAddress::toString() produces, e.g. "1.0.0.0".
 */
std::string decodeAsThePluginsDo(unsigned int value) {
    return PhysicalAddress((uint8_t)((value >> 24) & 0xFF), (uint8_t)((value >> 16) & 0xFF),
                           (uint8_t)((value >> 8) & 0xFF), (uint8_t)(value & 0xFF)).toString();
}

/**
 * @brief An IHdmiCec double that counts every method call, so a case can prove none was made.
 *
 * @note close() succeeds so a probe can be closed; every other method keeps the default answer.
 */
class CallCountingServiceDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief Calls made to any method of this double. */
    unsigned int calls = 0;

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status getState(cechal::State *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::getState(_aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [in]  property     - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status getProperty(cechal::Property property,
                                         ::std::optional<::com::rdk::hal::PropertyValue> *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::getProperty(property, _aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status getLogicalAddresses(::std::vector<int32_t> *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::getLogicalAddresses(_aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [in]  listener     - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status open(const ::android::sp<cechal::IHdmiCecEventListener> &listener,
                                  ::android::sp<cechal::IHdmiCecController> *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::open(listener, _aidl_return);
    }

    /**
     * @brief Counts the call and reports the session closed, so a probe can be closed.
     *
     * @param [out] _aidl_return - Set to true when non-null.
     * @return ::android::binder::Status - Always ok.
     */
    ::android::binder::Status close(const ::android::sp<cechal::IHdmiCecController> & /*controller*/,
                                   bool *_aidl_return) override {
        calls++;
        if (_aidl_return != nullptr) {
            *_aidl_return = true;
        }
        return ::android::binder::Status::ok();
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [in]  listener     - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status registerEventListener(const ::android::sp<cechal::IHdmiCecEventListener> &listener,
                                                   bool *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::registerEventListener(listener, _aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecDefault does.
     *
     * @param [in]  listener     - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status unregisterEventListener(const ::android::sp<cechal::IHdmiCecEventListener> &listener,
                                                     bool *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecDefault::unregisterEventListener(listener, _aidl_return);
    }

    /**
     * @brief Counts the call, then reports IHdmiCecDefault's interface version.
     *
     * @return int32_t - Always 0, the default's version.
     */
    int32_t getInterfaceVersion() override {
        calls++;
        return cechal::IHdmiCecDefault::getInterfaceVersion();
    }

    /**
     * @brief Counts the call, then reports IHdmiCecDefault's interface hash.
     *
     * @return std::string - Always empty, the default's hash.
     */
    std::string getInterfaceHash() override {
        calls++;
        return cechal::IHdmiCecDefault::getInterfaceHash();
    }
};

/**
 * @brief An IHdmiCecController double that counts every method call, so a case can prove none
 *        was made.
 */
class CallCountingControllerDouble : public cechal::IHdmiCecControllerDefault {
public:
    /** @brief Calls made to any method of this double. */
    unsigned int calls = 0;

    /**
     * @brief Counts the call, then answers as IHdmiCecControllerDefault does.
     *
     * @param [in]  addresses    - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t> &addresses,
                                                 bool *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecControllerDefault::addLogicalAddresses(addresses, _aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecControllerDefault does.
     *
     * @param [in]  addresses    - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t> &addresses,
                                                    bool *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecControllerDefault::removeLogicalAddresses(addresses, _aidl_return);
    }

    /**
     * @brief Counts the call, then answers as IHdmiCecControllerDefault does.
     *
     * @param [in]  message      - Forwarded unchanged.
     * @param [out] _aidl_return - Left unwritten by the default.
     * @return ::android::binder::Status - Always UNKNOWN_TRANSACTION, the default's answer.
     */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> &message,
                                         cechal::SendMessageStatus *_aidl_return) override {
        calls++;
        return cechal::IHdmiCecControllerDefault::sendMessage(message, _aidl_return);
    }

    /**
     * @brief Counts the call, then reports IHdmiCecControllerDefault's interface version.
     *
     * @return int32_t - Always 0, the default's version.
     */
    int32_t getInterfaceVersion() override {
        calls++;
        return cechal::IHdmiCecControllerDefault::getInterfaceVersion();
    }

    /**
     * @brief Counts the call, then reports IHdmiCecControllerDefault's interface hash.
     *
     * @return std::string - Always empty, the default's hash.
     */
    std::string getInterfaceHash() override {
        calls++;
        return cechal::IHdmiCecControllerDefault::getInterfaceHash();
    }
};

/**
 * @brief An IHdmiCecController double whose sendMessage() stalls until released, so write()
 *        holds the instance lock across an unanswered HAL call.
 *
 * @note The stall is bounded by kStalledTransmitBoundMs, so a failing case cannot hang the run.
 */
class StallingTransmitDouble : public cechal::IHdmiCecControllerDefault {
public:
    /** @brief Set once sendMessage() has been entered. */
    MonotonicLatch entered;
    /** @brief Set by the case to let sendMessage() return. */
    MonotonicLatch released;

    /**
     * @brief Sets @c entered, then waits for @c released for at most kStalledTransmitBoundMs.
     *
     * @param [out] _aidl_return - Set to ACK_STATE_0 when non-null.
     * @return ::android::binder::Status - Always ok, whether released or timed out.
     */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> & /*message*/,
                                         cechal::SendMessageStatus *_aidl_return) override {
        entered.notify();
        (void)released.wait(kStalledTransmitBoundMs);
        if (_aidl_return != nullptr) {
            *_aidl_return = cechal::SendMessageStatus::ACK_STATE_0;
        }
        return ::android::binder::Status::ok();
    }
};

/**
 * @brief Everything either stalled-transmit worker can reach, allocated off the test stack.
 *
 * A worker abandoned by a bounded disposal may still be inside production write() when the case
 * returns, so everything it reaches lives here, behind a pointer.
 */
struct StalledTransmitState {
    /** @brief Creates both doubles, a probe holding no session and an unwritten query result. */
    StalledTransmitState(void)
        : service(::android::sp<CallCountingServiceDouble>::make())
        , controller(::android::sp<StallingTransmitDouble>::make())
        , frame(directedFrame())
        , physicalAddress(kPluginUnsetPhysicalAddress)
    {}

    /** @brief The IHdmiCec double the probe's session holds. */
    ::android::sp<CallCountingServiceDouble> service;

    /** @brief The IHdmiCecController double whose sendMessage() stalls until released. */
    ::android::sp<StallingTransmitDouble> controller;

    /** @brief The instance under test, whose instance lock the stalled transmit holds. */
    SessionStateProbe probe;

    /** @brief The frame the transmit worker writes. */
    const CECFrame frame;

    /** @brief What the query worker's getPhysicalAddress() wrote, seeded with F.F.F.F. */
    unsigned int physicalAddress;

    /** @brief What the transmit worker's write() raised; empty when it raised nothing. */
    std::string transmitFailure;

    /** @brief What the query worker's getPhysicalAddress() raised; empty when it raised nothing. */
    std::string queryFailure;

    /** @brief The worker whose write() stalls inside sendMessage(). */
    BoundedWorker transmitter;

    /** @brief The worker that queries the physical address during the stall. */
    BoundedWorker querier;

private:
    /** @brief Copy construction is disabled: declared private and never defined. */
    StalledTransmitState(const StalledTransmitState &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    StalledTransmitState & operator = (const StalledTransmitState &);
};

/**
 * @brief Owns the stalled-transmit state and releases it without an unbounded wait.
 *
 * Releasing the stall lets both workers finish; each is joined if its body returns within
 * kStalledTransmitBoundMs and detached otherwise. The state is freed only when both were joined,
 * and retained by design once either was abandoned.
 */
class StalledTransmitHarness {
public:
    /**
     * @brief Allocates the shared state on the heap, off the test stack.
     * @post The state is reachable through operator*() and operator->(), and outlives this
     *       harness whenever a worker had to be abandoned.
     */
    StalledTransmitHarness(void) : state(new StalledTransmitState()), abandoned(false) {}

    /**
     * @brief Releases the stall and disposes both workers, then frees the state unless one was
     *        abandoned.
     */
    ~StalledTransmitHarness(void) {
        (void)releaseAndDisposeWorkers();

        if (abandoned) {
            /* Retained by design: a detached worker may still use it inside production code. */
            return;
        }

        delete state;
    }

    /**
     * @brief The shared state, for the case body to read and to hand to its workers.
     *
     * @return StalledTransmitState & - The heap-resident state this harness owns.
     */
    StalledTransmitState & operator *  (void) const { return *state; }

    /**
     * @brief The shared state, for the case body to read and to hand to its workers.
     *
     * @return StalledTransmitState * - The heap-resident state this harness owns; never null.
     */
    StalledTransmitState * operator -> (void) const { return state; }

    /**
     * @brief Releases the stalled transmit, then disposes both workers boundedly; idempotent.
     *
     * @return bool - true when both workers were joined; false once either had to be detached,
     *                which also makes the destructor retain the state.
     */
    bool releaseAndDisposeWorkers(void) {
        state->controller->released.notify();

        if (!state->querier.dispose(kStalledTransmitBoundMs)) {
            abandoned = true;
        }

        if (!state->transmitter.dispose(kStalledTransmitBoundMs)) {
            abandoned = true;
        }

        return !abandoned;
    }

private:
    /** @brief The heap-resident state; never null. */
    StalledTransmitState *state;

    /** @brief Set, and never cleared, once a bounded disposal had to detach a worker. */
    bool abandoned;

    /** @brief Copy construction is disabled: declared private and never defined. */
    StalledTransmitHarness(const StalledTransmitHarness &);
    /** @brief Copy assignment is disabled: declared private and never defined. */
    StalledTransmitHarness & operator = (const StalledTransmitHarness &);
};

} // namespace

/**
 * @brief getPhysicalAddress on a never-opened AIDL back-end reports 1.0.0.0, decoded as the
 *        plugins decode it, on every call.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl holding no proxy.
 * @note The variable is seeded with the plugins' F.F.F.F value, so an unwritten result fails.
 */
TEST_F(DriverAidlLocalInstanceTest, GetPhysicalAddressReportsTheFixedAddressOnANeverOpenedDriver) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);

    DriverAidlImpl aidlDriver;

    for (int call = 1; call <= 3; call++) {
        unsigned int physicalAddress = kPluginUnsetPhysicalAddress;

        EXPECT_NO_THROW({ aidlDriver.getPhysicalAddress(&physicalAddress); })
            << "call " << call << " raised; a physical-address query must always answer";
        EXPECT_EQ(physicalAddress, kFixedPhysicalAddress)
            << "call " << call << " did not report the fixed 1.0.0.0 encoding 0x01000000";
        EXPECT_EQ(decodeAsThePluginsDo(physicalAddress), "1.0.0.0")
            << "call " << call << " wrote a value the plugins would not decode as 1.0.0.0";
    }
}

/**
 * @brief getPhysicalAddress writes nothing through a null out-parameter and keeps answering.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl.
 */
TEST_F(DriverAidlLocalInstanceTest, GetPhysicalAddressToleratesANullOutParameter) {
    DriverAidlImpl aidlDriver;

    EXPECT_NO_THROW({ aidlDriver.getPhysicalAddress(nullptr); })
        << "a null out-parameter raised instead of being logged and ignored";

    unsigned int physicalAddress = kPluginUnsetPhysicalAddress;
    EXPECT_NO_THROW({ aidlDriver.getPhysicalAddress(&physicalAddress); });
    EXPECT_EQ(physicalAddress, kFixedPhysicalAddress)
        << "the query stopped answering 1.0.0.0 after a null out-parameter";
}

/**
 * @brief getPhysicalAddress reports 1.0.0.0 while OPENED and after close(), and calls no
 *        method on the service or the controller in either state.
 * @pre Runs under every invocation, on a SessionStateProbe holding call-counting doubles.
 * @note close() itself calls the service once; the assertion is on the delta across the query.
 */
TEST_F(DriverAidlLocalInstanceTest, GetPhysicalAddressIsFixedInEveryStateAndCallsNoAidlMethod) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _)).Times(0);

    const ::android::sp<CallCountingServiceDouble> service = ::android::sp<CallCountingServiceDouble>::make();
    const ::android::sp<CallCountingControllerDouble> controller = ::android::sp<CallCountingControllerDouble>::make();

    SessionStateProbe probe;
    probe.injectOpenSession(service, controller);
    ASSERT_EQ(probe.currentStatus(), probe.openedState());

    unsigned int physicalAddress = kPluginUnsetPhysicalAddress;
    EXPECT_NO_THROW({ probe.getPhysicalAddress(&physicalAddress); });
    EXPECT_EQ(physicalAddress, kFixedPhysicalAddress) << "the OPENED driver did not report 1.0.0.0";
    EXPECT_EQ(service->calls, 0u) << "the OPENED query called the IHdmiCec service";
    EXPECT_EQ(controller->calls, 0u) << "the OPENED query called the IHdmiCecController";

    ASSERT_NO_THROW({ probe.close(); });
    ASSERT_EQ(probe.currentStatus(), probe.closedState());
    const unsigned int serviceCallsAfterClose = service->calls;
    const unsigned int controllerCallsAfterClose = controller->calls;

    physicalAddress = kPluginUnsetPhysicalAddress;
    EXPECT_NO_THROW({ probe.getPhysicalAddress(&physicalAddress); });
    EXPECT_EQ(physicalAddress, kFixedPhysicalAddress) << "the CLOSED driver did not report 1.0.0.0";
    EXPECT_EQ(service->calls, serviceCallsAfterClose) << "the CLOSED query called the IHdmiCec service";
    EXPECT_EQ(controller->calls, controllerCallsAfterClose) << "the CLOSED query called the IHdmiCecController";
}

/**
 * @brief getPhysicalAddress answers while another thread's transmit is stalled inside the HAL
 *        holding the instance lock, so the query never waits on an AIDL call.
 * @pre Runs under every invocation, on a SessionStateProbe whose controller stalls sendMessage().
 */
TEST_F(DriverAidlLocalInstanceTest, GetPhysicalAddressAnswersWhileATransmitIsStalledInTheHal) {
    // The doubles, probe, frame, results and both workers live in the harness, freed once both
    // workers join and retained if either is abandoned.
    StalledTransmitHarness harness;
    StalledTransmitState  *shared = &*harness;

    shared->probe.injectOpenSession(shared->service, shared->controller);

    shared->transmitter.start([shared]() {
        try {
            shared->probe.write(shared->frame);
        }
        catch (const std::exception &e) {
            shared->transmitFailure = e.what();
        }
        catch (...) {
            shared->transmitFailure = "write() raised an exception that is not a std::exception";
        }
    });
    ASSERT_TRUE(shared->controller->entered.wait(kStalledTransmitBoundMs))
        << "the transmit never reached sendMessage(), so no HAL call is holding the lock";

    shared->querier.start([shared]() {
        try {
            shared->probe.getPhysicalAddress(&shared->physicalAddress);
        }
        catch (const std::exception &e) {
            shared->queryFailure = e.what();
        }
        catch (...) {
            shared->queryFailure =
                "getPhysicalAddress() raised an exception that is not a std::exception";
        }
    });
    const bool answeredDuringStall = shared->querier.finishedWithin(kPhysicalAddressAnswerBoundMs);

    ASSERT_TRUE(harness.releaseAndDisposeWorkers())
        << "a worker did not finish within " << kStalledTransmitBoundMs
        << " ms of the stall being released, so it was abandoned rather than joined and its "
           "state is retained by design. Nothing below can be interpreted";

    EXPECT_TRUE(answeredDuringStall)
        << "getPhysicalAddress waited for a stalled IHdmiCecController::sendMessage, so the "
           "physical address depends on an AIDL call completing";
    EXPECT_EQ(shared->physicalAddress, kFixedPhysicalAddress);
    EXPECT_TRUE(shared->transmitFailure.empty())
        << "the stalled transmit raised once released: [" << shared->transmitFailure << "]";
    EXPECT_TRUE(shared->queryFailure.empty())
        << "the physical-address query raised during the stall: [" << shared->queryFailure << "]";
}

/**
 * @brief The Sink call paths that the addLogicalAddress failure-mapping case models still match
 *        the real plugin source, so the model cannot go stale unnoticed.
 * @pre The Sink source is reachable through CEC_SINK_CALLER_SOURCE or beside the runner, under
 *      every invocation.
 * @note Asserts exactly two LibCCEC addLogicalAddress calls: the first inside a try with an
 *       IOException arm and a catch(...) arm, the enable-time one inside no try block.
 * @warning A missing or unreadable source fails the case rather than passing silently.
 * @see DriverAidlSessionTest::AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected
 */
TEST_F(DriverAidlLocalInstanceTest, TheModelledSinkCallPathsStillMatchTheRealSinkSource) {
    const SinkCallerSource source = locateAndReadSinkCallerSource();

    if (source.variableSet && !source.found) {
        FAIL() << kSinkCallerSourceVariable << " is set to [" << source.variableValue
               << "] but that file could not be read. Both CI workflows export this variable "
                  "only after confirming the file exists, so a set-but-unreadable value is a "
                  "WIRING FAULT in the environment: something pointed at a source that is not "
                  "there. It is deliberately diagnosed apart from 'nothing pointed at the "
                  "source at all', which the arm below reports, because the two call for "
                  "different fixes - and neither is tolerated, since either would leave the "
                  "only guard that notices the modelled Sink call paths drifting unexecuted";
    }

    if (!source.found) {
        // Missing required input, distinct from the wiring fault above. Failing rather than
        // returning keeps a skipped guard from being recorded as a pass.
        FAIL() << "the real Sink caller source could not be found, so the drift guard for the "
                  "modelled Sink call paths cannot run. IT IS A REQUIRED ACCEPTANCE INPUT, NOT "
                  "AN OPTIONAL DIAGNOSTIC: without it, "
                  "DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPaths"
                  "AsExpected still asserts against a MODEL of the plugin that nothing has "
                  "checked against its subject, and a model nobody checks is not evidence about "
                  "a caller. "
               << kSinkCallerSourceVariable << " was not set, and these paths were tried: "
               << joinPaths(source.pathsTried)
               << ". SUPPLY IT EITHER WAY: set " << kSinkCallerSourceVariable
               << " to the absolute path of " << kSinkCallerRelativePath
               << " - which is what both CI workflows do, from a checkout pinned at the "
                  "reviewed commit 2ad7e1a4712908a4d0fb838caebcbfcaabba55d8 - or check that "
                  "sibling component out beside this one so one of the candidate paths above "
                  "resolves";
    }

    CppSourceModel model;
    ASSERT_TRUE(model.scan(source.text, "addLogicalAddress("))
        << "the brace/comment/literal walk over [" << source.path
        << "] did not finish with a balanced block stack, so the model of that file cannot be "
           "trusted and no conclusion is drawn from it. The scanner handles line comments, block "
           "comments, string and character literals, but NOT raw string literals - if the plugin "
           "has adopted R\"(...)\" the guard must be re-derived rather than relaxed";

    const std::vector<SourceCallSite> &sites = model.callSites();

    ASSERT_EQ(sites.size(), 2u)
        << "[" << source.path << "] contains " << sites.size()
        << " calls to addLogicalAddress(, not the two the modelled case is written around. If the "
           "count has fallen the Sink has stopped calling the middleware on one of its paths and "
           "the model asserts about a caller that no longer exists; if it has risen there is a "
           "third call path whose exception disposition nobody has measured. Either way the model "
           "in DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected "
           "must be re-derived from the plugin before it is trusted again";

    for (size_t i = 0; i < sites.size(); i++) {
        EXPECT_NE(sites[i].precedingText.find("LibCCEC::getInstance()."), std::string::npos)
            << "the call at [" << source.path << ":" << sites[i].line
            << "] is not reached through LibCCEC::getInstance(), so it is some other "
               "addLogicalAddress and the guard is measuring the wrong call. Preceding code as "
               "read: [" << sites[i].precedingText << "]";
    }

    // Path 1, the allocation-loop call: inside a try whose handlers include the IOException arm
    // and the generic arm the model depends on.
    ASSERT_TRUE(sites[0].insideAnyTry)
        << "the first addLogicalAddress call, at [" << source.path << ":" << sites[0].line
        << "], is no longer inside a try block. The modelled case asserts that a refusal reaches "
           "a handled generic arm there and that a transport failure reaches a distinct "
           "IOException arm; with no try at all, both exceptions now propagate out of the "
           "allocation loop and the model describes handling that is gone";

    ASSERT_GE(sites[0].innermostTry, 0);
    const SourceTryBlock &guardingTry =
        model.tryBlocks()[static_cast<size_t>(sites[0].innermostTry)];

    bool hasIoExceptionHandler = false;
    bool hasGenericHandler = false;
    std::string handlerList;

    for (size_t i = 0; i < guardingTry.handlers.size(); i++) {
        if (!handlerList.empty()) {
            handlerList += " | ";
        }
        handlerList += guardingTry.handlers[i];

        if (guardingTry.handlers[i].find("IOException") != std::string::npos) {
            hasIoExceptionHandler = true;
        }
        if (guardingTry.handlers[i] == "...") {
            hasGenericHandler = true;
        }
    }

    EXPECT_TRUE(hasIoExceptionHandler)
        << "the try guarding the first addLogicalAddress call at [" << source.path << ":"
        << sites[0].line
        << "] has no IOException handler. The modelled case asserts that a NON-OK BINDER STATUS "
           "reaches a distinct IOException arm on this path - the one failure this caller "
           "diagnoses specifically - and without that arm it would land in the generic one "
           "instead, indistinguishable from a refused address. Handlers as read: [" << handlerList
        << "]";

    EXPECT_TRUE(hasGenericHandler)
        << "the try guarding the first addLogicalAddress call at [" << source.path << ":"
        << sites[0].line
        << "] has no catch(...) handler. The modelled case asserts that a REFUSED ADDRESS - which "
           "is AddressNotAvailableException, not IOException, and is authorized difference 3's "
           "coarser category - still lands in a handled arm on this path. Without a generic arm "
           "it would propagate out of the allocation loop instead. Handlers as read: ["
        << handlerList << "]";

    // Path 2 - the enable-time call. Outside every try, which is what makes the modelled
    // uncaught-propagation assertion the right shape for it.
    EXPECT_FALSE(sites[1].insideAnyTry)
        << "the second addLogicalAddress call, at [" << source.path << ":" << sites[1].line
        << "], is now inside a try block. The modelled case asserts that the exception PROPAGATES "
           "out of this path uncaught, which was the caller-visible consequence worth measuring; "
           "if the plugin now handles it, the interesting question becomes which arm it lands in "
           "and the model must be re-derived to ask that instead";

    std::cout << "[DriverAidlLocalInstanceTest] the modelled Sink call paths were checked against "
              << source.path << " (path 1 at line " << sites[0].line << ", inside a try with ["
              << handlerList << "]; path 2 at line " << sites[1].line << ", inside no try)."
              << std::endl;
}

/**
 * @brief The log-level guard raises the level, restores it verifiably, is idempotent, and leaves
 *        nothing behind at the shared path.
 * @pre The effective level is observable and custody of the shared configuration path can be
 *      obtained, under every invocation.
 * @note Lives in this fixture because it runs on every host, so the guard's custody protocol
 *       executes even where the session suite cannot.
 * @warning A refusal fails the case; tolerating it would make the guard's protocol unfalsifiable.
 */
TEST_F(DriverAidlLocalInstanceTest, TheLogLevelGuardRaisesTheLevelAndRestoresItVerifiably) {
    const int levelBefore = ScopedCecLogLevel::observeEffectiveLevel();

    ASSERT_NE(ScopedCecLogLevel::unobservableLevel(), levelBefore)
        << "no CCEC_LOG level produced any output, so the effective log level could not be "
           "observed at all and this case cannot tell a restored level from an unrestored one. "
           "That is an observation failure rather than a defect in the guard - stdout could not "
           "be captured - and it is reported as a failure because proceeding would assert "
           "nothing";

    {
        ScopedCecLogLevel debugLevel("DEBUG");

        ASSERT_TRUE(debugLevel.isRaised())
            << "the guard refused to take custody of " << "/tmp/cec_log_enabled"
            << " and so did not raise the level. This is asserted rather than tolerated: the "
               "refusal reasons are all conditions a reader must act on - another run_L1Tests "
               "holding the custody lock, a symlink or a non-regular file at the path, foreign "
               "ownership, or a file too large to reproduce byte-for-byte. Reason given: ["
            << debugLevel.failureReason() << "]";

        EXPECT_EQ(levelBefore, debugLevel.entryLevel())
            << "the guard recorded a different entry level than this case observed immediately "
               "before constructing it, which means something moved the process-wide level "
               "between the two observations. Every later comparison in this case rests on the "
               "two agreeing, so a mismatch is reported rather than reconciled";

        EXPECT_EQ(LOG_DEBUG, ScopedCecLogLevel::observeEffectiveLevel())
            << "the guard reported that it raised the level to DEBUG, but an emission at DEBUG "
               "did not survive - so the level did not actually move. The whole purpose of the "
               "guard is to make a LOG_DEBUG line observable in a case that asserts on it, and "
               "a guard that reports success without moving the level would let such a case "
               "assert against silence";

        std::string restoreDetail;
        ASSERT_TRUE(debugLevel.restoreAndVerify(restoreDetail))
            << "restoreAndVerify() reported that it could not prove the host was put back. It "
               "checks three things in order while still holding the custody lock - that the "
               "publication succeeded, that the path holds the original bytes or is absent "
               "again, and that the effective level returned - and a failure of any of them "
               "means this run altered shared state it did not restore. Detail: ["
            << restoreDetail << "]";

        EXPECT_EQ(levelBefore, ScopedCecLogLevel::observeEffectiveLevel())
            << "the level was NOT back to what this case found after restoreAndVerify() "
               "reported success, which is the one claim that cannot be taken on trust: it is "
               "checked here independently of the guard's own verification, so a restoration "
               "that reported success without performing one is caught rather than believed";

        std::string secondDetail;
        EXPECT_TRUE(debugLevel.restoreAndVerify(secondDetail))
            << "a second restoreAndVerify() failed, so the operation is not idempotent - and "
               "it must be, because the destructor makes exactly this call on the path where a "
               "fatal assertion skipped the explicit one. Detail: [" << secondDetail << "]";
    }

    EXPECT_EQ(levelBefore, ScopedCecLogLevel::observeEffectiveLevel())
        << "the destructor left the process-wide level somewhere other than where this case "
           "found it, which would leak verbosity into every later case in this binary";

    struct stat pathStatus;
    if (::lstat("/tmp/cec_log_enabled", &pathStatus) == 0) {
        EXPECT_TRUE(S_ISREG(pathStatus.st_mode))
            << "/tmp/cec_log_enabled is no longer a regular file after the guard released it. "
               "The guard publishes only through a rename of a regular file it created itself, "
               "so anything else at that path came from elsewhere - and a later run would "
               "refuse to take custody of it";
    }
}


/**
 * @brief The receive handoff fills the queue's 32 legacy slots and refuses the next frame, and a
 *        close() against the full queue drops its sentinel as the legacy queue does.
 * @pre Runs under every invocation, through ReceiveQueueProbe forced to OPENED.
 * @note Single-threaded, so it measures the capacity only; the concurrent cases below measure
 *       close()'s producer-lock acquisition.
 */
TEST_F(DriverAidlLocalInstanceTest, ReceiveQueueHoldsTheLegacyThirtyTwoEntriesAndDropsTheCloseSentinelWhenFull) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;

    // Pinned to the legacy queue's literal 32 rather than derived from `capacity`, so a queue
    // that gained or lost a slot against DriverImpl's still fails.
    ASSERT_EQ(capacity, 32u)
        << "the incoming queue holds " << capacity << " entries, not the 32 the legacy back-end "
           "gets from EventQueue's default capacity. Received frames and close()'s sentinel must "
           "share the same 32 slots on both back-ends";

    ReceiveQueueProbe probe;
    probe.markOpened();

    // 1. Fill every slot through the production handoff; every offer must be accepted, the one
    //    that takes the 32nd slot included.
    for (size_t i = 0; i < capacity; i++) {
        CECFrame *frame = new CECFrame(directedFrame());

        // Ownership follows the reported return value, as in onMessageReceived(), so broken
        // code yields a reported failure rather than a double free.
        const bool taken = probe.offer(frame);

        EXPECT_TRUE(taken)
            << "the handoff refused frame " << i << " of " << capacity << " before the queue was "
               "full, so received frames get less depth than the legacy queue gives them";

        if (!taken) {
            delete frame;
        }

        ASSERT_EQ(probe.occupancy(), i + 1)
            << "the queue holds " << probe.occupancy() << " entries after " << (i + 1)
            << " offers, so what the handoff reported and what the queue did have diverged";
    }

    ASSERT_EQ(probe.occupancy(), capacity)
        << "the queue did not fill, so the case that follows would not be testing a full queue "
           "at all";

    // 2. The next frame is refused and stays the caller's; EventQueue::offer() would have dropped
    //    it without a word.
    CECFrame *overflow = new CECFrame(directedFrame());
    const bool overflowTaken = probe.offer(overflow);

    EXPECT_FALSE(overflowTaken)
        << "the handoff reported a frame accepted onto a full queue. EventQueue::offer() drops it "
           "there, so the caller would forget a frame nothing holds - one leaked frame per event";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "the refused frame was queued anyway, so the return value and the queue disagree "
           "about who owns it - the caller will release a frame the queue still holds";

    // Released only because the handoff reported the caller still owns it, as
    // onMessageReceived() does on a false return.
    if (!overflowTaken) {
        delete overflow;
    }

    // 3. close() offers its sentinel to the full queue, which drops it as DriverImpl's does. Its
    //    IOException (no service proxy, so DEAD_OBJECT) is expected; the offer precedes it.
    EXPECT_THROW({ probe.close(); }, IOException)
        << "close() on an instance holding no service proxy must still report the failed "
           "transaction. If it returned cleanly, the sentinel assertion below would be proving "
           "something about a different code path";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "the full queue holds " << probe.occupancy() << " entries after close(). The legacy "
           "queue has no room for the sentinel at this point and drops it, so an entry beyond "
           "the 32 means this queue is larger than DriverImpl's";

    // 4. A frame arriving after close() is rejected by getIncomingQueue()'s state guard before
    //    anything is offered, and the caller releases it, as on the legacy path.
    CECFrame *afterClose = new CECFrame(directedFrame());
    bool afterCloseTaken = false;

    EXPECT_THROW({ afterCloseTaken = probe.offer(afterClose); }, InvalidStateException)
        << "a frame offered after close() was not rejected. Accepting it would put a frame on a "
           "queue nothing will drain, and reporting acceptance without queueing it would leak it";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "the post-close frame reached the queue although the state guard should have raised "
           "before the offer";

    if (!afterCloseTaken) {
        delete afterClose;
    }

    // 5. What the queue actually holds is exactly what was accepted and no sentinel. Counting
    //    both is what distinguishes "the sentinel went in" from "an extra frame did".
    size_t frames = 0;
    size_t sentinels = 0;

    probe.drainCounting(frames, sentinels);

    EXPECT_EQ(frames, capacity)
        << "the queue held " << frames << " frames where the handoff accepted " << capacity;

    EXPECT_EQ(sentinels, 0u)
        << "the queue held " << sentinels << " NULL sentinels although close() offered its one "
           "to a full queue, where the legacy queue drops it";
}

/**
 * @brief Across a queue driven past its capacity, every frame has exactly one owner - the queue
 *        or the caller, never both and never neither.
 * @pre Runs under every invocation, through ReceiveQueueProbe forced to OPENED.
 * @note Single-threaded: it pins the accounting (capacity frames accepted) and the ownership
 *       partition; the overlap case stages the concurrent zero-owner arm.
 */
TEST_F(DriverAidlLocalInstanceTest, EveryFrameOfferedToAFullReceiveQueueHasExactlyOneOwner) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GT(capacity, 0u)
        << "a queue with no capacity accepts nothing, so the partition below would be vacuous";

    // Comfortably past the capacity, so the refusal arm runs repeatedly, as it does while a
    // stalled Bus reader keeps the queue full.
    const size_t offeredCount = capacity + 8;

    ReceiveQueueProbe probe;
    probe.markOpened();

    std::vector<CECFrame *> allocated;
    std::vector<CECFrame *> ownedByCaller;   // the handoff returned false: the caller still owns
    std::vector<CECFrame *> ownedByQueue;    // the handoff returned true: the queue owns

    for (size_t i = 0; i < offeredCount; i++) {
        CECFrame *frame = new CECFrame(directedFrame());
        allocated.push_back(frame);

        if (probe.offer(frame)) {
            ownedByQueue.push_back(frame);
        }
        else {
            ownedByCaller.push_back(frame);
        }
    }

    EXPECT_EQ(ownedByQueue.size(), capacity)
        << "the handoff claimed " << ownedByQueue.size() << " frames where the queue holds "
           << capacity << ", every slot available to received frames";

    EXPECT_EQ(ownedByCaller.size(), offeredCount - capacity)
        << "the handoff returned false " << ownedByCaller.size() << " times for "
           << (offeredCount - capacity) << " offers made onto a full queue";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "the queue's occupancy does not match what the handoff claimed to have taken";

    // What the queue really holds. Taken out here rather than left for the probe's destructor,
    // because the identity of each entry is the evidence this case rests on.
    std::vector<CECFrame *> takenFromQueue;
    size_t sentinels = 0;

    probe.drainInto(takenFromQueue, sentinels);

    EXPECT_EQ(sentinels, 0u)
        << "a NULL sentinel was on the queue although close() was never called here";

    EXPECT_EQ(takenFromQueue.size(), ownedByQueue.size())
        << "the queue yielded " << takenFromQueue.size() << " frames where the handoff reported "
           << ownedByQueue.size() << " accepted. A shortfall is the ownership leak: the return "
              "value said the queue had taken a frame that EventQueue::offer() had discarded";

    // The partition. One owner per allocation, counted, so that "no owner" and "two owners" are
    // distinguished from each other and from the expected answer.
    for (size_t i = 0; i < allocated.size(); i++) {
        size_t owners = 0;

        for (size_t q = 0; q < takenFromQueue.size(); q++) {
            if (takenFromQueue[q] == allocated[i]) {
                owners++;
            }
        }

        for (size_t c = 0; c < ownedByCaller.size(); c++) {
            if (ownedByCaller[c] == allocated[i]) {
                owners++;
            }
        }

        EXPECT_EQ(owners, 1u)
            << "frame " << i << " of " << allocated.size() << " has " << owners << " owners. "
               "Zero means the handoff reported it accepted and the queue never took it, so "
               "nothing will ever release it - the ownership leak. Two means both the queue and "
               "the caller would release it";
    }

    // Every allocation is released exactly once, whatever the assertions above reported, so a
    // failing run does not also leak.
    for (size_t i = 0; i < allocated.size(); i++) {
        delete allocated[i];
    }
}

/**
 * @brief close()'s sentinel offer does not complete while another thread holds
 *        queueProducerMutex, and completes once it is released.
 * @pre Runs under every invocation. The harness holds the probe, latches and flags off this
 *      stack, because an abandoned worker outlives the case body.
 * @note A timing check, not a proof: the latch shows the worker started, not that it reached
 *       the lock. ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement carries the weight.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseSentinelOfferBlocksOnTheProducerLockAConcurrentTestHolds) {
    // The probe, latches and worker-written flags live in the harness, off this stack; `shared`
    // is captured by value into the worker, never as a reference to a local.
    QueueHandoffOverlapHarness harness;
    QueueHandoffOverlapState  *shared = &*harness;

    shared->probe.markOpened();

    // One resident frame, far from the limit: this case is about the lock, and a non-empty
    // queue makes an early sentinel visible in the occupancy.
    CECFrame *resident = new CECFrame(directedFrame());
    const bool residentTaken = shared->probe.offer(resident);

    // Ownership is settled before the assertion that can return, so a refusal reports a
    // failure instead of leaking the frame.
    if (!residentTaken) {
        delete resident;
    }

    ASSERT_TRUE(residentTaken)
        << "the handoff refused the first frame offered to an empty queue, so this case cannot "
           "establish its precondition";

    ASSERT_EQ(shared->probe.occupancy(), 1u)
        << "the queue does not hold the one frame the handoff reported accepting";

    // Held as a pointer declared outside the lock scope, because the completion observation
    // after that scope needs the same worker the observation inside it watched.
    BoundedWorker *closer = NULL;

    {
        CCEC_OSAL::AutoLock heldByTest(shared->probe.producerLock());

        // closeEntered fires just before close(), so "did not complete" cannot be met by a
        // thread that never ran; the finished latch fires once close() returns or raises.
        closer = &harness.start([shared]() {
            shared->closeEntered.notify();

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // Expected: no service proxy, so the close transaction reports DEAD_OBJECT;
                // the sentinel is offered before it, which is what this case reads.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                shared->closeRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->closeEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the worker never reached close(), so the observation below would be vacuous. "
               "This is a harness failure rather than a production one";

        // Observation 1: close() cannot get past its producer-lock acquisition.
        ASSERT_FALSE(closer->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "close() ran to COMPLETION while this test held queueProducerMutex, so it did "
               "not take that lock before offering its NULL sentinel. That is the "
               "missing close-side acquisition: close() is a producer on the incoming queue, and "
               "an unserialized sentinel can land between offerReceivedFrame()'s occupancy check "
               "and its offer, whereupon EventQueue::offer() discards the received frame "
               "silently while the handoff still reports it accepted - one leaked frame per "
               "event, on a path a remote HAL drives";

        // Observation 2: the same fact read from the queue rather than from the worker.
        EXPECT_EQ(shared->probe.occupancy(), 1u)
            << "an entry reached the incoming queue while this test held queueProducerMutex. "
               "The only other producer is close()'s sentinel offer, so the sentinel was "
               "offered without the lock - the close-side acquisition is missing";
    }

    // Lock released: close() must now finish, which separates "blocked on the lock" from
    // "stuck" and turns a deadlock into a failure rather than a hung run.
    ASSERT_TRUE(closer->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "close() did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of queueProducerMutex being released, so it is not merely waiting for that "
           "lock. Everything it has left to do is one EventQueue::offer() - which takes the "
           "queue's own mutex and may allocate, but never waits for capacity - and a "
           "transaction that fails immediately on an instance holding no proxy";

    ASSERT_TRUE(harness.disposeWorkers())
        << "the close worker did not finish within " << WORKER_ABANDON_DEADLINE_MS
        << " ms of its body being observed complete, so it was abandoned rather than joined "
           "and the state it holds is leaked by design. Nothing below can be interpreted";

    // Read only after the join disposeWorkers() performed, which is the happens-before that
    // makes these plain bools safe to inspect from this thread.
    EXPECT_TRUE(shared->closeRaisedIoException)
        << "close() on an instance holding no service proxy must report the failed transaction "
           "as IOException; it "
        << (shared->closeReturnedCleanly
                ? "returned cleanly"
                : (shared->closeRaisedSomethingElse ? "raised some other exception"
                                                    : "did neither"))
        << ", so the assertions above were describing a different code path";

    EXPECT_EQ(shared->probe.occupancy(), 2u)
        << "the queue holds " << shared->probe.occupancy() << " entries where it should hold the "
           "resident frame plus close()'s one sentinel. The sentinel is what wakes a blocked "
           "Bus reader and it must arrive exactly once";

    // What the queue actually holds, counted apart, so "the sentinel arrived" is distinguished
    // from "a second frame did".
    size_t frames = 0;
    size_t sentinels = 0;

    shared->probe.drainCounting(frames, sentinels);

    EXPECT_EQ(frames, 1u)
        << "the queue yielded " << frames << " frames where exactly one was ever accepted";

    EXPECT_EQ(sentinels, 1u)
        << "the queue yielded " << sentinels << " NULL sentinels; close() posts exactly one, "
           "and with a single frame queued there is room for it to land";
}

/**
 * @brief A receive handoff and close()'s sentinel contending for the queue's last free slot
 *        leave every frame with exactly one owner, in either arrival order.
 * @pre Runs under every invocation, through the harness, with the queue one entry short of full.
 * @note Each producer signals entry and is observed not to complete while the test holds
 *       queueProducerMutex; deleting either side's acquisition fails that observation.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 2u)
        << "this case needs a filled queue with one free slot for the two producers to contend "
           "for, so a capacity below two cannot express it";

    // The probe, latches, worker flags and frame ledger live in the harness, freed once every
    // worker joins and retained if one is abandoned.
    QueueHandoffOverlapHarness harness;
    QueueHandoffOverlapState  *shared = &*harness;

    shared->probe.markOpened();

    std::vector<CECFrame *> ownedByCaller;

    // Fill to capacity - 1 through the production handoff, leaving exactly one slot for the
    // contending frame and the sentinel to race for.
    for (size_t i = 0; i < capacity - 1; i++) {
        CECFrame *frame = new CECFrame(directedFrame());
        shared->allocated.push_back(frame);

        if (!shared->probe.offer(frame)) {
            ownedByCaller.push_back(frame);
        }
    }

    ASSERT_EQ(shared->probe.occupancy(), capacity - 1)
        << "the queue did not reach " << (capacity - 1) << " entries, so the two orders this "
           "case distinguishes would not differ";

    ASSERT_TRUE(ownedByCaller.empty())
        << "the handoff refused " << ownedByCaller.size() << " frames below its refusal point, "
           "so the precondition this case rests on does not hold";

    shared->contending = new CECFrame(directedFrame());
    shared->allocated.push_back(shared->contending);

    // Held as pointers declared outside the lock scope, because the completion observations
    // after that scope need the same two workers the observations inside it watched.
    BoundedWorker *receiver = NULL;
    BoundedWorker *closer   = NULL;

    {
        CCEC_OSAL::AutoLock heldByTest(shared->probe.producerLock());

        receiver = &harness.start([shared]() {
            shared->receiveEntered.notify();

            try {
                shared->contendingAccepted = shared->probe.offer(shared->contending);
            }
            catch (InvalidStateException &) {
                // Reachable if the driver left OPENED before this thread read the state; the
                // assertion after the join reports it.
                shared->contendingRefusedByStateGuard = true;
            }
            catch (...) {
                shared->contendingRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->receiveEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the receive worker never reached the handoff; this is a harness failure";

        // The receive side's acquisition: having signalled entry, the handoff must not complete
        // while this test holds queueProducerMutex.
        ASSERT_FALSE(receiver->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "the receive handoff COMPLETED while this test held queueProducerMutex, so "
               "offerReceivedFrame() did not take that lock around its occupancy check and its "
               "offer. Without it the check can go stale under the other producer and the "
               "frame is dropped while acceptance is still reported";

        closer = &harness.start([shared]() {
            shared->closeEntered.notify();

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // Expected: no service proxy, so the close transaction reports DEAD_OBJECT.
                // The sentinel is offered before it, which is the part this case reads.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                // Reported through the queue-content assertions: a close that never offered
                // leaves no sentinel, and the sentinel count is asserted.
                shared->closeRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->closeEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the close worker never reached close(); this is a harness failure";

        // The close side's acquisition, observed the same way: having signalled entry, close()
        // must not complete while this test holds queueProducerMutex.
        ASSERT_FALSE(closer->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "close() ran to completion while this test held queueProducerMutex, so its "
               "sentinel offer is not serialized against the receive path. That is finding "
               "the missing close-side acquisition";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 1)
            << "the occupancy moved while this test held the producer lock, so one of the two "
               "parked producers reached the queue without taking it";
    }

    // Released: the two producers now contend for real, in an order this case does not choose.
    ASSERT_TRUE(receiver->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the receive handoff did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of the producer lock being released";

    ASSERT_TRUE(closer->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "close() did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of the producer lock being released";

    ASSERT_TRUE(harness.disposeWorkers())
        << "a worker did not finish within " << WORKER_ABANDON_DEADLINE_MS
        << " ms of its body being observed complete, so it was abandoned rather than joined "
           "and the state it holds is leaked by design. Nothing below can be interpreted";

    // Everything below reads worker-written state after both joins, which is the happens-before
    // that makes plain bools sufficient here.
    ASSERT_FALSE(shared->contendingRaisedSomethingElse)
        << "the receive handoff raised an exception that is neither InvalidStateException nor "
           "nothing at all, so what follows cannot be interpreted";

    EXPECT_FALSE(shared->contendingRefusedByStateGuard)
        << "the receive handoff was refused by the OPENED-state guard, so it never reached the "
           "producer lock and no contention took place. The sequencing above exists to prevent "
           "exactly this: the handoff is required to be parked on the lock while the state is "
           "still OPENED, before close() is started at all";

    if (shared->contendingRefusedByStateGuard) {
        ownedByCaller.push_back(shared->contending);
    }
    else if (!shared->contendingAccepted) {
        // The legitimate close-first order: the sentinel took the last slot and the handoff then
        // refused, so the caller still owns this frame and must release it.
        ownedByCaller.push_back(shared->contending);
    }

    const size_t expectedFrames = (capacity - 1) + (shared->contendingAccepted ? 1u : 0u);

    // What the queue actually holds, by identity rather than by count alone.
    std::vector<CECFrame *> takenFromQueue;
    size_t sentinels = 0;

    shared->probe.drainInto(takenFromQueue, sentinels);

    // The invariant in both orders: the last slot goes to exactly one producer. Receive first,
    // the full queue drops the sentinel as the legacy queue does; close first, the frame is refused.
    EXPECT_EQ(sentinels, shared->contendingAccepted ? 0u : 1u)
        << "the queue yielded " << sentinels << " NULL sentinels after a close that overlapped "
           "a receive handoff which was "
        << (shared->contendingAccepted ? "accepted" : "refused") << ". One slot was free, so "
           "exactly one of the two producers can have landed: the sentinel only when the frame "
           "was refused";

    EXPECT_EQ(takenFromQueue.size(), expectedFrames)
        << "the queue yielded " << takenFromQueue.size() << " frames where the handoff's "
           "reports account for " << expectedFrames << " ("
        << (capacity - 1) << " filled, plus the shared->contending frame "
        << (shared->contendingAccepted ? "which was accepted" : "which was refused")
        << "). A shortfall is the ownership leak itself: acceptance reported for a frame "
           "EventQueue::offer() had discarded";

    // The shared->contending frame specifically: present in the queue if and only if the handoff said
    // the queue had taken it. This is the one frame whose ownership the race decided.
    size_t contendingInQueue = 0;

    for (size_t q = 0; q < takenFromQueue.size(); q++) {
        if (takenFromQueue[q] == shared->contending) {
            contendingInQueue++;
        }
    }

    EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
        << "the handoff reported the shared->contending frame "
        << (shared->contendingAccepted ? "accepted" : "refused") << " and the queue held it "
        << contendingInQueue << " times. The report and the queue must agree: reporting "
           "acceptance for a frame that is not there leaks it, and reporting refusal for one "
           "that is would have the caller free a frame the queue still owns";

    // The partition, over every allocation: exactly one owner each, counted so that "none" and
    // "two" are distinguished from each other and from the expected answer.
    for (size_t i = 0; i < shared->allocated.size(); i++) {
        size_t owners = 0;

        for (size_t q = 0; q < takenFromQueue.size(); q++) {
            if (takenFromQueue[q] == shared->allocated[i]) {
                owners++;
            }
        }

        for (size_t c = 0; c < ownedByCaller.size(); c++) {
            if (ownedByCaller[c] == shared->allocated[i]) {
                owners++;
            }
        }

        EXPECT_EQ(owners, 1u)
            << "frame " << i << " of " << shared->allocated.size() << " has " << owners << " owners "
               "after the overlap. Zero means the handoff reported it accepted and the queue "
               "never took it, so nothing will ever release it - the ownership leak. Two means "
               "both "
               "the queue and the caller would release it";
    }

    // Nothing is released here: the harness ledger releases every frame once every worker joined
    // and the queue is drained, and retains them if one was abandoned.
}

/**
 * @brief Every ordering of close()'s sentinel against the receive handoff keeps the handoff's
 *        report and the queue's contents in agreement, constructed and under real contention.
 * @pre Runs under every invocation, through the harness.
 * @note Part 1 constructs the three orderings single-threaded and carries the non-vacuity
 *       assertions; Part 2's sweep asserts the same invariants and only prints its census,
 *       which depends on the host's CPU count.
 */
TEST_F(DriverAidlLocalInstanceTest, ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 2u)
        << "this case needs a filled queue with one free slot for the two producers to contend "
           "for, so a capacity below two cannot express it";

    /* Part 1 - the three orderings, constructed single-threaded so no scheduler can lose an
     * arm; each asserts the same three invariants as Part 2. */

    /**
     * @brief The ordering a constructed arm produces; the three differ only in where the close
     *        side runs relative to the handoff.
     */
    enum ConstructedOrdering {
        RECEIVE_FIRST_ACCEPTS,            // the handoff takes the last slot; the sentinel is dropped
        CLOSE_FIRST_REFUSED_BY_OCCUPANCY, // the sentinel took the last slot, so the handoff refuses
        CLOSE_FIRST_REFUSED_BY_STATE      // close() has left OPENED, so the handoff raises
    };

    /**
     * @brief Runs production close() into the outcome flags the sweep's close worker writes;
     *        a proxyless instance raises IOException after offering its sentinel.
     */
    auto driveProductionClose = [](QueueHandoffOverlapState *shared) {
        try {
            shared->probe.close();
            shared->closeReturnedCleanly = true;
        }
        catch (IOException &) {
            shared->closeRaisedIoException = true;
        }
        catch (...) {
            shared->closeRaisedSomethingElse = true;
        }
    };

    /**
     * @brief Builds one ordering and asserts it and the three invariants, with EXPECT because a
     *        fatal assertion would return from the lambda alone.
     * @return Whether the arm produced the ordering it names, for the non-vacuity assertions.
     */
    auto constructOrdering = [&](ConstructedOrdering ordering, const char *arm) -> bool {
        // The same harness as Part 2: it owns the probe and frame ledger and releases every
        // allocation exactly once; no worker is started here.
        QueueHandoffOverlapHarness harness;
        QueueHandoffOverlapState  *shared = &*harness;

        shared->probe.markOpened();

        // The frames the handoff handed back, by identity. Pointers only - the ledger owns them.
        std::vector<CECFrame *> ownedByCaller;

        // Filled to capacity - 1 as in Part 2, so the frame and the sentinel contend for the
        // last slot and both outcomes stay expressible.
        for (size_t filled = 0; filled < capacity - 1; filled++) {
            CECFrame *frame = new CECFrame(directedFrame());
            shared->allocated.push_back(frame);

            if (!shared->probe.offer(frame)) {
                ownedByCaller.push_back(frame);
            }
        }

        EXPECT_TRUE(ownedByCaller.empty())
            << arm << ": the handoff refused " << ownedByCaller.size() << " frames below its "
               "refusal point, so this arm is not the ordering it names";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 1)
            << arm << ": the queue holds " << shared->probe.occupancy() << " entries where this "
               "arm needs " << (capacity - 1);

        shared->contending = new CECFrame(directedFrame());
        shared->allocated.push_back(shared->contending);

        // The close side runs ahead of the handoff on two of the three arms; this is the only
        // difference between them.
        if (ordering == CLOSE_FIRST_REFUSED_BY_OCCUPANCY) {
            // close()'s sentinel takes the last slot while the instance stays OPENED, so the
            // handoff meets the occupancy check rather than the state guard.
            shared->probe.postCloseSentinel();
        }
        else if (ordering == CLOSE_FIRST_REFUSED_BY_STATE) {
            // Production close() runs to completion first, so the handoff meets an instance
            // that is no longer OPENED - the earliest close-first order.
            driveProductionClose(shared);
        }

        // The production handoff, recorded exactly as reported: its agreement with the queue's
        // contents is the property under test.
        try {
            shared->contendingAccepted = shared->probe.offer(shared->contending);
        }
        catch (InvalidStateException &) {
            shared->contendingRefusedByStateGuard = true;
        }
        catch (...) {
            shared->contendingRaisedSomethingElse = true;
        }

        if (ordering == RECEIVE_FIRST_ACCEPTS) {
            // The close side runs after the accepted handoff filled the queue, so its sentinel
            // is dropped, as the legacy queue drops it.
            driveProductionClose(shared);
        }

        EXPECT_FALSE(shared->contendingRaisedSomethingElse)
            << arm << ": the receive handoff raised an exception that is neither "
               "InvalidStateException nor nothing at all, so what follows cannot be interpreted";

        // The close outcome, on the two arms that drive production close(); the occupancy arm
        // posts its sentinel through postCloseSentinel() instead.
        if (ordering != CLOSE_FIRST_REFUSED_BY_OCCUPANCY) {
            EXPECT_TRUE(shared->closeRaisedIoException)
                << arm << ": close() did not report the IOException a proxyless instance must "
                   "report, so the sentinel this arm reads may not have come from the code path "
                   "this arm means to exercise";

            EXPECT_FALSE(shared->closeReturnedCleanly)
                << arm << ": close() returned cleanly on an instance holding no service proxy, "
                   "so the assertions below are describing a different code path";

            EXPECT_FALSE(shared->closeRaisedSomethingElse)
                << arm << ": close() raised an exception that is not the IOException a proxyless "
                   "instance must report, so what follows cannot be interpreted";
        }

        // The decisive outcome per arm, which makes each arm the ordering it claims and is its
        // non-vacuity evidence.
        bool producedTheOrdering = false;

        switch (ordering) {
        case RECEIVE_FIRST_ACCEPTS:
            EXPECT_TRUE(shared->contendingAccepted)
                << arm << ": the handoff refused a frame offered onto a queue holding "
                << (capacity - 1) << " of " << capacity << " entries, which still has a free "
                   "slot, so the accepted-and-present arm of the invariant was not reached";

            EXPECT_FALSE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff was refused by the OPENED-state guard on an arm that "
                   "closes nothing until after the handoff has returned";

            producedTheOrdering =
                shared->contendingAccepted && !shared->contendingRefusedByStateGuard;
            break;

        case CLOSE_FIRST_REFUSED_BY_OCCUPANCY:
            EXPECT_FALSE(shared->contendingAccepted)
                << arm << ": the handoff accepted a frame offered onto a queue already holding "
                << capacity << " of " << capacity << " entries. EventQueue::offer() drops a "
                   "frame offered to a full queue, so reporting acceptance here leaks it";

            EXPECT_FALSE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff was refused by the OPENED-state guard rather than by "
                   "occupancy, so this arm did not exercise the full-queue refusal it exists "
                   "for. The instance is left OPENED precisely so that it cannot";

            producedTheOrdering =
                !shared->contendingAccepted && !shared->contendingRefusedByStateGuard;
            break;

        case CLOSE_FIRST_REFUSED_BY_STATE:
            EXPECT_TRUE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff did not raise InvalidStateException after a completed "
                   "close(), so the state guard that keeps a frame out of a queue nobody will "
                   "drain again was not exercised";

            EXPECT_FALSE(shared->contendingAccepted)
                << arm << ": the handoff accepted a frame on an instance that is no longer "
                   "OPENED, so the frame would sit in a queue the Bus reader has already been "
                   "told to stop draining";

            producedTheOrdering =
                shared->contendingRefusedByStateGuard && !shared->contendingAccepted;
            break;
        }

        // What the queue really holds, by identity. Draining here also leaves the harness's
        // ownership sweep nothing to drain, which is what keeps each frame released once.
        std::vector<CECFrame *> takenFromQueue;
        size_t                  sentinels = 0;

        shared->probe.drainInto(takenFromQueue, sentinels);

        if (!shared->contendingAccepted) {
            ownedByCaller.push_back(shared->contending);
        }

        // Invariant 1, as Part 2 asserts it: the last slot goes to exactly one producer, so the
        // sentinel is queued exactly when the frame was not.
        EXPECT_EQ(sentinels, shared->contendingAccepted ? 0u : 1u)
            << arm << ": the queue yielded " << sentinels << " NULL sentinels with the contending "
               "frame " << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << ". One slot "
               "was free, so the sentinel lands only when the frame did not, and a full queue "
               "drops it as the legacy queue does";

        // Invariant 2, as Part 2 asserts it: reported-accepted if and only if present.
        size_t contendingInQueue = 0;

        for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
            if (takenFromQueue[entry] == shared->contending) {
                contendingInQueue++;
            }
        }

        EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
            << arm << ": the handoff reported the contending frame "
            << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << " and the drained queue "
               "held it " << contendingInQueue << " times. The report and the queue must agree: "
               "acceptance reported for a frame that is not there leaves NOBODY to release it, "
               "and refusal reported for one that is there has the caller free a frame the queue "
               "still owns";

        const size_t expectedFrames = (capacity - 1) + (shared->contendingAccepted ? 1u : 0u);

        EXPECT_EQ(takenFromQueue.size(), expectedFrames)
            << arm << ": the queue yielded " << takenFromQueue.size() << " frames where the "
               "handoff's reports account for " << expectedFrames;

        // Invariant 3, as Part 2 asserts it: the partition over every allocation, so that "no
        // owner" and "two owners" are distinguished from each other and from the right answer.
        size_t frameWithoutAnOwner = 0;
        size_t frameWithTwoOwners  = 0;

        for (size_t allocation = 0; allocation < shared->allocated.size(); allocation++) {
            size_t owners = 0;

            for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
                if (takenFromQueue[entry] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            for (size_t kept = 0; kept < ownedByCaller.size(); kept++) {
                if (ownedByCaller[kept] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            if (owners == 0) {
                frameWithoutAnOwner++;
            }
            else if (owners > 1) {
                frameWithTwoOwners++;
            }
        }

        EXPECT_EQ(frameWithoutAnOwner, 0u)
            << arm << ": " << frameWithoutAnOwner << " frames have NO owner - reported accepted "
               "and not in the queue, so nothing in the process will ever release them";

        EXPECT_EQ(frameWithTwoOwners, 0u)
            << arm << ": " << frameWithTwoOwners << " frames have TWO owners - both the queue "
               "and the caller believe they hold them, which is a double free rather than a leak";

        return producedTheOrdering;
    };

    // The three arms, each run once: the handoff wins, the sentinel wins on occupancy, the
    // sentinel wins on the state.
    const bool receiveFirstConstructed =
        constructOrdering(RECEIVE_FIRST_ACCEPTS, "[constructed receive-first]");
    const bool refusedByOccupancyConstructed =
        constructOrdering(CLOSE_FIRST_REFUSED_BY_OCCUPANCY, "[constructed close-first, occupancy]");
    const bool refusedByStateConstructed =
        constructOrdering(CLOSE_FIRST_REFUSED_BY_STATE, "[constructed close-first, state guard]");

    // Printed so any run shows all three orderings were reached; the final assertions are made
    // on these outcomes.
    std::cout << "[queue handoff constructed orderings] receive-first accepted: "
              << (receiveFirstConstructed ? "yes" : "NO")
              << ", close-first refused by occupancy: "
              << (refusedByOccupancyConstructed ? "yes" : "NO")
              << ", close-first refused by the state guard: "
              << (refusedByStateConstructed ? "yes" : "NO") << std::endl;

    /* Part 2 - the contended sweep: real overlaps of both production producers on one queue.
     * Its invariants are asserted; its census is only printed. */

    // The interleaving census, printed and never asserted: which interleaving a host produces
    // is the scheduler's choice.
    size_t overlapsCompleted        = 0;
    size_t receiveFirstOverlaps     = 0;   // the handoff took the last slot first
    size_t closeFirstByOccupancy    = 0;   // the sentinel landed first, so the handoff refused
    size_t closeFirstByStateGuard   = 0;   // close() reached CLOSING before the handoff's guard

    for (size_t iteration = 0; iteration < OVERLAP_STRESS_ITERATIONS; iteration++) {
        // A fresh instance per iteration, so no iteration inherits another's queue, state or
        // rendezvous. The harness holds it off this stack and releases everything itself.
        QueueHandoffOverlapHarness harness;
        QueueHandoffOverlapState  *shared = &*harness;

        shared->probe.markOpened();

        // The frames the handoff handed back, by identity. Pointers only - the ledger owns them.
        std::vector<CECFrame *> ownedByCaller;

        // Fill to capacity - 1 through the production handoff, leaving one slot for the
        // contending frame and the sentinel to race for.
        for (size_t filled = 0; filled < capacity - 1; filled++) {
            CECFrame *frame = new CECFrame(directedFrame());
            shared->allocated.push_back(frame);

            if (!shared->probe.offer(frame)) {
                ownedByCaller.push_back(frame);
            }
        }

        EXPECT_TRUE(ownedByCaller.empty())
            << "iteration " << iteration << ": the handoff refused " << ownedByCaller.size()
            << " frames below its refusal point, so this iteration's overlap would not be the "
               "one the case describes";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 1)
            << "iteration " << iteration << ": the queue holds " << shared->probe.occupancy()
            << " entries where the overlap needs " << (capacity - 1);

        shared->contending = new CECFrame(directedFrame());
        shared->allocated.push_back(shared->contending);

        // The swept offset, alternating sides so both directions of skew are sampled.
        const unsigned int offset =
            static_cast<unsigned int>((iteration * 37u) % OVERLAP_OFFSET_SPREAD);
        const unsigned int receiveOffset = ((iteration % 2) == 0) ? 0u : offset;
        const unsigned int closeOffset   = ((iteration % 2) == 0) ? offset : 0u;

        BoundedWorker &receiver = harness.start([shared, receiveOffset]() {
            shared->receiveEntered.notify();

            if (!awaitOverlapRendezvous(shared)) {
                shared->rendezvousTimedOut.store(true, std::memory_order_release);
                return;
            }

            burnOffset(receiveOffset);

            try {
                shared->contendingAccepted = shared->probe.offer(shared->contending);
            }
            catch (InvalidStateException &) {
                // The earliest close-first order: close() left OPENED before this thread reached
                // the guard, so the frame stays the caller's.
                shared->contendingRefusedByStateGuard = true;
            }
            catch (...) {
                shared->contendingRaisedSomethingElse = true;
            }
        });

        BoundedWorker &closer = harness.start([shared, closeOffset]() {
            shared->closeEntered.notify();

            if (!awaitOverlapRendezvous(shared)) {
                shared->rendezvousTimedOut.store(true, std::memory_order_release);
                return;
            }

            burnOffset(closeOffset);

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // Expected: no service proxy, so the close transaction reports DEAD_OBJECT;
                // the sentinel is offered before it.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                shared->closeRaisedSomethingElse = true;
            }
        });

        // Bounded completion, then a bounded join: only the join makes the plain flags below
        // safe to read.
        EXPECT_TRUE(receiver.finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "iteration " << iteration << ": the receive handoff did not complete within "
            << PRODUCER_COMPLETION_TIMEOUT_MS << " ms. Nothing is holding the producer lock in "
               "this case, so it is stuck rather than waiting";

        EXPECT_TRUE(closer.finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "iteration " << iteration << ": close() did not complete within "
            << PRODUCER_COMPLETION_TIMEOUT_MS << " ms. Nothing is holding the producer lock in "
               "this case, so it is stuck rather than waiting";

        if (!harness.disposeWorkers()) {
            // A worker was abandoned, so its state is leaked by design and nothing it wrote can
            // be interpreted. Reported and the loop stopped, rather than read anyway.
            ADD_FAILURE()
                << "iteration " << iteration << ": a producer did not finish within "
                << WORKER_ABANDON_DEADLINE_MS << " ms and had to be abandoned rather than "
                   "joined, so this iteration's state is unreadable and the loop is stopped";
            break;
        }

        ASSERT_FALSE(shared->rendezvousTimedOut.load(std::memory_order_acquire))
            << "iteration " << iteration << ": a producer waited " << OVERLAP_RENDEZVOUS_BOUND_MS
            << " ms at the rendezvous and its partner never arrived, so no overlap took place. "
               "This is a harness failure rather than a production one";

        ASSERT_FALSE(shared->contendingRaisedSomethingElse)
            << "iteration " << iteration << ": the receive handoff raised an exception that is "
               "neither InvalidStateException nor nothing at all, so what follows cannot be "
               "interpreted";

        ASSERT_FALSE(shared->closeRaisedSomethingElse)
            << "iteration " << iteration << ": close() raised an exception that is not the "
               "IOException a proxyless instance must report, so what follows cannot be "
               "interpreted";

        EXPECT_FALSE(shared->closeReturnedCleanly)
            << "iteration " << iteration << ": close() returned cleanly on an instance holding "
               "no service proxy, so the assertions below are describing a different code path";

        // What the queue really holds, by identity. Draining here also leaves the harness's
        // ownership sweep nothing to drain, which is what keeps each frame released once.
        std::vector<CECFrame *> takenFromQueue;
        size_t                  sentinels = 0;

        shared->probe.drainInto(takenFromQueue, sentinels);

        if (!shared->contendingAccepted) {
            ownedByCaller.push_back(shared->contending);
        }

        // Invariant 1: the last slot goes to exactly one producer, so the sentinel is queued
        // exactly when the frame was not; a full queue drops it as the legacy queue does.
        EXPECT_EQ(sentinels, shared->contendingAccepted ? 0u : 1u)
            << "iteration " << iteration << ": the queue yielded " << sentinels
            << " NULL sentinels after an overlap whose handoff was "
            << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << ". One slot was free, so "
               "exactly one of the two producers can have landed";

        // Invariant 2: reported-accepted if and only if present. This is the one the missing
        // lock violates, in either direction, and it is the reason this case exists.
        size_t contendingInQueue = 0;

        for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
            if (takenFromQueue[entry] == shared->contending) {
                contendingInQueue++;
            }
        }

        EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
            << "iteration " << iteration << ": the handoff reported the contending frame "
            << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << " and the drained queue "
               "held it " << contendingInQueue << " times. The report and the queue must agree: "
               "acceptance reported for a frame that is not there leaves NOBODY to release it - "
               "the ownership leak - and refusal reported for one that is there has the caller "
               "free a frame the queue still owns. An unserialized close() sentinel landing "
               "between offerReceivedFrame()'s occupancy check and its offer produces exactly "
               "this disagreement";

        // Invariant 3: the partition, over every allocation, so "no owner" and "two owners" are
        // distinguished from each other and from the expected answer.
        size_t frameWithoutAnOwner = 0;
        size_t frameWithTwoOwners  = 0;

        for (size_t allocation = 0; allocation < shared->allocated.size(); allocation++) {
            size_t owners = 0;

            for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
                if (takenFromQueue[entry] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            for (size_t kept = 0; kept < ownedByCaller.size(); kept++) {
                if (ownedByCaller[kept] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            if (owners == 0) {
                frameWithoutAnOwner++;
            }
            else if (owners > 1) {
                frameWithTwoOwners++;
            }
        }

        EXPECT_EQ(frameWithoutAnOwner, 0u)
            << "iteration " << iteration << ": " << frameWithoutAnOwner << " frames have NO "
               "owner - the handoff reported them accepted and the queue does not hold them, so "
               "nothing in the process will ever release them. That is the ownership leak, one "
               "frame per event on a path a remote HAL drives";

        EXPECT_EQ(frameWithTwoOwners, 0u)
            << "iteration " << iteration << ": " << frameWithTwoOwners << " frames have TWO "
               "owners - both the queue and the caller believe they hold them, which is a double "
               "free rather than a leak";

        overlapsCompleted++;

        if (shared->contendingRefusedByStateGuard) {
            closeFirstByStateGuard++;
        }
        else if (shared->contendingAccepted) {
            receiveFirstOverlaps++;
        }
        else {
            closeFirstByOccupancy++;
        }

        // One clear failure rather than 256 copies of it: the first violated invariant stops the
        // loop, and the harness releases this iteration's frames on the way out.
        if (HasFailure()) {
            break;
        }
    }

    // The census, printed for the reader and not asserted: a 0 / 0 / 256 split is what one CPU
    // produces. The constructed arms guarantee each ordering ran.
    std::cout << "[queue handoff overlap stress] " << overlapsCompleted << " of "
              << OVERLAP_STRESS_ITERATIONS << " overlaps completed: " << receiveFirstOverlaps
              << " receive-first, " << closeFirstByOccupancy << " close-first refused by "
                 "occupancy, " << closeFirstByStateGuard << " close-first refused by the state "
                 "guard (informational; the constructed arms carry the non-vacuity requirement)"
              << std::endl;

    EXPECT_EQ(overlapsCompleted, OVERLAP_STRESS_ITERATIONS)
        << "only " << overlapsCompleted << " of " << OVERLAP_STRESS_ITERATIONS
        << " overlaps completed, so the loop stopped early on a failure reported above";

    // Non-vacuity is asserted on the constructed arms, which run on any CPU count, and fails if
    // an arm stops producing its ordering.
    EXPECT_TRUE(receiveFirstConstructed)
        << "the constructed receive-first arm did not produce an accepted handoff, so the "
           "accepted-and-present arm of the invariant was not exercised deterministically";

    EXPECT_TRUE(refusedByOccupancyConstructed)
        << "the constructed close-first-by-occupancy arm did not produce a full-queue refusal, so "
           "the refused-and-absent arm of the invariant was not exercised deterministically";

    EXPECT_TRUE(refusedByStateConstructed)
        << "the constructed close-first-by-state-guard arm did not produce an "
           "InvalidStateException, so the earliest form of the close-first order was not "
           "exercised deterministically";
}

/**
 * @brief read()'s teardown flush drains and releases a real frame queued behind the close
 *        sentinel, then raises.
 * @pre Runs under every invocation, through ReceiveQueueProbe. The test thread holds the
 *      instance lock as a gate so the reader reaches the flush with the frame still queued.
 * @note The only case that drains a frame through the flush; every other one is taken by the
 *       reader's first poll.
 */
TEST_F(DriverAidlLocalInstanceTest, TheReceiveFlushReleasesARealFrameQueuedBehindTheCloseSentinel) {
    ASSERT_NE(mock, nullptr);

    // Nothing on this path may reach the legacy HAL: the receive queue and its sentinel are
    // entirely middleware-side on both back-ends.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    ReceiveQueueProbe probe;
    MonotonicLatch    readerEnteredRead;
    MonotonicLatch    readerFinished;

    bool readerRaisedInvalidState = false;
    bool readerRaisedSomethingElse = false;
    bool readerReturnedCleanly = false;

    probe.markOpened();

    ASSERT_EQ(probe.occupancy(), 0u)
        << "the receive queue is not empty before the reader starts, so the reader would not "
           "block inside poll() and the gate below could not be established";

    std::thread reader([&]() {
        CECFrame received;

        readerEnteredRead.notify();

        try {
            probe.read(received);
            readerReturnedCleanly = true;
        }
        catch (InvalidStateException &) {
            readerRaisedInvalidState = true;
        }
        catch (...) {
            readerRaisedSomethingElse = true;
        }

        readerFinished.notify();
    });

    // The reader signals before read(); the margin covers its descent into poll(), and a short
    // margin shows up as the queue-empty assertion naming a missed flush.
    ASSERT_TRUE(readerEnteredRead.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader thread never reached read(), so nothing below was exercised";

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Ownership passes to the queue here and the flush releases it; if the flush never runs,
    // the probe's destructor drains it and the occupancy check reports the miss.
    CECFrame *behindSentinel = new CECFrame(directedFrame());

    {
        // The gate, held across the state change and both offers as close() holds it: the reader
        // is parked between its poll and its state re-check.
        CCEC_OSAL::AutoLock gate(probe.instanceLock());

        probe.markClosing();

        // Sentinel first, so the blocked poll returns NULL and descends into the flush; the
        // frame second, so the flush is what meets it.
        probe.postCloseSentinel();
        probe.postFrameBehindSentinel(behindSentinel);
    }

    ASSERT_TRUE(readerFinished.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader did not finish within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms after the gate was released. It is blocked somewhere in read()";

    reader.join();

    EXPECT_TRUE(readerRaisedInvalidState)
        << "read() did not raise InvalidStateException on a driver that stopped being OPENED. "
           "The flush drains whatever is queued and then raises regardless of what it drained, "
           "so a real frame in the queue must not change that outcome";

    EXPECT_FALSE(readerReturnedCleanly)
        << "read() returned the queued frame to its caller. The flush arm drains and raises; "
           "delivering a frame from it would hand the Bus reader a frame during teardown";

    EXPECT_FALSE(readerRaisedSomethingElse)
        << "read() raised something other than InvalidStateException while draining a real frame";

    EXPECT_EQ(probe.occupancy(), 0u)
        << "the receive queue still holds " << probe.occupancy()
        << " entries. One means the reader unwound before the flush, so the frame was never "
           "drained and this case did not exercise the non-NULL arm at all";
}

/**
 * @brief The receive guard accepts a frame while OPENED and rejects one while CLOSING or
 *        CLOSED, leaving a rejected frame with the caller.
 * @pre Runs under every invocation, through ReceiveQueueProbe with each state set directly.
 * @note Pins the verdict per state deterministically; the overlap cases above drive the
 *       concurrent form.
 */
TEST_F(DriverAidlLocalInstanceTest, TheReceiveGuardRejectsACallbackDuringAndAfterClose) {
    ASSERT_NE(mock, nullptr);

    // Nothing on the receive path reaches the legacy HAL on this back-end.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    ReceiveQueueProbe probe;

    // Positive control first, so a guard that rejected everything cannot pass: OPENED accepts
    // and acceptance transfers ownership.
    probe.markOpened();

    CECFrame *accepted = new CECFrame();
    accepted->append(0x0F);

    bool tookIt = false;

    ASSERT_NO_THROW({ tookIt = probe.offer(accepted); })
        << "the receive handoff raised while the driver was OPENED, so the guard is rejecting "
           "frames it must accept and the rejections below prove nothing";
    ASSERT_TRUE(tookIt)
        << "the receive handoff refused a frame on an OPENED driver with an empty queue";
    ASSERT_EQ(probe.occupancy(), 1u)
        << "the handoff reported it took the frame but the queue does not hold it";

    size_t frames = 0;
    size_t sentinels = 0;

    probe.drainCounting(frames, sentinels);

    ASSERT_EQ(frames, 1u) << "the queue did not hold the accepted frame";
    ASSERT_EQ(sentinels, 0u) << "a close sentinel appeared without a close";

    /**
     * @brief A teardown state, set as production sets it, in which the guard must reject.
     */
    struct TeardownState {
        const char *name;
        bool        closing;
    };

    const TeardownState states[] = {
        { "CLOSING - a callback arriving DURING a close", true },
        { "CLOSED - a callback arriving AFTER a close", false }
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (states[i].closing) {
            probe.markClosing();
        }
        else {
            probe.markClosed();
        }

        CECFrame *rejected = new CECFrame();
        rejected->append(0x0F);

        EXPECT_THROW({ probe.offer(rejected); }, InvalidStateException)
            << "the receive handoff accepted a frame in state " << states[i].name
            << ". The guard in getIncomingQueue() is what rejects a callback during teardown, "
               "and the legacy back-end rejects it the same way";

        EXPECT_EQ(probe.occupancy(), 0u)
            << "a frame reached the queue in state " << states[i].name
            << ", so it would be delivered to the Bus reader after the driver stopped being open";

        // Ownership stayed with the caller, which the listener's catch relies on; releasing it
        // here is that release.
        delete rejected;
    }
}

/**
 * @brief getLogicalAddress accepts only the contract range 0x0-0xE, validated on the raw int32
 *        before any narrowing, and reports 0 for anything else.
 * @pre Runs under every invocation, through SessionStateProbe with an injected service double.
 * @note 256 and 271 are included because they truncate to 0 and 0xF through a narrower type;
 *       0 is the existing no-address return, so the genuine address 0 is indistinguishable.
 */
TEST_F(DriverAidlLocalInstanceTest, TheLogicalAddressReadAcceptsOnlyContractRangeValues) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end never reaches the legacy address read, whatever the AIDL HAL reports.
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    /**
     * @brief One HAL-reported value and the address getLogicalAddress must return for it.
     */
    struct AddressCase {
        const char *name;
        int32_t     reported;
        int         expected;
    };

    const AddressCase cases[] = {
        // In contract: accepted verbatim, including both boundaries.
        { "0x0, the lowest contract value and the genuine zero address", 0x0,  0x0 },
        { "0x1, an interior contract value",                            0x1,  0x1 },
        { "0x4, the playback device address the plugins use",           0x4,  0x4 },
        { "0xE, the highest contract value",                            0xE,  0xE },
        // Out of contract: rejected to the no-address return.
        { "0xF, the broadcast/unregistered address - not a held one",   0xF,  0 },
        { "0x10, one past the contract range",                          0x10, 0 },
        { "256, which TRUNCATES TO 0 through a narrower type",          256,  0 },
        { "271, which TRUNCATES TO 0xF through a narrower type",        271,  0 },
        { "-1, a negative status value mistaken for an address",        -1,   0 },
        { "INT32_MAX",                                                  2147483647,        0 },
        { "INT32_MIN",                                                  (-2147483647 - 1), 0 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(cases[i].reported);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        int observed = -1;

        ASSERT_NO_THROW({ observed = probe.getLogicalAddress(0); })
            << "getLogicalAddress raised for " << cases[i].name
            << ". This method never throws on HAL failure - the zero return is the failure "
               "signal, and LibCCEC::getLogicalAddress() is what raises";

        EXPECT_EQ(observed, cases[i].expected)
            << "the HAL reported " << cases[i].reported << " (" << cases[i].name
            << ") and getLogicalAddress returned " << observed << " rather than "
            << cases[i].expected;

        EXPECT_EQ(service->readCalls, 1u)
            << "IHdmiCec::getLogicalAddresses was called " << service->readCalls
            << " times for " << cases[i].name << ", not once. The validation must reject the "
               "value the HAL gave rather than ask again";
    }
}

/**
 * @brief Validation applies to entry zero of a multi-address result: a valid entry zero is
 *        used, and an out-of-contract one is rejected even with valid entries behind it.
 * @pre Runs under every invocation, through SessionStateProbe.
 * @note Scanning for the first acceptable entry would substitute an address the HAL did not
 *       nominate.
 */
TEST_F(DriverAidlLocalInstanceTest, TheLogicalAddressReadValidatesEntryZeroOfAMultiAddressResult) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    // Entry zero valid, later entries irrelevant - including one that is out of contract, which
    // must not affect the verdict either.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(0x3);
        service->addresses.push_back(0xFF);
        service->addresses.push_back(0x5);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0x3)
            << "a multi-address result with a valid entry zero did not yield entry zero. "
               "Divergence 1 requires the first entry to be used and the condition logged";
    }

    // Entry zero OUT of contract, a valid entry behind it. Rejected, because entry zero is the
    // entry, and 271 would have truncated to 0xF had it been converted first.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(271);
        service->addresses.push_back(0x4);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0)
            << "an out-of-contract entry zero was accepted, or a later valid entry was "
               "substituted for it. Scanning for an acceptable entry is the iteration "
               "divergence 1 forbids, and it reports an address the HAL did not nominate";
    }

    // A non-ok transaction reports no address, and whatever vector it wrote is not read.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(0x4);
        service->readStatus = ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0)
            << "a failed transaction yielded an address. The vector is not to be trusted when "
               "the transaction reports failure, whatever it contains";
    }
}

/**
 * @brief Every documented send status, on directed and broadcast frames, maps to the outcome
 *        the legacy back-end produces for the same combination.
 * @pre Runs under every invocation, through SessionStateProbe with an injected session.
 * @note The broadcast sense is inverted: ACK_STATE_0 acknowledges a directed message and
 *       rejects a broadcast, and ACK_STATE_1 is the mirror.
 */
TEST_F(DriverAidlLocalInstanceTest, EveryDocumentedTransmitStatusKeepsItsLegacyMapping) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end never reaches the legacy transmit.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    /**
     * @brief The outcome write() must produce for a row.
     */
    enum ExpectedOutcome { OUTCOME_SUCCESS, OUTCOME_NO_ACK, OUTCOME_IO_ERROR };

    /**
     * @brief One destination kind, opcode and reported status, with the expected outcome.
     */
    struct TransmitCase {
        const char     *name;
        bool            broadcast;
        uint8_t         opcode;
        int32_t         reported;
        ExpectedOutcome expected;
    };

    const TransmitCase cases[] = {
        { "directed ACK_STATE_0 - acknowledged by the addressed follower", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_SUCCESS },
        { "directed ACK_STATE_1 - NOT acknowledged", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1),
          OUTCOME_NO_ACK },
        { "broadcast ACK_STATE_1 - sent and not rejected, the inverted sense", true,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1),
          OUTCOME_SUCCESS },
        { "broadcast ACK_STATE_0 on REPORT_PHYSICAL_ADDRESS - the CEC CTS 9-3-3 arm", true,
          REPORT_PHYSICAL_ADDRESS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_NO_ACK },
        { "broadcast ACK_STATE_0 on any other opcode - returns normally, as on legacy", true,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_SUCCESS },
        { "directed BUSY - arbitration failed, nothing was sent", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::BUSY),
          OUTCOME_IO_ERROR },
        { "broadcast BUSY - arbitration failed, nothing was sent", true,
          REPORT_PHYSICAL_ADDRESS, static_cast<int32_t>(cechal::SendMessageStatus::BUSY),
          OUTCOME_IO_ERROR }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
        const ::android::sp<TransmitResultDouble> controller = ::android::sp<TransmitResultDouble>::make();

        controller->reportedStatus = cases[i].reported;

        SessionStateProbe probe;

        probe.injectOpenSession(service, controller);

        const CECFrame frame =
            cases[i].broadcast ? broadcastFrame(cases[i].opcode) : directedFrame(cases[i].opcode);

        switch (cases[i].expected) {
            case OUTCOME_SUCCESS:
                EXPECT_NO_THROW({ probe.write(frame); })
                    << "write() raised for " << cases[i].name
                    << ", which the legacy back-end completes normally";
                break;

            case OUTCOME_NO_ACK:
                EXPECT_THROW({ probe.write(frame); }, CECNoAckException)
                    << "write() did not raise CECNoAckException for " << cases[i].name;
                break;

            case OUTCOME_IO_ERROR:
                EXPECT_THROW({ probe.write(frame); }, IOException)
                    << "write() did not raise IOException for " << cases[i].name;
                break;
        }

        // The transmit was attempted exactly once in every arm, including the raising ones:
        // the status is translated after the call, never instead of it.
        EXPECT_EQ(controller->sendCalls, 1u)
            << "IHdmiCecController::sendMessage was called " << controller->sendCalls
            << " times for " << cases[i].name << ", not once";

        // The probe leaves scope here, and its destructor forces the state to CLOSED so no
        // teardown re-enters close() against these doubles.
    }
}

/**
 * @brief A send status outside the three documented values returns normally on directed and
 *        broadcast frames, as the legacy status mapping does.
 * @pre Runs under every invocation, through SessionStateProbe with an injected session.
 * @note The values include 3 (one past the last enumerator), a negative and the extremes; the
 *       broadcast frame carries REPORT_PHYSICAL_ADDRESS, so no value may reach the CTS 9-3-3 arm.
 */
TEST_F(DriverAidlLocalInstanceTest, AnUndocumentedTransmitStatusReturnsNormallyAsTheLegacyMappingDoes) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    const int32_t undocumented[] = { 3, 4, 99, -1, 2147483647, (-2147483647 - 1) };

    for (size_t i = 0; i < sizeof(undocumented) / sizeof(undocumented[0]); i++) {
        for (int broadcast = 0; broadcast < 2; broadcast++) {
            const ::android::sp<ClosingServiceDouble> service =
                ::android::sp<ClosingServiceDouble>::make();
            const ::android::sp<TransmitResultDouble> controller =
                ::android::sp<TransmitResultDouble>::make();

            controller->reportedStatus = undocumented[i];

            SessionStateProbe probe;

            probe.injectOpenSession(service, controller);

            const CECFrame frame = (broadcast != 0) ? broadcastFrame() : directedFrame();

            EXPECT_NO_THROW({ probe.write(frame); })
                << "write() raised for the undocumented send status " << undocumented[i]
                << " on a " << ((broadcast != 0) ? "broadcast" : "directed")
                << " frame, which the legacy back-end completes normally because the status is "
                   "in neither its failure set nor its not-acknowledged arm";

            EXPECT_EQ(controller->sendCalls, 1u)
                << "the transmit was attempted " << controller->sendCalls
                << " times for status " << undocumented[i] << ", not once";
        }
    }
}

/**
 * @brief The allocation candidate table is the inverse of LogicalAddress::getType(), for every
 *        DeviceType and for an unknown type.
 * @pre Runs under every invocation; the table is a static member and touches no HAL.
 */
TEST_F(DriverAidlLocalInstanceTest, TheCandidateTableIsTheInverseOfTheLogicalAddressTypeTable) {
    /** @brief One device type and the candidate addresses the table must list for it, in order. */
    struct Expectation {
        int deviceType;               /**< @brief A DeviceType value, or one outside the enum. */
        std::vector<int> candidates;  /**< @brief The expected candidates; empty when none. */
    };
    const Expectation expectations[] = {
        { DeviceType::TV,               { 0 } },
        { DeviceType::RECORDING_DEVICE, { 1, 2, 9 } },
        { DeviceType::RESERVED,         { } },
        { DeviceType::TUNER,            { 3, 6, 7, 10 } },
        { DeviceType::PLAYBACK_DEVICE,  { 4, 8, 11 } },
        { DeviceType::AUDIO_SYSTEM,     { 5 } },
        { DeviceType::PURE_CEC_SWITCH,  { } },
        { DeviceType::VIDEO_PROCESSOR,  { } },
        { DeviceType::VIDEO_PROCESSOR + 1, { } },
        { -1,                           { } },
    };

    for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); i++) {
        EXPECT_EQ(AllocationProbe::logicalAddressCandidates(expectations[i].deviceType),
                  expectations[i].candidates)
            << "wrong candidate list for device type " << expectations[i].deviceType;
    }

    for (int address = LogicalAddress::TV; address <= LogicalAddress::SPECIFIC_USE; address++) {
        const int type = LogicalAddress(address).getType();
        const std::vector<int> candidates = AllocationProbe::logicalAddressCandidates(type);
        const bool listed = std::find(candidates.begin(), candidates.end(), address) != candidates.end();

        EXPECT_EQ(listed, type != DeviceType::RESERVED)
            << "logical address " << address << " is typed " << type
            << " by LogicalAddress::getType() but the candidate table disagrees";
    }
}

/**
 * @brief Enabling registers exactly one address - the first free PLAYBACK_DEVICE candidate - with
 *        one addLogicalAddresses() call, and getLogicalAddress() reads it back through the HAL.
 * @pre Runs under every invocation, on a local instance with an injected session over local fakes.
 */
TEST_F(DriverAidlLocalInstanceTest, EnablingRegistersExactlyOneAddressAndReadsItBackThroughTheHal) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    AllocationProbe probe;

    probe.injectOpenSession(service, controller);
    probe.registerAddress();

    EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }))
        << "allocation did not poll Playback Device 1 first and stop there";
    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
        << "enabling did not call addLogicalAddresses exactly once";
    EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 4 }))
        << "addLogicalAddresses was not given the one-element vector { 4 }";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(controller->getSendMessageCallCount(), 0)
        << "an allocation poll was counted as an application frame";
    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 1)
        << "enabling made a transmit other than its one allocation poll";
    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }));
    EXPECT_TRUE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)));

    const int32_t readsBefore = service->getGetLogicalAddressesCallCount();
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::RECORDING_DEVICE), 4)
        << "getLogicalAddress did not return the registered address whatever devType it was given";
    EXPECT_EQ(service->getGetLogicalAddressesCallCount(), readsBefore + 1)
        << "getLogicalAddress answered without asking IHdmiCec::getLogicalAddresses";
}

/**
 * @brief Occupied candidates are skipped in order, and a full set registers nothing without
 *        raising, leaving getLogicalAddress() at 0.
 * @pre Runs under every invocation, on local instances with injected sessions.
 */
TEST_F(DriverAidlLocalInstanceTest, OccupiedCandidatesAreSkippedAndAFullSetRegistersNothing) {
    /** @brief One set of occupied candidates and the polls and registration it must produce. */
    struct Scenario {
        std::vector<int32_t> occupied;       /**< @brief Addresses the fake reports as taken. */
        std::vector<int32_t> expectedPolls;  /**< @brief The allocation polls, in order. */
        std::vector<int> expectedHeld;       /**< @brief The address registered; empty when none. */
    };
    const Scenario scenarios[] = {
        { { 4 },        { 4, 8 },     { 8 } },
        { { 4, 8 },     { 4, 8, 11 }, { 11 } },
        { { 4, 8, 11 }, { 4, 8, 11 }, { } },
    };

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        AllocationProbe probe;

        for (size_t j = 0; j < scenarios[i].occupied.size(); j++) {
            controller->setLogicalAddressOccupied(scenarios[i].occupied[j], true);
        }

        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << "allocation raised in scenario " << i;

        EXPECT_EQ(controller->getAllocationPolls(), scenarios[i].expectedPolls) << "scenario " << i;
        EXPECT_EQ(probe.heldAddresses(), scenarios[i].expectedHeld) << "scenario " << i;
        EXPECT_EQ(controller->getAddLogicalAddressesCallCount(),
                  scenarios[i].expectedHeld.empty() ? 0 : 1) << "scenario " << i;
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE),
                  scenarios[i].expectedHeld.empty() ? 0 : scenarios[i].expectedHeld[0])
            << "scenario " << i;
    }
}

/**
 * @brief A failed poll registers its candidate, a HAL refusal moves allocation to the next
 *        candidate, and a non-ok add status stops it with nothing registered.
 * @pre Runs under every invocation, on local instances with injected sessions.
 */
TEST_F(DriverAidlLocalInstanceTest, AllocationTriesTheNextCandidateOnRefusalAndStopsOnTransportFailure) {
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        AllocationProbe probe;

        controller->setAllocationPollResult(4, cechal::SendMessageStatus::BUSY);
        probe.injectOpenSession(service, controller);
        probe.registerAddress();

        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }))
            << "a poll that failed with BUSY was not treated as free";
        EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
            << "the candidate whose poll failed with BUSY was not offered to the HAL exactly once";
        EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }))
            << "allocation polled another candidate after registering the one whose poll failed";
    }
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<DecliningControllerDouble> controller =
            ::android::sp<DecliningControllerDouble>::make();
        AllocationProbe probe;

        probe.injectOpenSession(service, controller);
        probe.registerAddress();

        EXPECT_EQ(controller->declinedCalls, 1u) << "the refused candidate was not offered once";
        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 8 }))
            << "a HAL refusal of the first candidate did not move allocation to the next one";
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 8 }));
    }
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        AllocationProbe probe;

        controller->setAddLogicalAddressesBinderStatus(
            ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); });

        EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
            << "allocation kept trying after a transport failure";
        EXPECT_TRUE(probe.heldAddresses().empty());
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 0);
    }
    {
        AllocationProbe probe;

        probe.injectOpenSession(::android::sp<FakeHdmiCecService>::make(), nullptr);
        ASSERT_NO_THROW({ probe.registerAddress(); });
        EXPECT_TRUE(probe.heldAddresses().empty()) << "an address was recorded with no controller";
    }
}

/**
 * @brief A transport failure on the first allocation poll marks that candidate free, so enabling
 *        polls only it, registers it with one add and getLogicalAddress() reads it back.
 * @pre Runs under every invocation, on a local instance with an injected session over local fakes.
 */
TEST_F(DriverAidlLocalInstanceTest, AllocationPollTransportFailuresRegisterNothing) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    AllocationProbe probe;

    controller->setSendMessageBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    probe.injectOpenSession(service, controller);
    ASSERT_NO_THROW({ probe.registerAddress(); }) << "a poll transport failure escaped the allocation";

    EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }))
        << "allocation polled another candidate after the poll of the first one failed in transport";
    EXPECT_EQ(controller->getTotalSendMessageCallCount(), 1);
    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
        << "the candidate whose poll failed in transport was not offered to the HAL exactly once";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }));
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 4);
}

/**
 * @brief A poll that raises a standard or a non-standard exception marks its candidate free, so
 *        allocation registers that candidate without raising and polls no other.
 * @pre Runs under every invocation, on local instances with injected sessions.
 */
TEST_F(DriverAidlLocalInstanceTest, AllocationTreatsANonCecPollExceptionAsTakenAndTriesTheNextCandidate) {
    for (int nonStandard = 0; nonStandard <= 1; nonStandard++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<RaisingPollControllerDouble> controller =
            ::android::sp<RaisingPollControllerDouble>::make();
        AllocationProbe probe;

        controller->raisesNonStandard = (nonStandard != 0);
        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); })
            << "a raising poll escaped allocation (non-standard: " << nonStandard << ")";

        EXPECT_EQ(controller->raisedPolls, 1u) << "the poll of candidate 4 was not attempted once";
        EXPECT_TRUE(controller->getAllocationPolls().empty())
            << "allocation polled another candidate after the raising poll";
        EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1);
        EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }))
            << "the HAL does not hold exactly the candidate whose poll raised";
        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }))
            << "a candidate whose poll raised was not treated as free";
    }
}

/**
 * @brief An add that raises std::bad_alloc before registering is contained: allocation stops,
 *        the compensating removal names the candidate, and nothing is held on either side.
 * @pre Runs under every invocation, on local instances with injected sessions; the removal
 *      reports true, false and a non-ok status in turn.
 */
TEST_F(DriverAidlLocalInstanceTest, AnAddThatRaisesBeforeRegisteringIsContainedAndRegistersNothing) {
    for (int removal = 0; removal < 3; removal++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<RaisingAddControllerDouble> controller =
            ::android::sp<RaisingAddControllerDouble>::make();
        AllocationProbe probe;

        if (removal == 1) {
            controller->setRemoveLogicalAddressesResult(false);
        } else if (removal == 2) {
            controller->setRemoveLogicalAddressesBinderStatus(
                ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        }
        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); })
            << "std::bad_alloc from the add escaped allocation (removal scenario " << removal << ")";

        EXPECT_EQ(controller->raisedAdds, 1u) << "allocation kept trying after the add raised";
        EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(controller->getRemoveLogicalAddressesCallCount(), 1)
            << "no compensating removal followed the raising add (removal scenario " << removal << ")";
        EXPECT_EQ(controller->getLastRemovedLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty())
            << "the HAL was left holding a registration (removal scenario " << removal << ")";
        EXPECT_TRUE(probe.heldAddresses().empty())
            << "an address was recorded although the add raised (removal scenario " << removal << ")";

        // The service fake reports its own controller, so it is pointed at this double's registrations.
        service->setLogicalAddressesResult(controller->getRegisteredLogicalAddresses());
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 0);
    }
}

/**
 * @brief An add that registers and then raises is withdrawn by the compensating removal, and a
 *        removal that raises as well is contained too, leaving nothing recorded locally.
 * @pre Runs under every invocation, on local instances with injected sessions.
 */
TEST_F(DriverAidlLocalInstanceTest, AnAddThatRaisesAfterRegisteringIsWithdrawnByACompensatingRemoval) {
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<RaisingAddControllerDouble> controller =
            ::android::sp<RaisingAddControllerDouble>::make();
        AllocationProbe probe;

        controller->registersBeforeRaising = true;
        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << "the raising add escaped allocation";

        EXPECT_EQ(controller->raisedAdds, 1u) << "allocation kept trying after the add raised";
        EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(controller->getRemoveLogicalAddressesCallCount(), 1);
        EXPECT_EQ(controller->getLastRemovedLogicalAddresses(), std::vector<int32_t>({ 4 }))
            << "the compensating removal did not name the candidate the add registered";
        EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty())
            << "the compensating removal left the registration in the HAL";
        EXPECT_TRUE(probe.heldAddresses().empty());

        service->setLogicalAddressesResult(controller->getRegisteredLogicalAddresses());
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 0);
    }
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<RaisingAddControllerDouble> controller =
            ::android::sp<RaisingAddControllerDouble>::make();
        AllocationProbe probe;

        controller->registersBeforeRaising = true;
        controller->removalRaises = true;
        probe.injectOpenSession(service, controller);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << "a raising compensating removal escaped allocation";

        EXPECT_EQ(controller->raisedAdds, 1u) << "allocation kept trying after the add raised";
        EXPECT_EQ(controller->raisedRemovals, 1u) << "the compensating removal was not attempted once";
        EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }));
        EXPECT_TRUE(probe.heldAddresses().empty()) << "an address was recorded although the add raised";
    }
}

/**
 * @brief A compensating removal that raises, reports false or fails in transport leaves the raising
 *        add's candidate for the next add to release first, so the HAL never holds two addresses.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 * @note Covers a candidate the HAL kept, one it never registered (settled by the read-back), and a
 *       successful close() discarding the record before the next registration.
 */
TEST_F(DriverAidlLocalInstanceTest, AnUnconfirmedCompensatingRemovalIsSettledBeforeTheNextAddressIsAdded) {
    const char *const failures[] = { "raises", "reports false", "fails with DEAD_OBJECT" };

    for (int kept = 1; kept >= 0; kept--) {
        for (size_t failure = 0; failure < sizeof(failures) / sizeof(failures[0]); failure++) {
            const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
            const ::android::sp<RaisingAddControllerDouble> controller =
                ::android::sp<RaisingAddControllerDouble>::make();
            const ::android::sp<JournalingControllerDouble> journalling =
                ::android::sp<JournalingControllerDouble>::make(controller);
            const std::string arm = std::string(kept ? "candidate kept by the HAL" : "candidate never registered") +
                                    ", compensating removal " + failures[failure];
            const std::vector<int32_t> keptRegistration = kept ? std::vector<int32_t>({ 4 }) : std::vector<int32_t>();
            AllocationProbe probe;

            controller->registersBeforeRaising = (kept != 0);
            if (failure == 0) {
                controller->removalRaises = true;
            } else if (failure == 1) {
                controller->setRemoveLogicalAddressesResult(false);
            } else {
                controller->setRemoveLogicalAddressesBinderStatus(
                    ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
            }
            probe.injectOpenSession(service, journalling);
            ASSERT_NO_THROW({ probe.registerAddress(); }) << arm;
            ASSERT_EQ(controller->raisedAdds, 1u) << arm;
            ASSERT_EQ(controller->getRegisteredLogicalAddresses(), keptRegistration) << arm;
            EXPECT_TRUE(probe.heldAddresses().empty()) << arm;

            // The service fake reports its own controller, so it is pointed at this double's registrations.
            service->setLogicalAddressesResult(controller->getRegisteredLogicalAddresses());

            // While the kept candidate's release still fails, the next add releases nothing and adds nothing.
            if (kept && (failure != 0)) {
                const size_t failedMark = journalling->journal.size();

                if (failure == 1) {
                    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                                 AddressNotAvailableException) << arm;
                } else {
                    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                                 IOException) << arm;
                }
                EXPECT_EQ(journalling->sequenceSince(failedMark), "remove{4}")
                    << arm << ": an address was added although the kept candidate was not released";
                EXPECT_EQ(controller->getRegisteredLogicalAddresses(), keptRegistration) << arm;
                EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 4) << arm;
            }

            // The HAL recovers; releasing an address it never added reports false, as its contract says.
            controller->addRaises = false;
            controller->removalRaises = false;
            controller->setRemoveLogicalAddressesBinderStatus(::android::binder::Status::ok());
            controller->setRemoveLogicalAddressesResult(kept != 0);
            const size_t mark = journalling->journal.size();
            const int32_t readsBefore = service->getGetLogicalAddressesCallCount();

            EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM))) << arm;
            EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{5}")
                << arm << ": the add did not release the raising add's candidate before adding";
            EXPECT_EQ(service->getGetLogicalAddressesCallCount(), readsBefore + (kept ? 0 : 1))
                << arm << ": only a declined release is settled by reading the HAL's addresses";
            EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 5 })) << arm;
            EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 5 })) << arm;
            service->setLogicalAddressesResult(controller->getRegisteredLogicalAddresses());
            EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 5) << arm;
            EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
                << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
        }
    }

    // A successful close drops the record, because IHdmiCec::close() removed every address.
    {
        const ::android::sp<RaisingAddControllerDouble> controller =
            ::android::sp<RaisingAddControllerDouble>::make();
        const ::android::sp<FakeHdmiCecService> reopened = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(reopened->getController());
        AllocationProbe probe;

        controller->registersBeforeRaising = true;
        controller->setRemoveLogicalAddressesResult(false);
        probe.injectOpenSession(::android::sp<FakeHdmiCecService>::make(), controller);
        ASSERT_NO_THROW({ probe.registerAddress(); });
        ASSERT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
        ASSERT_NO_THROW({ probe.close(); });

        reopened->getController()->setLogicalAddressOccupied(4, true);
        reopened->getController()->setLogicalAddressOccupied(8, true);
        reopened->getController()->setLogicalAddressOccupied(11, true);
        probe.injectOpenSession(reopened, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); });
        ASSERT_TRUE(probe.heldAddresses().empty());

        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));
        EXPECT_EQ(journalling->sequenceSince(0), "add{5}")
            << "the new session released an address only the previous session's record named";
        EXPECT_EQ(reopened->getController()->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 5 }));
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 5);
    }
}

/**
 * @brief Replacing the held address removes it before adding the new one, and a release the HAL
 *        does not confirm keeps the held address and adds nothing, so one address stays registered.
 * @pre Runs under every invocation, on a local instance whose injected controller journals calls.
 * @note After every arm the HAL's registrations, the HAL-backed query and the local list agree.
 */
TEST_F(DriverAidlLocalInstanceTest, AddingADifferentAddressReplacesTheRegisteredOne) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    const ::android::sp<JournalingControllerDouble> journalling =
        ::android::sp<JournalingControllerDouble>::make(controller);
    AllocationProbe probe;

    // The HAL, the HAL-backed query and the local list must each hold exactly this one address.
    const auto expectOnlyRegistered = [&](int address, const char *arm) {
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ address }))
            << arm << ": the HAL does not hold exactly logical address " << address;
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), address)
            << arm << ": the HAL-backed query reports a different address";
        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ address }))
            << arm << ": the local list disagrees with the HAL";
        EXPECT_TRUE(probe.isValidLogicalAddress(LogicalAddress(address))) << arm;
    };

    probe.injectOpenSession(service, journalling);
    probe.registerAddress();
    ASSERT_EQ(journalling->sequenceSince(0), "add{4}");
    expectOnlyRegistered(4, "enable");

    size_t mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{5}")
        << "the held address was not removed, as a one-element vector, before the new one was added";
    expectOnlyRegistered(5, "replacement");
    EXPECT_FALSE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)));

    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));
    EXPECT_EQ(journalling->sequenceSince(mark), "") << "re-adding the held address reached the HAL";

    // A DEAD_OBJECT release the read-back shows did not happen: IOException, nothing added.
    controller->setRemoveLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    mark = journalling->journal.size();
    const int32_t readsBefore = service->getGetLogicalAddressesCallCount();
    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::RECORDING_DEVICE_1)); },
                 IOException);
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{5}")
        << "an address was added although the held one was not released";
    EXPECT_EQ(service->getGetLogicalAddressesCallCount(), readsBefore + 1)
        << "the failed release was not checked against the HAL's own address list";
    expectOnlyRegistered(5, "failed release");
    EXPECT_FALSE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::RECORDING_DEVICE_1)));

    // A declined release while the HAL still lists the address: AddressNotAvailableException.
    controller->setRemoveLogicalAddressesBinderStatus(::android::binder::Status::ok());
    controller->setRemoveLogicalAddressesResult(false);
    mark = journalling->journal.size();
    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::RECORDING_DEVICE_1)); },
                 AddressNotAvailableException);
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{5}")
        << "an address was added although the HAL declined to release the held one";
    expectOnlyRegistered(5, "declined release, still listed");

    // A declined release of an address the HAL no longer lists counts as released.
    controller->clearRegisteredLogicalAddresses();
    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::RECORDING_DEVICE_1)));
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{5} add{1}");
    expectOnlyRegistered(1, "declined release, no longer listed");
    EXPECT_FALSE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));

    // A refused add after a confirmed release leaves nothing registered anywhere.
    controller->setRemoveLogicalAddressesResult(true);
    controller->setAddLogicalAddressesResult(false);
    mark = journalling->journal.size();
    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::TUNER_1)); },
                 AddressNotAvailableException);
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{1} add{3}");
    EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty());
    EXPECT_TRUE(probe.heldAddresses().empty())
        << "a refused replacement left the released address recorded";
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 0);

    EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
        << "the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
}

/**
 * @brief An address outside 0x0..0xE is refused with AddressNotAvailableException before any HAL
 *        call, so the registered address survives the request.
 * @pre Runs under every invocation, on a local instance whose injected controller journals calls.
 */
TEST_F(DriverAidlLocalInstanceTest, AnOutOfRangeAddressIsRefusedBeforeTheHeldOneIsReleased) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    const ::android::sp<JournalingControllerDouble> journalling =
        ::android::sp<JournalingControllerDouble>::make(controller);
    AllocationProbe probe;

    probe.injectOpenSession(service, journalling);
    probe.registerAddress();
    ASSERT_EQ(journalling->sequenceSince(0), "add{4}");

    const int outOfRange[] = { LogicalAddress::UNREGISTERED, 0x10, 0xFF };
    for (size_t i = 0; i < sizeof(outOfRange) / sizeof(outOfRange[0]); i++) {
        const size_t mark = journalling->journal.size();

        EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(outOfRange[i])); },
                     AddressNotAvailableException)
            << "logical address " << outOfRange[i] << " was not refused";
        EXPECT_EQ(journalling->sequenceSince(mark), "")
            << "logical address " << outOfRange[i] << " reached the HAL before it was refused";
    }

    EXPECT_EQ(controller->getRemoveLogicalAddressesCallCount(), 0)
        << "an invalid request released the registered address";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }));
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 4);
    EXPECT_TRUE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)));
}

/**
 * @brief A standalone removal the HAL fails or declines keeps the address recorded, and the next
 *        add releases it, or confirms it gone, before adding, so at most one address is registered.
 * @pre Runs under every invocation, on a local instance whose injected controller journals calls.
 * @note The removal itself still raises nothing and drops the local entry, as on legacy.
 */
TEST_F(DriverAidlLocalInstanceTest, AnUnconfirmedRemovalIsSettledBeforeTheNextAddressIsAdded) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    const ::android::sp<JournalingControllerDouble> journalling =
        ::android::sp<JournalingControllerDouble>::make(controller);
    AllocationProbe probe;

    probe.injectOpenSession(service, journalling);
    probe.registerAddress();
    ASSERT_EQ(journalling->sequenceSince(0), "add{4}");

    // A transport failure: the local entry goes, the HAL keeps the address.
    controller->setRemoveLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)); });
    EXPECT_TRUE(probe.heldAddresses().empty());
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    // While releases still fail, the next add cannot confirm the release and adds nothing.
    size_t mark = journalling->journal.size();
    EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                 IOException);
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{4}")
        << "the add did not release the address the failed removal left registered";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 4);
    EXPECT_TRUE(probe.heldAddresses().empty());

    // Once releases succeed, the recorded address is removed before the new one is added.
    controller->setRemoveLogicalAddressesBinderStatus(::android::binder::Status::ok());
    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{5}");
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 5 }));
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 5);
    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 5 }));

    // A declined removal that a retry then confirms leaves nothing for the next add to release.
    controller->setRemoveLogicalAddressesResult(false);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); });
    controller->setRemoveLogicalAddressesResult(true);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); });
    EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty());
    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_2)));
    EXPECT_EQ(journalling->sequenceSince(mark), "add{8}")
        << "the add released an address whose removal was already confirmed";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 8 }));

    // A declined removal of an address the HAL has since dropped is confirmed by the read-back.
    controller->setRemoveLogicalAddressesResult(false);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_2)); });
    controller->clearRegisteredLogicalAddresses();
    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_3)));
    EXPECT_EQ(journalling->sequenceSince(mark), "remove{8} add{11}");
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 11 }));
    EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 11);
    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 11 }));

    // A declined removal of an address this back-end does not hold records nothing, whether an
    // address is held at the time or not, so the next add has nothing to release.
    controller->setRemoveLogicalAddressesResult(false);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::TUNER_1)); });
    controller->setRemoveLogicalAddressesResult(true);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_3)); });
    controller->setRemoveLogicalAddressesResult(false);
    EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::TUNER_1)); });
    controller->setRemoveLogicalAddressesResult(true);
    mark = journalling->journal.size();
    EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)));
    EXPECT_EQ(journalling->sequenceSince(mark), "add{4}")
        << "a declined removal of an address this device did not hold was recorded as its own";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));

    EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
        << "the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
}

/**
 * @brief A declined release that cannot be read back, because the query fails or no service proxy
 *        is held, raises IOException and keeps the held address with nothing added.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 */
TEST_F(DriverAidlLocalInstanceTest, AReleaseThatCannotBeReadBackRaisesIoExceptionAndAddsNothing) {
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(controller);
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}");

        controller->setRemoveLogicalAddressesResult(false);
        service->setGetLogicalAddressesBinderStatus(
            ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                     IOException) << "a failed read-back did not raise IOException";
        EXPECT_EQ(journalling->sequenceSince(1), "remove{4}") << "an address was added unconfirmed";
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }))
            << "the held address was dropped although its release was never confirmed";

        service->setGetLogicalAddressesBinderStatus(::android::binder::Status::ok());
        EXPECT_EQ(probe.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 4);
    }
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(controller);
        AllocationProbe probe;

        probe.injectOpenSession(nullptr, journalling);
        probe.registerAddress();
        ASSERT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }));

        controller->setRemoveLogicalAddressesResult(false);
        EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                     IOException) << "a release with no service proxy to confirm it did not raise";
        EXPECT_EQ(journalling->sequenceSince(1), "remove{4}") << "an address was added unconfirmed";
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
        EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 4 }));
    }
}

/**
 * @brief A standalone removal of the held address that raises or fails, before or after the HAL
 *        drops it, keeps the address recorded, so the next add releases it before adding.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 * @note A raise propagates as std::bad_alloc; a failure is ignored, as on legacy.
 */
TEST_F(DriverAidlLocalInstanceTest, ARemovalThatRaisesOrFailsIsReleasedAgainBeforeTheNextAdd) {
    /** @brief Outcome enumeration of UnconfirmedOutcomeControllerDouble, shortened for the case. */
    typedef UnconfirmedOutcomeControllerDouble::Outcome Outcome;
    const Outcome outcomes[] = { Outcome::RAISES_UNAPPLIED, Outcome::RAISES_APPLIED,
                                 Outcome::FAILS_UNAPPLIED, Outcome::FAILS_APPLIED };
    const char *const names[] = { "raises unapplied", "raises applied", "fails unapplied", "fails applied" };

    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(service->getController());
        const bool applied = (outcomes[i] == Outcome::RAISES_APPLIED) || (outcomes[i] == Outcome::FAILS_APPLIED);
        const bool raises = (outcomes[i] == Outcome::RAISES_UNAPPLIED) || (outcomes[i] == Outcome::RAISES_APPLIED);
        const std::string arm = std::string("removal ") + names[i];
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}") << arm;
        expectRegistrationState(service, probe, { 4 }, { 4 }, arm + ", after enable");

        journalling->nextRemove = outcomes[i];
        if (raises) {
            EXPECT_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)); },
                         std::bad_alloc) << arm;
        } else {
            EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)); })
                << arm;
        }
        EXPECT_EQ(journalling->sequenceSince(1), "remove!{4}") << arm;
        expectRegistrationState(service, probe, applied ? std::vector<int32_t>() : std::vector<int32_t>({ 4 }),
                                { }, arm + ", after the removal");

        const size_t mark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM))) << arm;
        EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{5}")
            << arm << ": the add did not release the address the removal left unconfirmed";
        expectRegistrationState(service, probe, { 5 }, { 5 }, arm + ", after the next add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief An enable-time add that fails in transport, whether or not the HAL applied it, records
 *        nothing locally and keeps the candidate, so the next add releases it before adding.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 */
TEST_F(DriverAidlLocalInstanceTest, AnEnableTimeAddThatFailsInTransportIsReleasedBeforeTheNextAdd) {
    /** @brief Outcome enumeration of UnconfirmedOutcomeControllerDouble, shortened for the case. */
    typedef UnconfirmedOutcomeControllerDouble::Outcome Outcome;
    const Outcome outcomes[] = { Outcome::FAILS_UNAPPLIED, Outcome::FAILS_APPLIED };

    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(service->getController());
        const bool applied = (outcomes[i] == Outcome::FAILS_APPLIED);
        const std::string arm = applied ? "add applied, then FAILED_TRANSACTION" : "add unapplied, FAILED_TRANSACTION";
        AllocationProbe probe;

        journalling->nextAdd = outcomes[i];
        probe.injectOpenSession(service, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << arm;
        EXPECT_EQ(journalling->sequenceSince(0), "add!{4}") << arm << ": allocation went on after the failure";
        expectRegistrationState(service, probe, applied ? std::vector<int32_t>({ 4 }) : std::vector<int32_t>(),
                                { }, arm + ", after enable");

        const size_t mark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM))) << arm;
        EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{5}")
            << arm << ": the add did not release the candidate the failed enable-time add left unconfirmed";
        expectRegistrationState(service, probe, { 5 }, { 5 }, arm + ", after the next add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief An explicit add that raises or fails in transport, whether or not the HAL applied it,
 *        keeps its address recorded, so the next add releases it before adding.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 * @note A raise propagates as std::bad_alloc; a transport failure raises IOException. A confirmed
 *       removal of an unheld address in between leaves the record alone.
 */
TEST_F(DriverAidlLocalInstanceTest, AnExplicitAddThatRaisesOrFailsIsReleasedBeforeTheNextAdd) {
    /** @brief Outcome enumeration of UnconfirmedOutcomeControllerDouble, shortened for the case. */
    typedef UnconfirmedOutcomeControllerDouble::Outcome Outcome;
    const Outcome outcomes[] = { Outcome::RAISES_UNAPPLIED, Outcome::RAISES_APPLIED,
                                 Outcome::FAILS_UNAPPLIED, Outcome::FAILS_APPLIED };
    const char *const names[] = { "raises unapplied", "raises applied", "fails unapplied", "fails applied" };

    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(service->getController());
        const bool applied = (outcomes[i] == Outcome::RAISES_APPLIED) || (outcomes[i] == Outcome::FAILS_APPLIED);
        const bool raises = (outcomes[i] == Outcome::RAISES_UNAPPLIED) || (outcomes[i] == Outcome::RAISES_APPLIED);
        const std::string arm = std::string("explicit add ") + names[i];
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}") << arm;

        journalling->nextAdd = outcomes[i];
        if (raises) {
            EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                         std::bad_alloc) << arm;
        } else {
            EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                         IOException) << arm;
        }
        EXPECT_EQ(journalling->sequenceSince(1), "remove{4} add!{5}") << arm;
        expectRegistrationState(service, probe, applied ? std::vector<int32_t>({ 5 }) : std::vector<int32_t>(),
                                { }, arm + ", after the add");

        // TUNER_1 is registered on the fake directly, as another client would, so the HAL confirms a
        // removal this back-end does not hold; that confirmed removal leaves the pending record alone.
        bool foreignAdded = false;
        ASSERT_TRUE(service->getController()->addLogicalAddresses({ LogicalAddress::TUNER_1 }, &foreignAdded).isOk())
            << arm;
        ASSERT_TRUE(foreignAdded) << arm;
        EXPECT_NO_THROW({ probe.removeLogicalAddress(LogicalAddress(LogicalAddress::TUNER_1)); }) << arm;
        EXPECT_EQ(journalling->sequenceSince(3), "remove{3}") << arm;
        expectRegistrationState(service, probe, applied ? std::vector<int32_t>({ 5 }) : std::vector<int32_t>(),
                                { }, arm + ", after the confirmed removal of TUNER_1");

        const size_t mark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_2))) << arm;
        EXPECT_EQ(journalling->sequenceSince(mark), "remove{5} add{8}")
            << arm << ": the add did not release the address the previous add left unconfirmed";
        expectRegistrationState(service, probe, { 8 }, { 8 }, arm + ", after the next add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief An add the HAL declines, explicit or at enable, leaves nothing recorded, so the next add
 *        releases nothing before adding.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 */
TEST_F(DriverAidlLocalInstanceTest, ADeclinedAddLeavesNothingForTheNextAddToRelease) {
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(service->getController());
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}");

        service->getController()->setAddLogicalAddressesResult(false);
        EXPECT_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); },
                     AddressNotAvailableException);
        EXPECT_EQ(journalling->sequenceSince(1), "remove{4} add{5}");
        expectRegistrationState(service, probe, { }, { }, "explicit add declined");

        service->getController()->setAddLogicalAddressesResult(true);
        const size_t mark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_2)));
        EXPECT_EQ(journalling->sequenceSince(mark), "add{8}")
            << "the add released an address the HAL had declined to add";
        expectRegistrationState(service, probe, { 8 }, { 8 }, "after the declined explicit add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u) << journalling->sequenceSince(0);
    }
    {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(service->getController());
        AllocationProbe probe;

        service->getController()->setAddLogicalAddressesResult(false);
        probe.injectOpenSession(service, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); });
        EXPECT_EQ(journalling->sequenceSince(0), "add{4} add{8} add{11}");
        expectRegistrationState(service, probe, { }, { }, "every enable-time add declined");

        service->getController()->setAddLogicalAddressesResult(true);
        const size_t mark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)));
        EXPECT_EQ(journalling->sequenceSince(mark), "add{5}")
            << "the add released a candidate the HAL had declined at enable";
        expectRegistrationState(service, probe, { 5 }, { 5 }, "after the declined enable-time adds");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u) << journalling->sequenceSince(0);
    }
}

/**
 * @brief close() keeps the registered address locally, and the next registration replaces it
 *        so exactly one is held.
 * @pre Runs under every invocation, on a local instance with an injected session.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseKeepsTheAddressAndTheNextRegistrationReplacesIt) {
    const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
    const ::android::sp<FakeHdmiCecController> controller = service->getController();
    AllocationProbe probe;

    probe.injectOpenSession(service, controller);
    probe.registerAddress();
    ASSERT_NO_THROW({ probe.close(); });

    EXPECT_TRUE(probe.isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)))
        << "close() cleared the local address list";
    EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty())
        << "the fake kept registrations across a successful close";

    controller->setLogicalAddressOccupied(4, true);
    probe.injectOpenSession(service, controller);
    probe.registerAddress();

    EXPECT_EQ(probe.heldAddresses(), std::vector<int>({ 8 }))
        << "the re-registration appended instead of replacing";
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 8 }));
}

/**
 * @brief A re-open after a failed close() releases the address the HAL kept before allocating, so
 *        exactly one address is registered and every view of it agrees.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls;
 *      the fake's close() reports false, so it keeps its registrations.
 * @note Covers the first candidate free or occupied at re-open, and an enable-time add left
 *       unconfirmed before the failed close.
 */
TEST_F(DriverAidlLocalInstanceTest, AReopenAfterAFailedCloseReleasesTheKeptAddressBeforeAllocating) {
    /** @brief Outcome enumeration of UnconfirmedOutcomeControllerDouble, shortened for the case. */
    typedef UnconfirmedOutcomeControllerDouble::Outcome Outcome;
    /** @brief One arm: how the first enable-time add ends, whether 4 is taken at re-open, the result. */
    struct Arm {
        const char *name;       /**< @brief Names the arm in every failure message. */
        Outcome firstAdd;       /**< @brief How the first session's enable-time add ends. */
        bool occupiedAtReopen;  /**< @brief Whether logical address 4 answers its poll at re-open. */
        int expected;           /**< @brief The one address the re-open must leave registered. */
    };
    const Arm arms[] = {
        { "first candidate free at re-open", Outcome::CONFIRMED, false, LogicalAddress::PLAYBACK_DEVICE_1 },
        { "first candidate occupied at re-open", Outcome::CONFIRMED, true, LogicalAddress::PLAYBACK_DEVICE_2 },
        { "enable-time add unconfirmed before the failed close", Outcome::FAILS_APPLIED, false,
          LogicalAddress::PLAYBACK_DEVICE_1 },
    };

    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(controller);
        const bool confirmed = (arms[i].firstAdd == Outcome::CONFIRMED);
        const std::string arm = arms[i].name;
        AllocationProbe probe;

        journalling->nextAdd = arms[i].firstAdd;
        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), confirmed ? "add{4}" : "add!{4}") << arm;
        ASSERT_EQ(probe.heldAddresses(), confirmed ? std::vector<int>({ 4 }) : std::vector<int>()) << arm;

        service->setCloseResult(false);
        EXPECT_THROW({ probe.close(); }, IOException) << arm;
        ASSERT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }))
            << arm << ": the failed close was expected to leave the HAL holding logical address 4";

        controller->setLogicalAddressOccupied(4, arms[i].occupiedAtReopen);
        const size_t mark = journalling->journal.size();
        probe.injectOpenSession(service, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << arm;

        EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{" + std::to_string(arms[i].expected) + "}")
            << arm << ": the re-open did not release the kept address before adding";
        expectRegistrationState(service, probe, { arms[i].expected }, { arms[i].expected },
                                arm + ", after the re-open");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief A re-open whose release of the kept address the HAL declines follows the HAL's own list:
 *        an address it still holds is adopted with nothing added, one it no longer holds is released.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls;
 *      the fake's close() reports false.
 * @note The adopted address is then replaced by the next add like any registered one.
 */
TEST_F(DriverAidlLocalInstanceTest, AReopenWhoseReleaseIsDeclinedFollowsTheHalsOwnAddressList) {
    for (int stillListed = 1; stillListed >= 0; stillListed--) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(controller);
        const std::string arm = stillListed ? "declined release, still listed" : "declined release, no longer listed";
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}") << arm;
        service->setCloseResult(false);
        EXPECT_THROW({ probe.close(); }, IOException) << arm;

        if (stillListed) {
            controller->setRemoveLogicalAddressesResult(false);
        } else {
            // The HAL dropped the address on its own, so releasing it reports false, as its contract says.
            controller->clearRegisteredLogicalAddresses();
        }
        const size_t mark = journalling->journal.size();
        const int32_t readsBefore = service->getGetLogicalAddressesCallCount();
        const int32_t pollsBefore = controller->getTotalSendMessageCallCount();
        probe.injectOpenSession(service, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << arm;

        EXPECT_EQ(service->getGetLogicalAddressesCallCount(), readsBefore + 1)
            << arm << ": the declined release was not settled by one read of the HAL's addresses";
        if (stillListed) {
            EXPECT_EQ(journalling->sequenceSince(mark), "remove{4}")
                << arm << ": an address was added although the HAL kept the previous one";
            EXPECT_EQ(controller->getTotalSendMessageCallCount(), pollsBefore)
                << arm << ": a candidate was polled although the HAL's address was adopted";
            expectRegistrationState(service, probe, { 4 }, { 4 }, arm + ", after the re-open");

            controller->setRemoveLogicalAddressesResult(true);
            const size_t replaceMark = journalling->journal.size();
            EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM))) << arm;
            EXPECT_EQ(journalling->sequenceSince(replaceMark), "remove{4} add{5}")
                << arm << ": the adopted address was not replaced like a registered one";
            expectRegistrationState(service, probe, { 5 }, { 5 }, arm + ", after the next add");
        } else {
            EXPECT_EQ(journalling->sequenceSince(mark), "remove{4} add{4}")
                << arm << ": an address the HAL no longer holds was not treated as released";
            expectRegistrationState(service, probe, { 4 }, { 4 }, arm + ", after the re-open");
        }
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief A re-open whose release of the kept address cannot be confirmed registers nothing and keeps
 *        the address recorded, so the next add releases it first and one address remains.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls;
 *      the fake's close() reports false.
 * @note The arms: a DEAD_OBJECT release whose read-back fails, a declined release with no service
 *       proxy to read back from, and a release that raises std::bad_alloc or an int.
 */
TEST_F(DriverAidlLocalInstanceTest, AReopenWhoseReleaseCannotBeConfirmedRegistersNothingUntilTheNextAdd) {
    /** @brief Outcome enumeration of UnconfirmedOutcomeControllerDouble, shortened for the case. */
    typedef UnconfirmedOutcomeControllerDouble::Outcome Outcome;
    const char *const names[] = { "release fails and the read-back fails",
                                  "release declined with no service proxy", "release raises std::bad_alloc",
                                  "release raises a non-standard exception" };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<UnconfirmedOutcomeControllerDouble> journalling =
            ::android::sp<UnconfirmedOutcomeControllerDouble>::make(controller);
        const std::string arm = names[i];
        AllocationProbe probe;

        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), "add{4}") << arm;
        service->setCloseResult(false);
        EXPECT_THROW({ probe.close(); }, IOException) << arm;

        if (i == 0) {
            controller->setRemoveLogicalAddressesBinderStatus(
                ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
            service->setGetLogicalAddressesBinderStatus(
                ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        } else if (i == 1) {
            controller->setRemoveLogicalAddressesResult(false);
        } else {
            journalling->nextRemove = (i == 2) ? Outcome::RAISES_UNAPPLIED : Outcome::RAISES_NON_STANDARD_UNAPPLIED;
        }
        const size_t mark = journalling->journal.size();
        const int32_t pollsBefore = controller->getTotalSendMessageCallCount();
        probe.injectOpenSession((i == 1) ? ::android::sp<FakeHdmiCecService>() : service, journalling);
        ASSERT_NO_THROW({ probe.registerAddress(); }) << arm << ": the unconfirmed release escaped the allocation";

        EXPECT_EQ(journalling->sequenceSince(mark), (i >= 2) ? "remove!{4}" : "remove{4}")
            << arm << ": allocation went ahead although the kept address was not confirmed released";
        EXPECT_EQ(controller->getTotalSendMessageCallCount(), pollsBefore) << arm << ": a candidate was polled";
        EXPECT_EQ(probe.currentStatus(), probe.openedState()) << arm;
        EXPECT_TRUE(probe.heldAddresses().empty()) << arm;
        EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 })) << arm;

        // The HAL recovers; the next add finds the kept address still recorded and releases it first.
        controller->setRemoveLogicalAddressesBinderStatus(::android::binder::Status::ok());
        controller->setRemoveLogicalAddressesResult(true);
        service->setGetLogicalAddressesBinderStatus(::android::binder::Status::ok());
        probe.injectOpenSession(service, journalling);
        expectRegistrationState(service, probe, { 4 }, { }, arm + ", after the re-open");

        const size_t addMark = journalling->journal.size();
        EXPECT_TRUE(probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM))) << arm;
        EXPECT_EQ(journalling->sequenceSince(addMark), "remove{4} add{5}")
            << arm << ": the next add did not release the address the re-open left recorded";
        expectRegistrationState(service, probe, { 5 }, { 5 }, arm + ", after the next add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << arm << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }
}

/**
 * @brief A re-open with nothing recorded releases nothing and reads nothing back: neither after a
 *        successful close(), which removed every address, nor after a failed one that held none.
 * @pre Runs under every invocation, on local instances whose injected controllers journal calls.
 */
TEST_F(DriverAidlLocalInstanceTest, AReopenWithNothingRecordedReleasesNothingBeforeAllocating) {
    for (int closeFails = 0; closeFails <= 1; closeFails++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(controller);
        const std::string arm = closeFails ? "failed close holding no address" : "successful close";
        AllocationProbe probe;

        // A failed close holds no address only when enabling registered none.
        if (closeFails) {
            controller->setLogicalAddressOccupied(4, true);
            controller->setLogicalAddressOccupied(8, true);
            controller->setLogicalAddressOccupied(11, true);
        }
        probe.injectOpenSession(service, journalling);
        probe.registerAddress();
        ASSERT_EQ(journalling->sequenceSince(0), closeFails ? "" : "add{4}") << arm;

        service->setCloseResult(closeFails == 0);
        if (closeFails) {
            EXPECT_THROW({ probe.close(); }, IOException) << arm;
            controller->setLogicalAddressOccupied(4, false);
        } else {
            ASSERT_NO_THROW({ probe.close(); }) << arm;
        }
        ASSERT_TRUE(controller->getRegisteredLogicalAddresses().empty()) << arm;

        const size_t mark = journalling->journal.size();
        const int32_t readsBefore = service->getGetLogicalAddressesCallCount();
        probe.injectOpenSession(service, journalling);
        probe.registerAddress();

        EXPECT_EQ(journalling->sequenceSince(mark), "add{4}")
            << arm << ": the re-open released an address although nothing was recorded";
        EXPECT_EQ(controller->getRemoveLogicalAddressesCallCount(), 0) << arm;
        EXPECT_EQ(service->getGetLogicalAddressesCallCount(), readsBefore)
            << arm << ": the re-open read the HAL's addresses although there was nothing to settle";
        expectRegistrationState(service, probe, { 4 }, { 4 }, arm + ", after the re-open");
    }
}

/**
 * @brief Whichever operator new call of the enable-time allocation raises std::bad_alloc, nothing
 *        escapes, the session stays OPENED and the HAL holds at most the address the local list or
 *        the release record names.
 * @pre Runs under every invocation, on local instances with injected sessions; for each k from 1 to
 *      the K calls an unfailed allocation makes, the k-th call raises once.
 * @note A failure before any add reaches the outer std::exception handler; the next add proves the
 *       release record by leaving the HAL holding only its own address.
 */
TEST_F(DriverAidlLocalInstanceTest, AnAllocationFailureAnywhereInEnableTimeAllocationIsContained) {
    unsigned long unfailedAllocations = 0;

    // The first pass absorbs any one-time initialization, so the second counts what every pass makes.
    for (int pass = 0; pass < 2; pass++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        AllocationProbe probe;

        probe.injectOpenSession(service, service->getController());
        {
            ScopedAllocationFailure counting(0);
            probe.registerAddress();
            unfailedAllocations = counting.allocations();
        }
        ASSERT_EQ(probe.heldAddresses(), std::vector<int>({ 4 })) << "the unfailed allocation did not register 4";
    }
    ASSERT_GT(unfailedAllocations, 0u) << "the unfailed allocation made no operator new call to fail";

    unsigned long containedBeforeAnyAdd = 0;

    for (unsigned long k = 1; k <= unfailedAllocations; k++) {
        const ::android::sp<FakeHdmiCecService> service = ::android::sp<FakeHdmiCecService>::make();
        const ::android::sp<FakeHdmiCecController> controller = service->getController();
        const std::string step = "operator new call " + std::to_string(k) + " of " +
                                 std::to_string(unfailedAllocations) + " raising";
        AllocationProbe probe;
        bool escaped = false;
        bool injected = false;

        probe.injectOpenSession(service, controller);
        {
            ScopedAllocationFailure failure(k);
            try {
                probe.registerAddress();
            } catch (...) {
                escaped = true;
            }
            injected = failure.injected();
        }

        EXPECT_FALSE(escaped) << step << ": an exception escaped the allocation";
        EXPECT_TRUE(injected) << step << ": the failure was never injected, so the pass is not deterministic";
        EXPECT_EQ(probe.currentStatus(), probe.openedState()) << step << ": the session is no longer OPENED";

        const std::vector<int32_t> registered = controller->getRegisteredLogicalAddresses();
        const std::vector<int> held = probe.heldAddresses();

        EXPECT_LE(registered.size(), 1u) << step << ": the HAL holds more than one address";
        EXPECT_TRUE(held.empty() || (std::vector<int32_t>(held.begin(), held.end()) == registered))
            << step << ": the local list is neither empty nor the HAL's single registration";
        if ((controller->getAddLogicalAddressesCallCount() == 0) && registered.empty() && held.empty()) {
            containedBeforeAnyAdd++;
        }

        // An address the HAL holds without a local entry must be the one the next add releases first.
        const ::android::sp<JournalingControllerDouble> journalling =
            ::android::sp<JournalingControllerDouble>::make(controller);
        probe.injectOpenSession(service, journalling);
        EXPECT_NO_THROW({ probe.addLogicalAddress(LogicalAddress(LogicalAddress::AUDIO_SYSTEM)); }) << step;
        if ((registered.size() == 1) && held.empty()) {
            EXPECT_EQ(journalling->sequenceSince(0), "remove{" + std::to_string(registered[0]) + "} add{5}")
                << step << ": the release record did not name the address the HAL kept";
        }
        expectRegistrationState(service, probe, { 5 }, { 5 }, step + ", after the next add");
        EXPECT_LE(journalling->mostRegisteredAtOnce(), 1u)
            << step << ": the HAL held more than one address after a call in: " << journalling->sequenceSince(0);
    }

    EXPECT_GT(containedBeforeAnyAdd, 0u)
        << "no failure was contained before the add, so the outer std::exception handler was not reached";
    std::cout << "[DriverAidlLocalInstanceTest] each of the " << unfailedAllocations
              << " operator new calls of an unfailed enable-time allocation was failed in turn; "
              << containedBeforeAnyAdd << " failed before any add" << std::endl;
}

/**
 * @brief Fixture for the legacy halves of the declared back-end differences and the legacy
 *        baselines, on the open process-global legacy driver.
 * @pre Invocation A (CEC_TEST_AIDL_MODE absent or unset). SetUp asserts the legacy back-end is
 *      resolved and opens the shared driver; TearDown leaves it open.
 * @see DriverAidlSessionFixture for the AIDL halves, which need an opened AIDL session.
 */
class DriverAidlLegacyArmTest : public ::testing::Test {
protected:
    /**
     * @brief Asserts the legacy back-end is resolved and opens the shared driver, both fatally.
     */
    void SetUp() override {
        mock = HdmiCecDriverMock::getInstance();
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        ASSERT_NE(dynamic_cast<DriverImpl *>(&Driver::getInstance()), nullptr)
            << "this fixture requires the legacy back-end to be the resolved one, i.e. invocation "
               "A (CEC_TEST_AIDL_MODE=absent or unset). The selection resolves once per process "
               "inside LibCCEC::init and cannot be changed from here; re-run with the invocation-A "
               "filter from the fixture manifest above";

        // Not wrapped in catch(...): open() is silent when already opened, so its only exception
        // is the mock HAL's refusal, which must fail setup rather than leave the driver CLOSED.
        ASSERT_NO_THROW({ Driver::getInstance().open(); })
            << "the shared driver could not be opened, so this fixture's precondition does not "
               "hold; continuing would assert against a closed driver";
    }

    /**
     * @brief Verifies the legacy HAL expectations and restores the shared driver's opened
     *        baseline with a non-fatal expectation.
     */
    void TearDown() override {
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        // Restore the global environment's opened baseline; non-fatal, so a failure is reported
        // without cutting the cleanup short.
        EXPECT_NO_THROW({ Driver::getInstance().open(); })
            << "failed to restore the shared driver to its opened baseline";
    }

    /**
     * @brief The legacy HAL mock through which every case here injects and observes.
     *
     * @note Unlike the local-instance fixture, cases here do set expectations on it: the
     *       legacy arms are precisely the ones that must reach the legacy HAL.
     */
    HdmiCecDriverMock *mock = nullptr;
};

/**
 * @brief The legacy back-end reads the physical address through the legacy HAL API and
 *        delivers the HAL's value to the caller.
 * @pre Invocation A, with the shared legacy driver open.
 * @note The out-parameter is seeded with a sentinel that must be overwritten, so a case that
 *       wrote nothing cannot pass.
 */
TEST_F(DriverAidlLegacyArmTest, PhysicalAddressIsReadThroughTheLegacyHalApi) {
    ASSERT_NE(mock, nullptr);

    const unsigned int halPhysicalAddress = 0x1000u;
    const unsigned int sentinel = 0xDEADBEEFu;

    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<1>(halPhysicalAddress), Return(HDMI_CEC_IO_SUCCESS)));

    unsigned int physicalAddress = sentinel;
    EXPECT_NO_THROW({ Driver::getInstance().getPhysicalAddress(&physicalAddress); });

    EXPECT_EQ(physicalAddress, halPhysicalAddress)
        << "the legacy back-end did not deliver the HAL's physical address to the caller";
    EXPECT_NE(physicalAddress, sentinel)
        << "the out-parameter still holds the sentinel, so the legacy back-end wrote nothing";
}

/**
 * @brief Legacy half of the frame-length difference: frames of 16, 17 and 20 bytes are all sent
 *        un-truncated on the legacy back-end.
 * @pre Invocation A, with the shared legacy driver open.
 * @note 17 to 20 bytes is the band the AIDL back-end refuses and 16 the control both accept;
 *       the length captured at the HAL is asserted per size.
 * @see DriverAidlTransmitTest::FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated
 */
TEST_F(DriverAidlLegacyArmTest, FramesUpToTheLegacyMaximumAreSentOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    const size_t sizes[] = { kAidlMaxMessageLength, kJustOverAidlLimit, kLegacyMaxMessageLength };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ::testing::Mock::VerifyAndClearExpectations(mock);

        int capturedLength = -1;
        EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
            .Times(1)
            .WillOnce(DoAll(SaveArg<2>(&capturedLength),
                            SetArgPointee<3>(HDMI_CEC_IO_SUCCESS),
                            Return(HDMI_CEC_IO_SUCCESS)));

        CECFrame frame = frameOfLength(sizes[i]);
        ASSERT_EQ(frame.length(), sizes[i])
            << "the frame builder did not produce a frame of " << sizes[i] << " bytes, so this "
               "iteration is not testing the size it claims to";

        EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
            << "the legacy back-end refused a frame of " << sizes[i] << " bytes, which its own HAL "
               "specification permits";

        EXPECT_EQ(capturedLength, static_cast<int>(sizes[i]))
            << "the legacy back-end passed " << capturedLength << " bytes to the HAL for a frame of "
            << sizes[i] << ", so the frame was truncated or padded on the way through";
    }
}

/**
 * @brief Legacy half of the asynchronous-transmit difference: writeAsync succeeds on an open
 *        legacy driver.
 * @pre Invocation A, with the shared legacy driver open.
 * @note With the prelude and guard cases, identical on both back-ends, this confines the
 *       difference to one arm; no production call site reaches writeAsync today.
 * @see DriverAidlTransmitTest::WriteAsyncRaisesOperationNotSupportedOnAnOpenDriver
 */
TEST_F(DriverAidlLegacyArmTest, WriteAsyncSucceedsOnAnOpenLegacyDriver) {
    ASSERT_NE(mock, nullptr);

    int capturedLength = -1;
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<2>(&capturedLength), Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().writeAsync(frame); })
        << "asynchronous transmit failed on the legacy back-end, so the legacy half of authorized "
           "difference 2 no longer holds and the difference is no longer one-sided";

    EXPECT_EQ(capturedLength, 2)
        << "the legacy asynchronous transmit received " << capturedLength
        << " bytes rather than the frame's 2";
}

/**
 * @brief The legacy transmit-status translation raises for an unacknowledged directed frame
 *        and not for an unacknowledged broadcast, except the CEC CTS 9-3-3 opcode.
 * @pre Invocation A, with the shared legacy driver open.
 * @see DriverAidlTransmitTest for the mirrored AIDL arms.
 */
TEST_F(DriverAidlLegacyArmTest, LegacyTransmitStatusTranslationDistinguishesDirectedFromBroadcast) {
    ASSERT_NE(mock, nullptr);

    // A directed frame that was not acknowledged: the addressed follower did not answer.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame directed = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(directed); }, CECNoAckException)
        << "an unacknowledged DIRECTED frame did not raise CECNoAckException on the legacy "
           "back-end";

    ::testing::Mock::VerifyAndClearExpectations(mock);

    // The same status on a broadcast other than REPORT_PHYSICAL_ADDRESS returns normally: a
    // broadcast has no single follower to acknowledge it.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame broadcast = broadcastFrame(GIVE_DEVICE_POWER_STATUS);
    EXPECT_NO_THROW({ Driver::getInstance().write(broadcast); })
        << "an unacknowledged broadcast frame carrying an opcode other than "
           "REPORT_PHYSICAL_ADDRESS raised on the legacy back-end, so the CEC CTS 9-3-3 arm is no "
           "longer specific to that one opcode";

    ::testing::Mock::VerifyAndClearExpectations(mock);

    // And the CEC CTS 9-3-3 arm itself: a rejected broadcast REPORT_PHYSICAL_ADDRESS does
    // raise, so that the caller retries at least once.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame ctsBroadcast = broadcastFrame(REPORT_PHYSICAL_ADDRESS);
    EXPECT_THROW({ Driver::getInstance().write(ctsBroadcast); }, CECNoAckException)
        << "a rejected broadcast REPORT_PHYSICAL_ADDRESS did not raise on the legacy back-end, so "
           "the CEC CTS 9-3-3 retry arm is gone";
}

/**
 * @brief A frame injected through the legacy HAL's Rx callback reaches an application listener
 *        byte for byte, the baseline the AIDL receive cases are measured against.
 * @pre Invocation A, with the shared legacy driver open.
 * @note Uses the same helpers and bounded wait as the AIDL cases, so it also proves that
 *       observation machinery on a host without a binder driver.
 */
TEST_F(DriverAidlLegacyArmTest, ReceivedMessageReachesAnApplicationListenerOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlLegacyArmTest-receive",
                                            applicationListener);

    // The same shape the AIDL half delivers: a broadcast Report Physical Address with a
    // distinctive address, so a delivered frame cannot be confused with any other.
    const unsigned char injected[] = { 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };
    const std::vector<uint8_t> expected(injected, injected + sizeof(injected));

    mock->injectReceivedMessage(injected, static_cast<int>(sizeof(injected)));

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "a frame injected at the legacy HAL never reached an application listener within "
        << kFrameDeliveryTimeoutMs
        << " ms. Either the legacy receive path is broken - HAL callback, state-guarded queue "
           "accessor, Bus reader, Connection filter, listener - or the observation helpers the "
           "AIDL receive cases share with this one are wired wrongly, in which case those cases "
           "would fail on a binder-capable runner for a reason unrelated to the middleware";

    EXPECT_EQ(applicationListener.frameAt(0), expected)
        << "the delivered frame is not the frame that was injected, so the legacy inbound copy "
           "loses or rewrites bytes - and the AIDL half is measured against this baseline";

    EXPECT_EQ(applicationListener.frameCount(), 1u)
        << "one injected frame produced " << applicationListener.frameCount() << " deliveries";
}

/**
 * @brief Shared invocation-B precondition: the AIDL back-end is resolved and each case starts
 *        from a freshly reset fake and an open session.
 * @pre Invocation B (CEC_TEST_AIDL_MODE=compatible); SetUp fails rather than skips otherwise.
 * @note SetUp cycles close -> reset -> open, because the fake's reset clears the captured
 *       listener and only IHdmiCec::open() captures it again.
 */
class DriverAidlSessionFixture : public ::testing::Test {
protected:
    /**
     * @brief Asserts the AIDL back-end and the fake are present, then cycles the session
     *        close -> reset -> open, every step fatal.
     */
    void SetUp() override {
        aidlBackEnd = dynamic_cast<DriverAidlImpl *>(&Driver::getInstance());

        ASSERT_NE(aidlBackEnd, nullptr)
            << "this fixture requires the AIDL back-end to be the resolved one, i.e. invocation B "
               "(CEC_TEST_AIDL_MODE=compatible, on a host with a binder driver and a running "
               "service manager), and the factory returned the legacy back-end instead. The "
               "selection resolves once per process inside LibCCEC::init and cannot be changed "
               "from here. If this ran under invocation A, apply the invocation-A filter from the "
               "fixture manifest above, which excludes this fixture by name";

        fake = FakeHdmiCecService::getInstance();
        ASSERT_NE(fake, nullptr)
            << "the AIDL back-end resolved but no fake service was published for this process, so "
               "these cases have no HAL to program. The harness publishes it before init "
               "in publishFakeForMode(); if the back-end resolved against some other service, this "
               "run's outcome depends on a process this suite does not own";

        // Bring the session down, wipe the fake, bring it back up - see the ordering note above.
        ASSERT_NO_THROW({ Driver::getInstance().close(); })
            << "the shared driver could not be closed, so the session cannot be cycled and the "
               "fake's captured listener cannot be refreshed";

        fake->reset();
        ASSERT_NE(fake->getController(), nullptr)
            << "the fake reports no controller, so nothing here could reach addLogicalAddresses, "
               "removeLogicalAddresses or sendMessage";
        fake->getController()->reset();

        ASSERT_NO_THROW({ Driver::getInstance().open(); })
            << "the shared driver could not be re-opened after the reset, so this fixture's "
               "precondition does not hold; continuing would assert against a closed driver";

        ASSERT_EQ(fake->getOpenCallCount(), 1)
            << "the fake did not observe exactly one open() after the reset, so the session was "
               "not re-established through the HAL and the listener the triggers need was not "
               "captured";
    }

    /**
     * @brief Resets the fake, then closes and re-opens the shared driver non-fatally, so the
     *        enable-time address is re-registered against the reset fake.
     */
    void TearDown() override {
        if (fake != nullptr) {
            // Reset first, so a failing close or open status a case installed cannot make the
            // restoration below fail.
            if (fake->getController() != nullptr) {
                fake->getController()->reset();
            }
            fake->reset();
        }

        // Non-fatal, so a failure is reported rather than cutting the cleanup short.
        EXPECT_NO_THROW({ Driver::getInstance().close(); })
            << "failed to close the shared driver while restoring its opened baseline";
        EXPECT_NO_THROW({ Driver::getInstance().open(); })
            << "failed to restore the shared driver to its opened baseline";
    }

    /**
     * @brief The resolved AIDL back-end, held so cases can reach it without repeating the cast.
     *
     * @note Non-null is a fixture precondition rather than something a case need re-check.
     */
    DriverAidlImpl *aidlBackEnd = nullptr;

    /**
     * @brief The published fake HDMI CEC service, which is the programmable HAL these cases
     *        assert against.
     */
    FakeHdmiCecService *fake = nullptr;
};

/**
 * @brief Session lifecycle, address marshalling and event delivery on the AIDL back-end.
 * @pre Invocation B; see DriverAidlSessionFixture.
 */
class DriverAidlSessionTest : public DriverAidlSessionFixture {
};

/**
 * @brief A present, compatible service selects the AIDL back-end, asserted by concrete type in
 *        both directions and by the selected-path log line.
 * @pre Invocation B.
 */
TEST_F(DriverAidlSessionTest, CompatibleServiceSelectsTheAidlBackEndAndNamesItInTheLog) {
    Driver &resolved = Driver::getInstance();

    EXPECT_NE(dynamic_cast<DriverAidlImpl *>(&resolved), nullptr)
        << "a compatible service is registered but the resolved back-end is not the AIDL one";
    EXPECT_EQ(dynamic_cast<DriverImpl *>(&resolved), nullptr)
        << "the legacy back-end was selected although a compatible AIDL service is registered";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so nothing about the emitted line can be "
               "established";

        CCEC_LOG(LOG_INFO, kSelectedBackEndLogFormat, kSelectedBackEndAidl);

        captured = capture.read();
    }

    const std::string expected =
        std::string("HDMI CEC HAL back-end selected : ") + kSelectedBackEndAidl;

    EXPECT_NE(captured.find(expected), std::string::npos)
        << "the production logger did not emit the selected-path line naming the AIDL back-end. "
           "That line is how the coverage runner and device-level validation establish which "
           "back-end resolved, since no introspection API exists on the Driver interface. "
           "Captured instead: [" << captured << "]";
}

/**
 * @brief Enabling the driver registers exactly one logical address, Playback Device 1, with one
 *        addLogicalAddresses() call, and getLogicalAddress() reads it back through the HAL.
 * @pre Invocation B; SetUp's open() is the enable under test.
 */
TEST_F(DriverAidlSessionTest, EnablingTheDriverRegistersExactlyOnePlaybackAddress) {
    const ::android::sp<FakeHdmiCecController> controller = fake->getController();

    EXPECT_EQ(controller->getAllocationPolls(), std::vector<int32_t>({ 4 }))
        << "enabling did not poll Playback Device 1 and stop at the free address";
    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
        << "enabling did not call addLogicalAddresses exactly once";
    EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 4 }));
    EXPECT_TRUE(Driver::getInstance().isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)));

    const int32_t readsBefore = fake->getGetLogicalAddressesCallCount();
    EXPECT_EQ(Driver::getInstance().getLogicalAddress(DeviceType::TUNER), 4);
    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), readsBefore + 1)
        << "getLogicalAddress did not read the address through IHdmiCec::getLogicalAddresses";
}

/**
 * @brief LibCCEC::getLogicalAddress(), called as the Source plugin calls it, returns the address
 *        registered at enable, read through the HAL.
 * @pre Invocation B.
 */
TEST_F(DriverAidlSessionTest, LibCcecReturnsTheRegisteredAddressThroughTheHal) {
    const int32_t readsBefore = fake->getGetLogicalAddressesCallCount();

    EXPECT_EQ(LibCCEC::getInstance().getLogicalAddress(1), LogicalAddress::PLAYBACK_DEVICE_1);
    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), readsBefore + 1);
}

/**
 * @brief A re-open repeats the allocation and holds exactly one address, and an open while OPENED
 *        allocates nothing.
 * @pre Invocation B, with Playback Device 1 marked occupied before the re-open.
 */
TEST_F(DriverAidlSessionTest, ReopeningRepeatsTheRegistrationAndHoldsOneAddress) {
    const ::android::sp<FakeHdmiCecController> controller = fake->getController();

    ASSERT_NO_THROW({ Driver::getInstance().open(); });
    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1)
        << "an open() while OPENED allocated again";

    controller->setLogicalAddressOccupied(4, true);
    ASSERT_NO_THROW({ Driver::getInstance().close(); });
    ASSERT_NO_THROW({ Driver::getInstance().open(); });

    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 2);
    EXPECT_EQ(controller->getLastAddedLogicalAddresses(), std::vector<int32_t>({ 8 }));
    EXPECT_EQ(controller->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 8 }));
    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1)))
        << "the previous session's address is still held alongside the new one";
    EXPECT_EQ(Driver::getInstance().getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 8);
}

/**
 * @brief With every candidate occupied, enabling succeeds with no address registered, and
 *        LibCCEC reports that through its existing InvalidStateException.
 * @pre Invocation B, with Playback Devices 1 to 3 marked occupied before the re-open.
 */
TEST_F(DriverAidlSessionTest, NoFreeCandidateLeavesNoAddressAndLibCcecReportsInvalidState) {
    const ::android::sp<FakeHdmiCecController> controller = fake->getController();

    controller->setLogicalAddressOccupied(4, true);
    controller->setLogicalAddressOccupied(8, true);
    controller->setLogicalAddressOccupied(11, true);
    ASSERT_NO_THROW({ Driver::getInstance().close(); });
    ASSERT_NO_THROW({ Driver::getInstance().open(); }) << "allocation failure raised from open()";

    EXPECT_EQ(controller->getAddLogicalAddressesCallCount(), 1) << "an occupied address was added";
    EXPECT_TRUE(controller->getRegisteredLogicalAddresses().empty());
    EXPECT_EQ(Driver::getInstance().getLogicalAddress(DeviceType::PLAYBACK_DEVICE), 0);
    EXPECT_THROW({ LibCCEC::getInstance().getLogicalAddress(1); }, InvalidStateException);
}

/**
 * @brief Adding a different address releases the enable-time one before marshalling the new one
 *        as a one-element array, which is then held locally.
 * @pre Invocation B, with Playback Device 1 registered by SetUp's open().
 * @note Size and value are both asserted, so a padded vector cannot pass; the call order is read
 *       from the fake's own per-call log lines.
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressMarshalsExactlyOneElement) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_2);
    const int32_t addsBefore = fake->getController()->getAddLogicalAddressesCallCount();
    const int32_t removalsBefore = fake->getController()->getRemoveLogicalAddressesCallCount();
    bool added = false;
    bool captureWasValid = false;
    std::string logged;

    // The fake logs each call as it runs, so the captured order is the order the HAL saw them in.
    {
        StdoutCapture capture;
        captureWasValid = capture.isValid();
        added = Driver::getInstance().addLogicalAddress(address);
        logged = capture.read();
    }

    ASSERT_TRUE(captureWasValid) << "stdout could not be redirected, so the call order cannot be read";
    EXPECT_TRUE(added)
        << "addLogicalAddress reported failure although the fake accepts by default";

    EXPECT_EQ(fake->getController()->getAddLogicalAddressesCallCount(), addsBefore + 1)
        << "the HAL was not asked exactly once to add the address";
    EXPECT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), removalsBefore + 1)
        << "the HAL was not asked exactly once to release the enable-time address";
    EXPECT_EQ(fake->getController()->getLastRemovedLogicalAddresses(), std::vector<int32_t>({ 4 }))
        << "the enable-time address was not released before the new one was added";

    const size_t removal = logged.find("[FakeHdmiCecController::removeLogicalAddresses]");
    const size_t addition = logged.find("[FakeHdmiCecController::addLogicalAddresses]");
    ASSERT_NE(removal, std::string::npos) << "no release reached the HAL; captured: [" << logged << "]";
    ASSERT_NE(addition, std::string::npos) << "no add reached the HAL; captured: [" << logged << "]";
    EXPECT_LT(removal, addition)
        << "the new address was added before the enable-time one was released, so the HAL held two";

    const std::vector<int32_t> sent = fake->getController()->getLastAddedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "addLogicalAddresses received " << sent.size() << " entries for one middleware address. "
           "The array shape must stay confined to the adapter's temporary; a longer vector means "
           "multi-address behaviour leaked into the middleware";
    EXPECT_EQ(sent[0], address.toInt())
        << "the marshalled address does not match the one requested";

    EXPECT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was accepted by the HAL but not recorded locally, so isValidLogicalAddress "
           "would deny an address this device holds";
    EXPECT_EQ(fake->getController()->getRegisteredLogicalAddresses(), std::vector<int32_t>({ 8 }))
        << "the HAL holds more than one address for this device";
}

/**
 * @brief removeLogicalAddress marshals one element, and a HAL refusal raises nothing and still
 *        leaves the address removed locally.
 * @pre Invocation B, with an address added first.
 * @note Reproduces the legacy order: guard, local removal, then the HAL call whose result is
 *       discarded.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressMarshalsOneElementAndIgnoresHalRefusal) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address));

    // The HAL declines the removal, and a non-ok status is asserted separately below.
    fake->getController()->setRemoveLogicalAddressesResult(false);

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised when the HAL declined. The legacy back-end discards that "
           "return value and returns silently, so raising here would be an unregistered "
           "behaviour change - and one that Bus and LibCCEC do not expect";

    EXPECT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 1)
        << "the HAL was not asked exactly once to remove the address";

    const std::vector<int32_t> sent = fake->getController()->getLastRemovedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "removeLogicalAddresses received " << sent.size() << " entries for one middleware "
           "address";
    EXPECT_EQ(sent[0], address.toInt()) << "the marshalled address does not match the one released";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a declined removal. The local removal precedes "
           "the HAL call and is not rolled back, so it must be gone regardless of what the HAL "
           "reported - that ordering is the legacy behaviour being preserved";
}

/**
 * @brief On a HAL-accepted removal, exactly one call carries a one-element vector with the
 *        address, nothing is raised and the address is dropped locally.
 * @pre Invocation B, with the HAL result set explicitly rather than left to the fake's default.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressSucceedsMarshalsOneElementAndDropsItLocally) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was not held locally after a successful add, so the removal below would "
           "have nothing to remove and the local assertion would be vacuous";

    ASSERT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 0)
        << "the HAL was already asked to remove an address before this case asked it to, so the "
           "call count below would not describe this case's own call";

    // The HAL accepts the removal - the arm under test.
    fake->getController()->setRemoveLogicalAddressesResult(true);

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised on the success path. Nothing in the legacy shape raises "
           "here at all, and neither Bus nor LibCCEC guards this call";

    EXPECT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 1)
        << "the HAL was asked " << fake->getController()->getRemoveLogicalAddressesCallCount()
        << " times to remove one address. More than once means the adapter is retrying or looping "
           "over something; none means the local list was updated without telling the HAL, which "
           "leaves the middleware's view of this device's addresses and the HAL's permanently "
           "divergent";

    const std::vector<int32_t> sent = fake->getController()->getLastRemovedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "removeLogicalAddresses received " << sent.size()
        << " entries for one middleware address. The array shape must stay confined to the "
           "adapter's temporary - divergence 1 forbids multi-address state in the middleware";
    EXPECT_EQ(sent[0], address.toInt())
        << "the marshalled address does not match the one released, so some other address was "
           "removed at the HAL - which on a real device releases an address this device still "
           "believes it holds";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a successful removal, so isValidLogicalAddress "
           "would admit frames for an address the HAL no longer serves";
}

/**
 * @brief A reader that re-entered read() after its first read returned a delivered frame ends
 *        with InvalidStateException within the bound when close() then fails, on both failure arms.
 * @pre Invocation B. A local instance with its own queue is used, so the Bus reader does not
 *      compete for the sentinel.
 * @note The second read may begin before or after close(); the case asserts termination, not
 *       whether the sentinel or read()'s entry guard ended it.
 * @warning The driver and reader state are heap-held so an unreleasable reader can be abandoned
 *          rather than hang the run.
 */
TEST_F(DriverAidlSessionTest, AFailedCloseStillReleasesAReaderParkedOnTheIncomingQueue) {
    /**
     * @brief One of the two ways close() can report failure, driven identically.
     *
     * @note Tabulated rather than written out twice so the two arms cannot drift apart: they
     *       converge on one condition in production and each must release the reader alone.
     */
    struct Arm {
        const char *description;  /**< @brief Text for SCOPED_TRACE, so a failure names its arm. */
        bool reportNotClosed;     /**< @brief The HAL reports that the session was not closed. */
        bool failTransaction;     /**< @brief The close transaction itself fails. */
    };

    const Arm arms[] = {
        { "arm: the HAL reported that the session was not closed", true, false },
        { "arm: the close transaction itself failed", false, true },
    };

    for (size_t armIndex = 0; armIndex < (sizeof(arms) / sizeof(arms[0])); armIndex++) {
        const Arm &arm = arms[armIndex];
        SCOPED_TRACE(arm.description);

        // A clean slate for this arm, so a control left set by the previous one cannot decide
        // this one's outcome.
        fake->setCloseResult(true);
        fake->setCloseBinderStatus(::android::binder::Status::ok());

        std::shared_ptr<DriverAidlImpl> localDriver = std::make_shared<DriverAidlImpl>();
        std::shared_ptr<BlockedReaderState> reader = std::make_shared<BlockedReaderState>();

        ASSERT_TRUE(localDriver->isServiceAvailable())
            << "a local instance could not resolve the fake service, so it cannot open a session "
               "of its own and no reader can be parked on a queue this case controls";

        ASSERT_NO_THROW({ localDriver->open(); })
            << "the local instance could not open a session against the fake, so there is no queue "
               "to park a reader on";

        // Property 1: the session is live. If it were not, read() would return through its entry
        // guard and the release below would prove nothing.
        CECFrame livenessProbe = directedFrame();
        ASSERT_NO_THROW({ localDriver->write(livenessProbe); })
            << "the local session cannot transmit, so it is not really open and read()'s entry "
               "guard - not the close sentinel - would be what releases the reader";

        ASSERT_NE(fake->getListener(), nullptr)
            << "the local open did not hand a listener to the fake, so no frame can be delivered "
               "to the local queue and the reader cannot be proved parked";

        std::thread readerThread([localDriver, reader]() {
            CECFrame frame;

            try {
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->enteredRead = true;
                }
                reader->signal.notify_all();

                // First read: released by the frame the case delivers.
                localDriver->read(frame);
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->framesRead++;
                }
                reader->signal.notify_all();

                // Second read: re-entered with nothing queued, and ended by the failing close
                // through its sentinel or through read()'s entry guard.
                localDriver->read(frame);
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->framesRead++;
                }
            }
            catch (InvalidStateException &e) {
                (void)e;
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->releasedByInvalidState = true;
            }
            catch (...) {
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->releasedByOtherException = true;
            }

            {
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->finished = true;
            }
            reader->signal.notify_all();
        });

        // From here on every assertion is non-fatal: a fatal one would leave this scope with a
        // thread still running and nothing joined.
        EXPECT_TRUE(
            reader->waitFor(kFrameDeliveryTimeoutMs, [&reader]() { return reader->enteredRead; }))
            << "the reader thread never reached read(), so nothing was parked and the release "
               "asserted below would be meaningless";

        // Property 2: the first read completes on a delivered frame.
        const std::vector<uint8_t> wakeUpFrame{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x51, 0x00, 0x04 };
        EXPECT_TRUE(fake->fireOnMessageReceived(wakeUpFrame))
            << "the fake held no listener, so the frame that proves the reader is parked could not "
               "be delivered";

        EXPECT_TRUE(reader->waitFor(kFrameDeliveryTimeoutMs,
                                    [&reader]() { return reader->framesRead >= 1; }))
            << "the frame delivered to the local session never came back out of read(), so the "
               "reader is not parked on THIS driver's queue and the rest of this case cannot "
               "establish anything about the sentinel";

        // Now the failing close, with the queue empty and the reader past its first read.
        if (arm.reportNotClosed) {
            fake->setCloseResult(false);
        }
        if (arm.failTransaction) {
            fake->setCloseBinderStatus(
                ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        }

        EXPECT_THROW({ localDriver->close(); }, IOException)
            << "the failing close did not raise IOException, so this arm is not the failure path "
               "it is meant to exercise";

        const bool releasedInTime =
            reader->waitFor(kFrameDeliveryTimeoutMs, [&reader]() { return reader->finished; });

        EXPECT_TRUE(releasedInTime)
            << "the reader parked on the incoming queue was not released by a close that failed, "
               "within "
            << kFrameDeliveryTimeoutMs
            << " ms, while the driver was still alive and in scope. The NULL sentinel must be "
               "offered before the close transaction and its result are evaluated: on this path "
               "the result check raises, so an offer placed after it never happens and the reader "
               "parks forever. Every other assertion about close() - the CLOSED state, the "
               "IOException, the retained address list, the call counts - still passes when that "
               "happens, which is precisely why this case exists";

        EXPECT_TRUE(reader->releasedByInvalidState)
            << "the reader was released, but not by InvalidStateException. The sentinel plus a "
               "state that is no longer OPENED is what produces that exception, and it is the "
               "release Bus::Reader::run() is written around - it catches InvalidStateException "
               "and re-arms. Some other exception means the reader was woken by something else";

        EXPECT_FALSE(reader->releasedByOtherException)
            << "read() raised an exception that is neither InvalidStateException nor nothing at "
               "all, which would propagate out of Bus::Reader::run() and end the reader thread";

        if (!releasedInTime) {
            // The failure is already reported. This only stops a genuine regression from hanging
            // the entire run: re-open and close cleanly, which offers a fresh sentinel.
            fake->setCloseResult(true);
            fake->setCloseBinderStatus(::android::binder::Status::ok());

            try {
                localDriver->open();
                localDriver->close();
            }
            catch (...) {
                // Nothing to add: the assertion above has already reported the defect, and this
                // rescue is best-effort by construction.
            }

            if (!reader->waitFor(kFrameDeliveryTimeoutMs,
                                 [&reader]() { return reader->finished; })) {
                // Still parked: abandon the thread rather than hang; it shares ownership of the
                // driver and its state, so nothing it touches is freed underneath it.
                readerThread.detach();
                ADD_FAILURE() << "the reader could not be released even by a clean close, so the "
                                 "thread was abandoned to keep the run from hanging. This process "
                                 "now holds one parked thread and one driver that cannot be "
                                 "collected; treat every later result in this binary as suspect";
                continue;
            }
        }

        readerThread.join();

        fake->setCloseResult(true);
        fake->setCloseBinderStatus(::android::binder::Status::ok());
    }
}

/**
 * @brief getLogicalAddress reports entry zero of the result IHdmiCec::getLogicalAddresses()
 *        returns, asking the service interface rather than the controller.
 * @pre Invocation B, with the fake's address result holding exactly one address.
 */
TEST_F(DriverAidlSessionTest, GetLogicalAddressReadsEntryZeroFromTheServiceInterface) {
    const int32_t halAddress = 8;
    fake->setLogicalAddressesResult(std::vector<int32_t>{ halAddress });

    const int reported = Driver::getInstance().getLogicalAddress(DeviceType::TUNER);

    EXPECT_EQ(reported, static_cast<int>(halAddress))
        << "getLogicalAddress did not report the single address the HAL holds";

    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), 1)
        << "IHdmiCec::getLogicalAddresses was not called exactly once. It lives on the service "
           "interface, not on the controller, and a call routed to the controller instead would "
           "not compile against this snapshot - so a zero here means the address came from "
           "somewhere else entirely";
}

/**
 * @brief A multi-address HAL result operates on entry zero and is logged with the count and the
 *        entry used.
 * @pre Invocation B, with the fake's address result holding more than one address.
 * @note The expected log text is composed from the stimulus vector and matched whole, because
 *       every log line's timestamp would satisfy a bare-digit search.
 */
TEST_F(DriverAidlSessionTest, MultipleReturnedAddressesUseEntryZeroAndAreLogged) {
    const std::vector<int32_t> halAddresses{ 4, 8, 11 };
    fake->setLogicalAddressesResult(halAddresses);

    /* Transcribed from DriverAidlImpl::getLogicalAddress()'s inline format string with its two
     * substitutions; the terminator is omitted because the line is matched as a substring. */
    const std::string expectedMultiAddressReport =
        std::string("DriverAidlImpl::getLogicalAddress : the HAL reports ")
        + std::to_string(halAddresses.size()) + " logical addresses; operating on entry 0 ["
        + std::to_string(static_cast<int>(halAddresses[0])) + "]";

    int reported = 0;
    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be redirected, so the log half of this "
                                         "case cannot be established";

        reported = Driver::getInstance().getLogicalAddress(DeviceType::TV);

        captured = capture.read();
    }

    EXPECT_EQ(reported, static_cast<int>(halAddresses[0]))
        << "getLogicalAddress did not report entry 0 of a multi-entry result. Operating on the "
           "first entry is the specified behaviour; picking any other entry, or iterating, would "
           "introduce multi-address behaviour the middleware has no way to represent";

    // One contiguous match covers both the count and the entry used, each next to its label;
    // loose fragment searches could each be satisfied by text the report did not produce.
    EXPECT_NE(captured.find(expectedMultiAddressReport), std::string::npos)
        << "the multi-address report is missing or no longer names the count and the entry used. "
           "Expected to find: [" << expectedMultiAddressReport
        << "]. Without it an integrator has no signal that the HAL holds more addresses than the "
           "middleware can represent, and a first-entry pick that hides a real platform "
           "misconfiguration is indistinguishable from a single-address HAL. Captured: ["
        << captured << "]";
}

/**
 * @brief An empty HAL result reports 0, the documented "no address" outcome.
 * @pre Invocation B, with the fake holding no addresses.
 * @note LibCCEC turns a zero into InvalidStateException, the signal callers already handle; any
 *       other sentinel would suppress that throw.
 */
TEST_F(DriverAidlSessionTest, EmptyAddressResultReportsZeroSoTheExistingCallerSignalSurvives) {
    fake->setLogicalAddressesResult(std::vector<int32_t>());

    EXPECT_EQ(Driver::getInstance().getLogicalAddress(DeviceType::TV), 0)
        << "an empty HAL result did not report 0. Zero is what LibCCEC::getLogicalAddress turns "
           "into InvalidStateException, so any other value would report an address this device "
           "does not have";

    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), 1)
        << "the HAL was not asked exactly once";
}

/**
 * @brief A HAL refusal raises AddressNotAvailableException and a transport failure raises
 *        IOException, and neither records the address locally.
 * @pre Invocation B. Playback Device 2 is used because Playback Device 1 is already registered.
 * @note The AIDL call returns one boolean, so the legacy three-way status split cannot be
 *       carried; a refusal maps to the nearer legacy category.
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressMapsRefusalAndTransportFailureToDistinctExceptions) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_2);

    // The HAL refuses the address: it is out of range, or already taken.
    fake->getController()->setAddLogicalAddressesResult(false);

    EXPECT_THROW({ Driver::getInstance().addLogicalAddress(address); }, AddressNotAvailableException)
        << "a refused address did not raise AddressNotAvailableException. That is the nearer of "
           "the two legacy categories for 'the address is not available', and it is what the Sink "
           "plugin's allocation loop is written around";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was recorded locally although the HAL refused it, so the middleware's "
           "bookkeeping and the HAL's have diverged";

    fake->getController()->reset();

    // Transport failure: a dead binder, an EX_ILLEGAL_STATE, a marshalling fault. Distinct
    // from a refusal, and mapped distinctly.
    fake->getController()->setAddLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().addLogicalAddress(address); }, IOException)
        << "a non-ok binder status did not raise IOException. A transport failure is not an "
           "unavailable address, and the Sink plugin's call path distinguishes the two";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was recorded locally although the transaction failed";
}

/**
 * @brief Through LibCCEC, a refusal reaches the Sink's generic catch arm, a transport failure its
 *        IOException arm, and a refusal propagates uncaught on the enable-time path.
 * @pre Invocation B. Playback Device 2 is used because Playback Device 1 is already registered.
 * @note Both Sink call paths are modelled: the three-catch try block and the enable-time call
 *       outside any try.
 * @see DriverAidlLocalInstanceTest::TheModelledSinkCallPathsStillMatchTheRealSinkSource
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_2);

    // Path 1, refusal: the Sink's three-catch shape. A refusal is not an IOException, so it must
    // fall through to the generic arm.
    fake->getController()->setAddLogicalAddressesResult(false);
    {
        bool reachedInvalidStateArm = false;
        bool reachedIoArm = false;
        bool reachedGenericArm = false;

        try {
            LibCCEC::getInstance().addLogicalAddress(address);
        }
        catch (InvalidStateException &e) {
            (void)e;
            reachedInvalidStateArm = true;
        }
        catch (IOException &e) {
            (void)e;
            reachedIoArm = true;
        }
        catch (...) {
            reachedGenericArm = true;
        }

        EXPECT_FALSE(reachedInvalidStateArm)
            << "a refused address reached the InvalidStateException arm, which the Sink treats as "
               "'the library is not initialized' - a different diagnosis entirely";
        EXPECT_FALSE(reachedIoArm)
            << "a refused address reached the IOException arm at "
               "HdmiCecSinkImplementation.cpp:2793, so a full address pool would be logged and "
               "diagnosed as a transport failure";
        EXPECT_TRUE(reachedGenericArm)
            << "a refused address reached no catch arm on path 1, so nothing was raised at all and "
               "the Sink would carry on believing it holds an address the HAL refused";
    }

    fake->getController()->reset();

    // Path 1, transport failure: this one must reach the distinct IOException arm.
    fake->getController()->setAddLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    {
        bool reachedIoArm = false;
        bool reachedGenericArm = false;

        try {
            LibCCEC::getInstance().addLogicalAddress(address);
        }
        catch (IOException &e) {
            (void)e;
            reachedIoArm = true;
        }
        catch (...) {
            reachedGenericArm = true;
        }

        EXPECT_TRUE(reachedIoArm)
            << "a transport failure did not reach the IOException arm at "
               "HdmiCecSinkImplementation.cpp:2793, so the one failure the Sink diagnoses "
               "specifically would be logged as a generic exception instead";
        EXPECT_FALSE(reachedGenericArm) << "the transport failure reached the generic arm instead";
    }

    fake->getController()->reset();

    // Path 2: the enable-time call sits outside any try block, so the refusal must propagate
    // out of LibCCEC unabsorbed.
    fake->getController()->setAddLogicalAddressesResult(false);
    EXPECT_THROW({ LibCCEC::getInstance().addLogicalAddress(address); },
                 AddressNotAvailableException)
        << "the refusal did not propagate out of LibCCEC::addLogicalAddress. The enable-time Sink "
           "call at HdmiCecSinkImplementation.cpp:3065 is not inside a try block, so an exception "
           "absorbed in the middleware would leave that path believing it acquired an address";
}

/**
 * @brief A failed close raises IOException after reaching CLOSED, names the controller open()
 *        returned, and keeps the local address list.
 * @pre Invocation B, with an address held locally so the kept-list check is not vacuous.
 * @note The kept list matches the legacy close, which does not clear it either.
 */
TEST_F(DriverAidlSessionTest, FailedCloseStillReachesTheClosedStateAndKeepsTheLocalAddressList) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address));

    // The HAL reports that it did not close the session.
    fake->setCloseResult(false);

    EXPECT_THROW({ Driver::getInstance().close(); }, IOException)
        << "a close the HAL reported as failed did not raise IOException";

    EXPECT_EQ(fake->getCloseCallCount(), 1) << "the HAL was not asked exactly once to close";

    // The closed controller must be the opened one, on this failure path too; a wrong one would
    // leave the real session open HAL-side.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for this close, so close() passed nothing "
               "identifiable and the HAL cannot know which session to end";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the close named a different controller from the one open() returned, on the arm "
               "where the HAL reports the session was not closed";
    }

    // CLOSED was reached before the raise, observed through a guarded operation.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver still behaves as though it were open after a failed close. The state must "
           "reach CLOSED before the exception is raised, or a caller that swallows the IOException "
           "is left with an object that believes it holds a session it does not";

    EXPECT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "close() cleared the local logical-address list. The legacy back-end does not clear it "
           "either, so clearing here would be an unauthorized improvement and a fourth observable "
           "difference between the two back-ends";

    // A second close returns silently, as the legacy back-end does with its throw compiled out.
    EXPECT_NO_THROW({ Driver::getInstance().close(); })
        << "a close on an already-closed driver raised; the legacy back-end returns silently";
    EXPECT_EQ(fake->getCloseCallCount(), 1)
        << "the already-closed driver reached the HAL again, so the state guard is not short-"
           "circuiting the second close";
}

/**
 * @brief A close whose transaction fails also raises IOException after reaching CLOSED, and
 *        names the controller open() returned.
 * @pre Invocation B, with a failing close transaction installed on the fake.
 * @note Both failure arms converge on one condition, so each is covered to catch a short-circuit
 *       that skips the state assignment on this arm.
 */
TEST_F(DriverAidlSessionTest, CloseWithNonOkBinderStatusAlsoReachesTheClosedState) {
    fake->setCloseBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().close(); }, IOException)
        << "a close whose transaction failed did not raise IOException";

    // The fake captures the controller before applying the status, so identity holds here too.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for a close whose transaction failed, so close() "
               "passed nothing identifiable";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the close named a different controller from the one open() returned, on the arm "
               "where the transaction itself failed";
    }

    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver still behaves as though it were open after a close whose transaction "
           "failed, although the false-result arm reaches CLOSED correctly. The two arms converge "
           "on one condition, so this is a short-circuit that skips the state assignment";
}

/**
 * @brief A frame received while the driver is open reaches an application listener exactly once,
 *        byte for byte.
 * @pre Invocation B, with the listener captured by SetUp's session cycle.
 * @note Delivery is asserted rather than the trigger's result, which only reports a captured
 *       listener; delivery on a real binder thread is covered by invocation E.
 */
TEST_F(DriverAidlSessionTest, ReceivedMessageIsAcceptedWhileTheDriverIsOpen) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-open",
                                            applicationListener);

    // A well-formed broadcast Report Physical Address, with a distinctive address so that the
    // frame the listener sees cannot be confused with any other frame in this process.
    const std::vector<uint8_t> message{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so the middleware did not hand "
           "one to IHdmiCec::open() and no callback can ever arrive";

    ASSERT_TRUE(fake->fireOnMessageReceived(message))
        << "no listener was captured, so the trigger was a silent no-op and this case would prove "
           "nothing. SetUp cycles the session precisely so that open() re-captures it";

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the frame the HAL delivered never reached an application listener within "
        << kFrameDeliveryTimeoutMs
        << " ms, although the trigger reached the middleware's callback. Every step between is "
           "load-bearing: the callback must copy the bytes into a fresh CECFrame and offer it "
           "through the state-guarded incoming-queue accessor, and the Bus reader must be blocked "
           "on that queue rather than on anything else. A callback that logs and returns, or that "
           "offers onto a queue nothing drains, passes every weaker assertion and breaks the "
           "entire receive path";

    const std::vector<uint8_t> delivered = applicationListener.frameAt(0);

    EXPECT_EQ(delivered, message)
        << "the delivered frame is not the frame that was sent, so the byte-array marshalling is "
           "wrong - a truncation, an off-by-one on the length, or a stale buffer. Delivered "
        << delivered.size() << " bytes against " << message.size() << " sent";

    EXPECT_EQ(applicationListener.frameCount(), 1u)
        << "one HAL callback produced " << applicationListener.frameCount()
        << " deliveries, so the frame is being offered more than once or the queue is replaying "
           "it";
}

/**
 * @brief A zero-byte message from the HAL is discarded before allocation, and the receive path
 *        keeps delivering afterwards.
 * @pre Invocation B.
 * @note A queued empty CECFrame would raise std::out_of_range on the Bus reader thread, where
 *       nothing catches it; the trailing well-formed frame is the positive control.
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
TEST_F(DriverAidlSessionTest, AZeroByteMessageFromTheHalIsDiscardedBeforeAllocation) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-too-short",
                                            applicationListener);

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive and "
           "this case would prove nothing";

    // The empty payload; a true return only says the callback ran, not what it did.
    ASSERT_TRUE(fake->fireOnMessageReceived(std::vector<uint8_t>()))
        << "no listener was captured, so the trigger was a silent no-op";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a zero-byte message reached an application listener. It was therefore allocated as a "
           "CECFrame and queued, and the Bus reader drained it - which means the next consumer to "
           "read byte zero of an empty frame raises std::out_of_range on the reader thread, where "
           "nothing catches it. The guard has to discard ahead of the allocation, not after it";

    EXPECT_EQ(applicationListener.frameCount(), 0u)
        << "the listener holds " << applicationListener.frameCount()
        << " frame(s) after an empty delivery, where none was expected";

    // The positive control: the same chain, one byte longer, must deliver. This is what makes the
    // non-delivery above evidence about the guard rather than about a broken receive path.
    const std::vector<uint8_t> wellFormed{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    ASSERT_TRUE(fake->fireOnMessageReceived(wellFormed))
        << "the positive control could not be triggered, so the discard above is unattributable";

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "a well-formed frame delivered immediately after the empty one never arrived, so the "
           "receive path was dead for the whole case and the non-delivery above says nothing "
           "about the length guard";

    EXPECT_EQ(applicationListener.frameAt(0), wellFormed)
        << "the frame that did arrive is not the well-formed one, so the empty payload disturbed "
           "the marshalling of the frame after it";
}

/**
 * @brief An over-length message from the HAL is contained: nothing escapes the callback, no
 *        truncated frame is delivered, and the receive path keeps working.
 * @pre Invocation B. Two sizes derived from CECFrame::MAX_LENGTH, each under its own SCOPED_TRACE.
 * @note append() copies MAX_LENGTH bytes before raising, so the non-delivery checks rule out a
 *       truncated frame; each size ends with a positive control.
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
TEST_F(DriverAidlSessionTest, AnOverLengthMessageFromTheHalIsDiscardedWithoutEscaping) {
    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive and "
           "this case would prove nothing";

    // One byte past the capacity is the boundary; 32 times it is an arbitrarily large delivery.
    constexpr size_t frameCapacity = static_cast<size_t>(CECFrame::MAX_LENGTH);
    const size_t overLength[] = { frameCapacity + 1, frameCapacity * 32 };

    // The positive control, and the prefix of each over-length payload, so the two differ only in
    // length.
    const std::vector<uint8_t> wellFormed{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    for (size_t index = 0; index < sizeof(overLength) / sizeof(overLength[0]); index++) {
        const size_t size = overLength[index];

        SCOPED_TRACE("inbound message of " + std::to_string(size) + " bytes, against a CECFrame "
                     "capacity of " + std::to_string(frameCapacity) + " bytes");

        // A fresh listener and connection per size, so each size's counts start from zero and a
        // frame delivered under one size can never be read as a frame delivered under the other.
        RecordingFrameListener applicationListener;
        ListeningConnection listeningConnection(
            LogicalAddress(LogicalAddress::UNREGISTERED),
            "DriverAidlSessionTest-receive-too-long-" + std::to_string(size),
            applicationListener);

        // The control's bytes padded to the target size, so a truncated delivery is detectable.
        std::vector<uint8_t> oversized(wellFormed);
        oversized.resize(size, 0x00);

        bool triggered = false;
        EXPECT_NO_THROW({ triggered = fake->fireOnMessageReceived(oversized); })
            << "an exception escaped the middleware's oneway callback and reached the HAL's "
               "delivery. The fake invokes the listener directly and catches nothing, and this "
               "invocation's fake is in-process, so what surfaced here is what would escape into "
               "onTransact on a real transport - and it is exactly how the legacy back-end loses "
               "the process on this same input. The allocation and the append must both stay "
               "inside the try, and every catch arm must release the frame and return ok";

        if (!triggered) {
            ADD_FAILURE()
                << "the over-length delivery never reached the middleware's callback: either no "
                   "listener was captured, or the callback threw and the assertion above has "
                   "already said so. Either way nothing below would be attributable to the length "
                   "guard, so this size is unmeasured rather than passing";
            continue;
        }

        EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
            << "a message too long for a CECFrame reached an application listener. append() copies "
               "the first "
            << frameCapacity
            << " bytes before it raises, so what arrived is the TRUNCATED remains of the message - "
               "indistinguishable, to every consumer above the queue, from a frame the HAL really "
               "sent, and readable by a peer as a different message entirely. The frame must be "
               "released on the catch arm and never offered onto the queue";

        EXPECT_EQ(applicationListener.frameCount(), 0u)
            << "the listener holds " << applicationListener.frameCount()
            << " frame(s) after an over-length delivery, where none was expected - so a frame "
               "surfaced once the non-delivery window had closed rather than not at all";

        // The positive control: the same chain, a length it can hold, must deliver.
        bool controlTriggered = false;
        EXPECT_NO_THROW({ controlTriggered = fake->fireOnMessageReceived(wellFormed); })
            << "the positive control itself raised, so the over-length payload left the callback "
               "or the captured listener in a state the next delivery could not survive";

        if (!controlTriggered) {
            ADD_FAILURE()
                << "the positive control could not be triggered, so the non-delivery above is "
                   "unattributable: a receive path that was dead for this whole size reads exactly "
                   "like correct containment";
            continue;
        }

        EXPECT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
            << "a well-formed frame delivered immediately after the over-length one never arrived "
               "within "
            << kFrameDeliveryTimeoutMs
            << " ms, so the receive path was dead for this size and the non-delivery above says "
               "nothing about the length guard. The chain has to survive a std::out_of_range "
               "caught mid-append: a frame released on the catch arm must leave the owner lock, "
               "the incoming queue and the Bus reader exactly as they were";

        EXPECT_EQ(applicationListener.frameAt(0), wellFormed)
            << "the frame that did arrive is not the well-formed one, so the over-length payload "
               "was not discarded whole - its truncated remains were delivered ahead of the "
               "control, or they disturbed the marshalling of the frame after it";

        EXPECT_EQ(applicationListener.frameCount(), 1u)
            << "the listener holds " << applicationListener.frameCount()
            << " frames after one over-length delivery and one well-formed one, so the over-length "
               "payload was delivered too - late rather than never, which is the same defect with "
               "a delay in front of it";
    }
}

/**
 * @brief A frame offered while the incoming queue is at its refusal point is released rather than
 *        leaked, and delivery recovers once the reader moves again.
 * @pre Invocation B. A blocking listener parks the Bus reader so the queue can fill.
 * @note Asserted on the middleware's refusal log line, the release's only visible trace; the
 *       reader is released before any assertion so a fatal one cannot strand it.
 * @see DriverAidlImpl::offerReceivedFrame()
 */
TEST_F(DriverAidlSessionTest, AFrameArrivingWhileTheQueueIsFullIsReleasedRatherThanLeaked) {
    StallingFrameListener stallingListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-queue-full",
                                            stallingListener);

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive";

    const std::vector<uint8_t> filler{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    // One frame, to get the reader out of poll() and into the notification where it will stay.
    ASSERT_TRUE(fake->fireOnMessageReceived(filler))
        << "no listener was captured, so the trigger was a silent no-op";

    if (!stallingListener.waitUntilParked(kFrameDeliveryTimeoutMs)) {
        stallingListener.release();

        FAIL() << "the Bus reader never reached the listener within " << kFrameDeliveryTimeoutMs
               << " ms, so it was never parked and the queue could not have filled. That is a "
                  "broken receive path rather than a queue that refused nothing, and the two must "
                  "not share a failure message";
    }

    // Comfortably past the refusal point, whatever the queue's capacity is. Every
    // delivery from here is offered onto a queue nothing is draining.
    const size_t deliveriesWhileParked = 64;
    size_t triggered = 0;
    std::string logged;
    bool captureWasValid = false;

    {
        StdoutCapture capture;

        // Sampled before read(), which restores fd 1 and leaves the capture reporting invalid.
        captureWasValid = capture.isValid();

        for (size_t index = 0; index < deliveriesWhileParked; index++) {
            if (fake->fireOnMessageReceived(filler)) {
                triggered++;
            }
        }

        // Safe when the redirection failed: read() returns an empty string rather than raising,
        // and the assertion on captureWasValid below is what reports that case.
        logged = capture.read();
    }

    // Released before ANY assertion below, so a fatal one cannot leave the Bus reader parked
    // inside the listener and hang this fixture's teardown.
    stallingListener.release();

    ASSERT_TRUE(captureWasValid)
        << "stdout could not be redirected, so the middleware's own refusal line - the only "
           "externally visible trace a released allocation leaves - could not be read";

    EXPECT_EQ(triggered, deliveriesWhileParked)
        << "only " << triggered << " of " << deliveriesWhileParked
        << " deliveries reached the middleware's callback, so the listener was lost part way "
           "through and the queue was not driven to its refusal point";

    // The line must name both the condition (refused) and the disposition (released), since an
    // unreleased refused frame is a leak per event.
    EXPECT_THAT(logged, ::testing::HasSubstr("refused the frame at its"))
        << "none of " << deliveriesWhileParked
        << " frames delivered onto a queue nothing was draining was refused. Either the queue "
           "accepted every one of them - in which case EventQueue::offer() discarded the "
           "surplus silently and each discarded frame is a leak - or the refusal happened and "
           "went unreported, which is the same defect from a log reader's point of view";

    EXPECT_THAT(logged, ::testing::HasSubstr("rather than leaking it"))
        << "the queue reported a refusal but the line does not say the frame was released, so "
           "either the callback kept a frame the queue never took - one leaked heap CECFrame per "
           "refused event, without bound while the reader stays behind - or the disposition went "
           "unrecorded, and a reader has no way to tell those two apart";

    // The chain recovers. A refusal that also broke the receive path would be a different defect
    // from a refusal that did not, and this is what distinguishes them.
    RecordingFrameListener recoveredListener;
    ListeningConnection recoveredConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-queue-recovered",
                                            recoveredListener);

    ASSERT_TRUE(fake->fireOnMessageReceived(filler))
        << "the recovery delivery could not be triggered";

    EXPECT_TRUE(recoveredListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "no frame arrived after the reader was released, so the refusal left the receive path "
           "wedged rather than merely dropping the frames it could not take";
}

/**
 * @brief A synchronous AIDL call slower than the slow-call threshold is logged as slow, and its
 *        result is unchanged.
 * @pre Invocation B, with a session open holding address 4. Costs real wall-clock time, just past
 *      the threshold.
 * @note Adds PLAYBACK_DEVICE_2 so the timed call is a real HAL add rather than the same-address
 *       no-op; the elapsed figure is not asserted because it is a real measurement.
 * @see DriverAidlImpl::addLogicalAddress()
 * @see FakeHdmiCecController::setAddLogicalAddressesDelayMs()
 */
TEST_F(DriverAidlSessionTest, ASynchronousCallPastTheSlowThresholdIsReportedWithoutChangingItsResult) {
    ::android::sp<FakeHdmiCecController> controllerFake = fake->getController();

    ASSERT_NE(controllerFake, nullptr)
        << "the controller fake is not reachable, so no delay can be installed and this case "
           "cannot make a call slow";

    // Just past the threshold restated in kSlowHalCallWarnMs; a raised production threshold
    // makes the delay fall short and the assertion below fail loudly.
    const int32_t delayMs = static_cast<int32_t>(kSlowHalCallWarnMs) + 150;

    controllerFake->setAddLogicalAddressesDelayMs(delayMs);
    controllerFake->setAddLogicalAddressesResult(true);

    std::string logged;
    int addResult = -1;
    bool captureWasValid = false;

    {
        StdoutCapture capture;

        // Sampled before read(), which restores fd 1 and invalidates the capture.
        captureWasValid = capture.isValid();

        addResult = Driver::getInstance().addLogicalAddress(
            LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_2));

        logged = capture.read();
    }

    // Cleared immediately, so no later case in this fixture inherits the delay.
    controllerFake->setAddLogicalAddressesDelayMs(0);

    ASSERT_TRUE(captureWasValid)
        << "stdout could not be redirected, so the diagnostic line could not be read";

    EXPECT_EQ(addResult, 1)
        << "a call that crossed the slow-call threshold returned " << addResult
        << " where success was expected. The diagnostic is a threshold and not a timeout: it must "
           "leave the outcome, the status translation and the return value entirely alone";

    EXPECT_THAT(logged, ::testing::HasSubstr("past the"))
        << "a synchronous AIDL call that took " << delayMs << " ms, past the "
        << kSlowHalCallWarnMs
        << " ms threshold, produced no diagnostic line. That line is the whole of the in-scope "
           "mitigation for a call the pinned libbinder cannot bound, so its absence means a "
           "stalling HAL leaves no trace at all";

    EXPECT_THAT(logged, ::testing::HasSubstr("NO DEADLINE WAS ENFORCED"))
        << "the diagnostic line does not state that nothing was enforced, so a log reader could "
           "mistake it for evidence of a mitigation that abandoned or bounded the call";

    EXPECT_THAT(logged, ::testing::HasSubstr("addLogicalAddresses"))
        << "the diagnostic line does not name the operation that was slow, so it cannot be acted "
           "on: a reader learns that something stalled but not what";
}

/**
 * @brief A frame arriving while the driver is closed is refused and released, is not delivered,
 *        and does not surface after a re-open.
 * @pre Invocation B. The release is observed in the middleware's log, its only visible trace.
 * @note The re-open check separates "released" from "quietly queued", and its positive control
 *       rules out a dead delivery chain.
 */
TEST_F(DriverAidlSessionTest, MessageArrivingWhileClosedIsRejectedAndReleased) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-closed",
                                            applicationListener);

    ASSERT_NO_THROW({ Driver::getInstance().close(); })
        << "the driver could not be closed, so the rejecting-queue precondition does not hold";

    // A distinctive frame, distinct from the open-state case's, so that a delivery seen here
    // cannot be a frame left over from anywhere else.
    const std::vector<uint8_t> message{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x31, 0x00, 0x04 };

    // The fake still holds the listener after close(), modelling a HAL that calls back after the
    // session went down.
    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake no longer holds a listener after close(), so this trigger cannot reach the "
           "adapter and the rejection path would not be exercised at all";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be captured, so the release of the "
                                         "rejected frame cannot be observed";

        EXPECT_TRUE(fake->fireOnMessageReceived(message))
            << "the trigger did not reach the listener, so the rejection path was never exercised";

        captured = capture.read();
    }

    // Either logged disposition is correct (guard raised and frame released, or detached listener
    // dropped it); silence would mean the frame was queued.
    const bool releasedAfterGuardRaised =
        captured.find("Exception during frame offer...discarding") != std::string::npos;
    const bool droppedBecauseDetached =
        captured.find("message received after detach, dropping it") != std::string::npos;

    EXPECT_TRUE(releasedAfterGuardRaised || droppedBecauseDetached)
        << "the middleware logged neither of the two dispositions a frame arriving on a "
           "non-OPENED driver may have - the state-guarded accessor raising and the listener "
           "releasing the allocation, or the listener finding itself detached and dropping the "
           "message. Silence here means the frame was ACCEPTED while the driver was closed, which "
           "is both a divergence from the legacy path and a frame queued where nothing will drain "
           "it. Captured output was: [" << captured << "]";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a frame that arrived while the driver was CLOSED was delivered to an application "
           "listener. The state guard exists so that a late HAL callback cannot reach an "
           "application that has torn its session down";

    // Now the strong half: re-arm the driver and give the Bus reader a generous window. If the
    // frame had been queued rather than released, this is where it would appear.
    ASSERT_NO_THROW({ Driver::getInstance().open(); })
        << "the driver could not be re-opened, so the deferred-delivery half of this case cannot "
           "be established";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the frame that arrived while the driver was closed was delivered after the re-open, so "
           "it was queued rather than released. Every closed-state assertion above still passed, "
           "which is exactly why this one is here: the frame was merely deferred, and it reached "
           "the application on a session that had nothing to do with it";

    EXPECT_EQ(applicationListener.frameCount(), 0u)
        << "the application listener received " << applicationListener.frameCount()
        << " frames across the closed window and the re-open, and it must receive none";

    // Positive control: the re-opened session must still deliver.
    const std::vector<uint8_t> afterReopen{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x41, 0x00, 0x04 };
    ASSERT_TRUE(fake->fireOnMessageReceived(afterReopen))
        << "no listener was captured by the re-open, so the positive control cannot run";

    EXPECT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the re-opened session did not deliver either, so the delivery chain was dead "
           "throughout and the non-delivery assertions above prove nothing about the state guard. "
           "This control is what distinguishes 'the frame was correctly rejected' from 'no frame "
           "could have been delivered anyway'";

    EXPECT_EQ(applicationListener.frameAt(0), afterReopen)
        << "the frame delivered after the re-open is not the one that was sent then, so the "
           "closed-window frame surfaced after all";

    // TearDown restores the opened baseline; nothing here is left closed for a sibling suite.
}

/**
 * @brief The state-change and message-sent callbacks log what they were told and do nothing
 *        else: no frame is delivered and the session still transmits.
 * @pre Invocation B. The message-sent line is DEBUG-level, so the level is raised with
 *      ScopedCecLogLevel and its restoration asserted.
 * @note Asserted from captured output: both state names, and the sent message's status, length
 *       and bytes.
 */
TEST_F(DriverAidlSessionTest, DiagnosticCallbacksAreReportedWithoutDisturbingTheSession) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-diagnostics",
                                            applicationListener);

    const std::vector<uint8_t> sentMessage{ 0x40, GIVE_DEVICE_POWER_STATUS, 0x5A, 0xA5 };

    std::string captured;
    bool levelWasRaised = false;
    std::string levelRefusal;
    {
        // Raised before the capture opens, so the guard's output stays out of it; its verdict
        // and reason are copied while the guard is alive.
        ScopedCecLogLevel debugLevel("DEBUG");
        levelWasRaised = debugLevel.isRaised();
        levelRefusal = debugLevel.failureReason();

        // The capture closes before restoreAndVerify(), whose level probe logs and would
        // otherwise land in the captured text.
        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be captured, so what the two diagnostic callbacks report "
                   "cannot be observed and this case would degenerate into 'the session still "
                   "works'";

            EXPECT_TRUE(fake->fireOnStateChanged(cechal::State::STARTED, cechal::State::CLOSED))
                << "the state-change trigger did not reach the listener";

            EXPECT_TRUE(fake->fireOnMessageSent(sentMessage,
                                                cechal::SendMessageStatus::ACK_STATE_0))
                << "the message-sent trigger did not reach the listener";

            captured = capture.read();
        }

        // Restoration is asserted here, under the custody lock; the destructor is only a
        // backstop and cannot fail a test.
        std::string restoreDetail;
        ASSERT_TRUE(debugLevel.restoreAndVerify(restoreDetail)) << restoreDetail;
    }

    ASSERT_TRUE(levelWasRaised)
        << "the middleware log level could not be raised to DEBUG, so onMessageSent's report is "
           "suppressed and this case cannot assert it. The level is raised through production's "
           "own check_cec_log_status(), which reads /tmp/cec_log_enabled, and the guard refuses "
           "rather than forces whenever that path is not safely this run's to modify - so the "
           "refusal below distinguishes an ENVIRONMENT FAULT (the path is not writable, the "
           "directory is gone) from a CUSTODY REFUSAL (another run_L1Tests holds the lock, the "
           "path is a symlink or is owned by another user, a concurrent writer replaced it). "
           "Neither is a defect in the listener, and both are asserted rather than reported so "
           "that missing evidence cannot pass as evidence obtained. Reported reason: ["
        << levelRefusal << "]";

    // Unconditional: the state transition is reported, with both state names.
    EXPECT_NE(captured.find("HAL state changed from"), std::string::npos)
        << "onStateChanged reported nothing at all, although it logs at LOG_INFO and the default "
           "level prints LOG_INFO. Reporting the transition is this callback's whole behaviour, so "
           "silence here means an empty body - and the HAL's own view of its state would then be "
           "invisible in a field log, which is the one place it is ever needed. Captured output "
           "was: [" << captured << "]";

    EXPECT_NE(captured.find("STARTED"), std::string::npos)
        << "the OLD state was not named in the state-change report. Both names are needed: a "
           "report that says only where the HAL ended up cannot distinguish a transition from a "
           "repeated notification. Captured output was: [" << captured << "]";

    EXPECT_NE(captured.find("CLOSED"), std::string::npos)
        << "the new state was not named in the state-change report, so the report carries no "
           "information about what the HAL actually did. Captured output was: [" << captured << "]";

    // With DEBUG asserted above, the transmit report must name the status, length and bytes.
    EXPECT_NE(captured.find("onMessageSent received"), std::string::npos)
        << "onMessageSent reported nothing at all with the level raised to DEBUG, although "
           "reporting is this callback's whole behaviour. An empty body would leave a transmit "
           "outcome the HAL observed invisible everywhere - write() acts on sendMessage()'s return "
           "value, not on this callback, so nothing else records it. Captured output was: ["
        << captured << "]";

    EXPECT_NE(captured.find(cechal::toString(cechal::SendMessageStatus::ACK_STATE_0)),
              std::string::npos)
        << "the transmit result was reported without naming the SendMessageStatus it carried, so "
           "the report cannot distinguish an acknowledged transmit from a rejected one - which is "
           "the only thing this callback exists to tell anybody. Captured output was: [" << captured
        << "]";

    // Matched with its label from the producer's format ("message length: %zu"), since a bare
    // digit would match any timestamped line.
    const std::string expectedLengthReport =
        std::string("message length: ") + std::to_string(sentMessage.size());

    EXPECT_NE(captured.find(expectedLengthReport), std::string::npos)
        << "the transmit result was reported without the message length. Expected to find: ["
        << expectedLengthReport << "]. Captured output was: [" << captured << "]";

    EXPECT_NE(captured.find(lowercaseHexOf(sentMessage)), std::string::npos)
        << "the transmit report does not carry the message bytes (" << lowercaseHexOf(sentMessage)
        << "). A status and a length cannot tie a report to a message: two frames of the same "
           "length are indistinguishable, and with several transmits in flight the reader cannot "
           "tell which one the bus outcome belongs to. Captured output was: [" << captured << "]";

    // Level-independent: neither callback may put anything on the receive path.
    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "one of the two diagnostic callbacks delivered a frame to an application listener. "
           "Neither carries an inbound message: onStateChanged must not offer the close sentinel - "
           "an in-process HAL cannot vanish, so the legacy path has no such notion - and "
           "onMessageSent reports a frame that has already gone OUT";

    // Still open, and still able to transmit: neither callback changed any state.
    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the driver is no longer usable after the two diagnostic callbacks. A transition to "
           "CLOSED must not close the middleware's own session - reacting to it would be new "
           "behaviour with no legacy counterpart";
}

/**
 * @brief With a binder threadpool already started, a close/open cycle succeeds, the fake can
 *        invoke the listener the second open registered, and a directed write succeeds.
 * @pre Invocation B. The pool is observed via ProcessState::getThreadPoolMaxThreadCount() before
 *      the session is touched.
 * @note The "binder:*" thread-name probe is printed, not asserted, because this SDK applies the
 *       name only on Android builds; pool-thread delivery is covered by invocation E.
 */
TEST_F(DriverAidlSessionTest, OpenSucceedsAndDeliversWhenAThreadPoolWasAlreadyStarted) {
    // A non-zero maximum means startThreadPool() ran during LibCCEC::init; selfOrNull() cannot
    // create a ProcessState, so null means no pool exists.
    const ::android::sp<::android::ProcessState> processState =
        ::android::ProcessState::selfOrNull();
    const size_t poolMaxThreads =
        (processState != nullptr) ? processState->getThreadPoolMaxThreadCount() : 0u;

    EXPECT_GT(poolMaxThreads, 0u)
        << "libbinder reports no started threadpool in this process - although the AIDL back-end "
           "has been open since LibCCEC::init, and DriverAidlImpl::open() is obliged to call "
           "ProcessState::self()->startThreadPool(), which sets exactly the flag this value "
           "reads. Nothing else in this case would notice: an in-process fake dispatches its "
           "trigger on the CALLING thread, so frames arrive regardless, and the omission would "
           "first appear on a real device as a receive path that never delivers anything at all"
        << (processState == nullptr
                ? ". ProcessState::selfOrNull() returned null, so no ProcessState was ever "
                  "created in this process at all - which means the back-end never reached "
                  "libbinder, not merely that it skipped the pool"
                : "");

    // Corroboration only, printed and not asserted: on this port pool threads carry the process's
    // own name, so a miss is the ordinary case.
    bool procIsReadable = false;
    const bool poolExists = processHasABinderThread(&procIsReadable);

    if (!procIsReadable) {
        std::cout << "[DriverAidlSessionTest] /proc/self/task could not be read, so no thread "
                     "listing was available on this run."
                  << std::endl;
    }
    else if (poolExists) {
        std::cout << "[DriverAidlSessionTest] a thread named " << kBinderThreadNamePrefix
                  << "* is present, so this build applies the name "
                     "ProcessState::makeBinderThreadName() composes and the binder threadpool is "
                     "directly observable."
                  << std::endl;
    }
    else {
        std::cout << "[DriverAidlSessionTest] no thread named " << kBinderThreadNamePrefix
                  << "* is present. Expected on this port and NOT evidence that the threadpool is "
                     "absent: androidCreateRawThreadEtc() discards the name outside __ANDROID__ "
                     "builds, so a running pool inherits the process's own comm. The evidence that "
                     "DriverAidlImpl::open() started the pool is invocation E's, which receives a "
                     "oneway callback on a pool thread over real out-of-process IPC, in "
                     "hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp. The idempotency "
                     "assertions below are unaffected."
                  << std::endl;
    }

    // A close/open cycle so that open() genuinely runs its body again, rather than returning
    // silently on an already-open driver.
    ASSERT_NO_THROW({ Driver::getInstance().close(); });
    ASSERT_EQ(fake->getCloseCallCount(), 1) << "the close did not reach the HAL";

    // The successful close names the controller open() returned.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for a close that reported success, so close() "
               "passed nothing identifiable and the HAL cannot know which session to end";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the successful close named a different controller from the one open() returned. "
               "IHdmiCec::close takes the controller precisely so the HAL knows which session is "
               "being ended; naming another one would end the wrong session, or none";
    }

    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "open() failed on a second pass, with a binder threadpool already started in this "
           "process. startThreadPool() is idempotent, so a failure here means the back-end is "
           "tracking pool state of its own - which cannot see a pool another component started";

    EXPECT_EQ(fake->getOpenCallCount(), 2)
        << "the HAL did not observe a second open, so the session was not re-established";

    const std::vector<uint8_t> message{ 0x0F, GIVE_DEVICE_POWER_STATUS };
    EXPECT_TRUE(fake->fireOnMessageReceived(message))
        << "no callback arrives after the second open, so the listener was not re-registered and "
           "the receive path is dead for the rest of the session";

    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the re-opened session cannot transmit, so open() did not restore the controller";
}

/**
 * @brief open() on an already-open driver returns silently without reaching the HAL or replacing
 *        the controller or listener.
 * @pre Invocation B, with SetUp having left exactly one open behind it.
 * @note Asserted on the fake's open counter, because this fake tolerates duplicate opens a
 *       single-instance real HAL would fail.
 */
TEST_F(DriverAidlSessionTest, OpeningAnAlreadyOpenDriverIsASilentNoOpThatTouchesNothing) {
    // SetUp left exactly one open behind it, and the session it established is the baseline.
    ASSERT_EQ(fake->getOpenCallCount(), 1)
        << "the fixture did not leave exactly one open behind it, so the counter below cannot "
           "distinguish a no-op from a re-open";

    const ::android::sp<cechal::IHdmiCecController> controllerBefore = fake->getController();
    const ::android::sp<cechal::IHdmiCecEventListener> listenerBefore = fake->getListener();

    ASSERT_NE(controllerBefore.get(), nullptr)
        << "the fake holds no controller after the fixture's open, so there is no session identity "
           "to compare against";
    ASSERT_NE(listenerBefore.get(), nullptr)
        << "the fake holds no listener after the fixture's open, so there is no listener identity "
           "to compare against";

    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "open() raised on an ALREADY-OPEN driver. The legacy back-end's throw for this case is "
           "compiled out, so its observable behaviour is a silent return - raising here would be "
           "an unregistered difference between the back-ends, and one that Bus::start() and the "
           "fixture's own TearDown would both hit";

    EXPECT_EQ(fake->getOpenCallCount(), 1)
        << "the duplicate open reached the HAL: the open count is now " << fake->getOpenCallCount()
        << " where it must still be 1. IHdmiCec::open() is single-instance and a real HAL fails "
           "the second call with EX_ILLEGAL_STATE, so this would raise IOException on a device "
           "while passing here - this fake accepts duplicate opens on purpose, which is what makes "
           "the guard observable at all";

    EXPECT_EQ(fake->getController().get(), controllerBefore.get())
        << "the duplicate open replaced the controller, so the session the middleware has been "
           "transmitting on has been swapped underneath it and the previous one is stranded "
           "HAL-side with nothing able to close it";

    EXPECT_EQ(fake->getListener().get(), listenerBefore.get())
        << "the duplicate open re-registered a listener, so the HAL now holds one whose owner may "
           "already have moved on - and the fixture's close/reset/open cycle, which exists "
           "precisely to refresh that capture deliberately, would no longer control when it "
           "happens";

    // And the untouched session still works, which is the caller-visible form of "nothing
    // happened".
    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the session no longer transmits after a duplicate open that was supposed to do "
           "nothing at all";
}

/**
 * @brief A callback arriving after a failed close, and after its owner is destroyed, is dropped
 *        with the logged drop line and reaches no application listener.
 * @pre Invocation B. The case retains the listener so it outlives its owner, as the HAL's strong
 *      reference does on a device.
 * @note Guards a use-after-free through the listener's back pointer, so it must detach on both
 *       close arms and in the destructor; the drop line is matched verbatim.
 */
TEST_F(DriverAidlSessionTest, ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-post-detach",
                                            applicationListener);

    const char *const expectedDropLine =
        "DriverAidlImpl::EventListener: message received after detach, dropping it";

    // Held so the listener outlives its owner, as the HAL's strong reference does on a device.
    ::android::sp<cechal::IHdmiCecEventListener> retainedListener;

    const std::vector<uint8_t> afterFailedClose{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x61, 0x00, 0x04 };
    const std::vector<uint8_t> afterDestruction{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x71, 0x00, 0x04 };

    {
        // a local instance, so that its destruction can be driven from here. The process-global
        // driver outlives every case and could not be used for this.
        DriverAidlImpl localDriver;

        ASSERT_TRUE(localDriver.isServiceAvailable())
            << "a local instance could not resolve the fake service, so it cannot open a session "
               "and no listener would be registered to deliver to afterwards";
        ASSERT_NO_THROW({ localDriver.open(); })
            << "the local instance could not open a session, so there is no listener to retain";

        retainedListener = fake->getListener();
        ASSERT_NE(retainedListener.get(), nullptr)
            << "the local open handed no listener to the HAL, so nothing can arrive after the "
               "session ends and this case has nothing to drive";

        // Arm 1: a close the HAL reports as failed. Detachment must already have happened by the
        // time the exception escapes.
        fake->setCloseResult(false);

        EXPECT_THROW({ localDriver.close(); }, IOException)
            << "the close did not fail, so this arm is not the failure path it is meant to drive";

        std::string capturedAfterFailedClose;
        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be captured, so the drop cannot be observed and a delivery to "
                   "freed storage would be indistinguishable from a correct drop";

            const ::android::binder::Status delivered =
                retainedListener->onMessageReceived(afterFailedClose);

            capturedAfterFailedClose = capture.read();

            EXPECT_TRUE(delivered.isOk())
                << "the listener returned a non-ok status for a callback it should simply have "
                   "dropped. A oneway callback has no caller to receive a fault, so anything other "
                   "than ok here reaches onTransact and is worse than dropping one frame";
        }

        EXPECT_NE(capturedAfterFailedClose.find(expectedDropLine), std::string::npos)
            << "a callback arriving after a close that failed was not reported as dropped. The "
               "listener must be detached on both arms of close(), before the result is evaluated: "
               "place the detachment after the error check and this path leaves a live listener "
               "holding a pointer to a driver whose session is gone - and on a real device the "
               "next HAL callback follows. Expected the line [" << expectedDropLine
            << "] and captured [" << capturedAfterFailedClose << "]";

        // The owner is destroyed here. Its state was already CLOSED, so the destructor's own
        // close is a silent no-op and the unconditional detachment is what runs.
    }

    // Arm 2: the owner's storage is gone. The listener is still a live binder object, still
    // callable, and still held by the fake and by this case.
    std::string capturedAfterDestruction;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be captured, so the post-destruction drop cannot be observed";

        const ::android::binder::Status delivered =
            retainedListener->onMessageReceived(afterDestruction);

        capturedAfterDestruction = capture.read();

        EXPECT_TRUE(delivered.isOk())
            << "the listener returned a non-ok status for a callback arriving after its owner was "
               "destroyed, rather than dropping it";
    }

    EXPECT_NE(capturedAfterDestruction.find(expectedDropLine), std::string::npos)
        << "a callback arriving after the owner's destruction was not reported as dropped. The "
           "destructor must detach unconditionally - not only when the state says a session is "
           "open, and not only when its own close succeeded - because the HAL's strong reference "
           "keeps the listener callable after the storage its back pointer names has ceased to "
           "exist. Expected the line [" << expectedDropLine << "] and captured ["
        << capturedAfterDestruction << "]";

    // Nothing reached an application listener from either callback, which is the other half of
    // "touched nothing": a dropped frame must not be delivered through some other route either.
    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a callback delivered after the session ended reached an application listener. It was "
           "dropped according to the log and delivered according to the listener, so one of the "
           "two is wrong and the receive path is not in the state it reports";

    fake->setCloseResult(true);
}

/**
 * @brief open() raises IOException on a null controller even when the transaction succeeded, and
 *        leaves the driver closed.
 * @pre Invocation B, with the session closed and the fake reporting a null controller.
 * @note This is the easily omitted arm of a two-condition guard, because an ok status reads as
 *       success.
 */
TEST_F(DriverAidlSessionTest, OpenRejectsANullControllerEvenWhenTheStatusIsOk) {
    ASSERT_NO_THROW({ Driver::getInstance().close(); });

    fake->setOpenReturnsNullController(true);

    EXPECT_THROW({ Driver::getInstance().open(); }, IOException)
        << "open() accepted a null controller reported alongside an ok status, so the state would "
           "be OPENED with no session behind it";

    // Still closed, so the guards are live - open() did not move the state before its checks.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver believes it is open after an open() that reported no controller";

    // Restore a usable session for TearDown; the fake is reset there in any case.
    fake->setOpenReturnsNullController(false);
    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "the session could not be restored after the null-controller case";
}

/**
 * @brief A failed open transaction raises IOException, leaves the driver closed, and does not
 *        stop a later open from re-establishing the session.
 * @pre Invocation B, with the session closed and a failing open status installed.
 * @note The recovery is asserted on the fake's open counter, because a driver wrongly left open
 *       would return silently and look the same.
 */
TEST_F(DriverAidlSessionTest, OpenRejectsANonOkBinderStatus) {
    ASSERT_NO_THROW({ Driver::getInstance().close(); });

    const int openCountBeforeFailure = fake->getOpenCallCount();

    fake->setOpenBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().open(); }, IOException)
        << "open() accepted a failed transaction, so a dead HAL would be reported as an open "
           "session";

    EXPECT_EQ(fake->getOpenCallCount(), openCountBeforeFailure + 1)
        << "the failing open did not reach the HAL at all, so the IOException came from somewhere "
           "before the transaction and this case is not exercising the guard it names";

    // The state did not move: the driver is still CLOSED, so its guards are live.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver behaves as though it were open after an open() whose transaction failed. "
           "The state must only advance once both halves of open()'s guard have passed, or a "
           "caller that swallows the IOException is left holding a driver with no session behind "
           "it - and the next transmit fails on the controller check, far from the cause";

    fake->setOpenBinderStatus(::android::binder::Status::ok());
    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "the session could not be restored after the failed-open case";

    EXPECT_EQ(fake->getOpenCallCount(), openCountBeforeFailure + 2)
        << "the recovery open did not reach the HAL, although it raised nothing. That is what a "
           "driver which had wrongly retained the OPENED state looks like from outside: open() "
           "returns silently in that state, so the absence of an exception says nothing. The "
           "counter is what distinguishes a re-established session from a silent no-op";

    // And the restored session really works, which is the caller-visible form of the same claim.
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the driver still refuses to transmit after a successful recovery open, so the session "
           "was not actually re-established";
}

/**
 * @brief A transport failure on the address query reports 0 without raising.
 * @pre Invocation B, with a failing query status installed.
 * @note Zero is the no-address signal LibCCEC already turns into InvalidStateException, and the
 *       legacy back-end does not raise here either.
 */
TEST_F(DriverAidlSessionTest, GetLogicalAddressReportsZeroOnTransportFailureWithoutRaising) {
    fake->setGetLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    int reported = -1;
    EXPECT_NO_THROW({ reported = Driver::getInstance().getLogicalAddress(DeviceType::TV); })
        << "getLogicalAddress raised on a transport failure. Neither back-end throws here, and "
           "LibCCEC::getLogicalAddress does not guard the call - a new exception would propagate "
           "into the Source plugin's discovery path unhandled";

    EXPECT_EQ(reported, 0)
        << "a failed query did not report 0, so LibCCEC's zero-means-no-address signal is bypassed";
}

/**
 * @brief A transport failure on the removal is ignored, and the local removal still stands
 *        because it precedes the HAL call.
 * @pre Invocation B, with an address added first and a failing removal status installed.
 * @note Matches the legacy back-end, which discards its HAL's removal result.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressIgnoresTransportFailureAndStillRemovesLocally) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));

    fake->getController()->setRemoveLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised on a transport failure. The legacy back-end discards its "
           "HAL's return value, so raising here would be an unregistered behaviour change";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a removal whose transaction failed. The local "
           "removal precedes the HAL call and is not rolled back";
}

/**
 * @brief The AIDL back-end never calls getState, getProperty, registerEventListener or
 *        unregisterEventListener.
 * @pre Invocation B, after SetUp's session cycle and a representative add, write, poll and
 *      address query.
 * @note Asserted because a deliberate absence otherwise erodes silently.
 */
TEST_F(DriverAidlSessionTest, TheFourUnconsumedAidlMethodsAreNeverCalled) {
    // A representative slice of the surface the middleware does drive, so that the zeros below
    // cannot be explained by nothing having happened at all.
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    CECFrame frame = directedFrame();
    ASSERT_NO_THROW({ Driver::getInstance().write(frame); });
    ASSERT_NO_THROW({ Driver::getInstance().poll(address, LogicalAddress(LogicalAddress::TV)); });
    (void)Driver::getInstance().getLogicalAddress(DeviceType::TV);

    EXPECT_EQ(fake->getGetStateCallCount(), 0)
        << "IHdmiCec::getState was consulted. The middleware tracks its own lifecycle state, and a "
           "second source of truth is worse than none - poll() in particular is a CEC ping via a "
           "one-byte transmit, not a state query";
    EXPECT_EQ(fake->getGetPropertyCallCount(), 0)
        << "IHdmiCec::getProperty was consulted. HAL_CEC_VERSION and the METRIC_* properties have "
           "no legacy counterpart, so reading them would be new behaviour";
    EXPECT_EQ(fake->getRegisterEventListenerCallCount(), 0)
        << "IHdmiCec::registerEventListener was called. It exists for non-controlling diagnostic "
           "clients; this back-end is the controlling client and gets its events from the listener "
           "it passes to open(), so calling it would register a second listener";
    EXPECT_EQ(fake->getUnregisterEventListenerCallCount(), 0)
        << "IHdmiCec::unregisterEventListener was called, which is the mirror of a registration "
           "that never happened";
}

/**
 * @brief LibCCEC::getPhysicalAddress reports 1.0.0.0 through the resolved AIDL back-end without
 *        calling any method of the fake service or its controller.
 * @pre Invocation B, with the session open and the fake reset by SetUp.
 */
TEST_F(DriverAidlSessionTest, LibCCECReportsTheFixedPhysicalAddressWithoutAnyAidlCall) {
    ASSERT_NE(fake->getController(), nullptr);

    /**
     * @brief Snapshots the fake service and controller call counters this case compares.
     * @return std::vector<int32_t> - The counters in a fixed order, for a before/after comparison.
     */
    const auto halCallCounts = [this]() {
        return std::vector<int32_t>{
            fake->getOpenCallCount(), fake->getCloseCallCount(), fake->getGetLogicalAddressesCallCount(),
            fake->getGetStateCallCount(), fake->getGetPropertyCallCount(),
            fake->getRegisterEventListenerCallCount(), fake->getUnregisterEventListenerCallCount(),
            fake->getController()->getAddLogicalAddressesCallCount(),
            fake->getController()->getRemoveLogicalAddressesCallCount(),
            fake->getController()->getTotalSendMessageCallCount() };
    };
    const std::vector<int32_t> countsBefore = halCallCounts();

    unsigned int physicalAddress = kPluginUnsetPhysicalAddress;
    ASSERT_NO_THROW({ LibCCEC::getInstance().getPhysicalAddress(&physicalAddress); });

    EXPECT_EQ(physicalAddress, kFixedPhysicalAddress)
        << "LibCCEC did not report the fixed 1.0.0.0 encoding 0x01000000 on the AIDL back-end";
    EXPECT_EQ(decodeAsThePluginsDo(physicalAddress), "1.0.0.0");
    EXPECT_EQ(halCallCounts(), countsBefore)
        << "the physical-address query reached the fake HDMI CEC service or its controller";
}

/**
 * @brief Transmit, poll and writeAsync behaviour on an open AIDL back-end.
 *
 * Requires invocation B; see DriverAidlSessionFixture for the precondition and SetUp's session
 * cycle. Cases cover send-status translation on directed and broadcast frames, a failed send
 * transaction, the 16-byte frame limit, poll() framing and acknowledgement, and writeAsync's
 * refusal.
 */
class DriverAidlTransmitTest : public DriverAidlSessionFixture {
protected:
    /**
     * @brief Programs the canned send status and hands the frame to the AIDL back-end.
     *
     * @param [in] frame  - Frame to transmit, header byte first.
     * @param [in] status - Status the fake reports for the transmission.
     */
    void transmitWithStatus(const CECFrame &frame, cechal::SendMessageStatus status) {
        fake->getController()->setSendMessageResult(status);
        Driver::getInstance().write(frame);
    }

    /**
     * @brief Asserts the fake received exactly one frame, of the expected size.
     *
     * @param [in] length - Frame size in bytes the fake is required to have captured.
     *
     * @note A truncated or padded frame would reach the bus as a different CEC message.
     */
    void expectExactlyOneFrameOfLength(size_t length) {
        EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 1)
            << "the HAL was not asked exactly once to send";
        EXPECT_EQ(fake->getController()->getLastSentMessage().size(), length)
            << "the HAL received a frame of " << fake->getController()->getLastSentMessage().size()
            << " bytes rather than " << length << ", so the frame was truncated or padded";
    }
};

/**
 * @brief A directed frame reported ACK_STATE_0 (acknowledged) succeeds, and its exact bytes reach
 *        the HAL.
 * @pre Invocation B.
 * @note The same status means rejected on a broadcast, so this is where the inverted sense bites.
 */
TEST_F(DriverAidlTransmitTest, DirectedFrameAcknowledgedByTheFollowerSucceeds) {
    CECFrame frame = directedFrame();

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a directed frame the follower acknowledged was reported as a failure. ACK_STATE_0 "
           "means acknowledged for a directed message; reading it as a rejection would fail every "
           "successful directed transmit in the system";

    expectExactlyOneFrameOfLength(2u);

    const std::vector<uint8_t> sent = fake->getController()->getLastSentMessage();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0], 0x40) << "the header byte was altered on the way to the HAL";
    EXPECT_EQ(sent[1], GIVE_DEVICE_POWER_STATUS) << "the opcode was altered on the way to the HAL";
}

/**
 * @brief A directed frame reported ACK_STATE_1 (not acknowledged) raises CECNoAckException, and
 *        was still sent.
 * @pre Invocation B.
 * @note The exception type lets callers tell an absent peer from a broken HAL.
 */
TEST_F(DriverAidlTransmitTest, DirectedFrameNotAcknowledgedRaisesNoAck) {
    CECFrame frame = directedFrame();

    EXPECT_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_1); },
                 CECNoAckException)
        << "a directed frame that was not acknowledged did not raise CECNoAckException. "
           "ACK_STATE_1 is the not-acknowledged value for a directed message, and the exception "
           "type is what lets callers tell an absent peer from a broken HAL";

    // It was still sent: the exception describes the bus outcome, not a refusal to transmit.
    expectExactlyOneFrameOfLength(2u);
}

/**
 * @brief A broadcast frame reported ACK_STATE_1 (sent, not rejected) succeeds, the mirror image of
 *        the directed case.
 * @pre Invocation B.
 * @note Mapping ACK_STATE_1 to CECNoAckException unconditionally would fail every broadcast.
 */
TEST_F(DriverAidlTransmitTest, BroadcastFrameNotRejectedSucceeds) {
    CECFrame frame = broadcastFrame(GIVE_DEVICE_POWER_STATUS);

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_1); })
        << "a broadcast frame reported ACK_STATE_1 was treated as a failure. On a broadcast that "
           "value means sent and not rejected - the opposite of its meaning on a directed message "
           "- so this is the inverted sense being read the wrong way round";

    expectExactlyOneFrameOfLength(2u);
}

/**
 * @brief A rejected broadcast (ACK_STATE_0) carrying an opcode other than Report Physical Address
 *        returns normally, as on the legacy back-end.
 * @pre Invocation B.
 * @note The negative control for the CEC CTS 9-3-3 case that follows.
 */
TEST_F(DriverAidlTransmitTest, RejectedBroadcastReturnsNormallyForOpcodesOutsideTheCtsArm) {
    CECFrame frame = broadcastFrame(GIVE_DEVICE_POWER_STATUS);

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a rejected broadcast carrying an opcode outside the CEC CTS 9-3-3 arm raised. The "
           "legacy back-end raises only on a rejected REPORT_PHYSICAL_ADDRESS, so raising here "
           "would make every rejected broadcast an error the callers do not expect";

    expectExactlyOneFrameOfLength(2u);
}

/**
 * @brief The CEC CTS 9-3-3 arm: a rejected broadcast Report Physical Address raises
 *        CECNoAckException, so the caller retries.
 * @pre Invocation B.
 * @note The opcode is pinned by a static_assert at the top of this file, so this case and its
 *       negative control cannot drift onto the same opcode.
 */
TEST_F(DriverAidlTransmitTest, RejectedBroadcastReportPhysicalAddressRaisesForTheCtsRetry) {
    CECFrame frame = broadcastFrame(REPORT_PHYSICAL_ADDRESS);

    EXPECT_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); },
                 CECNoAckException)
        << "a rejected broadcast REPORT_PHYSICAL_ADDRESS did not raise CECNoAckException, so the "
           "caller has nothing to retry on and CEC CTS 9-3-3 fails";

    expectExactlyOneFrameOfLength(2u);
}

/**
 * @brief A one-byte broadcast reported ACK_STATE_0 returns normally, because the CTS arm needs an
 *        opcode byte to inspect.
 * @pre Invocation B, with a frame of exactly one byte.
 * @note poll() sends exactly such a frame, so reading the opcode without the length check would
 *       raise std::out_of_range out of every poll.
 */
TEST_F(DriverAidlTransmitTest, OneByteBroadcastDoesNotReachTheCtsArm) {
    CECFrame pollFrame;
    pollFrame.append(static_cast<uint8_t>(0x4F));
    ASSERT_EQ(pollFrame.length(), 1u);

    EXPECT_NO_THROW({ transmitWithStatus(pollFrame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a one-byte broadcast raised. The CTS arm needs an opcode byte to inspect, and reading "
           "frame.at(1) without the length guard would raise std::out_of_range out of every "
           "poll() - which is exactly the frame poll() sends";

    expectExactlyOneFrameOfLength(1u);
}

/**
 * @brief A BUSY result raises IOException on both a directed and a broadcast frame, because
 *        nothing reached the bus.
 * @pre Invocation B.
 * @note BUSY is decided before the destination nibble; the broadcast half uses the CTS opcode, so
 *       a CECNoAckException there would mean BUSY fell into the acknowledgement matrix.
 */
TEST_F(DriverAidlTransmitTest, BusyIsATransmitFailureOnBothDestinations) {
    CECFrame directed = directedFrame();

    EXPECT_THROW({ transmitWithStatus(directed, cechal::SendMessageStatus::BUSY); }, IOException)
        << "a BUSY result on a directed frame did not raise IOException. Arbitration failed and "
           "nothing was sent, which is the legacy send-failed family - not a missing "
           "acknowledgement";

    fake->getController()->reset();

    CECFrame broadcast = broadcastFrame(REPORT_PHYSICAL_ADDRESS);
    EXPECT_THROW({ transmitWithStatus(broadcast, cechal::SendMessageStatus::BUSY); }, IOException)
        << "a BUSY result on a broadcast frame did not raise IOException. BUSY is decided before "
           "the destination nibble is read, so it must give the same answer on both destinations - "
           "note that the frame used here is the CTS opcode, whose ACK_STATE_0 arm raises "
           "CECNoAckException, so a different exception type here means BUSY fell through into the "
           "ACK matrix";
}

/**
 * @brief A failed send transaction raises IOException even when the canned send status would
 *        succeed.
 * @pre Invocation B, with the send status set to ACK_STATE_0 and a failing transaction status.
 * @note The status out-parameter is meaningless when the transaction did not complete, so the
 *       transport check must run first.
 */
TEST_F(DriverAidlTransmitTest, NonOkBinderStatusOnSendRaisesIoException) {
    CECFrame frame = directedFrame();

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_0);
    fake->getController()->setSendMessageBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().write(frame); }, IOException)
        << "a failed transaction did not raise IOException. The canned send status was "
           "ACK_STATE_0, whose own arm succeeds on a directed frame, so a pass here would mean the "
           "status out-parameter was consulted despite the transaction never completing";
}

/**
 * @brief A frame at the 16-byte AIDL limit is sent whole, while 17- and 20-byte frames raise
 *        IOException and nothing is sent.
 * @pre Invocation B.
 * @note A refusal after a truncated frame reached the bus would be worse than either outcome, so
 *       the send counter is asserted to be zero.
 */
TEST_F(DriverAidlTransmitTest, FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated) {
    // The control: exactly at the limit, accepted, whole.
    CECFrame atLimit = frameOfLength(kAidlMaxMessageLength);
    ASSERT_EQ(atLimit.length(), kAidlMaxMessageLength);

    EXPECT_NO_THROW({ transmitWithStatus(atLimit, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a frame of exactly " << kAidlMaxMessageLength << " bytes was refused, although that is "
           "the AIDL contract's stated maximum and the legacy back-end accepts it too. The "
           "difference between the back-ends must be confined to the disputed band, not extended "
           "to the boundary itself";
    expectExactlyOneFrameOfLength(kAidlMaxMessageLength);

    const size_t overLimit[] = { kJustOverAidlLimit, kLegacyMaxMessageLength };

    for (size_t i = 0; i < sizeof(overLimit) / sizeof(overLimit[0]); i++) {
        fake->getController()->reset();

        CECFrame tooLong = frameOfLength(overLimit[i]);
        ASSERT_EQ(tooLong.length(), overLimit[i])
            << "the frame builder did not produce a frame of " << overLimit[i] << " bytes";

        EXPECT_THROW({ transmitWithStatus(tooLong, cechal::SendMessageStatus::ACK_STATE_0); },
                     IOException)
            << "a frame of " << overLimit[i] << " bytes was accepted, although the AIDL "
               "sendMessage contract states a maximum of " << kAidlMaxMessageLength;

        EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 0)
            << "a frame of " << overLimit[i] << " bytes reached the HAL before being refused. The "
               "guard must run AHEAD of the transmit: an IOException raised after a truncated "
               "frame is already on the wire is worse than either outcome alone, because a peer "
               "can interpret a corrupt frame as a different message entirely";
        EXPECT_TRUE(fake->getController()->getLastSentMessage().empty())
            << "a frame of " << overLimit[i] << " bytes was captured by the HAL, so something was "
               "transmitted despite the refusal";
    }
}

/**
 * @brief writeAsync on an open AIDL driver raises OperationNotSupportedException after the
 *        prelude and state guard, and sends nothing.
 * @pre Invocation B, with the session open.
 * @note Asynchronous transmit is deliberately neither migrated nor emulated; no production call
 *       site reaches it.
 */
TEST_F(DriverAidlTransmitTest, WriteAsyncRaisesOperationNotSupportedOnAnOpenDriver) {
    CECFrame frame = directedFrame();

    EXPECT_THROW({ Driver::getInstance().writeAsync(frame); }, OperationNotSupportedException)
        << "writeAsync did not raise OperationNotSupportedException on an open AIDL driver. This "
           "is the one arm where the two back-ends differ on writeAsync - the prelude ordering and "
           "the state guard are identical on both - so a different outcome here either emulates "
           "asynchrony or has lost the guard";

    EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 0)
        << "writeAsync transmitted the frame before raising, which is the worst of both outcomes: "
           "the frame is on the bus and the caller was told the operation is unsupported";
}

/**
 * @brief poll() sends one byte, initiator in the high nibble and destination in the low, through
 *        this back-end's own write().
 * @pre Invocation B, with the fake acknowledging the ping.
 * @see DriverAidlSessionTest::TheFourUnconsumedAidlMethodsAreNeverCalled, which keeps getState()
 *      out of poll().
 */
TEST_F(DriverAidlTransmitTest, PollTransmitsAOneByteFrameCarryingBothAddresses) {
    const LogicalAddress from(LogicalAddress::PLAYBACK_DEVICE_1);
    const LogicalAddress to(LogicalAddress::TV);

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_0);

    EXPECT_NO_THROW({ Driver::getInstance().poll(from, to); })
        << "poll() failed against a HAL that acknowledged the ping";

    expectExactlyOneFrameOfLength(1u);

    const std::vector<uint8_t> sent = fake->getController()->getLastSentMessage();
    ASSERT_EQ(sent.size(), 1u);

    const uint8_t expectedByte =
        static_cast<uint8_t>(((from.toInt() & 0x0F) << 4) | (to.toInt() & 0x0F));
    EXPECT_EQ(sent[0], expectedByte)
        << "the poll byte is not the initiator in the high nibble and the destination in the low "
           "one, so a peer would see the ping as addressed to the wrong device";
}

/**
 * @brief A poll reported ACK_STATE_1 raises CECNoAckException, which is how an absent device is
 *        reported up to Bus.
 * @pre Invocation B.
 * @note Bus discovery depends on this arm: a successful poll of an absent device would list a
 *       device that is not there.
 */
TEST_F(DriverAidlTransmitTest, UnansweredPollRaisesNoAck) {
    const LogicalAddress from(LogicalAddress::PLAYBACK_DEVICE_1);
    const LogicalAddress to(LogicalAddress::TV);

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_1);

    EXPECT_THROW({ Driver::getInstance().poll(from, to); }, CECNoAckException)
        << "an unanswered poll did not raise CECNoAckException, so bus discovery would record "
           "devices that are not present";

    expectExactlyOneFrameOfLength(1u);
}


/** @} */
/** @} */
