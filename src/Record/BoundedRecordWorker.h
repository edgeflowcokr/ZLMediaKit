#pragma once
#include <algorithm>
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
#include <vector>

namespace mediakit {
// Disk calls must never execute or be joined on the socket poller. Stuck OS
// calls retain one worker slot until they return; reconnects cannot leak threads.
//
// 큐 정책(2026-09-25, SMR HDD 실사고 — docs/STREAMING_RECORDING_STALL_20260912.md "큐 정책 교체"):
// 녹화 디스크가 SMR(ST4000LM024)이면 평균 대역폭은 충분해도 쓰기 한 번이 수 초~수십 초 멈춘다
// (실측 WriteLatencyMax 65,756ms). 예전 정책은 "항목 128개(≈30fps 4.3초)" 와 "디스크 호출 1회 5초" 로
// 실패를 판정해 바이트 버퍼가 한참 남았는데도 시간당 100~450회 녹화를 끊었다. 관측된 "HDD 한계"는
// 디스크 처리량이 아니라 버퍼 정책의 산물이었다. 지금은:
//  - 바이트 예산(record.queueMaxMB)이 메모리를 지배한다. 항목마다 kItemOverheadBytes 를 함께 계상하고
//    처리 중(in-flight) 항목도 끝날 때까지 계상하므로 작업자당 상주량 ≈ 예산 이하가 성립한다
//    (NAL 단위 SEI·SPS/PPS·다중 슬라이스 같은 작은 프레임 폭주도 바이트로 막힌다).
//  - 디스크 호출 1회 정지 허용시간(record.stallToleranceMs) 이내면 큐 여유가 있는 한 녹화를 유지한다.
//  - 항목 상한은 느슨한 안전값(초당 1024 x 허용시간)일 뿐 정상 스트림에서 걸리지 않는다.
//  - 정지 초과·예산 초과로 실패하면 새 입력만 거부하고, 이미 받은 프레임은 막힌 호출이 돌아온 뒤 끝까지
//    써서(drain) 파일을 정상 마감한다. 큐는 스트림의 연속 접두부라 써도 의존 프레임이 깨지지 않는다.
//    작업(muxer) 자체가 예외를 던진 경우만 남은 큐를 버린다(계속 써도 의미가 없다).
//  - 교체(canReplace)는 여전히 drain+마감 완료 후 쿨다운이 지나야 가능하다.
struct RecordQueueLimits {
    // 항목 1개(std::function + 캐시 프레임 객체 + deque 노드)의 근사 상주 비용. 바이트 예산에 함께 계상한다.
    enum : size_t { kItemOverheadBytes = 256 };
    // 항목 상한 유도: 허용시간(초) x 1024. NAL 단위 입력(프레임당 여러 항목)도 넉넉히 덮는다.
    enum : size_t { kItemsPerSecond = 1024, kMinItems = 16384, kMaxItems = 614400 };
    // 설정 허용 범위(Recorder.cpp configured()·백엔드 mediaconfig 클램프와 동일해야 한다).
    enum : int { kDefaultQueueMB = 96, kMinQueueMB = 4, kMaxQueueMB = 256 };
    enum : int { kDefaultStallMs = 90000, kMinStallMs = 5000, kMaxStallMs = 600000 };
    // 녹화기 하나의 상주 바이트 예산(페이로드 + 항목당 오버헤드, 처리 중 항목 포함).
    // 크기 규칙: 예산 >= 비트레이트 x 허용시간 x 1.2. .5 실측 스트림 최대 6.8Mbps → 6.8/8 x 90 x 1.2 ≈ 92MB → 96MiB.
    // 최악 상주 메모리 = 전역 작업자 상한 32 x 예산(기본 3GiB). 실제는 (녹화 채널 + 진행 중 이벤트 클립) x 예산.
    size_t max_bytes = static_cast<size_t>(kDefaultQueueMB) * 1024 * 1024;
    // 디스크 호출 1회가 이 시간을 넘기면 작업자 실패(큐 여유와 무관). 이하이면 큐 여유가 있는 한 정상.
    // 기본 90초 = SMR 실측 최대 정지 65.7초 + 여유.
    long long stall_tolerance_ms = kDefaultStallMs;
    size_t max_items = itemsFor(kDefaultStallMs);

