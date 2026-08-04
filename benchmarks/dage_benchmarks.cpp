#include "dage/dage.hpp"
#include "dage/ir.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#ifndef DAGE_BENCHMARK_COMPILER
#define DAGE_BENCHMARK_COMPILER "unknown"
#endif
#ifndef DAGE_BENCHMARK_BUILD_TYPE
#define DAGE_BENCHMARK_BUILD_TYPE "unknown"
#endif
#ifndef DAGE_BENCHMARK_LINKAGE
#define DAGE_BENCHMARK_LINKAGE "unknown"
#endif

#if defined(_WIN32)
constexpr const char* benchmark_platform="windows";
#elif defined(__APPLE__)
constexpr const char* benchmark_platform="macos";
#elif defined(__linux__)
constexpr const char* benchmark_platform="linux";
#else
constexpr const char* benchmark_platform="unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__)
constexpr const char* benchmark_architecture="x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
constexpr const char* benchmark_architecture="arm64";
#else
constexpr const char* benchmark_architecture="unknown";
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct LatencyResult {
    std::string name;
    std::string scope;
    std::uint64_t iterations;
    double total_ms;
    double mean_ns;
    double min_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    double max_ns;
    double operations_per_second;
};

struct ArtifactMetrics {
    std::uint64_t checkpoint_samples=0;
    double checkpoint_mean_bytes=0;
    std::uint64_t checkpoint_max_bytes=0;
    double state_store_mutations_per_run=0;
    double state_store_bytes_per_run=0;
    double state_store_write_amplification=0;
};

std::uint64_t observed=0;

class CountingStateStore final : public dage::StateStore {
public:
    dage::Result<bool> put(const std::string& run_id,const std::string& checkpoint) override {
        count_write(checkpoint.size());
        return delegate_.put(run_id,checkpoint);
    }
    dage::Result<std::string> get(const std::string& run_id) const override {
        return delegate_.get(run_id);
    }
    dage::Result<bool> erase(const std::string& run_id) override {
        mutations_.fetch_add(1,std::memory_order_relaxed);
        return delegate_.erase(run_id);
    }
    dage::Result<dage::StateRecord> load(const std::string& run_id) const override {
        return delegate_.load(run_id);
    }
    dage::Result<std::uint64_t> compare_exchange(
        const std::string& run_id,std::uint64_t expected,const std::string& checkpoint) override {
        count_write(checkpoint.size());
        return delegate_.compare_exchange(run_id,expected,checkpoint);
    }
    dage::Result<dage::StateRecord> claim(
        const std::string& run_id,std::uint64_t expected,const std::string& owner) override {
        mutations_.fetch_add(1,std::memory_order_relaxed);
        return delegate_.claim(run_id,expected,owner);
    }
    dage::Result<std::vector<std::string>> list(const std::string& prefix) const override {
        return delegate_.list(prefix);
    }
    std::uint64_t mutations() const noexcept {
        return mutations_.load(std::memory_order_relaxed);
    }
    std::uint64_t payload_bytes() const noexcept {
        return payload_bytes_.load(std::memory_order_relaxed);
    }
private:
    void count_write(std::size_t bytes) noexcept {
        mutations_.fetch_add(1,std::memory_order_relaxed);
        payload_bytes_.fetch_add(static_cast<std::uint64_t>(bytes),std::memory_order_relaxed);
    }
    dage::MemoryStateStore delegate_;
    std::atomic<std::uint64_t> mutations_{0};
    std::atomic<std::uint64_t> payload_bytes_{0};
};

std::uint64_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb=sizeof(counters);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    if(getrusage(RUSAGE_SELF,&usage)!=0)throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss)*1024U;
#endif
#endif
}

std::string linear_workflow(std::size_t tool_nodes) {
    std::ostringstream json;
    json << R"({"format":"dage-workflow","format_version":"0.2.0","id":"benchmark-linear",)"
         << R"("entry":"n0","limits":{"max_steps":10000},"nodes":{)";
    for(std::size_t i=0;i<tool_nodes;++i) {
        if(i)json << ',';
        json << "\"n" << i << "\":{\"type\":\"tool\",\"executor\":\"noop\","
             << "\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
             << "\"input\":\"${workflow.input}\",\"next\":\"n" << (i+1) << "\"}";
    }
    if(tool_nodes)json << ',';
    json << "\"n" << tool_nodes << "\":{\"type\":\"end\",\"input\":\"${workflow.input}\"}}}";
    return json.str();
}

