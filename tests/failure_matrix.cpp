#include "dage/dage.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

dage::Error state_error(const std::string& code,const std::string& message) {
    dage::Error error;error.category="state_store";error.code=code;error.message=message;return error;
}

class AdversarialStateStore final : public dage::StateStore {
public:
    enum class Mode { DiskFull, Throw, BadVersion };
    explicit AdversarialStateStore(Mode mode):mode_(mode) {}
    dage::Result<bool> put(const std::string&,const std::string&) override {
        return dage::Result<bool>::failure(state_error("DISK_FULL","injected disk full"));
    }
    dage::Result<std::string> get(const std::string&) const override {
        return dage::Result<std::string>::failure(state_error("STATE_NOT_FOUND","missing"));
    }
    dage::Result<bool> erase(const std::string&) override {
        return dage::Result<bool>::success(true);
    }
    dage::Result<dage::StateRecord> load(const std::string&) const override {
        return dage::Result<dage::StateRecord>::failure(
            state_error("STATE_NOT_FOUND","missing"));
    }
    dage::Result<std::uint64_t> compare_exchange(
        const std::string&,std::uint64_t expected,const std::string&) override {
        if(mode_==Mode::Throw)throw std::runtime_error("host StateStore exception");
        if(mode_==Mode::DiskFull)
            return dage::Result<std::uint64_t>::failure(
                state_error("DISK_FULL","injected disk full"));
        return dage::Result<std::uint64_t>::success(expected);
    }
    dage::Result<dage::StateRecord> claim(
        const std::string&,std::uint64_t,const std::string&) override {
        return dage::Result<dage::StateRecord>::failure(
            state_error("STATE_NOT_FOUND","missing"));
    }
    dage::Result<std::vector<std::string>> list(const std::string&) const override {
        return dage::Result<std::vector<std::string>>::success({});
    }
private:
    Mode mode_;
};

class ThrowingLeaseProvider final : public dage::ResourceLeaseProvider {
public:
    dage::Result<std::unique_ptr<dage::ResourceLease>> acquire(
        const dage::ResourceRequest&,const dage::CancellationToken*) override {
        throw std::runtime_error("host lease exception");
    }
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_=std::filesystem::current_path()/
            ("failure-matrix-state-"+std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;std::filesystem::remove_all(path_,error);
    }
    const std::filesystem::path& path() const noexcept {return path_;}
private:
    std::filesystem::path path_;
};

void require(bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}

const char* simple_workflow=R"({
  "format":"dage-workflow","format_version":"0.2.0","entry":"a","nodes":{
    "a":{"type":"tool","executor":"echo","effects":{"kind":"pure","replay":"safe"},"next":"z"},
    "z":{"type":"end","input":{"ok":true}}
  }
})";

dage::ExecutionResult execute_with_store(const std::shared_ptr<dage::StateStore>& store) {
    dage::Engine engine;engine.set_state_store(store);
    engine.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input) {
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> workflow=engine.load(simple_workflow);
    return engine.create_run(*workflow)->execute(dage::Value::object());
}

void state_store_failures() {
    dage::ExecutionResult disk_full=execute_with_store(
        std::make_shared<AdversarialStateStore>(AdversarialStateStore::Mode::DiskFull));
    require(!disk_full.success&&disk_full.error.code=="DISK_FULL",
            "disk-full Result was not propagated");
    dage::ExecutionResult exception=execute_with_store(
        std::make_shared<AdversarialStateStore>(AdversarialStateStore::Mode::Throw));
    require(!exception.success&&exception.error.code=="STATE_PROVIDER_EXCEPTION",
            "StateStore exception crossed the runtime boundary");
    dage::ExecutionResult bad_version=execute_with_store(
        std::make_shared<AdversarialStateStore>(AdversarialStateStore::Mode::BadVersion));
    require(!bad_version.success&&bad_version.error.code=="STATE_PROTOCOL_ERROR",
            "non-monotonic StateStore version was accepted");
}

