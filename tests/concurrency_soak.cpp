#include "dage/dage.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
const char* workflow_json=R"({
  "format":"dage-workflow","format_version":"0.2.0","id":"concurrency-soak",
  "entry":"fanout","limits":{"max_steps":64,"max_parallel":4},"nodes":{
    "fanout":{"type":"parallel","branches":["a","b","c","d"],"join":"join"},
    "a":{"type":"tool","executor":"async","effects":{"kind":"pure","replay":"safe"},"next":"join"},
    "b":{"type":"tool","executor":"async","effects":{"kind":"pure","replay":"safe"},"next":"join"},
    "c":{"type":"tool","executor":"async","effects":{"kind":"pure","replay":"safe"},"next":"join"},
    "d":{"type":"tool","executor":"async","effects":{"kind":"pure","replay":"safe"},"next":"join"},
    "join":{"type":"join","executor":"join","next":"done"},
    "done":{"type":"end","input":{"ok":true}}
  }
})";

std::uint64_t parse_number(const char* text,const char* name) {
    const std::string value(text);
    std::size_t consumed=0;
    const unsigned long long parsed=std::stoull(value,&consumed);
    if(consumed!=value.size()||parsed==0||parsed>1000000)
        throw std::invalid_argument(std::string("invalid ")+name);
    return static_cast<std::uint64_t>(parsed);
}
}

int main(int argc,char** argv) {
    try {
        std::uint64_t thread_count=4;
        std::uint64_t iterations=1000;
        for(int index=1;index<argc;++index) {
            const std::string argument(argv[index]);
            if((argument=="--threads"||argument=="--iterations")&&index+1<argc) {
                const std::uint64_t value=parse_number(argv[++index],argument.c_str());
                if(argument=="--threads")thread_count=value;else iterations=value;
            } else {
                std::cerr << "usage: dage_concurrency_soak [--threads N] [--iterations N]\n";
                return 2;
            }
        }

        dage::ThreadPoolOptions options;
        options.worker_count=static_cast<std::size_t>(std::max<std::uint64_t>(2,thread_count));
        // This soak verifies successful concurrent Run/completion lifetimes. Backpressure and
        // rejection semantics have dedicated deterministic tests, so provision this queue for
        // the largest permitted invocation rather than making host load decide the outcome.
        options.queue_capacity=static_cast<std::size_t>(thread_count*iterations*8);
        auto scheduler=std::make_shared<dage::ThreadPoolScheduler>(options);
        dage::Engine engine(scheduler);
        std::atomic<std::uint64_t> executor_calls{0};
        engine.register_async_executor(
            "async",[&](const dage::ExecutionContext&,const dage::Value& input,
                        const std::shared_ptr<dage::AsyncExecutorCompletion>& completion) {
                executor_calls.fetch_add(1,std::memory_order_relaxed);
                completion->complete(dage::ExecutionResult::ok(input));
            });
        engine.register_executor("join",[](const dage::ExecutionContext&,const dage::Value& input) {
            return dage::ExecutionResult::ok(input);
        });
        std::unique_ptr<dage::Workflow> workflow=engine.load(workflow_json);
        std::atomic<std::uint64_t> failures{0};
        std::mutex failure_mutex;
        std::string first_failure;
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(thread_count));
        for(std::uint64_t thread=0;thread<thread_count;++thread) {
            threads.emplace_back([&,thread] {
                for(std::uint64_t iteration=0;iteration<iterations;++iteration) {
                    dage::Value input=dage::Value::object();
                    input.set("thread",dage::Value(static_cast<std::int64_t>(thread)));
                    input.set("iteration",dage::Value(static_cast<std::int64_t>(iteration)));
                    try {
                        const dage::ExecutionResult result=
                            engine.create_run(*workflow)->execute(input);
                        if(!result.success||!result.output.get("ok").as_bool()) {
                            failures.fetch_add(1,std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lock(failure_mutex);
                            if(first_failure.empty())first_failure=result.success?"invalid success output":
                                result.error.category+":"+result.error.code+":"+result.error.message;
                        }
                    } catch(const std::exception& error) {
                        failures.fetch_add(1,std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lock(failure_mutex);
                        if(first_failure.empty())first_failure=std::string("exception: ")+error.what();
                    } catch(...) {
                        failures.fetch_add(1,std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lock(failure_mutex);
                        if(first_failure.empty())first_failure="unknown exception";
                    }
                }
            });
        }
        for(auto& thread:threads)thread.join();
        const std::uint64_t runs=thread_count*iterations;
        if(failures.load()!=0||executor_calls.load()!=runs*4) {
            std::cerr << "soak mismatch: failures=" << failures.load()
                      << " calls=" << executor_calls.load() << " expected=" << runs*4
                      << " first_failure=" << first_failure << '\n';
            return 1;
        }
        std::cout << "concurrency soak passed: runs=" << runs
                  << " async completions=" << executor_calls.load() << '\n';
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "concurrency soak failed: " << error.what() << '\n';
        return 1;
    }
}
