#include "dage/tools/bundle_store.hpp"
#include "dage/dage.hpp"
#include <json/json.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dage { namespace tools {
namespace {
std::atomic<std::uint64_t> next_temp{1};
Error tool_error(const std::string& code,const std::string& message){
    Error error;error.category="bundle_tools";error.code=code;error.message=message;return error;
}
bool hex_digest(const std::string& digest,std::string& hex){
    if(digest.rfind("sha256:",0)!=0||digest.size()!=71)return false;
    hex=digest.substr(7);
    for(char c:hex)
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
Json::Value parse(const std::string& text){
    Json::CharReaderBuilder builder;builder["collectComments"]=false;Json::Value value;std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(text.data(),text.data()+text.size(),&value,&errors))throw std::runtime_error(errors);
    return value;
}
std::filesystem::path temporary_sibling(const std::filesystem::path& path){
#ifdef _WIN32
    const std::uint64_t process=static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t process=static_cast<std::uint64_t>(::getpid());
#endif
    return path.parent_path()/("."+path.filename().string()+".tmp-"+std::to_string(process)+"-"+
                               std::to_string(next_temp.fetch_add(1)));
}
Result<bool> write_bytes(const std::filesystem::path& path,const std::uint8_t* data,std::size_t size,bool durable){
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return Result<bool>::failure(tool_error("FILE_CREATE_FAILED","cannot create temporary file"));
    std::size_t offset=0;bool ok=true;while(offset<size){DWORD written=0;
        const DWORD chunk=static_cast<DWORD>(std::min<std::size_t>(size-offset,1u<<30));
        if(!WriteFile(file,data+offset,chunk,&written,nullptr)||written!=chunk){ok=false;break;}offset+=written;}
    if(ok&&durable)ok=FlushFileBuffers(file)!=0;
    CloseHandle(file);
    if(!ok){DeleteFileW(path.c_str());return Result<bool>::failure(tool_error("FILE_WRITE_FAILED","cannot durably write temporary file"));}
#else
    const int file=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(file<0)return Result<bool>::failure(tool_error("FILE_CREATE_FAILED","cannot create temporary file"));
    std::size_t offset=0;bool ok=true;while(offset<size){const ssize_t written=::write(file,data+offset,size-offset);
        if(written<=0){ok=false;break;}offset+=static_cast<std::size_t>(written);}
    if(ok&&durable)ok=::fsync(file)==0;if(::close(file)!=0)ok=false;
    if(!ok){::unlink(path.c_str());return Result<bool>::failure(tool_error("FILE_WRITE_FAILED","cannot durably write temporary file"));}
#endif
    return Result<bool>::success(true);
}
Result<bool> replace_file(const std::filesystem::path& temp,const std::filesystem::path& target,bool durable){
#ifdef _WIN32
    const DWORD flags=MOVEFILE_REPLACE_EXISTING|(durable?MOVEFILE_WRITE_THROUGH:0);
    if(!MoveFileExW(temp.c_str(),target.c_str(),flags))
        return Result<bool>::failure(tool_error("LOCKFILE_REPLACE_FAILED","atomic lockfile replacement failed"));
#else
    if(::rename(temp.c_str(),target.c_str())!=0)
        return Result<bool>::failure(tool_error("LOCKFILE_REPLACE_FAILED","atomic lockfile replacement failed"));
    if(durable){
        int flags=O_RDONLY;
#ifdef O_DIRECTORY
        flags|=O_DIRECTORY;
#endif
        const int directory=::open(target.parent_path().c_str(),flags);
        if(directory<0||::fsync(directory)!=0){if(directory>=0)::close(directory);
            return Result<bool>::failure(tool_error("DIRECTORY_SYNC_FAILED","lockfile replaced but directory sync failed"));}
        ::close(directory);}
#endif
    return Result<bool>::success(true);
}
Result<bool> publish_directory(const std::filesystem::path& staging,
                               const std::filesystem::path& target){
#ifdef _WIN32
    if(!MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
        return Result<bool>::failure(tool_error("CACHE_PUBLISH_FAILED","cannot atomically publish cache entry"));
#else
    if(::rename(staging.c_str(),target.c_str())!=0)
        return Result<bool>::failure(tool_error("CACHE_PUBLISH_FAILED","cannot atomically publish cache entry"));
    int flags=O_RDONLY;
#ifdef O_DIRECTORY
    flags|=O_DIRECTORY;
#endif
    const int directory=::open(target.parent_path().c_str(),flags);
    if(directory<0||::fsync(directory)!=0){
        if(directory>=0)::close(directory);
        return Result<bool>::failure(tool_error("DIRECTORY_SYNC_FAILED",
            "cache entry published but directory sync failed"));
    }
    ::close(directory);
#endif
    return Result<bool>::success(true);
}
bool link_like(const std::filesystem::path& path){
    if(std::filesystem::is_symlink(std::filesystem::symlink_status(path)))return true;
#ifdef _WIN32
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
#else
    return false;
#endif
}
std::uint64_t tree_bytes(const std::filesystem::path& root){
    std::uint64_t total=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root)){
        if(link_like(entry.path()))
            throw std::runtime_error("link-like entry in cache");
        if(entry.is_regular_file())total+=entry.file_size();
    }
    return total;
}
Result<bool> write_text_atomic(const std::filesystem::path& path,const std::string& text,
                               const AtomicLockfileOptions& options){
    if(path.empty()||!options.max_bytes||text.size()>options.max_bytes)
        return Result<bool>::failure(tool_error("ATOMIC_WRITE_SIZE_LIMIT","invalid path or document size"));
    try{
        const std::filesystem::path parent=path.has_parent_path()?path.parent_path():std::filesystem::current_path();
        if(options.create_parent)std::filesystem::create_directories(parent);
        if(!std::filesystem::is_directory(parent))
            return Result<bool>::failure(tool_error("ATOMIC_WRITE_PARENT_INVALID","document parent is not a directory"));
        const std::filesystem::path target=std::filesystem::canonical(parent)/path.filename();
        const std::filesystem::path temp=temporary_sibling(target);
        Result<bool> written=write_bytes(temp,reinterpret_cast<const std::uint8_t*>(text.data()),
                                         text.size(),options.durable);
        if(!written)return written;
        Result<bool> replaced=replace_file(temp,target,options.durable);
        if(!replaced){std::error_code ignored;std::filesystem::remove(temp,ignored);return replaced;}
        return Result<bool>::success(true);
    }catch(const std::exception& ex){
        return Result<bool>::failure(tool_error("ATOMIC_WRITE_FAILED",ex.what()));
    }
}
}