std::string ai_fanout_workflow() {
    return R"({
      "format":"dage-workflow","format_version":"0.2.0","id":"benchmark-ai-fanout",
      "entry":"fanout","limits":{"max_steps":256,"max_parallel":8},"nodes":{
        "fanout":{"type":"parallel","branches":["retrieve","memory","policy","context"],"join":"merge"},
        "retrieve":{"type":"tool","executor":"provider","effects":{"kind":"external_read","replay":"safe"},"input":"${workflow.input}","next":"merge"},
        "memory":{"type":"tool","executor":"provider","effects":{"kind":"external_read","replay":"safe"},"input":"${workflow.input}","next":"merge"},
        "policy":{"type":"transform","executor":"compute","input":"${workflow.input}","next":"merge"},
        "context":{"type":"transform","executor":"compute","input":"${workflow.input}","next":"merge"},
        "merge":{"type":"join","executor":"compute","input":{"retrieve":"${nodes.retrieve.output}","memory":"${nodes.memory.output}","policy":"${nodes.policy.output}","context":"${nodes.context.output}"},"next":"infer"},
        "infer":{"type":"llm","executor":"provider","effects":{"kind":"external_read","replay":"safe"},"input":"${nodes.merge.output}","next":"postprocess"},
        "postprocess":{"type":"transform","executor":"compute","input":"${nodes.infer.output}","next":"done"},
        "done":{"type":"end","input":"${nodes.postprocess.output}"}
      }
    })";
}

std::string checkpoint_workflow() {
    return R"({
      "format":"dage-workflow","format_version":"0.2.0","id":"benchmark-recovery",
      "entry":"approval","nodes":{
        "approval":{"type":"human","executor":"approval","next":[{"to":"done","when":"${output.approved}"},{"to":"rejected","otherwise":true}]},
        "done":{"type":"end","input":{"approved":true}},
        "rejected":{"type":"end","input":{"approved":false}}
      }
    })";
}

double percentile(const std::vector<double>& sorted,double quantile) {
    if(sorted.empty())return 0;
    const double position=quantile*static_cast<double>(sorted.size()-1);
    const std::size_t lower=static_cast<std::size_t>(position);
    const std::size_t upper=std::min(lower+1,sorted.size()-1);
    const double fraction=position-static_cast<double>(lower);
    return sorted[lower]+(sorted[upper]-sorted[lower])*fraction;
}

template<class Function>
LatencyResult measure(const std::string& name,const std::string& scope,std::uint64_t iterations,
                      std::uint64_t warmup,Function function) {
    for(std::uint64_t i=0;i<warmup;++i)function();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    double total_ns=0;
    for(std::uint64_t i=0;i<iterations;++i) {
        const auto started=Clock::now();
        function();
        const double elapsed=static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count());
        samples.push_back(elapsed);
        total_ns+=elapsed;
    }
    std::sort(samples.begin(),samples.end());
    const double mean=total_ns/static_cast<double>(iterations);
    return LatencyResult{name,scope,iterations,total_ns/1000000.0,mean,samples.front(),
                         percentile(samples,0.50),percentile(samples,0.95),
                         percentile(samples,0.99),samples.back(),
                         mean>0?1000000000.0/mean:0};
}

void print_json(const std::vector<LatencyResult>& results,const ArtifactMetrics& artifacts,
                bool smoke,std::uint64_t peak_memory) {
    std::cout << "{\"schema_version\":2,\"smoke\":" << (smoke?"true":"false")
              << ",\"clock\":\"steady_clock\",\"compiler\":\"" << DAGE_BENCHMARK_COMPILER
              << "\",\"build_type\":\"" << DAGE_BENCHMARK_BUILD_TYPE
              << "\",\"linkage\":\"" << DAGE_BENCHMARK_LINKAGE
              << "\",\"platform\":\"" << benchmark_platform
              << "\",\"architecture\":\"" << benchmark_architecture
              << "\",\"process\":{\"peak_rss_bytes\":" << peak_memory << "},\"results\":[";
    for(std::size_t i=0;i<results.size();++i) {
        if(i)std::cout << ',';
        const LatencyResult& result=results[i];
        std::cout << "{\"name\":\"" << result.name << "\",\"scope\":\"" << result.scope
                  << "\",\"iterations\":" << result.iterations << std::fixed << std::setprecision(3)
                  << ",\"total_ms\":" << result.total_ms
                  << ",\"latency_ns\":{\"mean\":" << result.mean_ns
                  << ",\"min\":" << result.min_ns << ",\"p50\":" << result.p50_ns
                  << ",\"p95\":" << result.p95_ns << ",\"p99\":" << result.p99_ns
                  << ",\"max\":" << result.max_ns << "},\"operations_per_second\":"
                  << result.operations_per_second << '}';
    }
    std::cout << "],\"artifacts\":{\"checkpoint\":{\"samples\":" << artifacts.checkpoint_samples
              << ",\"mean_bytes\":" << artifacts.checkpoint_mean_bytes
              << ",\"max_bytes\":" << artifacts.checkpoint_max_bytes
              << "},\"state_store\":{\"mutations_per_run\":"
              << artifacts.state_store_mutations_per_run << ",\"payload_bytes_per_run\":"
              << artifacts.state_store_bytes_per_run << ",\"write_amplification\":"
              << artifacts.state_store_write_amplification << "}}}\n";
}

