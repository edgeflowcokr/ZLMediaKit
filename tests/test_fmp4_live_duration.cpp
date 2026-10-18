#include "Record/MP4Muxer.h"
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
int main(){
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
