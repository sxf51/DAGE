#ifndef DAGE_EXTENSIONS_OPENTELEMETRY_HPP
#define DAGE_EXTENSIONS_OPENTELEMETRY_HPP
#include "dage/trace.hpp"
#include <map>
#include <memory>
namespace dage { namespace extensions {
struct OpenTelemetrySpan {
    std::string trace_id,span_id,parent_span_id,name,status_code;
    std::uint64_t start_unix_ms=0,end_unix_ms=0;
    std::map<std::string,std::string> attributes;
};
class OpenTelemetryExporter {
public:
    virtual ~OpenTelemetryExporter()=default;
    virtual void export_span(const OpenTelemetrySpan& span)noexcept=0;
    virtual void flush()noexcept{}
};
// SDK-neutral bridge. Host adapters translate OpenTelemetrySpan into their installed
// OpenTelemetry SDK or OTLP transport.
class OpenTelemetryTraceSink final:public TraceSink {
public:
    explicit OpenTelemetryTraceSink(std::shared_ptr<OpenTelemetryExporter> exporter);
    ~OpenTelemetryTraceSink()override;
    void emit(const EventEnvelope& event)noexcept override;
    void flush()noexcept;
private:
    class Impl;std::unique_ptr<Impl> impl_;
};
}}
#endif
