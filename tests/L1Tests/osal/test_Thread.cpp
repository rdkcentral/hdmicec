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

#include <gtest/gtest.h>
#include "osal/Runnable.hpp"
#include "osal/Thread.hpp"
#include <string>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <pthread.h>

using namespace CCEC_OSAL;

namespace {

/**
 * Minimal synchronous test double for Runnable.
 *
 * Thread::run() forwards directly to the supplied Runnable, so an ordinary
 * counter provides a deterministic observation without worker threads, timing
 * assumptions, mocks, or heap deletion through an interface without a virtual
 * destructor.
 */
class CountingRunnable : public Runnable {
public:
    CountingRunnable() : invocationCount_(0) {
    }

    void run(void) override {
        ++invocationCount_;
    }

    int invocationCount(void) const {
        return invocationCount_;
    }

private:
    int invocationCount_;
};

/**
 * Test double for the one path that genuinely leaves this translation unit:
 * Thread::start() hands Thread::CEntry to pthread_create, so run() is invoked on
 * another thread and the caller has no join to wait on -- start() creates the
 * thread with PTHREAD_CREATE_DETACHED, and Thread::stop() and
 * Thread::getNativeHandle() are declared in the header but never defined
 * anywhere in the component, so neither can be called.
 *
 * The dispatch is therefore observed the only way it can be: the runnable itself
 * signals a condition variable, and the test waits on that with a bound.  It
 * records the thread it ran on as well, which is what distinguishes "start()
 * dispatched asynchronously" from "start() happened to call run() inline".
 */
class SignallingRunnable : public Runnable {
public:
    SignallingRunnable() : dispatched_(false), dispatchThread_() {
    }

    void run(void) override {
        std::unique_lock<std::mutex> lock(mutex_);
        dispatchThread_ = pthread_self();
        dispatched_ = true;
        condition_.notify_all();
    }

    /*
     * Returns true once run() has executed, false if it has not within the bound.
     *
     * This is a PREDICATE wait, not a pause: wait_for returns the instant run() signals, so the
     * case costs microseconds on the passing path and never sleeps for a fixed span.  The
     * millisecond argument is a FAILURE DEADLINE - the only thing that stops a dispatch which
     * never happens from hanging the suite - and is never the amount of time actually waited.
     * There is no other way to observe this particular behaviour: start() creates the worker with
     * PTHREAD_CREATE_DETACHED and Thread::stop()/getNativeHandle() are declared but never defined
     * anywhere in the component, so the caller has no join and no handle to wait on, and the
     * dispatch itself is what is under test rather than something a captured callback could stand
     * in for.
     */
    bool waitForDispatch(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [this]() { return dispatched_; });
    }

    pthread_t dispatchThread(void) {
        std::unique_lock<std::mutex> lock(mutex_);
        return dispatchThread_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool dispatched_;
    pthread_t dispatchThread_;
};

} // namespace

class ThreadTest : public ::testing::Test {
protected:
    CountingRunnable runnable;
};

TEST_F(ThreadTest, NamedConstructionDoesNotDispatch) {
    Thread thread(runnable, reinterpret_cast<const int8_t *>("worker"));

    EXPECT_EQ(0, runnable.invocationCount());
    (void)thread;
}

TEST_F(ThreadTest, RunDispatchesToRunnable) {
    Thread thread(runnable);

    thread.run();

    EXPECT_EQ(1, runnable.invocationCount());
}

TEST_F(ThreadTest, DestructionAtScopeExitPreservesDispatchResult) {
    {
        Thread thread(runnable);
        thread.run();
        EXPECT_EQ(1, runnable.invocationCount());
    }

    EXPECT_EQ(1, runnable.invocationCount());
}

TEST_F(ThreadTest, DetachThenScopeExitPreservesDispatchResult) {
    {
        Thread thread(runnable);
        thread.run();
        EXPECT_NO_THROW(thread.detach());
    }

    EXPECT_EQ(1, runnable.invocationCount());
}

TEST_F(ThreadTest, EmptyNameIsAccepted) {
    Thread thread(runnable, reinterpret_cast<const int8_t *>(""));

    thread.run();

    EXPECT_EQ(1, runnable.invocationCount());
}

TEST_F(ThreadTest, LongNameIsAccepted) {
    const std::string longName(1024U, 'n');
    Thread thread(
        runnable,
        reinterpret_cast<const int8_t *>(longName.c_str()));

    thread.run();

    EXPECT_EQ(1, runnable.invocationCount());
}

TEST_F(ThreadTest, RunDispatchIsRepeatable) {
    Thread thread(runnable, reinterpret_cast<const int8_t *>("repeat"));

    thread.run();
    thread.run();
    thread.run();

    EXPECT_EQ(3, runnable.invocationCount());
}

TEST_F(ThreadTest, NamedAndUnnamedConstructorsDispatchEquivalently) {
    CountingRunnable namedRunnable;
    Thread unnamedThread(runnable);
    Thread namedThread(
        namedRunnable,
        reinterpret_cast<const int8_t *>("named"));

    unnamedThread.run();
    namedThread.run();

    EXPECT_EQ(1, runnable.invocationCount());
    EXPECT_EQ(1, namedRunnable.invocationCount());
}

