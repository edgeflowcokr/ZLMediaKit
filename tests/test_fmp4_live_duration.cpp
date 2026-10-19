#include "Record/MP4Muxer.h"
#include "FMP4/FMP4MediaSourceMuxer.h"
#include "Rtsp/RtspMediaSourceMuxer.h"
#include "Poller/EventPoller.h"
#include "ext-codec/H264.h"
#include "ext-codec/AAC.h"
#include "Util/base64.h"
#include <iostream>
using namespace mediakit;
using namespace toolkit;
struct LiveCapture : MP4MuxerMemory {
    struct Segment { std::string bytes; uint64_t stamp; bool key; };
    std::vector<Segment> segments;
    explicit LiveCapture(bool enabled) { setSingleH264SampleLookahead(enabled); }
    bool enabledFor(CodecId codec) const { return singleH264SampleLookahead(codec); }
    void onSegmentData(std::string bytes,uint64_t stamp,bool key) override { segments.push_back({std::move(bytes),stamp,key}); }
};
static uint64_t read(const std::string &s,size_t p,size_t n) { uint64_t v=0;while(n--)v=(v<<8)|uint8_t(s[p++]);return v; }
struct Sample { uint64_t dts,duration; };
static Sample inspect(const std::string &s) {
    Sample result={0,0};
    // All boxes are generated locally by the production muxer. The single
    // sample's duration is the tfhd default when trun omits an override.
    auto tfdt=s.find("tfdt"),tfhd=s.find("tfhd");
    if(tfdt==std::string::npos||tfhd==std::string::npos)throw std::runtime_error("missing fragment metadata");
    result.dts=read(s,tfdt+8,uint8_t(s[tfdt+4])?8:4);
    auto flags=read(s,tfhd+4,4);size_t pos=tfhd+12;
    if(flags&1)pos+=8;if(flags&2)pos+=4;
    if(flags&8)result.duration=read(s,pos,4);
    auto trun=s.find("trun");
    if(trun!=std::string::npos){auto f=read(s,trun+4,4);pos=trun+12;if(f&1)pos+=4;if(f&4)pos+=4;if(f&0x100)result.duration=read(s,pos,4);}
    return result;
}
static Frame::Ptr nal(uint64_t stamp,bool key) {
    auto f=FrameImp::create<H264Frame>();f->_prefix_size=4;f->_buffer.assign("\0\0\0\1",4);
    f->_buffer.push_back(key?'\x65':'\x61');f->_buffer.push_back('\x80');f->_dts=f->_pts=stamp;return f;
}
static Track::Ptr video() { auto t=std::make_shared<H264Track>(decodeBase64("Z00AM4qKUDwBE/Kg"),decodeBase64("aO48gA=="),0,0);t->setIndex(0);return t; }
static int failures=0;
static void check(bool ok,const char *name){std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n';failures+=!ok;}
struct DemandEvents : MediaSourceEvent {
    EventPoller::Ptr poller;
    explicit DemandEvents(EventPoller::Ptr p) : poller(std::move(p)) {}
    EventPoller::Ptr getOwnerPoller(MediaSource &) override { return poller; }
    void onReaderChanged(MediaSource &,int) override {}
};
static void demandReordering() {
    EventPollerPool::setPoolSize(1);
    auto poller=EventPollerPool::Instance().getPoller();
    auto events=std::make_shared<DemandEvents>(poller);
    ProtocolOption option; option.fmp4_demand=true;
    auto mux=std::make_shared<FMP4MediaSourceMuxer>(MediaTuple{"__defaultVhost__","demand-test","local-only",""},option);
    mux->setListener(events);
    poller->sync([&] {
        check(mux->addTrack(video()),"real_fmp4_demand_fixture_has_ready_video"); mux->addTrackCompleted();
        auto source=std::dynamic_pointer_cast<FMP4MediaSource>(mux->getMediaSource());
        check(!source->getInitSegment().empty(),"real_fmp4_demand_fixture_has_init_segment");
        auto reader=source->getRing()->attach(poller,false);
        reader->setReadCB([](const FMP4MediaSource::RingDataType &){});
        check(source->readerCount()==1,"real_fmp4_ring_reader_attaches");
        // 이미 새 독자가 붙은 뒤에 옛 0명 알림을 의도적으로 전달한다.
        mux->onReaderChanged(*source,0);
        check(mux->inputFrame(nal(1000,true)),"stale_zero_notification_does_not_stop_active_fmp4_input");
        mux->onReaderChanged(*source,1);
        check(mux->inputFrame(nal(1040,true)),"current_reader_keeps_fmp4_input_enabled");
        mux->inputFrame(nal(1080,false)); mux->inputFrame(nal(1120,false));
        auto cached=[&] {size_t n=0;source->getRing()->flushGop([&](const FMP4MediaSource::RingDataType &list){n+=list->size();});return n;};
        const auto before=cached();
        check(before>0,"actual_mux_emits_fmp4_fragments_to_ring");
        mux->onReaderChanged(*source,0);
        check(mux->inputFrame(nal(1160,false))&&cached()>=before,"stale_zero_keeps_active_gop_and_fragment_output");
        auto reader2=source->getRing()->attach(poller,false);
        reader2->setReadCB([](const FMP4MediaSource::RingDataType &){});
        check(source->readerCount()==2,"second_actual_reader_attaches");
        mux->onReaderChanged(*source,1);
        check(mux->inputFrame(nal(1200,false)),"stale_count_one_keeps_two_readers_enabled");
        reader2.reset();
        mux->onReaderChanged(*source,0);
        check(source->readerCount()==1&&mux->inputFrame(nal(1240,false)),"one_remaining_reader_survives_old_zero");
        reader.reset();
        check(source->readerCount()==0,"real_fmp4_ring_reader_detaches");
        // 마지막 독자가 떠난 뒤에 옛 1명 알림이 와도 불필요하게 켜지면 안 된다.
        mux->onReaderChanged(*source,1);
        check(!mux->inputFrame(nal(1080,true)),"stale_one_notification_does_not_enable_readerless_mux");
        auto replacement=source->getRing()->attach(poller,false);
        replacement->setReadCB([](const FMP4MediaSource::RingDataType &){});
        mux->onReaderChanged(*source,0);
        check(mux->inputFrame(nal(1280,true)),"replacement_reader_recovers_after_actual_zero");
        replacement.reset();
    });
    poller->sync([&] { mux.reset(); });
    option.fmp4_demand=false;
    auto always=std::make_shared<FMP4MediaSourceMuxer>(MediaTuple{"__defaultVhost__","demand-test","always-on",""},option);
    always->setListener(events);
    poller->sync([&] {
        always->addTrack(video()); always->addTrackCompleted();
        auto source=always->getMediaSource();
        always->onReaderChanged(*source,0);
        check(always->inputFrame(nal(1000,true)),"demand_disabled_policy_is_unchanged");
        always.reset();
    });
}
static void run(const std::vector<uint64_t> &stamps,const char *name,size_t key_interval=4) {
    LiveCapture live(true);live.addTrack(video());live.getInitSegment();
    // Nonzero source epoch avoids DeltaStamp's legacy zero-as-uninitialized
    // sentinel; expected output stays relative to the first source picture.
    for(size_t i=0;i<stamps.size();++i)live.inputFrame(nal(stamps[i]+1000,i%key_interval==0));
    live.flush();
    bool exact=live.segments.size()==stamps.size()-1,markers=true,metadata=true;
    for(size_t i=0;i<live.segments.size();++i){auto sample=inspect(live.segments[i].bytes);
        exact=exact&&sample.dts==stamps[i]&&sample.duration==stamps[i+1]-stamps[i];
        markers=markers&&live.segments[i].key==(i%key_interval==0);
        metadata=metadata&&live.segments[i].stamp==sample.dts;
    }
    check(exact,name);check(markers,"completed_segment_keeps_previous_picture_gop_marker");check(metadata,"completed_segment_stamp_matches_previous_picture");
}
static void rtspDemandReordering() {
    auto poller=EventPollerPool::Instance().getPoller();
    auto events=std::make_shared<DemandEvents>(poller);
    ProtocolOption option; option.rtsp_demand=true;
    auto mux=std::make_shared<RtspMediaSourceMuxer>(MediaTuple{"__defaultVhost__","demand-test","rtsp-local-only",""},option);
    mux->setListener(events);
    poller->sync([&] {
        check(mux->addTrack(video()),"real_rtsp_demand_fixture_has_ready_video"); mux->addTrackCompleted();
        // RTSP ring은 첫 패킷을 수신할 때 생성된다. 시험에서는 원래 mux 입력으로 만든다.
        check(mux->inputFrame(nal(1000,true)),"real_rtsp_demand_fixture_accepts_initial_key_frame");
        mux->flush();
        auto source=std::dynamic_pointer_cast<RtspMediaSource>(mux->getMediaSource());
        check(bool(source->getRing()),"real_rtsp_demand_fixture_has_ring");
        if(!source->getRing())return;
        auto reader=source->getRing()->attach(poller,false);
        reader->setReadCB([](const RtspMediaSource::RingDataType &){});
        check(source->readerCount()==1,"actual_rtsp_reader_attaches");
        mux->onReaderChanged(*source,0);
        check(mux->inputFrame(nal(1040,true)),"stale_zero_keeps_active_rtsp_mux_enabled");
        mux->onReaderChanged(*source,1);
        check(mux->inputFrame(nal(1080,true)),"current_reader_keeps_rtsp_input_enabled");
        mux->inputFrame(nal(1120,false)); mux->flush();
        auto cached=[&] {size_t n=0;source->getRing()->flushGop([&](const RtspMediaSource::RingDataType &list){n+=list->size();});return n;};
        const auto before=cached();
        check(before>0,"actual_rtsp_mux_emits_packets_to_ring");
        mux->onReaderChanged(*source,0);
        const auto accepted=mux->inputFrame(nal(1160,false)); mux->flush();
        check(accepted&&cached()>=before,"stale_zero_preserves_actual_rtsp_gop_output");
        auto reader2=source->getRing()->attach(poller,false);
        reader2->setReadCB([](const RtspMediaSource::RingDataType &){});
        check(source->readerCount()==2,"second_rtsp_reader_attaches");
        reader2.reset(); mux->onReaderChanged(*source,0);
        check(source->readerCount()==1&&mux->inputFrame(nal(1200,true)),"remaining_rtsp_reader_survives_old_zero");
        reader.reset(); check(source->readerCount()==0,"actual_rtsp_reader_detaches");
        mux->onReaderChanged(*source,1);
        check(!mux->inputFrame(nal(1240,true)),"stale_one_does_not_enable_readerless_rtsp_mux");
        auto replacement=source->getRing()->attach(poller,false);
        replacement->setReadCB([](const RtspMediaSource::RingDataType &){});
        mux->onReaderChanged(*source,0);
        check(mux->inputFrame(nal(1280,true)),"replacement_rtsp_reader_recovers_after_actual_zero");
        replacement.reset();
        mux->onReaderChanged(*source,0);
        // 실제 idle의 정리 예약 직후 재접속해도 새 key부터 연속 출력되어야 한다.
        auto rapid=source->getRing()->attach(poller,false);
        rapid->setReadCB([](const RtspMediaSource::RingDataType &){});
        mux->onReaderChanged(*source,1);
        const auto keyAccepted=mux->inputFrame(nal(1320,true)); mux->flush();
        check(keyAccepted&&cached()>0,"pending_idle_clear_allows_new_rtsp_key_frame");
        const auto keyCache=cached();
        const auto pAccepted=mux->inputFrame(nal(1360,false)); mux->flush();
        check(pAccepted&&cached()>=keyCache,"pending_idle_clear_does_not_repeat_on_following_rtsp_picture");
        rapid.reset();
    });
    poller->sync([&] {mux.reset();});
    option.rtsp_demand=false;
    auto always=std::make_shared<RtspMediaSourceMuxer>(MediaTuple{"__defaultVhost__","demand-test","rtsp-always-on",""},option);
    always->setListener(events);
    poller->sync([&] {
        always->addTrack(video());always->addTrackCompleted();auto source=always->getMediaSource();
        always->onReaderChanged(*source,0);
        check(always->inputFrame(nal(1000,true)),"rtsp_demand_disabled_policy_is_unchanged");always.reset();
    });
}
int main(){
    demandReordering();
    rtspDemandReordering();
    run({0,40,41,100,800,801,820,860,1000,1001},"irregular_cadence_fragment_ends_at_exact_next_dts");
    run({0,40,80,120,160,200,240,280,320,360},"constant_cadence_is_unchanged");
    std::vector<uint64_t> long_gop={0};for(size_t i=1;i<230;++i)long_gop.push_back(long_gop.back()+(i%2?40:1));
    run(long_gop,"long_gop_does_not_force_a_premature_fragment_at_200_frames",1000);
    LiveCapture legacy(false);legacy.addTrack(video());legacy.getInitSegment();
    for(auto stamp:{0,40,41,100,800,801,820,860,1000,1001})legacy.inputFrame(nal(stamp+1000,stamp==0));
    legacy.flush();bool mismatch=false;
    for(size_t i=1;i<legacy.segments.size();++i){auto a=inspect(legacy.segments[i-1].bytes),b=inspect(legacy.segments[i].bytes);mismatch|=a.dts+a.duration!=b.dts;}
    check(mismatch,"regression_fixture_reproduces_legacy_boundary_holes");
    LiveCapture av(true);av.addTrack(video());auto audio=std::make_shared<AACTrack>(std::string("\x12\x10",2));audio->setIndex(1);av.addTrack(audio);
    check(!av.enabledFor(CodecH264),"multiple_tracks_keep_legacy_fragment_policy");
    LiveCapture defaults(false);defaults.addTrack(video());check(!defaults.enabledFor(CodecH264),"memory_muxers_are_opt_in_only");
    LiveCapture unsupported(true);unsupported.addTrack(video());check(!unsupported.enabledFor(CodecH265),"h265_policy_unchanged");
    return failures?1:0;
}