Result<std::set<std::string>> lockfile_digests(const std::string& text){
    try{Json::Value root=parse(text);
        if(root.get("format","").asString()!="dage-lock"||root.get("lock_version",0).asInt()!=1||!root["bundles"].isObject())
            return Result<std::set<std::string>>::failure(tool_error("LOCKFILE_INVALID","invalid dage.lock header"));
        std::set<std::string> digests;
        for(const std::string& id:root["bundles"].getMemberNames()){
            const std::string digest=root["bundles"][id].get("digest","").asString(),version=root["bundles"][id].get("version","").asString();
            std::string hex;if(id.empty()||version.empty()||!hex_digest(digest,hex))
                return Result<std::set<std::string>>::failure(tool_error("LOCKFILE_ENTRY_INVALID","invalid lock entry: "+id));
            digests.insert(digest);
        }
        return Result<std::set<std::string>>::success(std::move(digests));
    }catch(const std::exception& ex){return Result<std::set<std::string>>::failure(tool_error("LOCKFILE_INVALID",ex.what()));}
}
Result<bool> write_lockfile_atomic(const std::filesystem::path& path,const std::string& text,
                                   const AtomicLockfileOptions& options){
    Result<std::set<std::string>> valid=lockfile_digests(text);if(!valid)return Result<bool>::failure(valid.error());
    return write_text_atomic(path,text,options);
}
Result<bool> write_keyring_atomic(const std::filesystem::path& path,const Keyring& keyring,
                                  const AtomicLockfileOptions& options){
    Result<std::string> snapshot=keyring.snapshot_json();
    if(!snapshot)return Result<bool>::failure(snapshot.error());
    Result<Keyring> validated=Keyring::from_snapshot_json(snapshot.value());
    if(!validated)return Result<bool>::failure(validated.error());
    return write_text_atomic(path,snapshot.value(),options);
}

