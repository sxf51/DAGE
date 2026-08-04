#include "dage/resource_lease.hpp"
#include "dage/reliability.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>

namespace dage {
namespace {
std::uint64_t unix_ms(){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
Error lease_error(const std::string& code,const std::string& message){
    Error error;error.category="resource_limit";error.code=code;error.message=message;error.retryable=true;return error;
}
struct LeaseState {
    std::string id;std::uint64_t token=0;std::atomic<std::uint64_t> expires{0};
    std::map<std::string,std::uint64_t> resources;std::atomic<bool> released{false};
};
}

class FairResourceLeaseProvider::Impl {
public:
    struct Waiter {
        std::uint64_t sequence;std::int32_t priority;std::string key;
        std::chrono::steady_clock::time_point enqueued;
    };
    explicit Impl(std::map<std::string,std::uint64_t> value):capacity(std::move(value)),available(capacity){}
    void sweep(){
        const std::uint64_t now=unix_ms();
        for(auto it=active.begin();it!=active.end();){
            std::shared_ptr<LeaseState> state=it->lock();
            if(!state){it=active.erase(it);continue;}
            if(!state->released.load()&&state->expires.load()<=now){
                state->released.store(true);for(const auto& item:state->resources)available[item.first]+=item.second;
                changed.notify_all();
            }
            ++it;
        }
    }
    bool fits(const std::map<std::string,std::uint64_t>& resources)const{
        for(const auto& item:resources){auto found=available.find(item.first);
            if(found==available.end()||!item.second||found->second<item.second)return false;}
        return !resources.empty();
    }
    bool selected(const std::shared_ptr<Waiter>& waiter)const{
        const auto now=std::chrono::steady_clock::now();
        auto effective=[&](const std::shared_ptr<Waiter>& item){
            const std::int64_t age=std::chrono::duration_cast<std::chrono::milliseconds>(now-item->enqueued).count()/100;
            return static_cast<std::int64_t>(item->priority)+age;};
        std::int64_t best=effective(waiter);
        for(const auto& item:waiters)best=std::max(best,effective(item));
        if(effective(waiter)!=best)return false;
        for(const auto& item:waiters)
            if(effective(item)==best&&item->key!=last_key&&item->sequence<waiter->sequence)return false;
        if(waiter->key==last_key)for(const auto& item:waiters)
            if(effective(item)==best&&item->key!=last_key)return false;
        return true;
    }
    std::map<std::string,std::uint64_t> capacity,available;
    std::mutex mutex;std::condition_variable changed;std::list<std::shared_ptr<Waiter>> waiters;
    std::vector<std::weak_ptr<LeaseState>> active;std::string last_key;
    std::uint64_t next_sequence=1,next_token=1;
};
namespace {
class FairLease final:public ResourceLease {
public:
    FairLease(std::shared_ptr<FairResourceLeaseProvider::Impl> impl,std::shared_ptr<LeaseState> state)
        :impl_(std::move(impl)),state_(std::move(state)){}
    ~FairLease()override{std::lock_guard<std::mutex> lock(impl_->mutex);release();}
    const std::string& lease_id()const noexcept override{return state_->id;}
    std::uint64_t fencing_token()const noexcept override{return state_->token;}
    std::uint64_t expires_at_unix_ms()const noexcept override{return state_->expires.load();}
    Result<std::uint64_t> renew(std::uint64_t ttl_ms)override{
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(!ttl_ms)return Result<std::uint64_t>::failure(lease_error("LEASE_TTL_INVALID","lease TTL must be positive"));
        if(state_->released.load()||state_->expires.load()<=unix_ms()){
            if(!state_->released.load())release();
            return Result<std::uint64_t>::failure(lease_error("LEASE_EXPIRED","lease has expired"));
        }
        state_->expires.store(unix_ms()+ttl_ms);return Result<std::uint64_t>::success(state_->expires.load());
    }
private:
    void release(){if(state_->released.load())return;state_->released.store(true);
        for(const auto& item:state_->resources)impl_->available[item.first]+=item.second;
        impl_->changed.notify_all();}
    std::shared_ptr<FairResourceLeaseProvider::Impl> impl_;std::shared_ptr<LeaseState> state_;
};
}
FairResourceLeaseProvider::FairResourceLeaseProvider(std::map<std::string,std::uint64_t> capacities)
    :impl_(new Impl(std::move(capacities))){
    if(impl_->capacity.empty())throw std::invalid_argument("resource capacities must not be empty");
    for(const auto& item:impl_->capacity)if(item.first.empty()||!item.second)
        throw std::invalid_argument("resource capacity must have a name and positive value");
}
Result<std::unique_ptr<ResourceLease>> FairResourceLeaseProvider::acquire(
    const ResourceRequest& request,const CancellationToken* cancellation){
    std::map<std::string,std::uint64_t> resources=request.resources;
    if(resources.empty())resources["slots"]=request.units;
    if(!request.lease_ttl_ms)return Result<std::unique_ptr<ResourceLease>>::failure(
        lease_error("LEASE_TTL_INVALID","lease TTL must be positive"));
    for(const auto& item:resources){auto capacity=impl_->capacity.find(item.first);
        if(item.first.empty()||!item.second||capacity==impl_->capacity.end()||item.second>capacity->second)
            return Result<std::unique_ptr<ResourceLease>>::failure(lease_error("LEASE_CAPACITY","invalid resource request: "+item.first));}
    const auto started=std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    std::shared_ptr<Impl::Waiter> waiter(new Impl::Waiter{
        impl_->next_sequence++,request.priority,request.fairness_key.empty()?request.run_id:request.fairness_key,
        std::chrono::steady_clock::now()});
    impl_->waiters.push_back(waiter);
    auto remove_waiter=[&](){impl_->waiters.remove(waiter);impl_->changed.notify_all();};
    for(;;){
        impl_->sweep();
        if(impl_->selected(waiter)&&impl_->fits(resources)){
            for(const auto& item:resources)impl_->available[item.first]-=item.second;
            remove_waiter();impl_->last_key=waiter->key;
            std::shared_ptr<LeaseState> state(new LeaseState());state->token=impl_->next_token++;
            state->id="lease-"+std::to_string(state->token);state->expires.store(unix_ms()+request.lease_ttl_ms);
            state->resources=resources;impl_->active.push_back(state);
            return Result<std::unique_ptr<ResourceLease>>::success(
                std::unique_ptr<ResourceLease>(new FairLease(impl_,state)));
        }
        if(cancellation&&cancellation->is_cancelled()){remove_waiter();
            return Result<std::unique_ptr<ResourceLease>>::failure(lease_error("LEASE_CANCELLED",cancellation->reason()));}
        if(request.deadline_remaining_ms&&static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now()-started).count())>=request.deadline_remaining_ms){remove_waiter();
            return Result<std::unique_ptr<ResourceLease>>::failure(lease_error("LEASE_DEADLINE","resource lease deadline exceeded"));}
        impl_->changed.wait_for(lock,std::chrono::milliseconds(5));
    }
}
std::map<std::string,std::uint64_t> FairResourceLeaseProvider::capacities()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->capacity;
}
std::map<std::string,std::uint64_t> FairResourceLeaseProvider::available()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->sweep();return impl_->available;
}

class SemaphoreLeaseProvider::Impl {
public:
    explicit Impl(std::uint32_t value):provider({{"slots",value}}),capacity(value){}
    FairResourceLeaseProvider provider;std::uint32_t capacity;
};
SemaphoreLeaseProvider::SemaphoreLeaseProvider(std::uint32_t capacity):impl_(new Impl(capacity)){
    if(!capacity)throw std::invalid_argument("lease capacity must be positive");
}
Result<std::unique_ptr<ResourceLease>> SemaphoreLeaseProvider::acquire(
    const ResourceRequest& request,const CancellationToken* cancellation){
    ResourceRequest translated=request;if(translated.resources.empty())translated.resources["slots"]=request.units;
    return impl_->provider.acquire(translated,cancellation);
}
std::uint32_t SemaphoreLeaseProvider::capacity()const noexcept{return impl_->capacity;}
std::uint32_t SemaphoreLeaseProvider::available()const noexcept{
    return static_cast<std::uint32_t>(impl_->provider.available().at("slots"));
}
}
