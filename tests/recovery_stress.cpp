#include "dage/dage.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
const char* workflow_json =
    "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{"
    "\"a\":{\"type\":\"tool\",\"executor\":\"write\",\"effects\":{\"kind\":\"external_write\","
    "\"replay\":\"idempotent\",\"idempotency_key\":\"${run.id}:a\"},\"next\":\"z\"},"
    "\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}}";

int worker(const std::filesystem::path& directory) {
    auto store=std::make_shared<dage::FileStateStore>(directory.string());
    dage::Engine engine;engine.set_state_store(store);
    engine.register_executor("write",[directory](const dage::ExecutionContext& context,const dage::Value&){
        std::ofstream marker(directory/"effect.log",std::ios::app);
        marker<<"write\n";marker.flush();
        if(!marker)return dage::ExecutionResult::fail("io","MARKER_WRITE_FAILED","marker write failed");
        context.effect_committer->commit();
        return dage::ExecutionResult::ok(dage::Value::object());
    });
    engine.set_failure_injector([](const std::string& point,const std::string& node){
        if(point=="after_state_save"&&node=="a")std::_Exit(86);
        return false;
    });
    auto workflow=engine.load(workflow_json);
    (void)engine.create_run(*workflow)->execute(dage::Value::object());
    return 87;
}

int recover(const std::filesystem::path& directory) {
    auto store=std::make_shared<dage::FileStateStore>(directory.string());
    dage::Result<dage::StateRecord> record=store->load("run-1");
    if(!record)return 10;
    dage::Engine engine;engine.set_state_store(store);
    engine.register_executor("write",[directory](const dage::ExecutionContext& context,const dage::Value&){
        std::ofstream marker(directory/"effect.log",std::ios::app);marker<<"duplicate\n";
        context.effect_committer->commit();
        return dage::ExecutionResult::ok(dage::Value::object());
    });
    auto workflow=engine.load(workflow_json);
    auto run=engine.restore_run(*workflow,record.value().checkpoint);
    dage::ExecutionResult result=run->execute(dage::Value::object());
    if(!result.success)return 11;
    std::ifstream marker(directory/"effect.log");
    std::string line;unsigned lines=0;while(std::getline(marker,line))++lines;
    return lines==1?0:12;
}
}

int main(int argc,char** argv) {
    if(argc==3&&std::string(argv[1])=="--worker")return worker(argv[2]);
    const std::filesystem::path root=std::filesystem::current_path()/"recovery-stress-state";
    std::error_code error;std::filesystem::remove_all(root,error);
    std::filesystem::create_directories(root);
    for(unsigned iteration=0;iteration<12;++iteration){
        const std::filesystem::path directory=root/std::to_string(iteration);
        std::filesystem::create_directories(directory);
        const std::string executable=std::filesystem::absolute(argv[0]).string();
#ifdef _WIN32
        const std::string command=executable+" --worker "+directory.string();
#else
        const std::string command="\""+executable+"\" --worker \""+directory.string()+"\"";
#endif
        const int status=std::system(command.c_str());
        if(status==0){std::cerr<<"worker did not crash at iteration "<<iteration<<"\n";return 20;}
        const int recovered=recover(directory);
        if(recovered){std::cerr<<"recovery failed at iteration "<<iteration<<": "<<recovered<<"\n";return recovered;}
    }
    {
        const std::filesystem::path corrupt=root/"corrupt";std::filesystem::create_directories(corrupt);
        dage::FileStateStore store(corrupt.string());
        if(!store.compare_exchange("broken",0,"{\"checkpoint\":true}"))return 30;
        std::ofstream output(corrupt/"broken.state",std::ios::app);output<<"truncated";output.close();
        dage::Result<dage::StateRecord> record=store.load("broken");
        if(record||record.error().code!="STATE_CORRUPT")return 31;
    }
    std::filesystem::remove_all(root,error);
    return 0;
}