TEST_F(ThreadTest, DestructionWithoutStartIsHarmless) {
    EXPECT_EQ(0, runnable.invocationCount());
    {
        Thread thread(runnable);
        (void)thread;
    }

    EXPECT_EQ(0, runnable.invocationCount());
}

TEST_F(ThreadTest, DetachWithoutStartLeavesThreadUsable) {
    Thread thread(runnable);

    EXPECT_NO_THROW(thread.detach());
    EXPECT_EQ(0, runnable.invocationCount());

    thread.run();

    EXPECT_EQ(1, runnable.invocationCount());
}

// Traceability: #gap-mw-thread; COVERAGE_GAPS.md section 6.2 rank 34; P2.
// Symbols: CCEC_OSAL::Thread::Thread(Runnable &, const int8_t *),
// CCEC_OSAL::Thread::start, CCEC_OSAL::Thread::CEntry, CCEC_OSAL::Thread::~Thread.
//
// The remaining two symbols of the unit, and the only asynchronous behaviour it has:
// start() configures a detached pthread attribute set and passes Thread::CEntry to
// pthread_create, and CEntry casts its argument back to the Runnable and calls run().
// Waiting on the runnable's own signal rather than on a fixed sleep keeps the case
// deterministic: it returns as soon as the dispatch lands and fails rather than hangs
// if it never does.
TEST_F(ThreadTest, StartDispatchesRunnableOnAnotherThread) {
    // THE RUNNABLE IS HEAP-OWNED, AND OWNERSHIP IS ONLY GIVEN UP ON ONE PATH.
    //
    // Thread::start() hands &runnable to pthread_create with PTHREAD_CREATE_DETACHED, and
    // Thread::stop()/getNativeHandle() are declared in osal/Thread.hpp but defined nowhere in
    // the component, so there is NO join and NO handle: nothing in the public surface tells a
    // caller that the worker has left Thread::CEntry.  A stack-allocated runnable is therefore
    // only safe on the path where the worker is known to be finished with it, and it is not safe
    // on the path where it is not.
    //
    // Where the boundary actually is, argued rather than assumed.  SignallingRunnable::run()
    // takes mutex_, records the thread, sets dispatched_, notifies, and RELEASES mutex_ as its
    // unique_lock unwinds; CEntry then does nothing but `return NULL`, so the object is not
    // touched again (Thread.cpp:38-43).  waitForDispatch() can only observe dispatched_ == true
    // while it itself holds mutex_, which it cannot do until the worker has released it.  So a
    // TRUE return is a genuine happens-after edge: once it lands, no worker can reach this
    // object again, and destroying it is safe.
    //
    // A FALSE return is the opposite - it says nothing about the worker except that it has not
    // signalled yet, which includes "pthread_create has not scheduled it and it is about to call
    // run() on this object".  Destroying the runnable there is a use-after-free with no
    // diagnostic.  Ownership is consequently RELEASED (deliberately retained for the remainder
    // of the process) on exactly that path and freed normally on every other, so the passing run
    // leaks nothing and the failing run cannot corrupt memory while reporting its failure.
    //
    // Required production change, reported and NOT made here (Directive 6): a join or completion
    // seam on CCEC_OSAL::Thread - either a defined stop()/getNativeHandle(), or a non-detached
    // start() paired with a join - would let this case observe worker EXIT rather than run()'s
    // tail, and the retention below could then go away.
    std::unique_ptr<SignallingRunnable> asyncRunnable(new SignallingRunnable());
    const pthread_t callingThread = pthread_self();
    bool dispatched = false;

    {
        Thread thread(*asyncRunnable, reinterpret_cast<const int8_t *>("l1-start"));

        thread.start();

        // Bounded, and deliberately generous rather than tight: this runs inside a suite whose
        // Bus reader threads can be CPU-bound, and a tight bound would turn scheduling pressure
        // into a spurious failure (measured: one such failure in 25 full-suite runs at 5 s).
        dispatched = asyncRunnable->waitForDispatch(30000);
    }

    if (!dispatched) {
        // Retain the object for the process lifetime BEFORE failing: a worker that has not
        // signalled may still be about to dereference it, and there is no seam to wait on.
        (void)asyncRunnable.release();
        FAIL() << "Thread::start() never dispatched the runnable within 30s. start() creates the "
                  "worker detached and swallows a pthread_create failure, so the only way this "
                  "happens is that no worker exists; the runnable has been intentionally retained "
                  "rather than freed, because a worker that starts late would otherwise use it "
                  "after free.";
    }

    // Dispatch really crossed a thread boundary; start() did not degrade to an
    // inline call on the caller.
    EXPECT_EQ(0, pthread_equal(callingThread, asyncRunnable->dispatchThread()));

    // The worker was created detached, so scope exit above is a complete teardown:
    // ~Thread has no join to perform and must not block or fail.
    EXPECT_TRUE(asyncRunnable->waitForDispatch(0));
}
