/* Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * This file is governed by the license in the root of the source tree. */
#ifndef ZLMEDIAKIT_VIDEO_SAMPLE_STAMP_H
#define ZLMEDIAKIT_VIDEO_SAMPLE_STAMP_H
#include <cstdint>
#include <limits>
namespace mediakit {
// Call once per merged picture, not per slice or configuration NAL.
// Equal decode timestamps produce zero-duration MP4 samples, including TS
// transmuxed by a browser. Preserve CTS and change only colliding samples.
class VideoSampleStamp {
public:
    bool revise(int64_t &dts, int64_t &pts, int64_t maximum = (std::numeric_limits<int64_t>::max)()) {
        if (dts < 0 || dts > maximum || pts > maximum) return false;
        if (_have_previous && dts <= _last_dts) {
            if (_last_dts >= maximum) return false;
            const auto delta = _last_dts + 1 - dts;
            if (pts > maximum - delta) return false;
            dts += delta;
            pts += delta;
        }
        _last_dts = dts;
        _have_previous = true;
        return true;
    }
private:
    bool _have_previous = false;
    int64_t _last_dts = 0;
};
}
#endif
