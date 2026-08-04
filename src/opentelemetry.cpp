#include "dage/extensions/opentelemetry.hpp"
#include <mutex>
#include <stdexcept>
namespace dage { namespace extensions {
class OpenTelemetryTraceSink::Impl {
public:
    explicit Impl(std::shared_ptr<OpenTelemetryExporter> value):exporter(std::move(value)){}
    std::shared_ptr<OpenTelemetryExporter> exporter;std::mutex mutex;
    std::map<std::string,OpenTelemetrySpan> active;
};
OpenTelemetryTraceSink::OpenTelemetryTraceSink(std::shared_ptr<OpenTelemetryExporter> exporter)
    :impl_(new Impl(std::move(exporter))){if(!impl_->exporter)throw std::invalid_argument("exporter must not be null");}
OpenTelemetryTraceSink::~OpenTelemetryTraceSink()=default;
void OpenTelemetryTraceSink::emit(const EventEnvelope& event)noexcept{
    try{std::lock_guard<std::mutex> lock(impl_->mutex);
        if(event.event=="node_started"){
            OpenTelemetrySpan span;span.trace_id=event.trace_id;span.span_id=event.span_id;
            span.parent_span_id=event.parent_span_id;span.name="dage.node."+event.node_id;
            span.start_unix_ms=event.timestamp_unix_ms;span.attributes["dage.run_id"]=event.run_id;
            span.attributes["dage.workflow_digest"]=event.workflow_digest;span.attributes["dage.node.type"]=event.node_type;
            span.attributes["dage.causation_id"]=event.causation_id;
            span.attributes["dage.invocation_id"]=event.invocation_id;impl_->active[event.span_id]=std::move(span);return;
        }
        if(event.event=="node_succeeded"||event.event=="node_failed"||event.event=="node_blocked"||
           event.event=="run_suspended"){
            OpenTelemetrySpan span;auto found=impl_->active.find(event.span_id);
            if(found!=impl_->active.end()){span=std::move(found->second);impl_->active.erase(found);}
            else{span.trace_id=event.trace_id;span.span_id=event.span_id;span.parent_span_id=event.parent_span_id;
                span.name="dage.node."+event.node_id;span.start_unix_ms=event.timestamp_unix_ms;}
            span.end_unix_ms=event.timestamp_unix_ms;
            span.status_code=event.event=="node_succeeded"?"OK":(event.event=="run_suspended"?"UNSET":"ERROR");
            if(!event.category.empty())span.attributes["error.type"]=event.category;
            if(!event.code.empty())span.attributes["error.code"]=event.code;
            impl_->exporter->export_span(span);
        }
    }catch(...){}
}
void OpenTelemetryTraceSink::flush()noexcept{try{impl_->exporter->flush();}catch(...){}}
}}