BundleCache::BundleCache(std::filesystem::path root,ResourceLimits limits):root_(std::move(root)),limits_(limits){
    if(!limits_.max_resources||!limits_.max_resource_bytes||!limits_.max_total_bytes)
        throw std::invalid_argument("BundleCache resource limits must be positive");
    std::filesystem::create_directories(root_/"sha256");root_=std::filesystem::canonical(root_);
}
const std::filesystem::path& BundleCache::root()const noexcept{return root_;}
Result<std::string> BundleCache::install(const ResourceProvider& provider){
    std::filesystem::path staging;
    try{
        Engine engine;std::unique_ptr<Bundle> source=engine.load_bundle(provider);std::string hex;
        if(!hex_digest(source->digest(),hex))return Result<std::string>::failure(tool_error("DIGEST_INVALID","invalid Bundle digest"));
        const std::filesystem::path target=root_/"sha256"/hex;
        if(std::filesystem::exists(target)){DirectoryResourceProvider existing(target,limits_);
            if(engine.load_bundle(existing)->digest()!=source->digest())
                return Result<std::string>::failure(tool_error("CACHE_CORRUPT","existing cache entry digest mismatch"));
            return Result<std::string>::success(source->digest());}
        staging=root_/"sha256"/("."+hex+".tmp-"+std::to_string(next_temp.fetch_add(1)));
        std::filesystem::create_directory(staging);
        Result<std::vector<std::string>> listed=provider.list_resources();if(!listed)throw std::runtime_error(listed.error().message);
        for(const std::string& path:listed.value()){
            Result<std::string> normalized=normalize_resource_path(path);if(!normalized||normalized.value()!=path)
                throw std::runtime_error("non-normalized cache resource");
            Result<ResourceBytes> bytes=provider.read_resource(path);if(!bytes)throw std::runtime_error(bytes.error().message);
            const std::filesystem::path output=staging/std::filesystem::path(path);std::filesystem::create_directories(output.parent_path());
            Result<bool> saved=write_bytes(output,bytes.value().data(),bytes.value().size(),true);if(!saved)throw std::runtime_error(saved.error().message);
        }
        DirectoryResourceProvider snapshot(staging,limits_);
        if(engine.load_bundle(snapshot)->digest()!=source->digest())throw std::runtime_error("cache snapshot digest mismatch");
        Result<bool> published=publish_directory(staging,target);
        if(!published){
            if(!std::filesystem::exists(target))throw std::runtime_error(published.error().message);
            DirectoryResourceProvider winner(target,limits_);
            if(engine.load_bundle(winner)->digest()!=source->digest())
                throw std::runtime_error("concurrent cache entry digest mismatch");
            std::filesystem::remove_all(staging);
        }
        return Result<std::string>::success(source->digest());
    }catch(const std::exception& ex){if(!staging.empty()){std::error_code ignored;std::filesystem::remove_all(staging,ignored);}
        return Result<std::string>::failure(tool_error("CACHE_INSTALL_FAILED",ex.what()));}
}
Result<std::shared_ptr<const ResourceProvider>> BundleCache::open(const std::string& digest)const{
    std::string hex;if(!hex_digest(digest,hex))
        return Result<std::shared_ptr<const ResourceProvider>>::failure(tool_error("DIGEST_INVALID","invalid Bundle digest"));
    try{const std::filesystem::path path=root_/"sha256"/hex;
        return Result<std::shared_ptr<const ResourceProvider>>::success(std::make_shared<DirectoryResourceProvider>(path,limits_));
    }catch(const std::exception& ex){return Result<std::shared_ptr<const ResourceProvider>>::failure(tool_error("CACHE_MISS",ex.what()));}
}
Result<CacheGcResult> BundleCache::garbage_collect(const std::set<std::string>& retained,const CacheGcOptions& options){
    if(!options.max_removals)return Result<CacheGcResult>::failure(tool_error("GC_OPTIONS_INVALID","max_removals must be positive"));
    try{std::set<std::string> retained_hex;for(const std::string& digest:retained){std::string hex;
            if(!hex_digest(digest,hex))return Result<CacheGcResult>::failure(tool_error("DIGEST_INVALID","invalid retained digest"));
            retained_hex.insert(hex);}
        CacheGcResult result;const std::filesystem::path directory=root_/"sha256";
        for(const auto& entry:std::filesystem::directory_iterator(directory)){
            const std::string name=entry.path().filename().string(),digest="sha256:"+name;std::string checked;
            if(link_like(entry.path())||!entry.is_directory()||!hex_digest(digest,checked)||retained_hex.count(name))continue;
            result.candidates.push_back(digest);if(result.candidates.size()>options.max_removals)
                return Result<CacheGcResult>::failure(tool_error("GC_LIMIT_EXCEEDED","cache GC candidate limit exceeded"));
            const std::uint64_t bytes=tree_bytes(entry.path());
            if(!options.dry_run){std::filesystem::remove_all(entry.path());result.removed.push_back(digest);result.reclaimed_bytes+=bytes;}
        }
        std::sort(result.candidates.begin(),result.candidates.end());std::sort(result.removed.begin(),result.removed.end());
        return Result<CacheGcResult>::success(std::move(result));
    }catch(const std::exception& ex){return Result<CacheGcResult>::failure(tool_error("CACHE_GC_FAILED",ex.what()));}
}

Result<BundleResolverOptions> verified_production_profile(
    std::string lock,const BundleRepository& repository,const KeyProvider& keys,const TrustPolicy& trust,
    SignaturePolicy signature_policy,bool offline){
    if(signature_policy.kind==SignaturePolicyKind::Disabled||
       (signature_policy.kind==SignaturePolicyKind::Threshold&&!signature_policy.threshold))
        return Result<BundleResolverOptions>::failure(tool_error("SIGNATURE_POLICY_INVALID","production threshold must be positive"));
    Result<std::set<std::string>> valid=lockfile_digests(lock);if(!valid)return Result<BundleResolverOptions>::failure(valid.error());
    BundleResolverOptions options;options.mode=offline?BundleLoadMode::Offline:BundleLoadMode::Verified;
    options.repository=&repository;options.keys=&keys;options.trust=&trust;
    options.lock_json=std::move(lock);options.signature_policy=signature_policy;
    return Result<BundleResolverOptions>::success(std::move(options));
}
}} // namespace dage::tools
