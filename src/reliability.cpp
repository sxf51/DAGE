#include "dage/reliability.hpp"
#include "sha256.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dage {
namespace {
Error state_error(const std::string& code,const std::string& message){
    Error error;error.category="state_store";error.code=code;error.message=message;return error;
}
bool valid_run_id(const std::string& value){
    if(value.empty()||value=="."||value=="..")return false;
    for(char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;
    return true;
}
StateRecord decode_record(const std::string& bytes){
    std::istringstream input(bytes);std::string magic,version,epoch,owner,length,checksum;
    if(!std::getline(input,magic)||magic!="DAGESTATE1"||!std::getline(input,version)||
       !std::getline(input,epoch)||!std::getline(input,owner)||!std::getline(input,length)||
       !std::getline(input,checksum))throw std::runtime_error("invalid state record header");
    StateRecord record;record.version=std::stoull(version);record.epoch=std::stoull(epoch);record.owner=owner;
    record.checkpoint=std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    if(record.checkpoint.size()!=std::stoull(length))throw std::runtime_error("state record length mismatch");
    if(sha256_digest(record.checkpoint)!=checksum)throw std::runtime_error("state record checksum mismatch");
    return record;
}
std::string encode_record(const StateRecord& record){
    return "DAGESTATE1\n"+std::to_string(record.version)+"\n"+std::to_string(record.epoch)+"\n"+
        record.owner+"\n"+std::to_string(record.checkpoint.size())+"\n"+
        sha256_digest(record.checkpoint)+"\n"+record.checkpoint;
}
class DirectoryLock {
public:
    explicit DirectoryLock(const std::filesystem::path& path)
#ifdef _WIN32
        :handle_(INVALID_HANDLE_VALUE)
#else
        :fd_(-1)
#endif
    {
        for(unsigned i=0;i<500;++i){
#ifdef _WIN32
            handle_=CreateFileW(path.wstring().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
                                OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle_!=INVALID_HANDLE_VALUE)return;
#else
            if(fd_<0)fd_=::open(path.c_str(),O_CREAT|O_RDWR,0600);
            if(fd_>=0&&::flock(fd_,LOCK_EX|LOCK_NB)==0)return;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
#ifndef _WIN32
        if(fd_>=0)::close(fd_);
#endif
        throw std::runtime_error("state store lock timeout");
    }
    ~DirectoryLock(){
#ifdef _WIN32
        if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);
#else
        if(fd_>=0){::flock(fd_,LOCK_UN);::close(fd_);}
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int fd_;
#endif
};
std::string read_file(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("state record not found");
    return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}
void atomic_write(const std::filesystem::path& path,const std::string& content){
    const std::filesystem::path temporary=path.string()+".tmp."+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"."+
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    struct TemporaryCleanup {
        std::filesystem::path path;
        bool active=true;
        ~TemporaryCleanup(){if(active){std::error_code ignored;std::filesystem::remove(path,ignored);}}
    } cleanup{temporary};
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output)throw std::runtime_error("cannot create temporary state record");
        output.write(content.data(),static_cast<std::streamsize>(content.size()));
        output.flush();if(!output)throw std::runtime_error("cannot flush state record");
    }
#ifdef _WIN32
    {HANDLE file=CreateFileW(temporary.wstring().c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
     if(file==INVALID_HANDLE_VALUE||!FlushFileBuffers(file)){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        throw std::runtime_error("cannot durably flush state record");}CloseHandle(file);}
#else
    {int file=::open(temporary.c_str(),O_RDONLY);if(file<0||::fsync(file)!=0){if(file>=0)::close(file);
        throw std::runtime_error("cannot durably flush state record");}::close(file);}
#endif
#ifdef _WIN32
    if(!MoveFileExW(temporary.wstring().c_str(),path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        throw std::runtime_error("cannot atomically replace state record");
    }
#else
    std::error_code error;std::filesystem::rename(temporary,path,error);
    if(error)throw std::runtime_error("cannot atomically replace state record");
    const int directory=::open(path.parent_path().c_str(),O_RDONLY);
    if(directory<0||::fsync(directory)!=0){
        if(directory>=0)::close(directory);
        throw std::runtime_error("cannot durably flush state directory");
    }
    ::close(directory);
#endif
    cleanup.active=false;
}
}
bool CancellationToken::is_cancelled()const noexcept{return cancelled_.load();}
std::string CancellationToken::reason()const{std::lock_guard<std::mutex> lock(mutex_);return reason_;}
void CancellationToken::request_cancel(const std::string& reason)noexcept{
    try{{std::lock_guard<std::mutex> lock(mutex_);if(reason_.empty())reason_=reason;}
        cancelled_.store(true);changed_.notify_all();}catch(...){cancelled_.store(true);}
}
bool CancellationToken::wait_for(std::uint64_t timeout_ms)const{
    if(cancelled_.load())return true;
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock,std::chrono::milliseconds(timeout_ms),[this](){return cancelled_.load();});
}
Result<bool> MemoryStateStore::put(const std::string& run_id,const std::string& checkpoint){
    if(run_id.empty())return Result<bool>::failure(state_error("INVALID_RUN_ID","run id is empty"));
    std::lock_guard<std::mutex> lock(mutex_);StateRecord& record=states_[run_id];
    ++record.version;record.checkpoint=checkpoint;return Result<bool>::success(true);
}
Result<std::string> MemoryStateStore::get(const std::string& run_id)const{
    std::lock_guard<std::mutex> lock(mutex_);const auto found=states_.find(run_id);
    if(found==states_.end())return Result<std::string>::failure(state_error("STATE_NOT_FOUND","run state not found: "+run_id));
    return Result<std::string>::success(found->second.checkpoint);
}
Result<bool> MemoryStateStore::erase(const std::string& run_id){
    std::lock_guard<std::mutex> lock(mutex_);return Result<bool>::success(states_.erase(run_id)!=0);
}
Result<StateRecord> MemoryStateStore::load(const std::string& run_id)const{
    std::lock_guard<std::mutex> lock(mutex_);const auto found=states_.find(run_id);
    if(found==states_.end())return Result<StateRecord>::failure(state_error("STATE_NOT_FOUND","run state not found: "+run_id));
    return Result<StateRecord>::success(found->second);
}
Result<std::uint64_t> MemoryStateStore::compare_exchange(const std::string& run_id,std::uint64_t expected,
                                                         const std::string& checkpoint){
    std::lock_guard<std::mutex> lock(mutex_);StateRecord& record=states_[run_id];
    if(record.version!=expected)return Result<std::uint64_t>::failure(state_error("STATE_CONFLICT","state version conflict"));
    ++record.version;record.checkpoint=checkpoint;return Result<std::uint64_t>::success(record.version);
}
Result<StateRecord> MemoryStateStore::claim(const std::string& run_id,std::uint64_t expected,const std::string& owner){
    std::lock_guard<std::mutex> lock(mutex_);auto found=states_.find(run_id);
    if(found==states_.end())return Result<StateRecord>::failure(state_error("STATE_NOT_FOUND","run state not found"));
    if(found->second.version!=expected)return Result<StateRecord>::failure(state_error("STATE_CONFLICT","recovery ownership conflict"));
    found->second.version++;found->second.epoch++;found->second.owner=owner;return Result<StateRecord>::success(found->second);
}
Result<std::vector<std::string>> MemoryStateStore::list(const std::string& prefix)const{
    std::lock_guard<std::mutex> lock(mutex_);std::vector<std::string> result;
    for(const auto& item:states_)if(item.first.compare(0,prefix.size(),prefix)==0)result.push_back(item.first);
    return Result<std::vector<std::string>>::success(std::move(result));
}

FileStateStore::FileStateStore(std::string directory):directory_(std::move(directory)){
    if(directory_.empty())throw std::invalid_argument("state directory must not be empty");
    std::filesystem::create_directories(directory_);
}
const std::string& FileStateStore::directory()const noexcept{return directory_;}
Result<StateRecord> FileStateStore::load(const std::string& run_id)const{
    if(!valid_run_id(run_id))return Result<StateRecord>::failure(state_error("INVALID_RUN_ID","invalid run id"));
    const std::filesystem::path path=std::filesystem::path(directory_)/(run_id+".state");
    if(!std::filesystem::exists(path))return Result<StateRecord>::failure(state_error("STATE_NOT_FOUND","state record not found"));
    try{return Result<StateRecord>::success(decode_record(read_file(path)));}
    catch(const std::exception& ex){return Result<StateRecord>::failure(state_error("STATE_CORRUPT",ex.what()));}
}
Result<std::string> FileStateStore::get(const std::string& run_id)const{
    Result<StateRecord> record=load(run_id);
    return record?Result<std::string>::success(record.value().checkpoint):
        Result<std::string>::failure(record.error());
}
Result<std::uint64_t> FileStateStore::compare_exchange(const std::string& run_id,std::uint64_t expected,
                                                       const std::string& checkpoint){
    if(!valid_run_id(run_id))return Result<std::uint64_t>::failure(state_error("INVALID_RUN_ID","invalid run id"));
    try{
        DirectoryLock lock(std::filesystem::path(directory_)/(run_id+".lock"));
        const std::filesystem::path path=std::filesystem::path(directory_)/(run_id+".state");
        std::uint64_t current=0;
        if(std::filesystem::exists(path))current=decode_record(read_file(path)).version;
        if(current!=expected)return Result<std::uint64_t>::failure(state_error("STATE_CONFLICT","state version conflict"));
        StateRecord next;next.version=current+1;next.checkpoint=checkpoint;
        if(std::filesystem::exists(path)){StateRecord old=decode_record(read_file(path));next.epoch=old.epoch;next.owner=old.owner;}
        atomic_write(path,encode_record(next));
        return Result<std::uint64_t>::success(current+1);
    }catch(const std::exception& ex){return Result<std::uint64_t>::failure(state_error("STATE_IO_ERROR",ex.what()));}
}
Result<StateRecord> FileStateStore::claim(const std::string& run_id,std::uint64_t expected,const std::string& owner){
    if(!valid_run_id(run_id)||!valid_run_id(owner))return Result<StateRecord>::failure(state_error("INVALID_OWNER","invalid recovery owner"));
    try{DirectoryLock lock(std::filesystem::path(directory_)/(run_id+".lock"));
        const std::filesystem::path path=std::filesystem::path(directory_)/(run_id+".state");
        StateRecord record=decode_record(read_file(path));
        if(record.version!=expected)return Result<StateRecord>::failure(state_error("STATE_CONFLICT","recovery ownership conflict"));
        record.version++;record.epoch++;record.owner=owner;atomic_write(path,encode_record(record));
        return Result<StateRecord>::success(record);
    }catch(const std::exception& ex){return Result<StateRecord>::failure(state_error("STATE_IO_ERROR",ex.what()));}
}
Result<std::vector<std::string>> FileStateStore::list(const std::string& prefix)const{
    try{std::vector<std::string> result;
        for(const auto& entry:std::filesystem::directory_iterator(directory_)){
            if(!entry.is_regular_file()||entry.path().extension()!=".state")continue;
            const std::string id=entry.path().stem().string();
            if(id.compare(0,prefix.size(),prefix)==0)result.push_back(id);
        }
        std::sort(result.begin(),result.end());return Result<std::vector<std::string>>::success(std::move(result));
    }catch(const std::exception& ex){return Result<std::vector<std::string>>::failure(state_error("STATE_IO_ERROR",ex.what()));}
}
Result<bool> FileStateStore::put(const std::string& run_id,const std::string& checkpoint){
    for(unsigned attempt=0;attempt<16;++attempt){
        Result<StateRecord> record=load(run_id);
        const std::uint64_t version=record?record.value().version:0;
        Result<std::uint64_t> saved=compare_exchange(run_id,version,checkpoint);
        if(saved)return Result<bool>::success(true);
        if(saved.error().code!="STATE_CONFLICT")return Result<bool>::failure(saved.error());
    }
    return Result<bool>::failure(state_error("STATE_CONFLICT","state update retries exhausted"));
}
Result<bool> FileStateStore::erase(const std::string& run_id){
    if(!valid_run_id(run_id))return Result<bool>::failure(state_error("INVALID_RUN_ID","invalid run id"));
    try{DirectoryLock lock(std::filesystem::path(directory_)/(run_id+".lock"));std::error_code error;
        const bool removed=std::filesystem::remove(std::filesystem::path(directory_)/(run_id+".state"),error);
        if(error)return Result<bool>::failure(state_error("STATE_IO_ERROR",error.message()));
        return Result<bool>::success(removed);
    }catch(const std::exception& ex){return Result<bool>::failure(state_error("STATE_IO_ERROR",ex.what()));}
}
} // namespace dage
