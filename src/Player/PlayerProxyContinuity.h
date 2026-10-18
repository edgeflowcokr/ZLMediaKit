#pragma once

namespace mediakit {
// Compile-time capability, not runtime configuration or a promise of source uptime.
// The backend must separately verify general.resetWhenRePlay=0 before enabling it.
struct PlayerProxyContinuityCapability {
    int revision;
    int preserve_media_on_retry;
    int compatible_tracks;
    int requires_reset_when_replay_zero;
};
inline constexpr PlayerProxyContinuityCapability playerProxyContinuityCapability() {
    return {1, 1, 1, 1};
}
} // namespace mediakit
