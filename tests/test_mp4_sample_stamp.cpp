#include <iostream>
#include "Record/MP4Muxer.h"
using namespace mediakit;
static int failures=0;
static void check(bool ok,const char *name){std::cout<<name<<" result="<<(ok?"PASS":"FAIL")<<'\n';failures+=!ok;}
int main(){
    MP4SampleStamp stamp;int64_t dts=0,pts=0;
    check(stamp.revise(dts,pts)&&dts==0&&pts==0,"first_zero_picture_is_preserved");
    dts=pts=0;check(stamp.revise(dts,pts)&&dts==1&&pts==1,"second_equal_picture_has_positive_duration");
    dts=pts=0;check(stamp.revise(dts,pts)&&dts==2&&pts==2,"third_equal_picture_remains_distinct");
    dts=pts=1;check(stamp.revise(dts,pts)&&dts==3&&pts==3,"collision_catchup_is_monotonic");
    dts=pts=40;check(stamp.revise(dts,pts)&&dts==40&&pts==40,"normal_cadence_catches_up_without_extra_offset");
    dts=80;pts=120;check(stamp.revise(dts,pts)&&dts==80&&pts==120,"positive_composition_offset_unchanged");
    dts=120;pts=80;check(stamp.revise(dts,pts)&&dts==120&&pts==80,"reordered_picture_pts_unchanged");
    dts=120;pts=80;check(stamp.revise(dts,pts)&&dts==121&&pts==81,"duplicate_preserves_negative_composition_offset");
    dts=119;pts=159;check(stamp.revise(dts,pts)&&dts==122&&pts==162,"backward_decode_stamp_preserves_composition_offset");
    dts=pts=160;check(stamp.revise(dts,pts)&&dts==160&&pts==160,"later_normal_picture_unchanged");
    MP4SampleStamp reset;dts=pts=0;check(reset.revise(dts,pts)&&dts==0&&pts==0,"new_track_resets_collision_state");
    dts=-1;pts=0;check(!reset.revise(dts,pts)&&dts==-1&&pts==0,"negative_decode_stamp_rejected_without_mutation");
    dts=pts=0;check(reset.revise(dts,pts)&&dts==1&&pts==1,"rejected_input_does_not_advance_state");
    MP4SampleStamp maximum;dts=pts=(std::numeric_limits<int64_t>::max)();
    check(maximum.revise(dts,pts),"maximum_first_stamp_is_not_incremented");
    check(!maximum.revise(dts,pts)&&dts==(std::numeric_limits<int64_t>::max)(),"decode_overflow_rejected_without_wrap");
    MP4SampleStamp composition;dts=pts=0;composition.revise(dts,pts);dts=0;pts=(std::numeric_limits<int64_t>::max)();
    check(!composition.revise(dts,pts)&&dts==0&&pts==(std::numeric_limits<int64_t>::max)(),"composition_overflow_rejected_without_mutation");
    return failures?1:0;
}
