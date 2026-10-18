#pragma once
#include "Common/MediaSource.h"

namespace mediakit {
// Only the product's explicit same-channel main/sub mirror can share a pull.
// Different cameras, credentials, virtual hosts and real substreams stay isolated.
inline std::string mirroredMainStream(const MediaTuple &tuple, const std::string &origin) {
    if (tuple.vhost != DEFAULT_VHOST || tuple.app != "live" ||
        tuple.stream.compare(0, 4, "sub/") || tuple.stream.size() <= 4 ||
        origin.compare(0, 7, "rtsp://")) return {};
    const auto channel = tuple.stream.substr(4);
    if (channel.size() > 128 || channel.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) return {};
    // A source already referring to either local serving name may form a cycle.
    // Conservatively leave it independent, including equivalent remote paths.
    if (origin.find("/live/main/" + channel) != std::string::npos ||
        origin.find("/live/sub/" + channel) != std::string::npos) return {};
    return "main/" + channel;
}

inline std::string mirroredPullUrl(const MediaTuple &tuple, const std::string &origin,
                                  const std::string &main_origin, bool main_rtsp,
                                  uint16_t rtsp_port) {
    const auto main = mirroredMainStream(tuple, origin);
    if (main.empty() || origin != main_origin || !main_rtsp || !rtsp_port) return {};
    return "rtsp://127.0.0.1:" + std::to_string(rtsp_port) + "/live/" + main;
}
} // namespace mediakit
