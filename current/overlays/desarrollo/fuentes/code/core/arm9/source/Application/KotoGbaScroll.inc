// R31: one bounded gesture model for the reader, its popups and K20 viewer.
// Input ticks advance momentum; audio retries and render slices do not.
enum : u8 { ScrollNone, ScrollReader, ScrollModal, ScrollKanji };
struct ScrollMotion {
    int anchorScroll,anchorY,lastY,velocity,pendingScroll;
    u8 owner;
    bool rail,pending;
};
[[gnu::section(".ewram.bss")]] static ScrollMotion sScrollMotion;
static int ScrollClamp(int value,int limit)
{return value<0?0:value>limit?limit:value;}
static void ScrollMotionCancel()
{sScrollMotion.owner=ScrollNone;sScrollMotion.velocity=0;sScrollMotion.rail=false;sScrollMotion.pending=false;}
static bool ScrollMotionMoving(){return sScrollMotion.velocity!=0;}
static void ScrollMotionStart(u8 owner,int scroll,int y,bool rail,int pendingScroll=-1)
{sScrollMotion={scroll,y,y,0,pendingScroll<0?scroll:pendingScroll,owner,rail,!rail&&pendingScroll>=0&&pendingScroll!=scroll};}
static int ScrollMotionDrag(int y,int limit)
{
    ScrollMotion& m=sScrollMotion;
    const int dy=m.lastY-y;
    m.lastY=y;
    // A pause while the stylus remains down removes its fling velocity.
    if((dy<0&&m.velocity>0)||(dy>0&&m.velocity<0))m.velocity=0;
    m.velocity=(m.velocity+dy*16)/2;
    if(m.velocity>384)m.velocity=384;
    if(m.velocity<-384)m.velocity=-384;
    // A fresh touch first owns the displayed page, so taps and holds keep
    // their exact hit coordinates. Once vertical motion proves a continuing
    // drag, retain the preceding drag's requested, not yet painted distance.
    // Reversing direction intentionally discards that unseen destination.
    const int travel=m.anchorY-y;
    if(m.pending&&(travel>6||travel<-6)){
        const int deferred=m.pendingScroll-m.anchorScroll;
        if((travel>0&&deferred>0)||(travel<0&&deferred<0))m.anchorScroll=m.pendingScroll;
        m.pending=false;
    }
    const int wanted=m.anchorScroll+m.anchorY-y,next=ScrollClamp(wanted,limit);
    // Reversing at an edge must respond immediately, without first unwinding
    // a finger movement beyond the document boundary.
    if(next!=wanted){m.anchorScroll=next;m.anchorY=y;m.velocity=0;m.pending=false;}
    return next;
}
static void ScrollMotionRelease(bool moved)
{sScrollMotion.pending=false;if(!moved||sScrollMotion.rail)sScrollMotion.velocity=0;}
static int ScrollMotionTick(u8 owner,int value,int limit)
{
    value=ScrollClamp(value,limit);
    if(sScrollMotion.owner!=owner){ScrollMotionCancel();return value;}
    const int delta=sScrollMotion.velocity/16;
    sScrollMotion.velocity=sScrollMotion.velocity*7/8;
    if(!delta){sScrollMotion.velocity=0;return value;}
    const int next=ScrollClamp(value+delta,limit);
    if(next==value)sScrollMotion.velocity=0;
    return next;
}
static int ScrollRail(int y,int top,int visible,int limit)
{
    if(limit<=0)return 0;
    int thumb=visible*visible/(visible+limit);
    if(thumb<8)thumb=8;
    const int track=visible-thumb,position=ScrollClamp(y-top-thumb/2,track);
    return (position*limit+track/2)/track;
}
