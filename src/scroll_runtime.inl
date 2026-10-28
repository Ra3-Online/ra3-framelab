// FL_G_SCROLL right-drag extension. Only the local vector in sub_ABFF40 is
// scaled; the shared ScrollBy vtable and keyboard/edge readers are untouched.
struct DragScrollSite {
    unsigned char* address;
    unsigned char original[sizeof kSigDragScroll/sizeof(short)];
    unsigned char installed[8];
};
static float g_dragScrollScale=1.0f;
static bool g_dragScrollOn=false;
static volatile long g_dragScrollCalls=0;

extern "C" __declspec(naked) void fl_drag_scroll_stub() {
    __asm {
        // Replaced native MULSS instructions, preserving their upper lanes.
        mulss xmm0,xmm3
        mulss xmm1,xmm4
        mulss xmm0,dword ptr [g_dragScrollScale]
        mulss xmm1,dword ptr [g_dragScrollScale]
        pushfd
        inc dword ptr [g_dragScrollCalls]
        popfd
        ret
    }
}
static int resolve_drag_scroll(int target,int retail,int groups,bool measure,DragScrollSite& site) {
    site=DragScrollSite();
    if(measure || !(groups&FL_G_SCROLL) || target<=retail) return FL_OK;
    if(retail!=30) return FL_ERR_SIG_MISMATCH;
    Pattern pattern={"右键拖动卷屏位移",kSigDragScroll,sizeof kSigDragScroll/sizeof(short),0xABFFB6};
    unsigned char* address=scan(pattern);
    if(!address) return FL_ERR_SIG_MISS;
    if(address==(unsigned char*)-1) return FL_ERR_SIG_AMBIGUOUS;
    if((uintptr_t)address!=pattern.expectedVa || !mem::read_ok(address,sizeof site.original))
        return FL_ERR_SIG_MISMATCH;
    site.address=address;
    memcpy(site.original,address,sizeof site.original);
    return FL_OK;
}
static bool verify_drag_scroll(const DragScrollSite& site,bool installed) {
    if(!site.address) return true;
    if(!mem::read_ok(site.address,sizeof site.original)) return false;
    if(!installed) return memcmp(site.address,site.original,sizeof site.original)==0;
    return memcmp(site.address,site.installed,sizeof site.installed)==0 &&
        memcmp(site.address+sizeof site.installed,site.original+sizeof site.installed,
               sizeof site.original-sizeof site.installed)==0;
}
static bool install_drag_scroll(DragScrollSite& site) {
    if(!site.address || !verify_drag_scroll(site,false)) return false;
    memset(site.installed,0x90,sizeof site.installed);
    site.installed[0]=0xE8;
    const int relative=(int)((unsigned char*)&fl_drag_scroll_stub-(site.address+5));
    memcpy(site.installed+1,&relative,4);
    if(!patch(site.address,site.installed,sizeof site.installed,"右键拖动卷屏按原版速度归一") ||
       !verify_drag_scroll(site,true)) return false;
    g_dragScrollOn=true;
    return true;
}