    static size_t itemsFor(long long tolerance_ms) {
        // 곱하기 전에 허용시간부터 범위로 묶는다(오버플로 방지).
        const long long ms = tolerance_ms < 0 ? 0 : (tolerance_ms > kMaxStallMs ? static_cast<long long>(kMaxStallMs) : tolerance_ms);
        const size_t items = static_cast<size_t>((ms + 999) / 1000) * static_cast<size_t>(kItemsPerSecond);
        const size_t lo = kMinItems, hi = kMaxItems;
        return items < lo ? lo : (items > hi ? hi : items);
    }
    static int clampSetting(int raw, int def, int lo, int hi) {
        return raw <= 0 ? def : (raw < lo ? lo : (raw > hi ? hi : raw));
    }
    // 설정값 → 한도(순수 함수). 0 이하·파싱 실패는 기본값, 범위 밖은 경계값. 바뀐 값이 있으면 warning 에 사유.
    static RecordQueueLimits fromSettings(int queue_mb, int stall_ms, std::string* warning = nullptr) {
        const int mb = clampSetting(queue_mb, kDefaultQueueMB, kMinQueueMB, kMaxQueueMB);
        const int ms = clampSetting(stall_ms, kDefaultStallMs, kMinStallMs, kMaxStallMs);
        if (warning) {
            warning->clear();
            if (mb != queue_mb)
                *warning += "record.queueMaxMB=" + std::to_string(queue_mb) + " out of range [" + std::to_string(int(kMinQueueMB)) + ","
                    + std::to_string(int(kMaxQueueMB)) + "] (<=0 -> default " + std::to_string(int(kDefaultQueueMB)) + "); using " + std::to_string(mb) + ". ";
            if (ms != stall_ms)
                *warning += "record.stallToleranceMs=" + std::to_string(stall_ms) + " out of range [" + std::to_string(int(kMinStallMs)) + ","
                    + std::to_string(int(kMaxStallMs)) + "] (<=0 -> default " + std::to_string(int(kDefaultStallMs)) + "); using " + std::to_string(ms) + ".";
        }
        RecordQueueLimits limits;
        limits.max_bytes = static_cast<size_t>(mb) * 1024 * 1024;
        limits.stall_tolerance_ms = ms;
        limits.max_items = itemsFor(ms);
        return limits;
    }
    // config.ini [record] queueMaxMB / stallToleranceMs 를 호출 스레드에서 GET_CONFIG 로 읽어 fromSettings 로
    // 클램프(바뀌면 로그). 정의는 Recorder.cpp(이 헤더는 config 의존 없이 단독 회귀 시험 가능).
    // 설정 재적재(reload)는 이후 생성되는 녹화기부터 반영된다(작업자는 생성 시 값을 고정).
    static RecordQueueLimits configured();
};

// 이벤트 프리버퍼(pre-event) 자르기 지점 계산(순수 함수 — AsyncEventMP4 와 회귀 시험 공용).
// 계상 = 프레임 페이로드 + 프레임당 per_frame. 합이 cap 이하이면 그대로. 넘으면 가장 오래된 GOP 부터 버려
// cap 이하가 되는 가장 이른 GOP 경계에서 시작한다. GOP 경계 = 설정(SPS/PPS)·키프레임 연속 구간의 첫 영상
// 프레임이되, 그 구간 안에 실제 키프레임이 있을 때만 인정한다(설정 프레임만 있는 구간은 디코드 시작점이 아님).
// 영상이 없으면(오디오만) 어느 프레임이든 경계다. 마지막 GOP 하나가 cap 을 넘으면 hard_limit 이하일 때만 그
// GOP 로 시작하고, 그것도 넘으면(또는 영상인데 키프레임 경계가 없으면) ok=false(이력 표현 불가).
struct RecordHistoryFrame { size_t bytes; bool video; bool head; bool key; }; // head = configFrame() || keyFrame(), key = keyFrame()
struct RecordHistoryTrim { bool ok = true; size_t start = 0, total_bytes = 0, kept_bytes = 0; };
inline RecordHistoryTrim recordHistoryTrim(const std::vector<RecordHistoryFrame>& frames, size_t cap, size_t hard_limit, size_t per_frame) {
    RecordHistoryTrim out;
    std::vector<size_t> suffix(frames.size() + 1, 0); // suffix[i] = frames[i..] 계상 바이트
    for (size_t i = frames.size(); i-- > 0;) suffix[i] = suffix[i + 1] + frames[i].bytes + per_frame;
    out.total_bytes = out.kept_bytes = suffix[0];
    if (suffix[0] <= cap) return out;
    const size_t npos = static_cast<size_t>(-1);
    size_t chosen = npos, last_boundary = npos;
    bool has_video = false, prev_head = false, run_has_key = false;
    size_t run_start = npos;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].video) continue;
        has_video = true;
        if (frames[i].head) {
            if (!prev_head) { run_start = i; run_has_key = false; }
            if (frames[i].key && !run_has_key) {
                run_has_key = true; // 이 구간은 실제 키프레임을 가진 GOP 시작
                last_boundary = run_start;
                if (suffix[run_start] <= cap) { chosen = run_start; break; }
            }
        }
        prev_head = frames[i].head;
    }
    if (!has_video)
        for (size_t i = 0; i < frames.size() && chosen == npos; ++i)
            if (suffix[i] <= cap) chosen = i;
    if (chosen == npos && last_boundary != npos && suffix[last_boundary] <= hard_limit) chosen = last_boundary;
    if (chosen == npos) { out.ok = false; return out; }
    out.start = chosen;
    out.kept_bytes = suffix[chosen];
    return out;
}

