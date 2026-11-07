// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopProfileContext.h"
#include <array>
#include <mutex>

// Native-game-IAT write barrier only. Mod-owned atomic receipt/native commits
// use their OWN DLL imports and never bypass this gate by a global bool. The
// barrier does no file IO and must be bound before the native personal profile
// can open writes. All open/close/rename/delete calls retain Lease until the
// actual OS operation and tracking finish, closing the Arm/CreateFile race.
namespace coop_special_save {
struct Id {uint64_t session=0;uint32_t ticket=0,operation=0;};
inline bool Valid(Id id){return id.session&&id.ticket&&id.operation;}
inline bool Same(Id a,Id b){return a.session==b.session&&a.ticket==b.ticket&&a.operation==b.operation;}
enum class Settlement {Paid,Cancelled};
struct DurableSettlement {Id id{};Settlement result=Settlement::Paid;};
enum class Mode {Inactive,Open,Fenced,Faulted};
class Fence {
    struct Handle {uintptr_t id=0;std::wstring path;};
    std::mutex mutex_;Mode mode_=Mode::Inactive;
    std::wstring observedRoot_,root_,profile_,directory_;Id id_{};
    std::array<Handle,64> handles_{};
    bool OwnPath(const std::wstring& path)const{
        std::wstring canonical;
        return mode_!=Mode::Inactive&&coop_save::CanonicalWindowsPath(path,canonical)&&coop_save::PathIsWithin(canonical,directory_);
    }
    bool EmptyOwnHandles()const{for(const auto& h:handles_)if(h.id&&OwnPath(h.path))return false;return true;}
    bool ObservedPath(const std::wstring& path)const{
        std::wstring canonical;
        return !observedRoot_.empty()&&coop_save::CanonicalWindowsPath(path,canonical)&&coop_save::PathIsWithin(canonical,observedRoot_);
    }
    bool Space()const{for(const auto& h:handles_)if(!h.id)return true;return false;}
    static bool Mutation(uint32_t access,uint32_t creation){
        // GENERIC_WRITE/ALL, DELETE, WRITE_DAC/OWNER, native data/append/EA/
        // attribute/delete-child rights. CREATE/TRUNCATE may modify even with
        // no write access, so OPEN_EXISTING(3) is the only read-only disposition.
        return (access&0x500D0156u)!=0||creation!=3;
    }
public:
    class Lease {
        Fence& owner_;std::unique_lock<std::mutex> lock_;
    public:
        explicit Lease(Fence& owner):owner_(owner),lock_(owner.mutex_){}
        Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        bool MayOpen(const std::wstring& path,uint32_t access,uint32_t creation)const{
            if(!Mutation(access,creation))return true;
            if(owner_.OwnPath(path)&&owner_.mode_!=Mode::Open)return false;
            return !owner_.ObservedPath(path)||owner_.Space();
        }
        bool Opened(uintptr_t handle,const std::wstring& path,uint32_t access,uint32_t creation){
            if(!handle||handle==uintptr_t(-1)||!owner_.ObservedPath(path)||!Mutation(access,creation))return true;
            if(owner_.OwnPath(path)&&owner_.mode_!=Mode::Open){owner_.mode_=Mode::Faulted;return false;}
            for(const auto& h:owner_.handles_)if(h.id==handle){owner_.mode_=Mode::Faulted;return false;}
            std::wstring canonical;if(!coop_save::CanonicalWindowsPath(path,canonical)){owner_.mode_=Mode::Faulted;return false;}
            for(auto& h:owner_.handles_)if(!h.id){h.id=handle;h.path=std::move(canonical);return true;}
            owner_.mode_=Mode::Faulted;return false;
        }
        void Closed(uintptr_t handle,bool success){
            if(!success)return;
            for(auto& h:owner_.handles_)if(h.id==handle){h={};return;}
        }
        bool MayMutate(const std::wstring& path)const{return !owner_.OwnPath(path)||owner_.mode_==Mode::Open;}
        bool MayMove(const std::wstring& from,const std::wstring& to)const{return MayMutate(from)&&MayMutate(to);}
    };
    Lease Lock(){return Lease(*this);}
    bool ObserveRoot(const std::wstring& coopRoot){
        std::lock_guard<std::mutex> lock(mutex_);std::wstring canonical;
        if(!coop_save::CanonicalWindowsPath(coopRoot,canonical))return false;
        // Called once before native CO-OP root routing is enabled, after all
        // required game-IAT hooks are verified. Writers to any CO-OP profile
        // are observed even BEFORE the selected personal name is bound.
        if(!observedRoot_.empty())return coop_save::EqualPathText(observedRoot_,canonical);
        if(mode_!=Mode::Inactive)return false;
        observedRoot_=std::move(canonical);return true;
    }
    bool Observing(){std::lock_guard<std::mutex> lock(mutex_);return !observedRoot_.empty();}
    bool Bind(const std::wstring& roleRoot,const std::wstring& personal){
        std::lock_guard<std::mutex> lock(mutex_);std::wstring root;
        if(!coop_save::CanonicalWindowsPath(roleRoot,root)||!coop_save::IsSafeProfileName(personal)||
            observedRoot_.empty()||!coop_save::PathIsWithin(root,observedRoot_))return false;
        if(mode_!=Mode::Inactive){
            // No implicit reset/rebind may retire a pending or faulted receipt.
            return coop_save::EqualPathText(root_,root)&&coop_save::EqualPathText(profile_,personal)&&mode_!=Mode::Faulted;
        }
        root_=std::move(root);profile_=personal;directory_=root_+L"\\"+personal;mode_=Mode::Open;return true;
    }
    bool Arm(Id id){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!Valid(id)||mode_==Mode::Inactive||mode_==Mode::Faulted)return false;
        if(mode_==Mode::Fenced)return Same(id_,id);
        if(!EmptyOwnHandles())return false; // Includes writers observed BEFORE Bind.
        id_=id;mode_=Mode::Fenced;return true;
    }
    bool Held(Id id){
        std::lock_guard<std::mutex> lock(mutex_);
        return Valid(id)&&mode_==Mode::Fenced&&Same(id_,id)&&EmptyOwnHandles();
    }
    bool Finish(const DurableSettlement& proof){
        std::lock_guard<std::mutex> lock(mutex_);
        if(mode_!=Mode::Fenced||!Same(id_,proof.id)||!Valid(proof.id)||!EmptyOwnHandles()||
            (proof.result!=Settlement::Paid&&proof.result!=Settlement::Cancelled))return false;
        // Caller must have flushed and verified its exact own Paid/Cancelled
        // receipt and native image. Wire Commit/Saved is NOT a settlement.
        id_={};mode_=Mode::Open;return true;
    }
    bool Unbind(){
        std::lock_guard<std::mutex> lock(mutex_);
        if((mode_!=Mode::Open&&mode_!=Mode::Inactive)||!EmptyOwnHandles())return false;
        mode_=Mode::Inactive;root_.clear();profile_.clear();directory_.clear();id_={};return true;
    }
    Mode Status(){std::lock_guard<std::mutex> lock(mutex_);return mode_;}
};
} // namespace coop_special_save
