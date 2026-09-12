#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace mediakit {
// Disk calls must never execute or be joined on the socket poller. Stuck OS
// calls retain one worker slot until they return; reconnects cannot leak threads.
class BoundedRecordWorker {
    struct Item { std::function<void()> run; size_t bytes; };
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Item> tasks;
        size_t bytes = 0;
        bool stop = false;
        std::atomic<bool> failed{false};
        std::atomic<long long> started{0};
        std::function<void(const char*)> report;
        std::function<void()> finalize;
    };
    std::shared_ptr<State> _state;
    static std::atomic<unsigned>& slots() { static std::atomic<unsigned> count{0}; return count; }
    static long long now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
public:
    static constexpr size_t max_items = 128;
    static constexpr size_t max_bytes = 16 * 1024 * 1024;
    BoundedRecordWorker(std::function<void()> finalize, std::function<void(const char*)> report) : _state(std::make_shared<State>()) {
        auto state = _state;
        state->report = std::move(report);
        state->finalize = std::move(finalize);
        if (slots().fetch_add(1) >= 32) { --slots(); throw std::runtime_error("record worker limit reached (32); storage may be stalled"); }
        try {
            std::thread([state]() {
                for (;;) {
                    Item item;
                    {
                        std::unique_lock<std::mutex> lock(state->mutex);
                        state->ready.wait(lock, [&]() { return state->stop || !state->tasks.empty(); });
                        if (state->tasks.empty() && state->stop) break;
                        item = std::move(state->tasks.front()); state->tasks.pop_front(); state->bytes -= item.bytes;
                    }
                    state->started = now();
                    try { item.run(); }
                    catch (const std::exception& ex) { state->failed = true; state->report(ex.what()); }
                    catch (...) { state->failed = true; state->report("non-standard recording exception"); }
                    state->started = 0;
                    if (state->failed) { std::lock_guard<std::mutex> lock(state->mutex); state->tasks.clear(); state->bytes = 0; state->stop = true; }
                }
                try { state->finalize(); }
                catch (const std::exception& ex) { state->report(ex.what()); }
                catch (...) { state->report("record finalization exception"); }
                // Release captured recorder on this worker, never on the caller.
                state->finalize = nullptr;
                --slots();
            }).detach();
        } catch (...) { --slots(); throw; }
    }
    ~BoundedRecordWorker() { close(); }
    BoundedRecordWorker(const BoundedRecordWorker&) = delete;
    BoundedRecordWorker& operator=(const BoundedRecordWorker&) = delete;
    bool healthy() const {
        const auto started = _state->started.load();
        return !_state->failed && (!started || now() - started <= 5000);
    }
    bool submit(std::function<void()> task, size_t bytes = 0) {
        bool failure = false;
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(_state->mutex);
            if (_state->stop) return false;
            if (!healthy() || _state->tasks.size() >= max_items || bytes > max_bytes - _state->bytes) {
                const auto started = _state->started.load();
                reason = "recording stopped: cause=";
                reason += !healthy() ? "disk_task_timeout_or_worker_failed" :
                          (_state->tasks.size() >= max_items ? "queue_items_limit" : "queue_bytes_limit");
                reason += " queued_items=" + std::to_string(_state->tasks.size())
                    + " queued_bytes=" + std::to_string(_state->bytes)
                    + " incoming_bytes=" + std::to_string(bytes)
                    + " active_task_ms=" + std::to_string(started ? now() - started : 0)
                    + "; live reception preserved";
                _state->failed = true; _state->stop = true; _state->tasks.clear(); _state->bytes = 0; failure = true;
            } else { _state->tasks.push_back({std::move(task), bytes}); _state->bytes += bytes; }
        }
        _state->ready.notify_one();
        if (failure) _state->report(reason.c_str());
        return !failure;
    }
    void close() { std::lock_guard<std::mutex> lock(_state->mutex); _state->stop = true; _state->ready.notify_one(); }
};
}
