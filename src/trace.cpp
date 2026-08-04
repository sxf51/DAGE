#include "dage/trace.hpp"
#include "sha256.hpp"
#include <json/json.h>
#include <functional>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace dage {
std::uint32_t trace_sample_bucket(const std::string& trace_id)noexcept{
    try{const std::string digest=sha256_digest(trace_id);const std::size_t offset=digest.find(':')+1;std::uint32_t value=0;
        for(std::size_t i=offset;i<offset+8&&i<digest.size();++i){const char c=digest[i];
            value=(value<<4)|static_cast<std::uint32_t>(c>='0'&&c<='9'?c-'0':10+(c>='a'&&c<='f'?c-'a':c-'A'));
        }return value%10000;}catch(...){return 0;}
}
void MemoryTraceSink::emit(const EventEnvelope& event)noexcept{
    try{std::lock_guard<std::mutex> lock(mutex_);events_.push_back(event);}catch(...){}
}
std::vector<EventEnvelope> MemoryTraceSink::events()const{
    std::lock_guard<std::mutex> lock(mutex_);return events_;
}
void MemoryTraceSink::clear(){std::lock_guard<std::mutex> lock(mutex_);events_.clear();}

class AsyncTraceSink::Impl {
public:
    Impl(std::shared_ptr<TraceSink> value,const AsyncTraceOptions& configured)
        :downstream(std::move(value)),options(configured),stopping(false),active(false),
         worker([this](){run();}){}
    ~Impl(){{
        std::lock_guard<std::mutex> lock(mutex);stopping=true;}ready.notify_all();space.notify_all();
        if(worker.joinable())worker.join();}
    void run(){
        for(;;){EventEnvelope event;{
            std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[this](){return stopping||!queue.empty();});
            if(stopping&&queue.empty())break;
            event=std::move(queue.front());queue.pop_front();active=true;space.notify_all();}
            downstream->emit(event);{
                std::lock_guard<std::mutex> lock(mutex);++statistics.exported;active=false;
                if(queue.empty())drained.notify_all();}
        }
    }
    std::shared_ptr<TraceSink> downstream;AsyncTraceOptions options;bool stopping,active;
    mutable std::mutex mutex;std::condition_variable ready,space,drained;
    std::deque<EventEnvelope> queue;TraceQueueStats statistics;std::thread worker;
};
AsyncTraceSink::AsyncTraceSink(std::shared_ptr<TraceSink> downstream,const AsyncTraceOptions& options)
    :impl_(new Impl(std::move(downstream),options)){
    if(!impl_->downstream||!impl_->options.capacity)throw std::invalid_argument("invalid async trace options");
}
AsyncTraceSink::~AsyncTraceSink()=default;
void AsyncTraceSink::emit(const EventEnvelope& event)noexcept{
    try{std::unique_lock<std::mutex> lock(impl_->mutex);
        if(impl_->stopping)return;
        if(impl_->queue.size()>=impl_->options.capacity){
            if(impl_->options.full_policy==TraceQueueFullPolicy::DropNewest){++impl_->statistics.dropped;return;}
            if(impl_->options.full_policy==TraceQueueFullPolicy::DropOldest){impl_->queue.pop_front();++impl_->statistics.dropped;}
            else{impl_->space.wait(lock,[this](){return impl_->stopping||impl_->queue.size()<impl_->options.capacity;});
                if(impl_->stopping)return;}
        }
        impl_->queue.push_back(event);++impl_->statistics.accepted;lock.unlock();impl_->ready.notify_one();
    }catch(...){}
}
void AsyncTraceSink::flush(){std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->drained.wait(lock,[this](){return impl_->queue.empty()&&!impl_->active;});}
TraceQueueStats AsyncTraceSink::stats()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->statistics;}

SamplingTraceSink::SamplingTraceSink(std::shared_ptr<TraceSink> downstream,const TraceSamplingOptions& options)
    :downstream_(std::move(downstream)),options_(options){
    if(!downstream_||options_.rate_basis_points>10000)throw std::invalid_argument("invalid sampling options");
}
void SamplingTraceSink::emit(const EventEnvelope& event)noexcept{
    try{const bool error=!event.code.empty();const std::string key=event.trace_id.empty()?event.run_id:event.trace_id;
        const bool sampled=trace_sample_bucket(key)<options_.rate_basis_points;
        if(sampled||(error&&options_.always_sample_errors))downstream_->emit(event);}catch(...){}
}
namespace {
void redact(Json::Value& value,const std::vector<std::string>& fields,const std::string& replacement){
    if(value.isObject()){for(const std::string& name:value.getMemberNames()){
        if(std::find(fields.begin(),fields.end(),name)!=fields.end())value[name]=replacement;else redact(value[name],fields,replacement);}}
    else if(value.isArray())for(Json::ArrayIndex i=0;i<value.size();++i)redact(value[i],fields,replacement);
}
}
RedactingTraceSink::RedactingTraceSink(std::shared_ptr<TraceSink> downstream,std::vector<std::string> fields,
                                       std::string replacement)
    :downstream_(std::move(downstream)),fields_(std::move(fields)),replacement_(std::move(replacement)){
    if(!downstream_)throw std::invalid_argument("redaction downstream must not be null");
}
void RedactingTraceSink::emit(const EventEnvelope& event)noexcept{
    try{EventEnvelope copy=event;if(!copy.payload_json.empty()){
        Json::CharReaderBuilder rb;Json::Value root;std::string errors;std::istringstream input(copy.payload_json);
        if(Json::parseFromStream(rb,input,&root,&errors)){redact(root,fields_,replacement_);
            Json::StreamWriterBuilder wb;wb["indentation"]="";copy.payload_json=Json::writeString(wb,root);}}
        downstream_->emit(copy);}catch(...){}
}
} // namespace dage