class BoundedRecordWorker {
public:
    using Limits = RecordQueueLimits;
    enum : size_t { kItemOverheadBytes = RecordQueueLimits::kItemOverheadBytes };
    // 계측 스냅샷. queued_* 는 현재 상주량(대기 + 처리 중, 오버헤드 포함), peak_* 는 작업자 수명 동안 최대값.
    struct Stats {
        size_t queued_bytes = 0, queued_items = 0;
        size_t peak_bytes = 0, peak_items = 0;
        long long peak_call_ms = 0, active_task_ms = 0;
        Limits limits;
        bool failed = false;
    };
    // [recording-queue] 계측 한 줄(채널 태그 없이 필드만)과 경고 수준 여부.
    using Observer = std::function<void(const std::string& fields, bool warn)>;
    // Live = 실시간 입력(사용률 돌파 보고 대상). Baseline = 이벤트 프리버퍼 같은 일괄 선적재: 그 직후 수위를
    // 기준선으로 삼아 돌파 보고를 하지 않는다(이벤트마다 cross50/80 경고가 뜨지 않게).
    enum class Admission { Live, Baseline };
    static constexpr long long kReportIntervalMs = 60000;

private:
    struct Item { std::function<void()> run; size_t bytes; };
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Item> tasks;
        size_t bytes = 0;          // 대기 + 처리 중 항목의 계상 바이트(오버헤드 포함)
        size_t inflight_items = 0; // 처리 중 항목(0 또는 1). 그 바이트는 끝날 때까지 bytes 에 남는다
        bool stop = false;
        std::atomic<bool> failed{false};
        std::atomic<long long> started{0};
        std::atomic<long long> finished{0};
        std::function<void(const char*)> report;
        std::function<void()> finalize;
        Limits limits;
        Observer observe;
        // 실패 후 drain/폐기 집계(마감 시 [recording-queue] trigger=drained 로 보고). drained = 실패 시점 이후 예외 없이
        // 끝난 항목(실패 순간 처리 중이던 항목 포함), dropped = muxer 예외로 버린 대기 항목.
        long long failed_at = 0;
        size_t drained_items = 0, drained_bytes = 0, dropped_items = 0, dropped_bytes = 0;
        // 계측(보고 창 = kReportIntervalMs). win_* 는 창마다 초기화, peak_* 는 누적.
        size_t win_hw_bytes = 0, win_hw_items = 0, peak_bytes = 0, peak_items = 0;
        std::atomic<long long> win_max_call_ms{0}, peak_call_ms{0};
        long long last_report = 0;
        int reported_level = 0;       // 이번 창에서 이미 보고한(또는 기준선으로 삼은) 사용률 단계(1=50%, 2=80%)
        bool win_has_baseline = false; // 이번 창에 일괄 선적재가 있었음 → 창 종료 보고는 수위로 경고하지 않음
    };
    std::shared_ptr<State> _state;
    static std::atomic<unsigned>& slots() { static std::atomic<unsigned> count{0}; return count; }
    static long long now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    static void raiseMax(std::atomic<long long>& target, long long value) {
        auto cur = target.load();
        while (value > cur && !target.compare_exchange_weak(cur, value)) {}
    }
    static std::string limitFields(const Limits& limits) {
        return " budget_bytes=" + std::to_string(limits.max_bytes)
            + " item_cap=" + std::to_string(limits.max_items)
            + " tolerance_ms=" + std::to_string(limits.stall_tolerance_ms);
    }
    size_t residentItems() const { return _state->tasks.size() + _state->inflight_items; }
