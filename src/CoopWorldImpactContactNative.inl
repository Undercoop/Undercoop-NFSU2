// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// The four sites are the paired Witness impulse dispatches in 5AF0E0 and
// its collision-response helper 59FB70. Both expose actual paired witnesses;
// road/prop-only contacts cannot authorize a human/managed-car pair.
namespace CoopWorldImpactContactNative {
struct Body {
    uintptr_t car=0, witness=0, wrapper=0, body=0;
    float mass=0, inverseMass=0, position[3]{}, matrix[16]{}, inertia[16]{};
};
struct Contact {
    uintptr_t car=0, human=0;
    float mass=0, position[3]{}, humanPosition[3]{}, lever[3]{}, impulse[3]{}, angularImpulse[3]{}, velocity[3]{}, angular[3]{};
};
static bool enabled=false;
static bool (*authorize)(uintptr_t,uintptr_t,bool&)=nullptr;
static void (*report)(const Contact&)=nullptr;
// A true narrow-phase contact can perform positional separation without any
// restitution impulse. Permit the owning client to keep that native body
// alive through its next stock integration step, without inventing an impulse.
static void (*touch)(uintptr_t,uintptr_t)=nullptr;
struct Pending { bool active=false; Body target{}; uintptr_t human=0; float lever[3]{}, momentum[3]{}, spin[3]{}; };
static Pending pending{};
struct Entries { uintptr_t first=0x005AF981,second=0x005AF996,extraFirst=0x0059FE97,extraSecond=0x005A0082;
    uintptr_t pairCall=0x005FFBA4,pairTarget=0x005B2610; };
static Entries entries;
static uintptr_t firstContinue=0x005AF987,secondContinue=0x005AF99D;
static uintptr_t extraFirstContinue=0x0059FE9E,extraSecondContinue=0x005A008A,extraSecondFlag=0x0089E7D9;
static uintptr_t pairContinue=0x005FFBA9,pairSolve=0x005B2610;
static uint32_t captured=0,refused=0,suppressed=0,paired=0,authorized=0,invalid=0,dispatches=0;
static uint32_t pairDispatches=0,pairTouches=0;
enum class Refusal : unsigned { Identity,Wrapper,Body,Witness,Human,Mass,Pose,InertiaKind,Inertia,Writable,Lever,Momentum,Recapture,Count };
static uint32_t refusalStages[static_cast<unsigned>(Refusal::Count)]{};
// All four verified dispatch sites pass a LEA of their current stack frame as
// argument 2 (59FE6B, 5A0076, 5AF974 and 5AF989). ReadGameMemory intentionally
// excludes addresses below the image base; the game's x86 stack can live there.
// Do not broaden native-object reads. Read this solver-owned stack vector only
// after checking the current thread's exact stack allocation and byte extent.
static bool ReadContactLever(uintptr_t address,float* output) {
    const auto* tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t limit=reinterpret_cast<uintptr_t>(tib->StackLimit),base=reinterpret_cast<uintptr_t>(tib->StackBase);
    constexpr size_t bytes=sizeof(float)*3;
    if(!output || (address&3) || address<limit || base<limit || base-limit<bytes || address>base-bytes) return false;
    __try {std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
static bool Refuse(Refusal stage,uintptr_t car,uintptr_t body=0,const float* lever=nullptr) {
    const unsigned index=static_cast<unsigned>(stage);
    if(++refusalStages[index]>3) return false;
    static const char* names[]{"identity","wrapper","body","witness","human","mass","pose","inertia_kind","inertia","writable","lever","momentum","recapture"};
    uint32_t mode=0,selector=0;float mass=0,inverseMass=0,matrix[16]{},diagonal[3]{},length[3]{},determinant=0;
    ReadGameMemory(car+0x28,&mode,4);
    if(IsValidUserPtr(body)) {
        ReadGameMemory(body+0x164,&mass,4);ReadGameMemory(body+0x168,&inverseMass,4);
        ReadGameMemory(body+0x158,&selector,4);ReadGameMemory(body+0x30,matrix,64);
        const uintptr_t inertia=body+(selector?0x1B0:0x170);
        for(unsigned row=0;row<3;++row) {ReadGameMemory(inertia+row*0x14,&diagonal[row],4);
            for(unsigned col=0;col<3;++col) length[row]+=matrix[row*4+col]*matrix[row*4+col];}
        determinant=matrix[0]*(matrix[5]*matrix[10]-matrix[6]*matrix[9])-matrix[1]*(matrix[4]*matrix[10]-matrix[6]*matrix[8])+matrix[2]*(matrix[4]*matrix[9]-matrix[5]*matrix[8]);
    }
    const float zero[3]{};if(!lever) lever=zero;
    char line[512]{};
    std::snprintf(line,sizeof(line),"[AI_IMPACT_NATIVE] refusal=%s count=%u car=%08lX mode=%u body=%08lX mass=%.9g invmass=%.9g selector=%u inertia=(%.9g,%.9g,%.9g) rlen=(%.9g,%.9g,%.9g) det=%.9g lever=(%.9g,%.9g,%.9g)\r\n",
        names[index],refusalStages[index],static_cast<unsigned long>(car),mode,static_cast<unsigned long>(body),mass,inverseMass,selector,
        diagonal[0],diagonal[1],diagonal[2],length[0],length[1],length[2],determinant,lever[0],lever[1],lever[2]);
    LogMessage(line);return false;
}

static bool ReadBody(uintptr_t car,Body& output,bool diagnose=false) {
    using namespace CoopPhysicalMotionNative;
    Body b{}; b.car=car;
    uintptr_t param=0,ride=0,geometry=0,base=0,owner=0,vt=0,bodyGeometry=0,witnessBody=0,witnessPhysics=0;
    uint32_t mode=0,kind=0,inertiaKind=0;uint8_t hasBody=0;
    if(!enabled || !IsValidUserPtr(car) ||
        !ReadGameMemory(car+0x14,&param,4) || !IsValidUserPtr(param) ||
        !ReadGameMemory(car+0x10,&ride,4) || ride!=param+0x10 ||
        !ReadGameMemory(param+4,&kind,4) || (kind!=2 && kind!=3) ||
        !ReadGameMemory(car+0x20,&geometry,4) || geometry!=ride+0x40 ||
        !ReadGameMemory(car+0x40,&base,4) || base!=geometry ||
        !ReadGameMemory(car+0x28,&mode,4) || (mode!=1 && mode!=3))
        return diagnose?Refuse(Refusal::Identity,car):false;
    if(!ReadGameMemory(car+0x34,&b.wrapper,4) || !IsValidUserPtr(b.wrapper) ||
        !ReadGameMemory(b.wrapper,&vt,4) || vt!=(mode==3?kRemotePhysicsVtable:kPhysicsVtable) ||
        !ReadGameMemory(b.wrapper+(mode==3?0x34:0x5C),&owner,4) || owner!=car ||
        !ReadGameMemory(b.wrapper+(mode==3?0x30:0x58),&base,4) || base!=car+0x40 ||
        !ReadGameMemory(b.wrapper+(mode==3?0x2C:0x54),&bodyGeometry,4) || bodyGeometry!=geometry ||
        !ReadGameMemory(b.wrapper+0x20,&b.body,4) || !IsValidUserPtr(b.body))
        return diagnose?Refuse(Refusal::Wrapper,car,b.body):false;
    if(!ReadGameMemory(b.body,&vt,4) || vt!=kBodyVtable ||
        !ReadGameMemory(b.body+0x14,&bodyGeometry,4) || bodyGeometry!=geometry+0x110)
        return diagnose?Refuse(Refusal::Body,car,b.body):false;
    if(!ReadGameMemory(car+0x3C,&b.witness,4) || !IsValidUserPtr(b.witness) ||
        !ReadGameMemory(b.witness,&vt,4) || vt!=kWitnessVtable ||
        !ReadGameMemory(b.witness+0x130,&owner,4) || owner!=car ||
        !ReadGameMemory(b.witness+0x5C,&hasBody,1) || hasBody!=1 ||
        !ReadGameMemory(b.witness+0x64,&witnessPhysics,4) || witnessPhysics!=b.wrapper+8 ||
        !ReadGameMemory(b.witness+0x68,&witnessBody,4) || witnessBody!=b.body)
        return diagnose?Refuse(Refusal::Witness,car,b.body):false;
    if(!CoopWorldPhysicalMotionNative::ExcludesHumanBody(CoopRaceCar(false),b.wrapper,b.body) ||
        !CoopWorldPhysicalMotionNative::ExcludesHumanBody(CoopRaceCar(true),b.wrapper,b.body))
        return diagnose?Refuse(Refusal::Human,car,b.body):false;
    if(!ReadGameMemory(b.body+0x164,&b.mass,4) || !std::isfinite(b.mass) || b.mass<=0 || b.mass>1000000 ||
        !ReadGameMemory(b.body+0x168,&b.inverseMass,4) || !std::isfinite(b.inverseMass) ||
        b.inverseMass<=0 || std::fabs(b.mass*b.inverseMass-1)>0.01f)
        return diagnose?Refuse(Refusal::Mass,car,b.body):false;
    if(!ReadGameMemory(b.body+0x20,b.position,12) || !FiniteVector(b.position,1000000) ||
        !ReadGameMemory(b.body+0x30,b.matrix,64) || !coop_motion::IsRotation(b.matrix))
        return diagnose?Refuse(Refusal::Pose,car,b.body):false;
    if(!ReadGameMemory(b.body+0x158,&inertiaKind,4) || inertiaKind>1)
        return diagnose?Refuse(Refusal::InertiaKind,car,b.body):false;
    if(!ReadGameMemory(b.body+(inertiaKind?0x1B0:0x170),b.inertia,64))
        return diagnose?Refuse(Refusal::Inertia,car,b.body):false;
    if(!WritableRange(b.body+0x70,0x5C) || !WritableRange(car+0x70,12) || !WritableRange(car+0x110,12))
        return diagnose?Refuse(Refusal::Writable,car,b.body):false;
    for(unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col)
        if(!std::isfinite(b.inertia[row*4+col]) || std::fabs(b.inertia[row*4+col])>1000000)
            return diagnose?Refuse(Refusal::Inertia,car,b.body):false;
    for(unsigned row=0;row<3;++row) if(b.inertia[row*5]<=0)
        return diagnose?Refuse(Refusal::Inertia,car,b.body):false;
    output=b;return true;
}
// Same R * I^-1 * R^T mapping as native 59C71A..59C759. The wire contains
// the actual solver momentum delta, not a client-side teleported AI position.
static void AngularDelta(const Body& b,const float* torque,float* output) {
    float local[3]{},scaled[3]{};
    for(unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col) local[row]+=b.matrix[row*4+col]*torque[col];
    for(unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col) scaled[row]+=b.inertia[col*4+row]*local[col];
    for(unsigned row=0;row<3;++row) {output[row]=0;for(unsigned col=0;col<3;++col) output[row]+=b.matrix[col*4+row]*scaled[col];}
}
static bool CarFromWitness(uintptr_t witness,uintptr_t& car) {
    uintptr_t vt=0,back=0;uint8_t type=1;
    return IsValidUserPtr(witness) && ReadGameMemory(witness,&vt,4) && vt==CoopPhysicalMotionNative::kWitnessVtable &&
        ReadGameMemory(witness+0x11,&type,1) && type==0 && ReadGameMemory(witness+0x130,&car,4) &&
        IsValidUserPtr(car) && ReadGameMemory(car+0x3C,&back,4) && back==witness;
}
static void __cdecl TouchPair(uintptr_t pair) {
    ++pairDispatches;
    if(!enabled || !authorize || !touch) return;
    uintptr_t a=0,b=0,collision=0,collisionA=0,collisionB=0,carA=0,carB=0;
    uint8_t actualContact=0;
    // 5B51C0..5B51C6 only constructs this solver pair when the narrow-phase
    // record's +14 flag is nonzero. 5B4F90 stores that same record at pair+14.
    // Check it again, including both exact record owners: a neighbourhood or
    // a stale/mismatched broad-phase pair must never release replica physics.
    if(!IsValidUserPtr(pair) || !ReadGameMemory(pair+0x0C,&a,4) || !ReadGameMemory(pair+0x10,&b,4) || a==b ||
        !ReadGameMemory(pair+0x14,&collision,4) || !IsValidUserPtr(collision) ||
        !ReadGameMemory(collision+0x14,&actualContact,1) || !actualContact ||
        !ReadGameMemory(collision+0x238,&collisionA,4) || !ReadGameMemory(collision+0x23C,&collisionB,4) ||
        !((collisionA==a && collisionB==b) || (collisionA==b && collisionB==a)) ||
        !CarFromWitness(a,carA) || !CarFromWitness(b,carB)) return;
    const uintptr_t cars[]{carA,carB},witnesses[]{a,b};
    for(unsigned side=0;side<2;++side) {
        bool skip=false;Body body{};
        if(!authorize(cars[side],cars[side^1],skip) || skip ||
            !ReadBody(cars[side],body,true) || body.witness!=witnesses[side]) continue;
        ++pairTouches;touch(cars[side],cars[side^1]);
    }
}
static int BeginPair(uintptr_t witness,uintptr_t a,uintptr_t b,const uintptr_t* args) {
    pending={};
    if(!enabled || !authorize || !report || !args) return 0;
    uintptr_t target=0,human=0;
    if(a==b ||
        (witness!=a && witness!=b) || !CarFromWitness(witness,target) ||
        !CarFromWitness(witness==a?b:a,human)) return 0;
    ++paired;
    bool skip=false;
    if(!authorize(target,human,skip)) return 0;
    ++authorized;
    Body body{}; if(!ReadBody(target,body,true) || body.witness!=witness) {++refused;return 0;}
    if(skip) {++suppressed;return 1;}
    Pending p{};p.target=body;p.human=human;
    if(!ReadContactLever(args[1],p.lever) || !CoopPhysicalMotionNative::FiniteVector(p.lever,8))
        {++refused;Refuse(Refusal::Lever,target,body.body,p.lever);return 0;}
    if(!ReadGameMemory(body.body+0x70,p.momentum,12) || !ReadGameMemory(body.body+0x80,p.spin,12))
        {++refused;Refuse(Refusal::Momentum,target,body.body);return 0;}
    p.active=true;pending=p;return 0;
}
static int __cdecl Begin(uintptr_t witness,uintptr_t pair,const uintptr_t* args) {
    pending={};++dispatches;uintptr_t a=0,b=0;
    if(!enabled || !ReadGameMemory(pair+0x0C,&a,4) || !ReadGameMemory(pair+0x10,&b,4)) return 0;
    return BeginPair(witness,a,b,args);
}
static int __cdecl BeginExtra(uintptr_t witness,uintptr_t response,const uintptr_t* args) {
    pending={};++dispatches;uintptr_t collision=0,a=0,b=0;
    // 59FBC6..59FBF1: response+64 owns the narrow-phase collision record.
    // Its +238/+23C are the two witnesses that generated this response.
    if(!enabled || !ReadGameMemory(response+0x64,&collision,4) ||
        !ReadGameMemory(collision+0x238,&a,4) || !ReadGameMemory(collision+0x23C,&b,4)) return 0;
    return BeginPair(witness,a,b,args);
}
static void __cdecl End() {
    const auto p=pending;pending={};if(!p.active || !report || !authorize) return;
    bool skip=false;Body current{};float momentum[3]{},spin[3]{};
    if(!authorize(p.target.car,p.human,skip) || skip || !ReadBody(p.target.car,current,true) || current.body!=p.target.body ||
        !ReadGameMemory(current.body+0x70,momentum,12) || !ReadGameMemory(current.body+0x80,spin,12)) {++refused;return;}
    Contact c{};c.car=current.car;c.human=p.human;c.mass=current.mass;
    std::memcpy(c.position,current.position,12);std::memcpy(c.lever,p.lever,12);
    if(!ReadGameMemory(c.human+0x60,c.humanPosition,12)) return;
    for(unsigned i=0;i<3;++i) {c.impulse[i]=momentum[i]-p.momentum[i];c.angularImpulse[i]=spin[i]-p.spin[i];c.velocity[i]=c.impulse[i]*current.inverseMass;}
    AngularDelta(current,c.angularImpulse,c.angular);
    if(!CoopPhysicalMotionNative::FiniteVector(c.velocity,160) || !CoopPhysicalMotionNative::FiniteVector(c.angular,100)) {++invalid;return;}
    ++captured;report(c);
}
// Preserve EFLAGS, all GPRs and x87/SSE around the C++ observer. Align the
// private FXSAVE area even when the game's incoming stack is unaligned.
__declspec(naked) static void PairHook() {
    __asm {
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        push dword ptr [ebp+24]
        call TouchPair
        add esp,4
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        // This is only an observer. Preserve native wake-up, positional
        // separation, both-human response and registry/contact lifecycles.
        call dword ptr [pairSolve]
        jmp dword ptr [pairContinue]
    }
}
__declspec(naked) static void FirstHook() {
    __asm {
        lea esp,[esp-4]
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        lea eax,[ebp+40]
        push eax
        push dword ptr [ebp+4]
        push dword ptr [ebp+24]
        call Begin
        add esp,12
        mov [ebp+36],eax
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        pushfd
        cmp dword ptr [esp+4],0
        jne skipFirst
        popfd
        lea esp,[esp+4]
        call dword ptr [edx+50h]
        jmp endFirst
    skipFirst:
        popfd
        lea esp,[esp+12]
    endFirst:
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        call End
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        mov ecx,[esi+0Ch]
        jmp dword ptr [firstContinue]
    }
}
__declspec(naked) static void SecondHook() {
    __asm {
        lea esp,[esp-4]
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        lea eax,[ebp+40]
        push eax
        push dword ptr [ebp+4]
        push dword ptr [ebp+24]
        call Begin
        add esp,12
        mov [ebp+36],eax
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        pushfd
        cmp dword ptr [esp+4],0
        jne skipSecond
        popfd
        lea esp,[esp+4]
        call dword ptr [edx+50h]
        jmp endSecond
    skipSecond:
        popfd
        lea esp,[esp+12]
    endSecond:
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        call End
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        fld dword ptr [esp+68h]
        jmp dword ptr [secondContinue]
    }
}
__declspec(naked) static void ExtraFirstHook() {
    __asm {
        lea esp,[esp-4]
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        lea eax,[ebp+40]
        push eax
        push dword ptr [ebp]
        push dword ptr [ebp+4]
        call BeginExtra
        add esp,12
        mov [ebp+36],eax
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        pushfd
        cmp dword ptr [esp+4],0
        jne skipExtraFirst
        popfd
        lea esp,[esp+4]
        call dword ptr [eax+50h]
        jmp endExtraFirst
    skipExtraFirst:
        popfd
        lea esp,[esp+12]
    endExtraFirst:
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        call End
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        // Replay the stolen short JMP directly to its original destination.
        jmp dword ptr [extraFirstContinue]
    }
}
__declspec(naked) static void ExtraSecondHook() {
    __asm {
        lea esp,[esp-4]
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        lea eax,[ebp+40]
        push eax
        push dword ptr [ebp]
        push dword ptr [ebp+4]
        call BeginExtra
        add esp,12
        mov [ebp+36],eax
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        pushfd
        cmp dword ptr [esp+4],0
        jne skipExtraSecond
        popfd
        lea esp,[esp+4]
        call dword ptr [edx+50h]
        jmp endExtraSecond
    skipExtraSecond:
        popfd
        lea esp,[esp+12]
    endExtraSecond:
        pushfd
        pushad
        mov ebp,esp
        sub esp,528
        lea ebx,[esp+15]
        and ebx,-16
        fxsave [ebx]
        fninit
        call End
        fxrstor [ebx]
        mov esp,ebp
        popad
        popfd
        // Replay MOV AL,[89E7D9], using a replaceable operand in fixtures.
        push edx
        mov edx,dword ptr [extraSecondFlag]
        mov al,byte ptr [edx]
        pop edx
        jmp dword ptr [extraSecondContinue]
    }
}
static bool Install() {
    if(enabled) return true;
    const uint8_t a[]{0xFF,0x52,0x50,0x8B,0x4E,0x0C},b[]{0xFF,0x52,0x50,0xD9,0x44,0x24,0x68},
        c[]{0xFF,0x50,0x50,0xEB,0x02},d[]{0xFF,0x52,0x50,0xA0,0xD9,0xE7,0x89,0x00};
    uint8_t pairSignature[]{0xE8,0,0,0,0};
    const int32_t pairRelative=static_cast<int32_t>(entries.pairTarget-(entries.pairCall+5));
    std::memcpy(pairSignature+1,&pairRelative,4);
    struct Site {uintptr_t address,hook;const uint8_t* signature;size_t bytes;DWORD protection;};
    Site sites[]{{entries.first,reinterpret_cast<uintptr_t>(&FirstHook),a,sizeof(a),0},
        {entries.second,reinterpret_cast<uintptr_t>(&SecondHook),b,sizeof(b),0},
        {entries.extraFirst,reinterpret_cast<uintptr_t>(&ExtraFirstHook),c,sizeof(c),0},
        {entries.extraSecond,reinterpret_cast<uintptr_t>(&ExtraSecondHook),d,sizeof(d),0},
        {entries.pairCall,reinterpret_cast<uintptr_t>(&PairHook),pairSignature,sizeof(pairSignature),0}};
    if(!CoopPhysicalMotionNative::enabled || !CoopWorldContactNative::enabled ||
        !CoopWorldContactNative::Signature(entries.first,a,sizeof(a)) || !CoopWorldContactNative::Signature(entries.second,b,sizeof(b)) ||
        !CoopWorldContactNative::Signature(entries.extraFirst,c,sizeof(c)) || !CoopWorldContactNative::Signature(entries.extraSecond,d,sizeof(d)) ||
        !CoopWorldContactNative::Signature(entries.pairCall,pairSignature,sizeof(pairSignature))) return false;
    DWORD unused=0;size_t protectedCount=0;
    for(auto& site:sites) {
        if(!VirtualProtect(reinterpret_cast<void*>(site.address),site.bytes,PAGE_EXECUTE_READWRITE,&site.protection)) {
            for(size_t i=0;i<protectedCount;++i) VirtualProtect(reinterpret_cast<void*>(sites[i].address),sites[i].bytes,sites[i].protection,&unused);
            return false;
        }
        ++protectedCount;
    }
    for(const auto& site:sites) {
        uint8_t patch[8]{0xE9,0,0,0,0,0x90,0x90,0x90};
        const int32_t relative=static_cast<int32_t>(site.hook-(site.address+5));std::memcpy(patch+1,&relative,4);
        std::memcpy(reinterpret_cast<void*>(site.address),patch,site.bytes);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(site.address),site.bytes);
    }
    for(const auto& site:sites) VirtualProtect(reinterpret_cast<void*>(site.address),site.bytes,site.protection,&unused);
    firstContinue=entries.first+sizeof(a);secondContinue=entries.second+sizeof(b);
    extraFirstContinue=entries.extraFirst+7;extraSecondContinue=entries.extraSecond+sizeof(d);
    pairContinue=entries.pairCall+sizeof(pairSignature);pairSolve=entries.pairTarget;enabled=true;return true;
}
}
