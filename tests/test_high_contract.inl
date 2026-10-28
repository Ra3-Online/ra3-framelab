// Actual resolver/transaction, using only the test PE's owned non-code section.
static void high_contract() {
    const uintptr_t first = 0x529000, last = 0x8FD000;
    const uintptr_t owned = (uintptr_t)canonicalOwned;
    if (first < owned || last > owned + sizeof canonicalOwned) {
        check(false,"HIGH canonical fixtures fit the owned test IMAGE array");
        return;
    }
    unsigned char* span = (unsigned char*)first;
    const size_t size = last-first;
    DWORD old = 0;
    if (!VirtualProtect(span,size,PAGE_READWRITE,&old)) { check(false,"HIGH fixture protection"); return; }
    struct Spec { uintptr_t va; const short* bytes; int n; };
    const Spec specs[] = {
        {0x6027D0,kSigHighEntry,sizeof kSigHighEntry/sizeof(short)},
        {0x60282F,kSigHighAdvance,sizeof kSigHighAdvance/sizeof(short)},
        {0x60294F,kSigHighExit,sizeof kSigHighExit/sizeof(short)},
        {0x5FFAD0,kSigFpsGlobals,sizeof kSigFpsGlobals/sizeof(short)},
        {0x629829,kSigHighLimiter,sizeof kSigHighLimiter/sizeof(short)},
        {0x5296CA,kSigHighWindow,sizeof kSigHighWindow/sizeof(short)},
        {0x52F527,kSigHighWindow,sizeof kSigHighWindow/sizeof(short)},
        {0x52F5FF,kSigHighDrawableAlpha,sizeof kSigHighDrawableAlpha/sizeof(short)},
        {0x8FB6D2,kSigHighTurretRead,sizeof kSigHighTurretRead/sizeof(short)},
        {0x8FB7BD,kSigHighTurretYaw,sizeof kSigHighTurretYaw/sizeof(short)},
        {0x8FB91C,kSigHighTurretPitch,sizeof kSigHighTurretPitch/sizeof(short)},
        {0x8FBCAA,kSigHighTurretSave,sizeof kSigHighTurretSave/sizeof(short)},
        {0x8E8236,kSigHighBoneAlpha,sizeof kSigHighBoneAlpha/sizeof(short)},
        {0x8F3D85,kSigHighStructureAlpha,sizeof kSigHighStructureAlpha/sizeof(short)}
    };
    auto setup = [&]() {
        memset(span,0xCC,size);
        for (const Spec& s: specs) for(int j=0;j<s.n;++j)
            ((unsigned char*)s.va)[j] = s.bytes[j]<=0xFF ? (unsigned char)s.bytes[j] : 0;
        for(unsigned j=0;j<sizeof kSigHighDrawableCaller/sizeof(short);++j)
            ((unsigned char*)0x53A790)[j]=(unsigned char)kSigHighDrawableCaller[j];
        for(unsigned j=0;j<sizeof kSigHighTurretPrefix/sizeof(short);++j)
            ((unsigned char*)0x8FB6C5)[j]=(unsigned char)kSigHighTurretPrefix[j];
        fl::g_text=span; fl::g_textSize=size; fl::g_targetFps=240;
        fl::g_highOn=false; fl::g_installed=false;
        mem::g_writeFault=0;
    };
    setup();
    fl::HighSites h = {};
    check(fl::resolve_high_sites(h)==FL_OK && fl::verify_high_sites(h,false),
          "HIGH all fourteen owned sites resolve with canonical context");
    check(fl::g_undoCount==0,"HIGH resolve performs no patch");
    ((unsigned char*)0x8FB6C5)[0]^=1;
    check(fl::resolve_high_sites(h)==FL_ERR_SIG_MISMATCH && !fl::verify_high_sites(h,false),
          "HIGH turret engine/flags prefix drift rejects full transaction");
    ((unsigned char*)0x8FB6C5)[0]^=1;
    ((unsigned char*)0x53A790)[0]^=1;
    check(fl::resolve_high_sites(h)==FL_ERR_SIG_MISMATCH && !fl::verify_high_sites(h,false),
          "HIGH drawable caller drift rejects resolver and frozen preimage check");
    ((unsigned char*)0x53A790)[0]^=1;
    const uintptr_t visualPrefixes[] = {0x8E8236u,0x8F3D85u};
    for (uintptr_t prefix : visualPrefixes) {
        ((unsigned char*)prefix)[0]^=1;
        check(fl::resolve_high_sites(h)!=FL_OK && !fl::verify_high_sites(h,false),
              "HIGH extra visual reader engine-load prefix drift rejects installation");
        ((unsigned char*)prefix)[0]^=1;
    }
    for (int i=0;i<fl::kHighSiteCount;++i) {
        setup();
        ((unsigned char*)specs[i].va)[0]^=1;
        h = fl::HighSites();
        check(fl::resolve_high_sites(h)!=FL_OK,"HIGH missing mandatory site rejects full set");
        setup();
        memcpy(span+0x600,(void*)specs[i].va,specs[i].n);
        h = fl::HighSites();
        check(fl::resolve_high_sites(h)!=FL_OK,"HIGH duplicate mandatory site rejects full set");
    }
    setup(); h=fl::HighSites();
    fl::resolve_high_sites(h);
    for(int i=0;i<fl::kHighSiteCount;++i) {
        fl::HighPatchSite* p=fl::high_site(h,i);
        p->address[p->contextSize-1]^=1;
        check(!fl::verify_high_sites(h,false),"HIGH full context detects drift immediately before install");
        p->address[p->contextSize-1]^=1;
    }
    check(fl::install_high_sites(h) && fl::g_highOn && fl::g_undoCount==fl::kHighSiteCount && fl::verify_high_sites(h,true),
          "HIGH actual fourteen-site write/readback transaction");
    check(h.window[0].address[8]==18 && h.window[1].address[8]==18,
          "HIGH actual 240 interpolation windows are both 18 frames");
    for(int i=0;i<fl::kHighSiteCount;++i) {
        fl::HighPatchSite* p=fl::high_site(h,i);
        p->address[p->writeSize-1]^=1;
        check(!fl::verify_high_sites(h,true),"HIGH installed readback detects any patched-site drift");
        p->address[p->writeSize-1]^=1;
    }
    check(fl::rollback()==0 && fl::verify_high_sites(h,false) && fl::g_undoCount==0,
          "HIGH all fourteen sites roll back to full original context");
    fl::g_highOn=false;
    for(int site=0;site<fl::kHighSiteCount;++site) for(int fault=1;fault<=4;++fault) {
        setup(); h=fl::HighSites(); fl::resolve_high_sites(h);
        bool prior=true;
        for(int i=0;i<site;++i) {
            fl::HighPatchSite* p=fl::high_site(h,i);
            unsigned char fake[48]; memcpy(fake,p->original,p->writeSize); fake[0]^=1;
            prior=fl::patch(p->address,fake,p->writeSize,"owned HIGH fault prelude") && prior;
        }
        fl::HighPatchSite* p=fl::high_site(h,site);
        unsigned char fake[48]; memcpy(fake,p->original,p->writeSize); fake[0]^=1;
        mem::g_writeFault=fault;
        check(prior && !fl::patch(p->address,fake,p->writeSize,"owned HIGH fault target"),
              "HIGH each partial transaction rejects write/protection/readback failure");
        check(fl::rollback()==0 && fl::verify_high_sites(h,false) && !fl::g_patchPoisoned,
              "HIGH partial transaction restores every saved byte");
    }
    // Exercise the real ratio resolver: native dispatcher alone gets 30;
    // display budgets keep targetFps, tick predicate belongs to the new hook.
    static int clientFps=30, logicFps=15;
    int* previousClient=fl::g_fpsClient;
    int* previousLogic=fl::g_fpsLogic;
    fl::g_fpsClient=&clientFps; fl::g_fpsLogic=&logicFps;
    const uintptr_t readers[]={0x5FFAD0,0x60283D,0x616256,0x61B59B,0x62935D};
    auto putRatio = [&](uintptr_t va) {
        unsigned char* p=(unsigned char*)va;
        for(unsigned j=0;j<sizeof kSigRatio/sizeof(short);++j)
            p[j]=kSigRatio[j]<=0xFF ? (unsigned char)kSigRatio[j] : 0;
        memcpy(p+1,&fl::g_fpsClient,4); memcpy(p+9,&fl::g_fpsLogic,4);
    };
    auto ratioSetup = [&]() {
        memset(span,0xCC,size);
        for(uintptr_t va:readers) putRatio(va);
    };
    ratioSetup(); fl::Sites r={};
    check(fl::resolve_ratio_redirects(240,r)==FL_OK && r.redirectCount==4 && fl::g_undoCount==0,
          "HIGH real ratio resolver recognizes all five readers and only redirects four operands");
    bool roles=true;
    for(int i=0;i<r.redirectCount;++i) {
        const uintptr_t va=(uintptr_t)r.redirect[i].operand-1;
        roles=roles && va!=readers[0] && r.redirect[i].newTarget==
            (va==readers[2] ? (void*)&fl::g_constRetailFps : NULL);
    }
    check(roles,"HIGH native dispatcher keeps r=2 while all three display budgets keep target ratio");
    for(uintptr_t va:readers) {
        ratioSetup(); *(unsigned char*)va^=1; r=fl::Sites();
        check(fl::resolve_ratio_redirects(240,r)!=FL_OK,"HIGH missing ratio reader rejects installation");
        ratioSetup(); *(unsigned*)((char*)va+9)=0; r=fl::Sites();
        check(fl::resolve_ratio_redirects(240,r)!=FL_OK,"HIGH wrong ratio denominator rejects installation");
    }
    ratioSetup(); putRatio(first+0x600); r=fl::Sites();
    check(fl::resolve_ratio_redirects(240,r)!=FL_OK,"HIGH extra ratio reader at unknown location rejects installation");
    ratioSetup(); r=fl::Sites();
    check(fl::resolve_ratio_redirects(90,r)==FL_OK && r.redirectCount==5,
          "legacy 90 still redirects all five readers to its display ratio");
    bool legacy=true;
    for(int i=0;i<r.redirectCount;++i) legacy=legacy && !r.redirect[i].newTarget;
    check(legacy,"legacy ratio operand targets remain unchanged");
    fl::g_fpsClient=previousClient; fl::g_fpsLogic=previousLogic;
    fl::g_text=text; fl::g_textSize=textSize; fl::g_targetFps=90; fl::g_highOn=false;
}
