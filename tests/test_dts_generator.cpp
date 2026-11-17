#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>
#include "Common/Stamp.h"

using namespace mediakit;

namespace {

constexpr size_t kFrameCount = 300;
constexpr uint64_t kBasePts = 10000;
constexpr int64_t kMinSpan = 9500;
constexpr int64_t kMaxSpan = 10500;

enum class StartPosition { B1, P, I };

const char *startName(StartPosition start) {
    switch (start) {
    case StartPosition::B1: return "B1 시작";
    case StartPosition::P: return "P 시작";
    case StartPosition::I: return "I 시작";
    }
    return "알 수 없는 시작 위치";
}

uint64_t ptsAtDisplayFrame(size_t frame) {
    // 0을 미입력으로 취급하는 기존 경로를 피하고, 30fps의 소수 밀리초는 버림한다.
    return kBasePts + static_cast<uint64_t>(frame) * 1000 / 30;
}

size_t displayFrameAt(size_t decode_frame, size_t b_count) {
    if (!decode_frame) return 0;
    const size_t stride = b_count + 1;
    const size_t group = (decode_frame - 1) / stride;
    const size_t member = (decode_frame - 1) % stride;
    // I0 다음에는 미래 참조 P, 그 참조보다 앞선 B1..BM 순서로 디코드한다.
    return member ? group * stride + member : (group + 1) * stride;
}

std::vector<uint64_t> makePts(StartPosition start, size_t b_count) {
    std::vector<uint64_t> pts;
    pts.reserve(kFrameCount);
    const size_t skip = start == StartPosition::I ? 0 : start == StartPosition::P ? 1 : 2;
    for (size_t frame = 0; frame < kFrameCount; ++frame) {
        if (!b_count) {
            // M=0에는 B1이 없다. 해당 이름의 경우도 명시적으로 순차 수열을 사용한다.
            pts.emplace_back(ptsAtDisplayFrame(frame + (start == StartPosition::I ? 0 : 1)));
        } else {
            pts.emplace_back(ptsAtDisplayFrame(displayFrameAt(frame + skip, b_count)));
        }
    }
    // M=2에서 I 시작은 0,3,1,2,6,4,5,9,..., P 시작은 3,1,2,6,4,5,9,...다.
    // B1 시작은 1,2,6,4,5,9,...로, 처음 1→2→6이 모두 증가하여 기존 판정이
    // 정렬 크기 1에 잠긴다. 이후 P→B1의 역행을 Relative가 직전 양의 증가량으로
    // 대체하면 한 묶음의 +133,-67,+33ms가 약 +133,+133,+33ms로 바뀐다.
    // 따라서 100ms 입력마다 약 300ms가 흘러 수정 전 10초가 약 30초가 된다.
    return pts;
}

std::vector<uint64_t> makeDelayedBPts() {
    std::vector<uint64_t> pts;
    pts.reserve(kFrameCount);
    constexpr size_t prefix_frames = 90;
    for (size_t frame = 0; frame < kFrameCount; ++frame) {
        const size_t display_frame = frame < prefix_frames
            ? frame
            : prefix_frames - 1 + displayFrameAt(frame - prefix_frames + 1, 2);
        pts.emplace_back(ptsAtDisplayFrame(display_frame));
    }
    // 0..89 다음 92,90,91,95,93,94,...다. 크기 1 판정 뒤에도 최대 PTS를
    // 계속 갱신해야 첫 B의 92→90 역행을 놓치지 않는다.
    return pts;
}

struct FrameResult {
    FrameResult(const char *start_name, size_t count) : start(start_name), b_count(count) {}

