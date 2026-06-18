// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#ifdef __linux__
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__x86_64__) && defined(USE_UNWIND) && USE_UNWIND
#ifndef UNW_LOCAL_ONLY
#define UNW_LOCAL_ONLY
#endif
#include <libunwind.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#endif

namespace doris {

#ifdef __linux__
namespace {

#if defined(__x86_64__) && defined(USE_UNWIND) && USE_UNWIND

struct ReproducerState {
    std::atomic<pid_t> tid {0};
    std::atomic<int> callback_entered {0};
    std::atomic<int> signal_handler_entered {0};
    std::atomic<int> release_callback {0};
    std::atomic<int> release_signal_handler {0};
};

ReproducerState s_state;
ucontext_t s_saved_context {};

void reset_state() {
    s_state.tid.store(0, std::memory_order_release);
    s_state.callback_entered.store(0, std::memory_order_release);
    s_state.signal_handler_entered.store(0, std::memory_order_release);
    s_state.release_callback.store(0, std::memory_order_release);
    s_state.release_signal_handler.store(0, std::memory_order_release);
}

int reproducer_signal() {
    const int signal = SIGRTMIN + 12;
    return signal <= SIGRTMAX ? signal : -1;
}

int send_rt_signal(pid_t tid, int signal) {
    siginfo_t info {};
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    return static_cast<int>(syscall(__NR_rt_tgsigqueueinfo, getpid(), tid, signal, &info));
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void signal_handler(int /*signal*/, siginfo_t* /*info*/, void* context) {
    // 3. The interrupted thread saves its ucontext, then waits for the test thread.
    s_saved_context = *reinterpret_cast<ucontext_t*>(context);
    s_state.signal_handler_entered.store(1, std::memory_order_release);
    while (s_state.release_signal_handler.load(std::memory_order_acquire) == 0) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

class Cleanup {
public:
    Cleanup(int signal, const struct sigaction& old_action, std::thread* worker)
            : _signal(signal), _old_action(old_action), _worker(worker) {}

    ~Cleanup() {
        s_state.release_signal_handler.store(1, std::memory_order_release);
        s_state.release_callback.store(1, std::memory_order_release);
        if (_worker->joinable()) {
            _worker->join();
        }
        static_cast<void>(sigaction(_signal, &_old_action, nullptr));
    }

private:
    int _signal;
    struct sigaction _old_action {};
    std::thread* _worker;
};

#endif

} // namespace
#endif

TEST(LibunwindSignalDeadlockReproducerTest, DISABLED_ReenterDlIteratePhdrFromRtSignal) {
#if !defined(__linux__) || !defined(__x86_64__) || !defined(USE_UNWIND) || !USE_UNWIND
    GTEST_SKIP() << "loader-lock libunwind reproducer requires Linux x86_64 libunwind";
#else
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        _exit(124);
    }).detach();

    const int signal = reproducer_signal();
    ASSERT_GT(signal, 0);

    reset_state();

    struct sigaction action {};
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    ASSERT_EQ(0, sigemptyset(&action.sa_mask)) << strerror(errno);

    struct sigaction old_action {};
    ASSERT_EQ(0, sigaction(signal, &action, &old_action)) << strerror(errno);

    std::thread worker;
    Cleanup cleanup(signal, old_action, &worker);

    worker = std::thread([&] {
        s_state.tid.store(static_cast<pid_t>(syscall(SYS_gettid)), std::memory_order_release);

        auto callback = [](dl_phdr_info* /*info*/, size_t /*size*/, void* /*data*/) -> int {
            // 1. T1 enters dl_iterate_phdr(); the callback blocks while holding loader lock.
            s_state.callback_entered.store(1, std::memory_order_release);
            while (s_state.release_callback.load(std::memory_order_acquire) == 0) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            return 1;
        };
        static_cast<void>(dl_iterate_phdr(callback, nullptr));
    });

    ASSERT_TRUE(wait_until(
            [&] {
                return s_state.tid.load(std::memory_order_acquire) != 0 &&
                       s_state.callback_entered.load(std::memory_order_acquire) != 0;
            },
            std::chrono::seconds(5)));

    // 2. The test thread interrupts T1 with a real-time signal.
    const pid_t tid = s_state.tid.load(std::memory_order_acquire);
    ASSERT_EQ(0, send_rt_signal(tid, signal)) << strerror(errno);
    ASSERT_TRUE(wait_until(
            [&] { return s_state.signal_handler_entered.load(std::memory_order_acquire) != 0; },
            std::chrono::seconds(5)));

    // 4. Release the dl_iterate_phdr() callback so the callback wait is not the deadlock source.
    s_state.release_callback.store(1, std::memory_order_release);

    // 5. The test thread enters libunwind; this blocks on the loader lock held by T1.
    void* frames[64];
    const int frame_count = unw_backtrace(frames, 64);
    s_state.release_signal_handler.store(1, std::memory_order_release);
    FAIL() << "unw_backtrace returned " << frame_count
           << " frames; expected watchdog exit after loader-lock deadlock";
#endif
}

} // namespace doris
