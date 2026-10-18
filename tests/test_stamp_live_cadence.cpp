#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include "Common/Stamp.h"
using namespace mediakit;

static int check(const char* name, const std::vector<int64_t>& input,
                 int64_t expected, bool playback = false) {
    Stamp stamp;
    stamp.setPlayBack(playback);
    int64_t first = 0, last = 0, pts = 0;
    bool monotonic = true;
    for (size_t i = 0; i < input.size(); ++i) {
        int64_t dts;
        stamp.revise(input[i], input[i], dts, pts, false);
        if (!i) first = dts;
        else monotonic = monotonic && (playback || dts >= last);
        last = dts;
    }
    const auto duration = last - first;
    const bool ok = duration == expected && monotonic;
    std::cout << name << " frames=" << input.size() << " input_ms=" << input.back()-input.front()
        << " output_ms=" << duration << " expected_ms=" << expected
        << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    return !ok;
}

int main(int argc, char** argv) {
    int failures = 0;
    failures += check("normal_25fps", {1000,1040,1080,1120,1160}, 160);
    failures += check("same_picture_timestamp", {1000,1000,1040,1040,1080}, 80);
    failures += check("forward_gap_300", {1000,1040,1340,1380}, 380);
    failures += check("forward_gap_301", {1000,1040,1341,1381}, 381);
    failures += check("observed_gop_gap_435", {1000,1020,1455,1475}, 475);
    std::vector<int64_t> gops{1000};
    for (int g = 0; g < 12; ++g) {
        for (int f = 0; f < 29; ++f) gops.push_back(gops.back()+20);
        gops.push_back(gops.back()+420);
    }
    failures += check("twelve_vbr_gops", gops, 12000);
    failures += check("short_source_pause", {1000,1040,2540,2580}, 1580);
    failures += check("live_discontinuity_boundary", {1000,1040,4040,4080}, 3080);
    failures += check("beyond_live_boundary_bounded", {1000,1040,4041,4081}, 120);
    failures += check("large_discontinuity_bounded", {1000,1040,6040,6080}, 120);
    failures += check("source_clock_reset_bounded", {1000,1040,20,60}, 120);
    failures += check("playback_original_timeline", {1000,1040,6040,20,60}, -940, true);
    DeltaStamp recorder;
    recorder.deltaStamp(1000); recorder.deltaStamp(1040);
    const bool recorder_ok = recorder.deltaStamp(1475) == 40;
    std::cout << "base_delta_policy_unchanged result=" << (recorder_ok ? "PASS" : "FAIL") << '\n';
    failures += !recorder_ok;
    if (argc == 2) {
        std::ifstream file(argv[1]);
        std::vector<int64_t> samples;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || !isdigit(static_cast<unsigned char>(line[0]))) continue;
            for (auto& c : line) if (c == ',') c = ' ';
            std::istringstream row(line);
            int index, stream, key; int64_t arrival, pts, dts;
            if (!(row >> index >> stream >> arrival >> pts >> dts >> key)) continue;
            if (stream == 0 && arrival >= 2000000 && pts > -1000000000)
                samples.push_back(pts/1000+100000);
        }
        if (samples.empty()) return 2;
        failures += check("actual_source_cadence", samples, samples.back()-samples.front());
    }
    return failures ? 1 : 0;
}