void print_human(const std::vector<LatencyResult>& results,const ArtifactMetrics& artifacts,
                 std::uint64_t peak_memory) {
    std::cout << std::left << std::setw(30) << "benchmark" << std::right << std::setw(12) << "mean us"
              << std::setw(12) << "p50 us" << std::setw(12) << "p95 us" << std::setw(12)
              << "p99 us" << std::setw(12) << "max us" << '\n';
    for(const LatencyResult& result:results) {
        std::cout << std::left << std::setw(30) << result.name << std::right << std::fixed
                  << std::setprecision(1) << std::setw(12) << result.mean_ns/1000.0
                  << std::setw(12) << result.p50_ns/1000.0 << std::setw(12) << result.p95_ns/1000.0
                  << std::setw(12) << result.p99_ns/1000.0 << std::setw(12)
                  << result.max_ns/1000.0 << '\n';
    }
    std::cout << "peak RSS bytes: " << peak_memory << "\ncheckpoint mean/max bytes: "
              << artifacts.checkpoint_mean_bytes << '/' << artifacts.checkpoint_max_bytes
              << "\nStateStore mutations/payload bytes per Run/write amplification: "
              << artifacts.state_store_mutations_per_run << '/'
              << artifacts.state_store_bytes_per_run << '/'
              << artifacts.state_store_write_amplification << '\n';
}

} // namespace

