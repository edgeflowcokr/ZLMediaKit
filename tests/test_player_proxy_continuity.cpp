#include <iostream>
#include "Player/PlayerProxy.h"
#include "Player/PlayerProxyContinuity.h"
#include "Rtsp/RtspMediaSource.h"
#include "ext-codec/H264.h"
#include "ext-codec/AAC.h"
#include "Util/util.h"
#include "Util/base64.h"
#include "FMP4/FMP4MediaSource.h"
#include "FMP4/HttpFmp4Timeline.h"

using namespace mediakit;
using namespace toolkit;

// Explicit instantiation permits exercising private lifecycle methods without
// changing their production visibility or adding a runtime test endpoint.
template<class Tag, typename Tag::type Member> struct TestMember {
    friend typename Tag::type access(Tag) { return Member; }
};
struct SuccessTag { using type=void(PlayerProxy::*)(); friend type access(SuccessTag); };
struct MuxerTag { using type=MultiMediaSourceMuxer::Ptr PlayerProxy::*; friend type access(MuxerTag); };
struct CloseTag { using type=bool(PlayerProxy::*)(MediaSource&); friend type access(CloseTag); };
struct TimerTag { using type=Timer::Ptr PlayerProxy::*; friend type access(TimerTag); };
struct ResetTag { using type=bool(PlayerProxy::*)(const Track::Ptr&,const Track::Ptr&,const Track::Ptr&,const Track::Ptr&) const; friend type access(ResetTag); };
template struct TestMember<SuccessTag, &PlayerProxy::onPlaySuccess>;
template struct TestMember<MuxerTag, &PlayerProxy::_muxer>;
template struct TestMember<CloseTag, &PlayerProxy::close>;
template struct TestMember<TimerTag, &PlayerProxy::_timer>;
template struct TestMember<ResetTag, &PlayerProxy::shouldResetTracks>;