    const char *start;
    size_t b_count;
    size_t frame = 0;
    uint64_t input_pts = 0;
    uint64_t generated_dts = 0;
    bool determined = false;
    int64_t relative_dts = 0;
    int64_t relative_pts = 0;
};

int expect(bool ok, const FrameResult &result, const char *reason) {
    if (ok) return 0;
    std::cerr << "실패: 시작 위치=" << result.start << ", M=" << result.b_count
              << ", 프레임=" << result.frame << ", 입력PTS=" << result.input_pts
              << ", 생성DTS=" << result.generated_dts
              << ", DTS 반환=" << (result.determined ? "참" : "거짓")
              << ", 상대DTS=" << result.relative_dts << ", 상대PTS=" << result.relative_pts
              << ", 실패 이유=" << reason << '\n';
    return 1;
}

int check(const char *start, size_t b_count, const std::vector<uint64_t> &input,
          bool repeat_pts = false) {
    DtsGenerator generator;
    Stamp stamp;
    FrameResult result{start, b_count};
    int failures = 0;
    size_t undetermined_frames = 0;
    uint64_t previous_generated_dts = 0;
    bool previous_determined = false;
    int64_t first_relative_dts = 0;
    int64_t previous_relative_dts = 0;
    int64_t min_relative_pts = 0;
    int64_t max_relative_pts = 0;
    for (size_t frame = 0; frame < input.size(); ++frame) {
        result.frame = frame;
        result.input_pts = input[frame];
        result.determined = generator.getDts(result.input_pts, result.generated_dts);
        // 첫 판정 전 dts=pts와 재판정 중 보류값을 포함해, 실제 출력을 가공하거나 버리지 않는다.
        // false는 시스템 시각 대체를 사용하지 않는 Relative 경로다.
        stamp.revise(result.generated_dts, result.input_pts,
                     result.relative_dts, result.relative_pts, false);

        failures += expect(result.generated_dts <= result.input_pts, result,
                           "생성 DTS가 입력 PTS보다 큼");
        if (!result.determined) {
            ++undetermined_frames;
        }
        // 참 반환 전에도 크기 1 판정과 재판정이 일어날 수 있으므로, 반환값으로
        // 최초 판정 여부를 추측하지 않는다. dts=pts 계약은 순차 수열에서 확인한다.
        if (!b_count) {
            failures += expect(result.generated_dts == result.input_pts, result,
                               "B가 없는 순차 수열에서 기존 dts=pts 계약을 지키지 않음");
        }
        // 미확정 임시 DTS는 원래 역행할 수 있고, 최초 확정값도 그 임시값보다 작을 수
        // 있다. 생성기 자체의 비감소 검사는 연속 참 반환 구간에 적용한다.
        if (frame && previous_determined && result.determined) {
            failures += expect(result.generated_dts >= previous_generated_dts, result,
                               "연속 참 반환 구간에서 생성 DTS가 역행함");
        }
        // Relative 출력 불변식은 판정 여부에 관계없이 첫 프레임부터 전부 검사한다.
        failures += expect(result.relative_dts <= result.relative_pts, result,
                           "상대 DTS가 상대 PTS보다 큼");
        if (frame) {
            failures += expect(result.relative_dts >= previous_relative_dts, result,
                               "상대 DTS가 직전 프레임보다 작음");
        } else {
            first_relative_dts = result.relative_dts;
            min_relative_pts = max_relative_pts = result.relative_pts;
        }
        min_relative_pts = (std::min)(min_relative_pts, result.relative_pts);
        max_relative_pts = (std::max)(max_relative_pts, result.relative_pts);
        if (repeat_pts) {
            FrameResult repeated = result;
            repeated.determined = generator.getDts(repeated.input_pts, repeated.generated_dts);
            stamp.revise(repeated.generated_dts, repeated.input_pts,
                         repeated.relative_dts, repeated.relative_pts, false);
            // 같은 화면의 여러 조각은 판정 반환값이 달라도 DTS와 상대 시각이 같아야 한다.
            failures += expect(repeated.generated_dts == result.generated_dts, repeated,
                               "동일 PTS 반복에서 생성 DTS가 달라짐");
            failures += expect(repeated.relative_dts == result.relative_dts
                                   && repeated.relative_pts == result.relative_pts,
                               repeated, "동일 PTS 반복이 상대 시각을 진행시킴");
            failures += expect(repeated.generated_dts <= repeated.input_pts
                                   && repeated.relative_dts <= repeated.relative_pts,
                               repeated, "동일 PTS 반복에서 DTS가 PTS보다 큼");
        }
        previous_generated_dts = result.generated_dts;
        previous_determined = result.determined;
        previous_relative_dts = result.relative_dts;
    }

    const int64_t dts_span = result.relative_dts - first_relative_dts;
    // 표시 순서가 바뀌는 PTS는 최솟값과 최댓값으로 전체 시간 범위를 구한다.
    const int64_t pts_span = max_relative_pts - min_relative_pts;
    failures += expect(input.size() == kFrameCount, result, "300프레임을 모두 검증하지 못함");
    failures += expect(result.determined, result, "마지막 프레임의 DTS 반환이 거짓임");
    failures += expect(dts_span >= kMinSpan && dts_span <= kMaxSpan, result,
                       "10초 입력의 상대 DTS 범위가 9500~10500ms를 벗어남");
    failures += expect(pts_span >= kMinSpan && pts_span <= kMaxSpan, result,
                       "10초 입력의 상대 PTS 범위가 9500~10500ms를 벗어남");
    std::cout << "시작 위치=" << start << ", M=" << b_count << ", 검증프레임=" << input.size()
              << ", 입력기간=10000ms, 상대DTS범위=" << dts_span << "ms"
              << ", 상대PTS범위=" << pts_span << "ms, 거짓반환프레임=" << undetermined_frames
              << ", 동일PTS반복=" << (repeat_pts ? "있음" : "없음")
              << ", 결과=" << (failures ? "실패" : "통과") << '\n';
    return failures;
}

int checkRestartLimit() {
    DtsGenerator generator;
    Stamp stamp;
    FrameResult result{"재판정 상한", 0};
    int failures = 0;
    size_t frame = 0;
    int64_t previous_relative_dts = 0;
    auto feed = [&](uint64_t pts) {
        result.frame = frame++;
        result.input_pts = pts;
        result.determined = generator.getDts(pts, result.generated_dts);
        stamp.revise(result.generated_dts, result.input_pts,
                     result.relative_dts, result.relative_pts, false);
        failures += expect(result.generated_dts <= result.input_pts, result,
                           "재판정 상한 시험에서 생성 DTS가 PTS보다 큼");
        failures += expect(result.relative_dts >= previous_relative_dts, result,
                           "재판정 상한 시험에서 상대 DTS가 역행함");
        failures += expect(result.relative_dts <= result.relative_pts, result,
                           "재판정 상한 시험에서 상대 DTS가 상대 PTS보다 큼");
        previous_relative_dts = result.relative_dts;
    };
    for (uint64_t pts : {100, 200, 300, 400}) {
        feed(pts);
        failures += expect(result.generated_dts == pts, result,
                           "초기 순차 수열에서 dts=pts 경로가 달라짐");
    }
    failures += expect(result.determined, result, "초기 순차 수열의 판정을 완료하지 못함");

    uint64_t high = 400;
    for (size_t restart = 0; restart < 8; ++restart) {
        // 하강마다 재판정하고, 두 번의 상승으로 다시 정렬 크기 1이 된다.
        // 공개 반환값만 검사하며 생성기의 구현이나 내부 상태를 시험에 복제하지 않는다.
        const uint64_t low = high - 50;
        for (uint64_t pts : {low, high + 100, high + 200}) {
            feed(pts);
            failures += expect(!result.determined, result,
                               "8회 이내 재판정에서 확정 상태를 잘못 반환함");
            failures += expect(result.generated_dts == low, result,
                               "재판정 중 잠정 DTS 기준을 유지하지 못함");
        }
        high += 200;
    }
    // 9번째 하강부터는 재시작하지 않고 마지막 크기 1 판정의 dts=pts를 유지한다.
    for (uint64_t pts : {high - 50, high - 60, high - 70}) {
        feed(pts);
        failures += expect(result.determined && result.generated_dts == pts, result,
                           "재판정 8회 상한 뒤 마지막 판정을 유지하지 못함");
    }
    std::cout << "시작 위치=재판정 상한, M=0, 검증입력=" << frame
              << ", 결과=" << (failures ? "실패" : "통과") << '\n';
    return failures;
}

} // 이름 없는 네임스페이스

int main() {
    int failures = 0;
    for (auto start : {StartPosition::B1, StartPosition::P, StartPosition::I}) {
        for (size_t b_count = 0; b_count <= 3; ++b_count) {
            failures += check(startName(start), b_count, makePts(start, b_count));
        }
    }
    failures += check("P 시작 뒤 B 전환", 2, makeDelayedBPts());
    failures += check("B1 시작 동일 PTS 반복", 3, makePts(StartPosition::B1, 3), true);
    failures += checkRestartLimit();
    std::cout << "전체 결과=" << (failures ? "실패" : "통과")
              << ", 실패 검사 수=" << failures << '\n';
    return failures ? 1 : 0;
}
