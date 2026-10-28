static void drag_contract() {
    const uintptr_t first=0xABF000;
    const size_t size=0x2000;
    const uintptr_t begin=(uintptr_t)canonicalOwned;
    if(first<begin || first+size>begin+sizeof canonicalOwned) {
        check(false,"DRAG fixture must remain in owned non-executable IMAGE data"); return;
    }
    unsigned char* span=(unsigned char*)first;
    auto setup=[&]() {
        memset(span,0xCC,size);
        for(unsigned i=0;i<sizeof kSigDragScroll/sizeof(short);++i)
            ((unsigned char*)0xABFFB6)[i]=(unsigned char)kSigDragScroll[i];
        fl::g_text=span; fl::g_textSize=size;
        fl::g_dragScrollOn=false; mem::g_writeFault=0;
    };
    setup();
    fl::DragScrollSite s={};
    const int targets[]={60,90,120,165,180,240};
    for(int fps : targets) {
        check(fl::resolve_drag_scroll(fps,30,FL_G_SCROLL,false,s)==FL_OK &&
              s.address==(unsigned char*)0xABFFB6 && fl::verify_drag_scroll(s,false) && !fl::g_undoCount,
              "DRAG real resolver supports old/high targets without writing");
    }
    for(int mode=0;mode<3;++mode) {
        check(fl::resolve_drag_scroll(mode==0 ? 30 : 165,30,mode==1 ? 0 : FL_G_SCROLL,mode==2,s)==FL_OK && !s.address,
              "DRAG native 30, disabled option and measure mode request no patch");
    }
    check(fl::resolve_drag_scroll(165,60,FL_G_SCROLL,false,s)==FL_ERR_SIG_MISMATCH,
          "DRAG refuses an already modified non-native FPS baseline");
    ((unsigned char*)0xABFFB6)[0]^=1;
    check(fl::resolve_drag_scroll(165,30,FL_G_SCROLL,false,s)==FL_ERR_SIG_MISS,"DRAG missing signature rejects");
    setup(); memcpy(span,(void*)0xABFFB6,sizeof s.original);
    check(fl::resolve_drag_scroll(165,30,FL_G_SCROLL,false,s)==FL_ERR_SIG_AMBIGUOUS,"DRAG duplicate signature rejects");
    memset((void*)0xABFFB6,0xCC,sizeof s.original);
    check(fl::resolve_drag_scroll(165,30,FL_G_SCROLL,false,s)==FL_ERR_SIG_MISMATCH,"DRAG unexpected location rejects");
    setup(); fl::resolve_drag_scroll(165,30,FL_G_SCROLL,false,s);
    for(unsigned i=0;i<sizeof s.original;++i) {
        s.address[i]^=1;
        check(!fl::verify_drag_scroll(s,false),"DRAG frozen preimage checks complete multiplication/store/caller context");
        s.address[i]^=1;
    }
    check(fl::install_drag_scroll(s) && fl::g_dragScrollOn && fl::g_undoCount==1 && fl::verify_drag_scroll(s,true),
          "DRAG actual eight-byte call transaction and full surrounding readback");
    check(s.address[0]==0xE8 && s.address[5]==0x90 && s.address[6]==0x90 && s.address[7]==0x90 &&
          (uintptr_t)(s.address+5+*(int*)(s.address+1))==(uintptr_t)&fl::fl_drag_scroll_stub,
          "DRAG installed call points to actual adapter; original vector stores remain outside patch");
    for(unsigned i=0;i<sizeof s.original;++i) {
        s.address[i]^=1;
        check(!fl::verify_drag_scroll(s,true),"DRAG installed verification covers code and unchanged suffix");
        s.address[i]^=1;
    }
    check(fl::rollback()==0 && !fl::g_undoCount && fl::verify_drag_scroll(s,false),
          "DRAG rollback restores original multiplication instructions and context");
    for(int fault=1;fault<=4;++fault) {
        setup(); fl::resolve_drag_scroll(165,30,FL_G_SCROLL,false,s);
        mem::g_writeFault=fault;
        check(!fl::install_drag_scroll(s),"DRAG actual write/protection/readback failures reject install");
        check(fl::rollback()==0 && !fl::g_patchPoisoned && fl::verify_drag_scroll(s,false),
              "DRAG failure restores every recorded byte without claiming success");
    }
    const int savedGroups=fl::g_groups,savedTarget=fl::g_targetFps,savedRetail=fl::g_retailFps;
    const int savedSites=fl::g_scrollSites;
    const bool savedInstalled=fl::g_installed,savedMeasure=fl::g_measureOnly,savedScroll=fl::g_scrollOn;
    fl::g_installed=true; fl::g_measureOnly=false; fl::g_groups=FL_G_SCROLL;
    fl::g_retailFps=30; fl::g_targetFps=30; fl::g_scrollSites=0; fl::g_scrollOn=false;
    check(FrameLabScrollStatus()==1 && !fl::g_undoCount,
          "DRAG checked GUI option at native 30 reports original speed without requiring a patch");
    fl::g_targetFps=165;
    check(FrameLabScrollStatus()==-2,"DRAG requested missing normalization is never reported as active");
    fl::g_scrollSites=2; fl::g_scrollOn=true; fl::g_dragScrollOn=false;
    check(FrameLabScrollStatus()==-2,"DRAG keyboard-only partial install cannot claim right-drag success");
    fl::g_dragScrollOn=true;
    check(FrameLabScrollStatus()==1,"DRAG complete keyboard/edge/right group reports active");
    fl::g_measureOnly=true;
    check(FrameLabScrollStatus()==0,"DRAG measure mode never reports normalization installed");
    fl::g_measureOnly=false; fl::g_groups=0;
    check(FrameLabScrollStatus()==0,"DRAG unchecked option reports inactive");
    fl::g_groups=savedGroups; fl::g_targetFps=savedTarget; fl::g_retailFps=savedRetail;
    fl::g_scrollSites=savedSites; fl::g_installed=savedInstalled;
    fl::g_measureOnly=savedMeasure; fl::g_scrollOn=savedScroll;
    fl::g_text=text; fl::g_textSize=textSize; fl::g_dragScrollOn=false;
}