void file_corruption_and_partial_write() {
    TemporaryDirectory temporary;
    dage::FileStateStore store(temporary.path().string());
    require(static_cast<bool>(store.compare_exchange("stable",0,"checkpoint")),
            "could not create stable record");
    {
        std::ofstream orphan(
            temporary.path()/"stable.state.tmp.partial",std::ios::binary|std::ios::trunc);
        orphan << "DAGESTATE1\npartial";
    }
    dage::Result<dage::StateRecord> stable=store.load("stable");
    require(stable&&stable.value().checkpoint=="checkpoint",
            "orphan partial write affected committed state");

    require(static_cast<bool>(store.compare_exchange("truncated",0,"checkpoint")),
            "could not create truncation fixture");
    std::filesystem::resize_file(temporary.path()/"truncated.state",12);
    dage::Result<dage::StateRecord> truncated=store.load("truncated");
    require(!truncated&&truncated.error().code=="STATE_CORRUPT",
            "truncated record was not rejected");

    require(static_cast<bool>(store.compare_exchange("checksum",0,"checkpoint")),
            "could not create checksum fixture");
    {
        std::fstream corrupt(
            temporary.path()/"checksum.state",std::ios::binary|std::ios::in|std::ios::out);
        corrupt.seekp(-1,std::ios::end);corrupt.put('X');corrupt.flush();
    }
    dage::Result<dage::StateRecord> checksum=store.load("checksum");
    require(!checksum&&checksum.error().code=="STATE_CORRUPT",
            "checksum corruption was not rejected");

    auto shared_store=std::make_shared<dage::FileStateStore>(temporary.path().string());
    dage::Engine engine;engine.set_state_store(shared_store);
    engine.register_executor("approval",[](const dage::ExecutionContext&,const dage::Value&) {
        return dage::ExecutionResult::fail(
            "suspended","AWAITING_APPROVAL","waiting");
    });
    std::unique_ptr<dage::Workflow> workflow=engine.load(R"({
      "format":"dage-workflow","format_version":"0.2.0","entry":"approval","nodes":{
        "approval":{"type":"human","executor":"approval","next":"done"},
        "done":{"type":"end"}
      }
    })");
    std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
    require(!run->execute(dage::Value::object()).success&&run->suspended(),
            "restore fixture did not suspend");
    const std::string checkpoint=run->checkpoint();
    const std::string run_id=run->snapshot().get("run_id").as_string();
    std::filesystem::resize_file(temporary.path()/(run_id+".state"),16);
    try {
        (void)engine.restore_run(*workflow,checkpoint);
        throw std::runtime_error("restore accepted corrupt durable state");
    } catch(const std::runtime_error& error) {
        require(std::string(error.what()).find("STATE_CORRUPT")!=std::string::npos,
                "restore did not report corrupt durable state");
    }
}

#ifndef _WIN32
void real_write_failure(const std::filesystem::path& directory) {
    dage::FileStateStore initial(directory.string());
    require(static_cast<bool>(initial.compare_exchange("limited",0,"committed")),
            "could not create write-limit fixture");
    const pid_t child=fork();
    require(child>=0,"fork failed");
    if(child==0) {
        std::signal(SIGXFSZ,SIG_IGN);
        rlimit limit{};limit.rlim_cur=1;limit.rlim_max=1;
        if(setrlimit(RLIMIT_FSIZE,&limit)!=0)std::_Exit(90);
        dage::FileStateStore store(directory.string());
        const auto result=store.compare_exchange("limited",1,std::string(65536,'x'));
        std::_Exit(!result&&result.error().code=="STATE_IO_ERROR"?0:91);
    }
    int status=0;
    require(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
            "limited filesystem write did not return STATE_IO_ERROR");
    dage::FileStateStore after(directory.string());
    const auto committed=after.load("limited");
    require(committed&&committed.value().version==1&&committed.value().checkpoint=="committed",
            "failed partial write replaced committed state");
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        require(entry.path().filename().string().find(".tmp.")==std::string::npos,
                "failed atomic write left a temporary file");
}
#endif

