#include <iostream>
#include "Player/MirrorProxy.h"
#include "Player/PlayerProxy.h"
#include "Poller/EventPoller.h"
using namespace mediakit;
static int failures = 0;
static void expect(bool ok, const char* name) {
    std::cout << name << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    failures += !ok;
}
int main() {
    MediaTuple sub{DEFAULT_VHOST, "live", "sub/camera", ""};
    const std::string source = "rtsp://camera:secret@192.0.2.1/main";
    auto resolve = [&](const MediaTuple& t, const std::string& origin, const std::string& main) {
        return mirroredPullUrl(t, origin, main, true, 554);
    };
    expect(resolve(sub, source, source) == "rtsp://127.0.0.1:554/live/main/camera", "identical_source_reuses_main");
    expect(resolve(sub, source+"-sub", source).empty(), "real_substream_stays_independent");
    expect(resolve(sub, "rtsp://other:secret@192.0.2.1/main", source).empty(), "different_credentials_isolated");
    auto main = sub; main.stream = "main/camera";
    expect(resolve(main, source, source).empty(), "main_never_points_to_self");
    auto other = sub; other.app = "other";
    expect(resolve(other, source, source).empty(), "other_application_isolated");
    other = sub; other.vhost = "other.example";
    expect(resolve(other, source, source).empty(), "virtual_host_isolated");
    other = sub; other.stream = "sub/";
    expect(resolve(other, source, source).empty(), "empty_channel_rejected");
    other = sub; other.stream = "sub/camera?argument=1";
    expect(resolve(other, source, source).empty(), "query_in_channel_rejected");
    other = sub; other.stream = "sub/../camera";
    expect(resolve(other, source, source).empty(), "nested_path_rejected");
    expect(resolve(sub, "rtmp://camera/live", "rtmp://camera/live").empty(), "non_rtsp_unchanged");
    expect(resolve(sub, "rtsp://127.0.0.1/live/sub/camera", "rtsp://127.0.0.1/live/sub/camera").empty(), "sub_cycle_rejected");
    expect(resolve(sub, "rtsp://host/live/main/camera", "rtsp://host/live/main/camera").empty(), "main_cycle_rejected");
    expect(mirroredPullUrl(sub, source, source, false, 554).empty(), "disabled_main_rtsp_unchanged");
    expect(mirroredPullUrl(sub, source, source, true, 0).empty(), "disabled_rtsp_port_unchanged");
    expect(mirroredPullUrl(sub, source, source, true, 8554) == "rtsp://127.0.0.1:8554/live/main/camera", "configured_port_preserved");
    toolkit::EventPollerPool::setPoolSize(1);
    ProtocolOption option;
    auto player = std::make_shared<PlayerProxy>(sub, option, 0);
    // Invalid fixture scheme fails locally; no operating service or camera is opened.
    player->play("fixture://logical-source", "fixture://local-mirror");
    expect(player->getUrl() == "fixture://logical-source", "logical_identity_preserved");
    expect(player->getTransportUrl() == "fixture://local-mirror", "transport_identity_separate");
    player->getPoller()->sync([&] {player->update("fixture://new-source", {});});
    expect(player->getTransportUrl() == "fixture://new-source", "source_update_clears_old_mirror");
    return failures ? 1 : 0;
}
