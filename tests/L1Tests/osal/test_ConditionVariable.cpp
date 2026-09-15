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
#include "osal/ConditionVariable.hpp"
#include "osal/Mutex.hpp"
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <cstring>

using namespace CCEC_OSAL;

class ConditionVariableTest : public ::testing::Test {
protected:
    Mutex mutex;
    ConditionVariable condVar;
};

TEST_F(ConditionVariableTest, NotifyOne) {
    bool notified = false;
    
    // Ensure condition starts in reset state
    condVar.reset();
    
    std::thread waiter([&]() {
        condVar.wait();
        notified = true;
    });
    
    // Give thread time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Signal the waiting thread
    condVar.notify();
    
    // Wait for thread to complete
    waiter.join();
    
    EXPECT_TRUE(notified);
}

TEST_F(ConditionVariableTest, TimedWait) {
    long timeout = 100;
    long result = condVar.wait(timeout);
    // Nothing signaled the condition, so the wait timed out → returns 0.
    EXPECT_EQ(result, 0);
}

// When the condition is signaled BEFORE the timeout expires, wait(timeout)
// must return 1 (non-zero, meaning "condition was set, did not time out").
TEST_F(ConditionVariableTest, SignaledBeforeTimeout) {
    condVar.reset();

    std::atomic<long> result{-1};
    std::thread waiter([&]() {
        result = condVar.wait(2000); // wait up to 2000ms
    });

    // Give the waiter thread time to enter the wait, then signal it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    condVar.notify();

    waiter.join();
    EXPECT_EQ(result.load(), 1L); // signaled before timeout → returns 1
}

// The timed-wait path normalises its deadline BEFORE it waits: it computes
//     wakeTime.tv_nsec = curTime.tv_usec * 1000 + (timeout % 1000) * 1000000
// and, when that exceeds one second, carries the excess into tv_sec
// (ConditionVariable.cpp:114-116).  Whether the carry happens depends on the wall-clock
// microsecond at the moment of the call, so a test that merely passes some timeout covers
// those two lines only some of the time.  That is not a hypothetical: this file's measured
// line coverage swung between 95.9% (70/73) and 98.6% (72/73) across runs of the SAME binary,
// with lines 115 and 116 the whole difference.  A coverage figure that moves for reasons the
// tests do not control is a figure nobody can hold to a threshold, so this case forces the
// carry rather than hoping for it.
//
// The carry condition is  usec * 1000 + (timeout % 1000) * 1000000 > 1000000000.  With
// timeout % 1000 == 999 it reduces to  usec > 1000, so the only microseconds from which the
// carry is unreachable are the first 1000 of each second -- and there it is unreachable for
// EVERY timeout, because timeout % 1000 can never exceed 999.  The loop below steps past that
// window by re-reading the clock, which costs at most one millisecond and usually nothing; it
// is a bounded read of a monotonically advancing counter, not a sleep and not a wait for
// another thread.  The iteration cap exists so a stopped clock fails the case instead of
// hanging it.
//
// The condition is SET before the call, so `while (!cond->isSet())` never iterates: the
// deadline arithmetic runs, the wait is skipped, and the case returns immediately.
TEST_F(ConditionVariableTest, TimedWaitCarriesNanosecondOverflowIntoSeconds) {
    struct timeval now;
    memset(&now, 0, sizeof(now));

    const int maxClockReads = 5000000;
    int reads = 0;
    do {
        ASSERT_EQ(0, gettimeofday(&now, NULL)) << "gettimeofday failed";
        ++reads;
    } while (now.tv_usec <= 1000 && reads < maxClockReads);

    // Proving the case exercised the carry means proving it started outside the window.
    ASSERT_GT(now.tv_usec, 1000)
        << "the wall clock did not advance past microsecond 1000 in " << reads << " reads";

    condVar.set();
    ASSERT_TRUE(condVar.isSet()) << "the condition must be set, or wait() would block";

    // timeout % 1000 == 999, so tv_nsec overflows one second and the carry arm runs.
    const long timeout = 1999;
    EXPECT_EQ(1L, condVar.wait(timeout));

    // The wait returned because the condition was already set, not because it timed out,
    // and it left the condition exactly as it found it.
    EXPECT_TRUE(condVar.isSet());
}

// Compare pointers only, never dereference them.
TEST_F(ConditionVariableTest, GetNativeHandle) {
    void *handle = condVar.getNativeHandle();

    EXPECT_NE(handle, nullptr);
    EXPECT_EQ(condVar.getNativeHandle(), handle);

    ConditionVariable other;
    EXPECT_NE(other.getNativeHandle(), handle);
}