int main(int argc,char** argv) {
    try {
        bool smoke=false;
        bool json=false;
        for(int i=1;i<argc;++i) {
            const std::string argument(argv[i]);
            if(argument=="--smoke")smoke=true;
            else if(argument=="--json")json=true;
            else {
                std::cerr << "usage: dage_benchmarks [--smoke] [--json]\n";
                return 2;
            }
        }

        const std::uint64_t fast_iterations=smoke?2:1000;
        const std::uint64_t run_iterations=smoke?1:200;
        const std::uint64_t warmup=smoke?0:20;
        const std::string large_workflow=linear_workflow(64);
        const std::string run_workflow=linear_workflow(16);
        const dage::Value input=dage::Value::parse(
            R"({"prompt":"Summarize retrieved context","request_id":"benchmark-1","tokens":512})");

        dage::Engine engine;
        const auto echo=[](const dage::ExecutionContext&,const dage::Value& value) {
            return dage::ExecutionResult::ok(value);
        };
        engine.register_executor("noop",echo);
        engine.register_executor("provider",echo);
        engine.register_executor("compute",echo);
        engine.register_executor("approval",[](const dage::ExecutionContext&,const dage::Value&) {
            return dage::ExecutionResult::fail(
                "suspended","AWAITING_APPROVAL","Waiting for benchmark approval.");
        });

        std::vector<LatencyResult> results;
        results.push_back(measure("parse_workflow_64","JSON parse only",fast_iterations,warmup,[&] {
            dage::Value parsed=dage::Value::parse(large_workflow);
            observed+=parsed.size();
        }));
        results.push_back(measure("validate_workflow_64","parse + normalize + validate",
            fast_iterations,warmup,[&] {
                const auto diagnostics=engine.validate(large_workflow);
                if(!diagnostics.empty())throw std::runtime_error("benchmark Workflow failed validation");
                observed+=diagnostics.size()+1;
            }));
        results.push_back(measure("load_workflow_64","parse + normalize + validate + compile IR",
            fast_iterations,warmup,[&] {
                std::unique_ptr<dage::Workflow> workflow=engine.load(large_workflow);
                observed+=workflow->ir().nodes().size();
            }));

        std::unique_ptr<dage::Workflow> linear=engine.load(run_workflow);
        results.push_back(measure("run_linear_16","create Run + schedule 16 tool nodes",
            run_iterations,warmup,[&] {
                const dage::ExecutionResult result=engine.create_run(*linear)->execute(input);
                if(!result.success)throw std::runtime_error("linear benchmark Run failed");
                observed+=result.output.size();
            }));

        std::unique_ptr<dage::Workflow> ai=engine.load(ai_fanout_workflow());
        results.push_back(measure("run_ai_fanout","4-way fanout + join + provider/compute nodes",
            run_iterations,warmup,[&] {
                const dage::ExecutionResult result=engine.create_run(*ai)->execute(input);
                if(!result.success)throw std::runtime_error("AI fanout benchmark Run failed");
                observed+=result.output.size();
            }));

        std::unique_ptr<dage::Workflow> recovery=engine.load(checkpoint_workflow());
        std::vector<std::string> checkpoints;
        checkpoints.reserve(static_cast<std::size_t>(run_iterations+warmup));
        std::uint64_t checkpoint_bytes=0;
        std::uint64_t checkpoint_max=0;
        for(std::uint64_t i=0;i<run_iterations+warmup;++i) {
            std::unique_ptr<dage::Run> suspended=engine.create_run(*recovery);
            const dage::ExecutionResult suspended_result=suspended->execute(input);
            if(suspended_result.success||!suspended->suspended())
                throw std::runtime_error("recovery benchmark did not suspend");
            checkpoints.push_back(suspended->checkpoint());
            if(i>=warmup) {
                const std::uint64_t bytes=static_cast<std::uint64_t>(checkpoints.back().size());
                checkpoint_bytes+=bytes;
                checkpoint_max=std::max(checkpoint_max,bytes);
            }
        }
        const dage::Value approval=dage::Value::parse(R"({"approved":true})");
        std::size_t checkpoint_index=0;
        results.push_back(measure("recover_human_checkpoint","decode + validate + restore + resume",
            run_iterations,warmup,[&] {
                std::unique_ptr<dage::Run> restored=
                    engine.restore_run(*recovery,checkpoints.at(checkpoint_index++));
                const dage::ExecutionResult result=restored->resume(approval);
                if(!result.success)throw std::runtime_error("recovery benchmark resume failed");
                observed+=result.output.size();
            }));

        const std::uint64_t write_runs=smoke?1:20;
        std::shared_ptr<CountingStateStore> counting_store=std::make_shared<CountingStateStore>();
        dage::Engine persistent;
        persistent.set_state_store(counting_store);
        persistent.register_executor("noop",echo);
        std::unique_ptr<dage::Workflow> persistent_workflow=persistent.load(run_workflow);
        std::uint64_t final_checkpoint_bytes=0;
        for(std::uint64_t i=0;i<write_runs;++i) {
            std::unique_ptr<dage::Run> run=persistent.create_run(*persistent_workflow);
            const dage::ExecutionResult result=run->execute(input);
            if(!result.success)throw std::runtime_error("write amplification Run failed");
            final_checkpoint_bytes+=static_cast<std::uint64_t>(run->checkpoint().size());
        }

        ArtifactMetrics artifacts;
        artifacts.checkpoint_samples=run_iterations;
        artifacts.checkpoint_mean_bytes=
            static_cast<double>(checkpoint_bytes)/static_cast<double>(run_iterations);
        artifacts.checkpoint_max_bytes=checkpoint_max;
        artifacts.state_store_mutations_per_run=
            static_cast<double>(counting_store->mutations())/static_cast<double>(write_runs);
        artifacts.state_store_bytes_per_run=
            static_cast<double>(counting_store->payload_bytes())/static_cast<double>(write_runs);
        const double logical_bytes=
            static_cast<double>(final_checkpoint_bytes)/static_cast<double>(write_runs);
        artifacts.state_store_write_amplification=
            logical_bytes>0?artifacts.state_store_bytes_per_run/logical_bytes:0;

        if(observed==0)throw std::runtime_error("benchmark observations were optimized away");
        const std::uint64_t peak_memory=peak_rss_bytes();
        if(json)print_json(results,artifacts,smoke,peak_memory);
        else print_human(results,artifacts,peak_memory);
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