public:
    BoundedRecordWorker(std::function<void()> finalize, std::function<void(const char*)> report,
                        Limits limits = Limits(), Observer observe = nullptr) : _state(std::make_shared<State>()) {
        auto state = _state;
        state->report = std::move(report);
        state->finalize = std::move(finalize);
        state->limits = limits;
        state->observe = std::move(observe);
        state->last_report = now();
        if (slots().fetch_add(1) >= 32) { --slots(); throw std::runtime_error("record worker limit reached (32); storage may be stalled"); }
        try {
            std::thread([state]() {
                for (;;) {
                    Item item;
                    {
                        std::unique_lock<std::mutex> lock(state->mutex);
                        state->ready.wait(lock, [&]() { return state->stop || !state->tasks.empty(); });
                        if (state->tasks.empty() && state->stop) break;
                        // 처리 중 항목도 끝날 때까지 예산에 남긴다(상주 메모리 = 예산 이하 보장).
                        item = std::move(state->tasks.front()); state->tasks.pop_front(); state->inflight_items = 1;
                    }
                    const auto begin = now();
                    state->started = begin;
                    std::string error;
                    bool threw = false;
                    try { item.run(); }
                    catch (const std::exception& ex) { threw = true; error = ex.what(); }
                    catch (...) { threw = true; error = "non-standard recording exception"; }
                    state->started = 0;
                    const auto elapsed = now() - begin;
                    raiseMax(state->win_max_call_ms, elapsed);
                    raiseMax(state->peak_call_ms, elapsed);
                    item.run = nullptr; // 캡처한 프레임을 계상 해제 전에 놓는다
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->bytes -= item.bytes; state->inflight_items = 0;
                        if (state->failed && !threw) { ++state->drained_items; state->drained_bytes += item.bytes; }
                        if (threw) {
                            // muxer 예외: 이후 프레임을 써도 의미가 없으므로 남은 큐를 버린다.
                            if (!state->failed) state->failed_at = now();
                            state->failed = true; state->stop = true;
                            state->dropped_items += state->tasks.size(); state->dropped_bytes += state->bytes;
                            error += " dropped_items=" + std::to_string(state->tasks.size()) + " dropped_bytes=" + std::to_string(state->bytes);
                            state->tasks.clear(); state->bytes = 0;
                        }
                    }
                    if (threw) state->report(error.c_str());
                }
                try { state->finalize(); }
                catch (const std::exception& ex) { state->report(ex.what()); }
                catch (...) { state->report("record finalization exception"); }
                // Release captured recorder on this worker, never on the caller.
                state->finalize = nullptr;
                const auto done = now();
                if (state->failed && state->observe) {
                    std::string fields;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        fields = "trigger=drained drained_items=" + std::to_string(state->drained_items)
                            + " drained_bytes=" + std::to_string(state->drained_bytes)
                            + " dropped_items=" + std::to_string(state->dropped_items)
                            + " dropped_bytes=" + std::to_string(state->dropped_bytes)
                            + " drain_ms=" + std::to_string(state->failed_at ? done - state->failed_at : 0)
                            + limitFields(state->limits);
                    }
                    try { state->observe(fields, true); } catch (...) {} // 계측 실패가 슬롯 반환을 막으면 안 된다
                }
                --slots();
                state->finished = done;
            }).detach();
        } catch (...) { --slots(); throw; }
    }
    ~BoundedRecordWorker() { close(); }
    BoundedRecordWorker(const BoundedRecordWorker&) = delete;
    BoundedRecordWorker& operator=(const BoundedRecordWorker&) = delete;
    const Limits& limits() const { return _state->limits; }
    // 허용시간 안의 느린 디스크 호출은 실패가 아니다(SMR 정지). 넘기면 실패로 본다.
    bool healthy() const {
        const auto started = _state->started.load();
        return !_state->failed && (!started || now() - started <= _state->limits.stall_tolerance_ms);
    }
    // Never replace a recorder while its old disk call/finalizer owns the file.
    // Cooldown also bounds repeated attempts on an unavailable storage device.
    bool canReplace(long long cooldown_ms = 30000) const {
        const auto finished = _state->finished.load();
        return _state->failed && finished && now() - finished >= cooldown_ms;
    }
    Stats stats() const {
        Stats out;
        std::lock_guard<std::mutex> lock(_state->mutex);
        const auto started = _state->started.load();
        out.queued_bytes = _state->bytes; out.queued_items = residentItems();
        out.peak_bytes = _state->peak_bytes; out.peak_items = _state->peak_items;
        out.active_task_ms = started ? now() - started : 0;
        out.peak_call_ms = (std::max)(_state->peak_call_ms.load(), out.active_task_ms);
        out.limits = _state->limits; out.failed = _state->failed;
        return out;
    }
    // bytes = 페이로드 바이트(일괄 항목이면 그 안의 프레임별 오버헤드까지 호출자가 포함). 항목 오버헤드는 여기서 더한다.
    bool submit(std::function<void()> task, size_t bytes = 0, Admission admission = Admission::Live) {
        bool failure = false;
        std::string reason, observed;
        bool warn = false;
        const size_t cost = bytes + kItemOverheadBytes;
        {
            std::lock_guard<std::mutex> lock(_state->mutex);
            if (_state->stop) return false;
            const auto& limits = _state->limits;
            const bool is_healthy = healthy(); // 한 번만 평가(사유 오분류 방지)
            const bool items_full = residentItems() >= limits.max_items;
            if (!is_healthy || items_full || cost > limits.max_bytes - _state->bytes) {
                const auto started = _state->started.load();
                // 이 줄 형식([recording-degraded] ... reason=recording stopped: cause=...)은 다른 도구가 grep 한다.
                // 필드는 "; live reception preserved" 앞에 추가만 한다.
                reason = "recording stopped: cause=";
                reason += !is_healthy ? "disk_task_timeout_or_worker_failed" : (items_full ? "queue_items_limit" : "queue_bytes_limit");
                reason += " queued_items=" + std::to_string(residentItems())
                    + " queued_bytes=" + std::to_string(_state->bytes)
                    + " incoming_bytes=" + std::to_string(bytes)
                    + " active_task_ms=" + std::to_string(started ? now() - started : 0)
                    + limitFields(limits)
                    // 새 입력만 거부. 이미 받은 항목(대기 + 처리 중)은 막힌 호출이 돌아오면 써서 마감한다(drain).
                    // 집계 기준은 마감 시 trigger=drained 의 drained_items/bytes 와 같다(둘 다 처리 중 항목 포함).
                    + " drain_items=" + std::to_string(residentItems())
                    + " drain_bytes=" + std::to_string(_state->bytes)
                    + "; live reception preserved";
                if (!_state->failed) _state->failed_at = now();
                _state->failed = true; _state->stop = true; failure = true;
            } else {
                _state->tasks.push_back({std::move(task), cost}); _state->bytes += cost;
                observed = sample(admission == Admission::Baseline, warn);
            }
        }
        _state->ready.notify_one();
        if (failure) _state->report(reason.c_str());
        else if (!observed.empty() && _state->observe) _state->observe(observed, warn);
        return !failure;
    }
    void close() { std::lock_guard<std::mutex> lock(_state->mutex); _state->stop = true; _state->ready.notify_one(); }

