#ifndef MEDIAKIT_RECORD_IO_TRACE_H
#define MEDIAKIT_RECORD_IO_TRACE_H

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace mediakit {

// 정상 호출은 시계 두 번 외에 로그·잠금·저장이 없다. 파일/영상/문자 내용은 받지 않는다.
enum class RecordIoOp { Create, Read, Write, Seek, Tell, Flush, Close, MuxerClose, Rename, FileSize, Remove, Mtime, Count };

inline const char *recordIoName(RecordIoOp op) {
    static const char *names[] = {"create_file", "fread", "fwrite", "fseek", "ftell", "fflush", "fclose", "muxer_close", "rename", "file_size", "delete_file", "mtime"};
    auto i = static_cast<size_t>(op);
    return i < static_cast<size_t>(RecordIoOp::Count) ? names[i] : "invalid";
}

struct RecordIoSample {
    RecordIoOp op;
    uint64_t file_id, elapsed_ms, bytes, suppressed;
    char volume;
    bool failed;
    int error;
};

// 프로세스 전체 연산별60초당1줄(최대12줄/분). 파일 수에 비례하는 맵을 만들지 않는다.
class RecordIoLimiter {
public:
    RecordIoLimiter() { for (auto &v : _next) v.store(0); for (auto &v : _suppressed) v.store(0); }
    bool claim(RecordIoOp op, uint64_t now_ms, uint64_t &suppressed) {
        auto i = static_cast<size_t>(op);
        if (i >= _next.size()) return false;
        auto next = _next[i].load(std::memory_order_relaxed);
        while (now_ms >= next) {
            if (_next[i].compare_exchange_weak(next, now_ms + 60000, std::memory_order_relaxed)) {
                suppressed = _suppressed[i].exchange(0, std::memory_order_relaxed);
                return true;
            }
        }
        _suppressed[i].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
private:
    std::array<std::atomic<uint64_t>, static_cast<size_t>(RecordIoOp::Count)> _next, _suppressed;
};

class RecordIoTrace {
public:
    using Reporter = std::function<void(const RecordIoSample &)>;
    explicit RecordIoTrace(char volume, Reporter report, RecordIoLimiter &limiter = globalLimiter())
        : _volume(volume >= 'A' && volume <= 'Z' ? volume : '?'), _id(nextId()), _report(std::move(report)), _limiter(limiter) {}

    // 경로에서 드라이브 문자만 취한다. UNC/URL/파일명·채널명은 로그로 넘기지 않는다.
    static char volume(const char *path) {
        if (!path || !path[0] || path[1] != ':') return '?';
        char c = path[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return c >= 'A' && c <= 'Z' ? c : '?';
    }
    static uint64_t now() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void record(RecordIoOp op, uint64_t elapsed_ms, uint64_t bytes, bool failed, int error, uint64_t now_ms) noexcept {
        if (!failed && elapsed_ms < 1000) return;
        uint64_t suppressed = 0;
        if (!_limiter.claim(op, now_ms, suppressed)) return;
        try { if (_report) _report({op, _id, elapsed_ms, bytes, suppressed, _volume, failed, failed ? error : 0}); }
        catch (...) { /* 진단 실패가 원래 파일 반환값·예외·마감을 바꾸면 안 된다. */ }
    }
    template <class Fn, class Failed> auto measure(RecordIoOp op, uint64_t bytes, Fn fn, Failed failed) -> decltype(fn()) {
        const auto start = now();
        try {
            auto result = fn();
            const int saved_errno = errno;
            const bool error = failed(result);
            const auto end = now();
            record(op, end - start, bytes, error, saved_errno, end);
            errno = saved_errno;
            return result;
        } catch (...) {
            const int saved_errno = errno;
            const auto end = now();
            record(op, end - start, bytes, true, saved_errno, end);
            errno = saved_errno;
            throw;
        }
    }
private:
    static uint64_t nextId() { static std::atomic<uint64_t> id{0}; return ++id; }
    static RecordIoLimiter &globalLimiter() { static RecordIoLimiter limiter; return limiter; }
    char _volume;
    uint64_t _id;
    Reporter _report;
    RecordIoLimiter &_limiter;
};

} // namespace mediakit
#endif
