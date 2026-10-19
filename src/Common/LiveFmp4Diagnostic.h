#pragma once
#include <atomic>
#include <cstdint>

namespace mediakit {
// 라이브 fMP4 전용 상수 크기 진단. source poller와 HTTP poller가 공유하므로
// 카운터는 relaxed atomic이며 여러 필드가 한 시점의 원자적 트랜잭션은 아니다.
// 연결 시작/종료 차이로 비교하고, demand OFF 누계를 활성 출력 정지로 오해하지 않는다.
struct LiveFmp4Snapshot {
    uint64_t input=0,enabled=0,no_init=0,missing_track=0,waiting_key=0,merged=0;
    uint64_t stamp_rejected=0,writer_failed=0,empty_output=0,fragments=0,ring_batches=0,ring_generation=0;
    uint64_t input_at=0,enabled_at=0,fragment_at=0,ring_at=0;
};
struct LiveFmp4Diagnostic {
    std::atomic<uint64_t> input{0},enabled{0},no_init{0},missing_track{0},waiting_key{0},merged{0};
    std::atomic<uint64_t> stamp_rejected{0},writer_failed{0},empty_output{0},fragments{0},ring_batches{0},ring_generation{0};
    std::atomic<uint64_t> input_at{0},enabled_at{0},fragment_at{0},ring_at{0};
    LiveFmp4Snapshot snapshot() const {
        LiveFmp4Snapshot s;
#define EF_FMP4_LOAD(field) s.field=field.load(std::memory_order_relaxed)
        EF_FMP4_LOAD(input);EF_FMP4_LOAD(enabled);EF_FMP4_LOAD(no_init);EF_FMP4_LOAD(missing_track);
        EF_FMP4_LOAD(waiting_key);EF_FMP4_LOAD(merged);EF_FMP4_LOAD(stamp_rejected);EF_FMP4_LOAD(writer_failed);
        EF_FMP4_LOAD(empty_output);EF_FMP4_LOAD(fragments);EF_FMP4_LOAD(ring_batches);EF_FMP4_LOAD(ring_generation);
        EF_FMP4_LOAD(input_at);EF_FMP4_LOAD(enabled_at);EF_FMP4_LOAD(fragment_at);EF_FMP4_LOAD(ring_at);
#undef EF_FMP4_LOAD
        return s;
    }
    static uint64_t delta(uint64_t end,uint64_t begin) { return end>=begin?end-begin:0; }
};
} // namespace mediakit