private:
    // mutex 보유 상태에서 호출. 보고할 게 있으면 필드 문자열을 돌려준다(로그 출력은 락 밖에서).
    // 보고 조건(작업자당, 창 60초): 사용률이 50%/80% 를 처음 넘은 순간 각 1회 + 창 종료 시 1회
    // (단 창 최고 사용률 >= 10% 또는 최장 디스크 호출 >= 1초일 때만 — 한가한 채널은 조용히).
    std::string sample(bool baseline, bool& warn) {
        auto& s = *_state;
        const auto t = now();
        const size_t items = residentItems();
        s.win_hw_bytes = (std::max)(s.win_hw_bytes, s.bytes);
        s.win_hw_items = (std::max)(s.win_hw_items, items);
        s.peak_bytes = (std::max)(s.peak_bytes, s.bytes);
        s.peak_items = (std::max)(s.peak_items, items);
        const size_t budget = s.limits.max_bytes ? s.limits.max_bytes : 1;
        const int level = s.bytes * 100 >= budget * 80 ? 2 : (s.bytes * 100 >= budget * 50 ? 1 : 0);
        if (baseline) {
            // 일괄 선적재 직후 수위를 기준선으로: 이 수위까지는 돌파 보고하지 않는다.
            s.reported_level = (std::max)(s.reported_level, level);
            s.win_has_baseline = true;
            return std::string();
        }
        const char* trigger = nullptr;
        bool periodic = false;
        if (level > s.reported_level) {
            trigger = level == 2 ? "cross80" : "cross50";
            s.reported_level = level;
        } else if (t - s.last_report >= kReportIntervalMs) {
            trigger = "periodic";
            periodic = true;
        }
        if (!trigger) return std::string();
        const auto started = s.started.load();
        const auto active_ms = started ? t - started : 0;
        const auto max_call_ms = (std::max)(s.win_max_call_ms.load(), active_ms);
        const auto hw_pct = s.win_hw_bytes * 100 / budget;
        std::string fields = "trigger=" + std::string(trigger)
            + " hw_bytes=" + std::to_string(s.win_hw_bytes)
            + " hw_pct=" + std::to_string(hw_pct)
            + " hw_items=" + std::to_string(s.win_hw_items)
            + " max_call_ms=" + std::to_string(max_call_ms)
            + " queued_bytes=" + std::to_string(s.bytes)
            + " queued_items=" + std::to_string(items)
            + " window_ms=" + std::to_string(t - s.last_report)
            + limitFields(s.limits);
        const bool trivial = hw_pct < 10 && max_call_ms < 1000;
        const bool slow_call = max_call_ms * 2 >= s.limits.stall_tolerance_ms;
        // 창에 일괄 선적재가 있었으면 창 최고수위는 선적재 몫이므로 수위로는 경고하지 않는다.
        warn = slow_call || (periodic ? (!s.win_has_baseline && hw_pct >= 50) : true);
        if (periodic) {
            s.last_report = t;
            s.win_hw_bytes = s.bytes;
            s.win_hw_items = items;
            s.win_max_call_ms.exchange(0);
            s.reported_level = 0;
            s.win_has_baseline = false;
            if (trivial) return std::string();
        }
        return fields;
    }
};
}