struct FixtureInput : PlayerImp<PlayerBase, PlayerBase> {
    std::vector<Track::Ptr> tracks;
    std::vector<Track::Ptr> getTracks(bool) const override { return tracks; }
};
struct FixtureProxy : PlayerProxy {
    using PlayerProxy::PlayerProxy;
    void inject(const std::shared_ptr<FixtureInput>& p) { _delegate=p; }
    void disconnect() { if (_on_shutdown) _on_shutdown(SockException(Err_timeout,"fixture interruption")); }
};
static int failures=0;
static void expect(bool ok,const char* name) {
    std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n'; failures+=!ok;
}
static Track::Ptr video(const std::string& pps="aO48gA==") {
    auto t=std::make_shared<H264Track>(decodeBase64("Z00AM4qKUDwBE/Kg"),decodeBase64(pps),0,0);
    t->setIndex(0); return t;
}
static Frame::Ptr picture(uint64_t stamp,bool key) {
    auto f=FrameImp::create<H264Frame>();f->_prefix_size=4;f->_buffer.assign("\0\0\0\1",4);
    f->_buffer.push_back(key?'\x65':'\x61');f->_buffer.push_back('\x80');f->_pts=f->_dts=stamp;return f;
}
static void liveFmp4Reconnect(const ProtocolOption &base) {
    auto option=base;option.enable_fmp4=true;option.fmp4_demand=true;option.enable_audio=false;
    auto p=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","fmp4-continuity",""},option,-1,nullptr,60,60,60);
    auto input=std::make_shared<FixtureInput>();input->tracks={video()};
    FMP4MediaSource::Ptr source;FMP4MediaSource::RingType::RingReader::Ptr reader;
    size_t delivered=0,before=0;bool translated_ok=true;
    auto attach=[&] {
        auto timeline=std::make_shared<HttpFmp4Timeline>();
        translated_ok=translated_ok&&timeline->configure(source->getInitSegment());
        timeline->requireRandomAccess(true);
        reader=source->getRing()->attach(p->getPoller(),false);
        reader->setReadCB([&,timeline](const FMP4MediaSource::RingDataType &batch) {
            batch->for_each([&](const FMP4Packet::Ptr &packet) {
                if (timeline->waitingForRandomAccess()&&!timeline->acceptRandomAccess(packet->key_frame))return;
                std::string bytes;translated_ok=timeline->translate(packet->toString(),bytes)&&translated_ok;++delivered;
            });
        });
    };
    auto feed=[&](uint64_t epoch) {for(size_t i=0;i<12;++i)input->tracks[0]->inputFrame(picture(epoch+i*40,i%4==0));};
    p->getPoller()->sync([&] {
        p->play("fixture://fmp4-continuity");p->inject(input);(p.get()->*access(SuccessTag{}))();feed(10000);
        source=std::dynamic_pointer_cast<FMP4MediaSource>(MediaSource::find(FMP4_SCHEMA,DEFAULT_VHOST,"test","fmp4-continuity",false));
        expect(bool(source),"fmp4_proxy_fixture_registers_real_fmp4_source");
        if(source){attach();feed(11000);}
    });
    p->getPoller()->sync([&] {
        if(!source)return;
        expect(delivered>0&&translated_ok,"fmp4_proxy_real_reader_receives_translatable_fragments");before=delivered;
        auto original_ring=source->getRing();p->disconnect();input->tracks={video()};p->inject(input);
        (p.get()->*access(SuccessTag{}))();feed(1000); // 새 입력 세대의 DTS 원점이 되돌아간다.
        auto current=MediaSource::find(FMP4_SCHEMA,DEFAULT_VHOST,"test","fmp4-continuity",false);
        expect(current==source&&source->getRing()==original_ring,"compatible_reconnect_preserves_active_fmp4_source_and_ring");
    });
    p->getPoller()->sync([&] {
        if(!source)return;
        expect(delivered>before&&translated_ok,"compatible_reconnect_keeps_existing_http_timeline_decodable");
        const auto active=source->getLiveDiagnostic()->snapshot();reader.reset();feed(100000);
        const auto idle=source->getLiveDiagnostic()->snapshot();
        expect(idle.input>active.input&&idle.enabled==active.enabled,"fmp4_proxy_idle_input_is_not_active_mux_input");
        before=delivered;attach();feed(200000);
    });
    p->getPoller()->sync([&] {
        if(!source)return;
        const auto end=source->getLiveDiagnostic()->snapshot();
        expect(delivered>before&&translated_ok,"fmp4_proxy_new_reader_recovers_after_long_idle_epoch");
        expect(end.ring_generation==1&&end.no_init==0&&end.missing_track==0&&end.writer_failed==0,
               "fmp4_proxy_reconnect_and_idle_do_not_reset_init_or_lose_track");
        reader.reset();RtspMediaSource sender({DEFAULT_VHOST,"test","fmp4-continuity",""});
        (p.get()->*access(CloseTag{}))(sender);source.reset();
    });
}
int main() {
    EventPollerPool::setPoolSize(1);
    const auto cap=playerProxyContinuityCapability();
    expect(cap.revision==1 && cap.preserve_media_on_retry==1 && cap.compatible_tracks==1
        && cap.requires_reset_when_replay_zero==1,"capability_requires_explicit_runtime_policy");
    ProtocolOption option;
    option.enable_hls=false; option.enable_hls_fmp4=false; option.enable_mp4=false;
    option.enable_rtmp=false; option.enable_ts=false; option.enable_fmp4=false;
    option.enable_audio=true; option.add_mute_audio=false;
    option.enable_rtsp=true; option.rtsp_demand=false;
    auto p=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","continuity",""},option,-1,nullptr,60,60,60);
    auto audio=std::make_shared<AACTrack>(std::string("\x12\x10",2)); audio->setIndex(1);
    auto disabled_option=option;disabled_option.enable_audio=false;
    auto disabled=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","audio-disabled",""},disabled_option,0);
    expect(!(disabled.get()->*access(ResetTag{}))(video(),audio,video(),nullptr),"disabled_source_audio_does_not_reset_video");
    auto muted_option=option;muted_option.add_mute_audio=true;
    auto muted=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","mute-audio",""},muted_option,0);
    auto synthetic=std::make_shared<AACTrack>(std::string("\x12\x10",2));synthetic->setIndex(0xFFFF);
    expect(!(muted.get()->*access(ResetTag{}))(video(),nullptr,video(),synthetic),"generated_mute_audio_is_not_a_source_change");
    expect((muted.get()->*access(ResetTag{}))(video(),audio,video(),synthetic),"real_audio_replacing_mute_resets_tracks");
    p->getPoller()->sync([&] {
        mINI::Instance()[General::kResetWhenRePlay]=0;
        NOTICE_EMIT(BroadcastReloadConfigArgs,Broadcast::kBroadcastReloadConfig);
        // This unsupported fixture URI never connects to a camera or service.
        p->play("fixture://continuity");
        auto input=std::make_shared<FixtureInput>(); input->tracks={video()}; p->inject(input);
        (p.get()->*access(SuccessTag{}))();
        auto original=p.get()->*access(MuxerTag{});
        auto original_track=original->getTrack(TrackVideo,false);
        expect(original_track && original_track->ready(),"fixture_has_ready_video");
        p->disconnect();
        expect((p.get()->*access(MuxerTag{}))==original,"retry_keeps_output_muxer");
        expect(bool(p.get()->*access(TimerTag{})),"retry_is_scheduled_without_output_reset");
        input->tracks={video()};
        (p.get()->*access(SuccessTag{}))();
        expect((p.get()->*access(MuxerTag{}))==original,"compatible_reconnect_keeps_muxer");
        expect(original->getTrack(TrackVideo,false)==original_track,"compatible_reconnect_keeps_track_identity");
        input->tracks={video("aO48gAE=")};
        (p.get()->*access(SuccessTag{}))();
        expect(original->getTrack(TrackVideo,false)!=original_track,"changed_parameter_set_resets_tracks");
        auto audio=std::make_shared<AACTrack>(std::string("\x12\x10",2)); audio->setIndex(1);
        input->tracks={video(),audio};
        (p.get()->*access(SuccessTag{}))();
        expect(bool(original->getTrack(TrackAudio,false)),"added_audio_is_not_silently_reused");
        input->tracks={video()};
        (p.get()->*access(SuccessTag{}))();
        expect(!original->getTrack(TrackAudio,false),"removed_audio_resets_tracks");
        int closed=0;p->setOnClose([&](const SockException&){++closed;});
        RtspMediaSource sender({DEFAULT_VHOST,"test","continuity",""});
        (p.get()->*access(CloseTag{}))(sender);
        expect(!(p.get()->*access(MuxerTag{})),"explicit_close_releases_muxer");
        expect(!(p.get()->*access(TimerTag{})),"explicit_close_cancels_retry");
        expect(closed==1,"explicit_close_callback_once");
    });
    liveFmp4Reconnect(option);
    auto terminal=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","terminal",""},option,0);
    terminal->getPoller()->sync([&] {
        terminal->play("fixture://terminal");
        auto input=std::make_shared<FixtureInput>();input->tracks={video()};terminal->inject(input);
        (terminal.get()->*access(SuccessTag{}))();
        terminal->disconnect();
        expect(!(terminal.get()->*access(MuxerTag{})),"exhausted_retry_releases_output");
        expect(!(terminal.get()->*access(TimerTag{})),"exhausted_retry_has_no_timer");
    });
    auto legacy=std::make_shared<FixtureProxy>(MediaTuple{DEFAULT_VHOST,"test","legacy",""},option,-1,nullptr,60,60,60);
    legacy->getPoller()->sync([&] {
        mINI::Instance()[General::kResetWhenRePlay]=1;
        NOTICE_EMIT(BroadcastReloadConfigArgs,Broadcast::kBroadcastReloadConfig);
        legacy->play("fixture://legacy");
        auto input=std::make_shared<FixtureInput>();input->tracks={video()};legacy->inject(input);
        (legacy.get()->*access(SuccessTag{}))();
        auto before=legacy.get()->*access(MuxerTag{});
        legacy->disconnect();
        expect(!(legacy.get()->*access(MuxerTag{})),"explicit_legacy_reset_policy_preserved");
        (legacy.get()->*access(SuccessTag{}))();
        expect((legacy.get()->*access(MuxerTag{}))!=before,"legacy_reconnect_rebuilds_output");
        RtspMediaSource sender({DEFAULT_VHOST,"test","legacy",""});
        (legacy.get()->*access(CloseTag{}))(sender);
    });
    return failures?1:0;
}