void hostile_lease_provider() {
    dage::Engine engine;
    engine.set_resource_lease_provider(std::make_shared<ThrowingLeaseProvider>());
    engine.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input) {
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> workflow=engine.load(simple_workflow);
    const dage::ExecutionResult result=
        engine.create_run(*workflow)->execute(dage::Value::object());
    require(!result.success&&result.error.code=="LEASE_PROVIDER_EXCEPTION",
            "lease provider exception crossed the runtime boundary");
}

void cancellation_completion_races() {
    for(unsigned iteration=0;iteration<50;++iteration) {
        dage::Engine engine;
        std::promise<void> started;
        std::shared_ptr<dage::AsyncExecutorCompletion> completion;
        engine.register_async_executor(
            "wait",[&](const dage::ExecutionContext&,const dage::Value&,
                       const std::shared_ptr<dage::AsyncExecutorCompletion>& value) {
                completion=value;started.set_value();
            });
        std::unique_ptr<dage::Workflow> workflow=engine.load(R"({
          "format":"dage-workflow","format_version":"0.2.0","entry":"a","nodes":{
            "a":{"type":"tool","executor":"wait","effects":{"kind":"pure","replay":"safe"}}
          }
        })");
        std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
        std::promise<dage::ExecutionResult> finished;
        std::atomic<unsigned> callbacks{0};
        run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& result) {
            if(callbacks.fetch_add(1)==0)finished.set_value(result);
        });
        started.get_future().get();
        std::thread cancel([&](){run->cancel("race cancellation");});
        std::thread complete([&](){
            completion->complete(dage::ExecutionResult::ok(dage::Value::object()));
        });
        cancel.join();complete.join();
        std::future<dage::ExecutionResult> result=finished.get_future();
        require(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,
                "cancellation race did not finish");
        const dage::ExecutionResult value=result.get();
        require(value.success||value.error.category=="cancelled",
                "cancellation race produced an unexpected result");
        require(callbacks.load()==1,"Run completion callback was delivered more than once");
    }
}

void timeout_completion_races() {
    for(unsigned iteration=0;iteration<50;++iteration) {
        dage::Engine engine;
        std::promise<void> started;
        std::shared_ptr<dage::AsyncExecutorCompletion> completion;
        engine.register_async_executor(
            "wait",[&](const dage::ExecutionContext&,const dage::Value&,
                       const std::shared_ptr<dage::AsyncExecutorCompletion>& value) {
                completion=value;started.set_value();
            });
        std::unique_ptr<dage::Workflow> workflow=engine.load(R"({
          "format":"dage-workflow","format_version":"0.2.0","entry":"a","nodes":{
            "a":{"type":"tool","executor":"wait","timeout_ms":2,
                 "effects":{"kind":"pure","replay":"safe"}}
          }
        })");
        std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
        std::promise<dage::ExecutionResult> finished;
        std::atomic<unsigned> callbacks{0};
        run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& result) {
            if(callbacks.fetch_add(1)==0)finished.set_value(result);
        });
        started.get_future().get();
        std::thread complete([&](){
            std::this_thread::sleep_for(std::chrono::milliseconds(iteration%3));
            completion->complete(dage::ExecutionResult::ok(dage::Value::object()));
        });
        std::future<dage::ExecutionResult> result=finished.get_future();
        require(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,
                "timeout race did not finish");
        const dage::ExecutionResult value=result.get();
        complete.join();
        require(value.success||value.error.code=="NODE_TIMEOUT",
                "timeout race produced an unexpected result");
        require(callbacks.load()==1,"timeout race delivered completion more than once");
    }
}

} // namespace

int main() {
    try {
        state_store_failures();
        file_corruption_and_partial_write();
#ifndef _WIN32
        {
            TemporaryDirectory write_limit;
            real_write_failure(write_limit.path());
        }
#endif
        hostile_lease_provider();
        cancellation_completion_races();
        timeout_completion_races();
        std::cout << "failure and race matrix passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "failure matrix failed: " << error.what() << '\n';
        return 1;
    }
}
