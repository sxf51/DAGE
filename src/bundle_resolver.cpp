#include "dage/bundle_resolver.hpp"
#include "bundle_internal.hpp"

#include <json/json.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dage {
namespace {
Error make_error(const std::string& category,const std::string& code,const std::string& message){
    Error error;error.category=category;error.code=code;error.message=message;return error;
}
Json::Value parse_json(const std::string& text){
    Json::CharReaderBuilder builder;builder["collectComments"]=false;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;std::string errors;if(!reader->parse(text.data(),text.data()+text.size(),&root,&errors))throw std::runtime_error(errors);return root;
}
std::string write_json(const Json::Value& value){
    Json::StreamWriterBuilder builder;builder["indentation"]="  ";builder["emitUTF8"]=true;return Json::writeString(builder,value);
}
bool namespace_match(const std::string& bundle_id,const std::string& scope){
    return bundle_id==scope||(bundle_id.size()>scope.size()&&
        bundle_id.compare(0,scope.size(),scope)==0&&bundle_id[scope.size()]=='.');
}
std::string hex_key(const std::vector<std::uint8_t>& bytes){
    static const char digits[]="0123456789abcdef";std::string result;result.reserve(bytes.size()*2);
    for(std::uint8_t byte:bytes){result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}
    return result;
}
bool parse_key_hex(const std::string& text,std::vector<std::uint8_t>& bytes){
    if(text.size()!=64)return false;
    bytes.clear();bytes.reserve(32);
    auto nibble=[](char c)->int{
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        return -1;
    };
    for(std::size_t i=0;i<text.size();i+=2){const int high=nibble(text[i]),low=nibble(text[i+1]);
        if(high<0||low<0)return false;
        bytes.push_back(static_cast<std::uint8_t>((high<<4)|low));}
    return true;
}
struct Version {int major=0,minor=0,patch=0;std::string text;};
bool parse_version(const std::string& text,Version& out){
    if(text.empty())return false;
    std::istringstream in(text);char a=0,b=0;
    if(!(in>>out.major>>a>>out.minor>>b>>out.patch)||a!='.'||b!='.'||out.major<0||out.minor<0||out.patch<0)return false;
    std::string rest;if(in>>rest)return false;out.text=text;return true;
}
int compare(const Version&a,const Version&b){
    if(a.major!=b.major)return a.major<b.major?-1:1;
    if(a.minor!=b.minor)return a.minor<b.minor?-1:1;
    return a.patch==b.patch?0:(a.patch<b.patch?-1:1);
}
bool satisfies_atom(const Version& version,const std::string& atom){
    if(atom.empty()||atom=="*")return true;
    if(atom[0]=='^'){Version base;if(!parse_version(atom.substr(1),base))return false;
        Version upper=base;if(base.major>0){++upper.major;upper.minor=upper.patch=0;}
        else if(base.minor>0){++upper.minor;upper.patch=0;}else ++upper.patch;
        return compare(version,base)>=0&&compare(version,upper)<0;
    }
    const char* operators[]={">=","<=",">","<","="};
    for(const char* op:operators){const std::string prefix=op;if(atom.rfind(prefix,0)==0){Version target;if(!parse_version(atom.substr(prefix.size()),target))return false;
        const int c=compare(version,target);if(prefix==">=")return c>=0;if(prefix=="<=")return c<=0;if(prefix==">")return c>0;if(prefix=="<")return c<0;return c==0;}}
    Version exact;return parse_version(atom,exact)&&compare(version,exact)==0;
}
bool satisfies(const std::string& version_text,const std::string& constraint){
    Version version;if(!parse_version(version_text,version))return false;std::istringstream input(constraint);std::string atom;bool any=false;
    while(input>>atom){any=true;if(!satisfies_atom(version,atom))return false;}return any;
}
std::vector<std::uint8_t> decode_base64(const std::string& text){
    if(text.empty()||text.size()%4!=0)return {};
    std::vector<std::uint8_t> out(text.size()/4*3);
    const int size=EVP_DecodeBlock(out.data(),reinterpret_cast<const unsigned char*>(text.data()),static_cast<int>(text.size()));
    if(size<0)return {};
    std::size_t actual=static_cast<std::size_t>(size);
    if(!text.empty()&&text.back()=='=')--actual;
    if(text.size()>1&&text[text.size()-2]=='=')--actual;
    out.resize(actual);return out;
}
bool verify_ed25519(const PublicKey& key,const std::string& message,const std::string& signature_base64){
    const std::vector<std::uint8_t> signature=decode_base64(signature_base64);if(key.bytes.size()!=32||signature.size()!=64)return false;
    EVP_PKEY* public_key=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,key.bytes.data(),key.bytes.size());if(!public_key)return false;
    EVP_MD_CTX* context=EVP_MD_CTX_new();bool valid=false;
    if(context&&EVP_DigestVerifyInit(context,nullptr,nullptr,nullptr,public_key)==1)
        valid=EVP_DigestVerify(context,signature.data(),signature.size(),
                               reinterpret_cast<const unsigned char*>(message.data()),message.size())==1;
    EVP_MD_CTX_free(context);EVP_PKEY_free(public_key);return valid;
}
struct Locked {std::string version,digest,source;};
std::map<std::string,Locked> parse_lock(const std::string& text){
    std::map<std::string,Locked> result;if(text.empty())return result;Json::Value root=parse_json(text);
    if(root.get("format","").asString()!="dage-lock"||root.get("lock_version",0).asInt()!=1||!root["bundles"].isObject())
        throw std::runtime_error("INVALID_LOCKFILE");
    for(const std::string& id:root["bundles"].getMemberNames()){const Json::Value& value=root["bundles"][id];
        Locked lock{value.get("version","").asString(),value.get("digest","").asString(),value.get("source","").asString()};
        if(lock.version.empty()||lock.digest.empty())throw std::runtime_error("INVALID_LOCK_ENTRY: "+id);
        result[id]=std::move(lock);}
    return result;
}
void verify_signatures(const Bundle& bundle,const BundleResolverOptions& options){
    const SignaturePolicy& policy=options.signature_policy;
    if(policy.kind==SignaturePolicyKind::Disabled)return;
    if(!options.keys)throw std::runtime_error("KEY_PROVIDER_REQUIRED");
    if(!options.trust)throw std::runtime_error("TRUST_POLICY_REQUIRED");
    if(policy.kind==SignaturePolicyKind::Threshold&&policy.threshold==0)
        throw std::runtime_error("SIGNATURE_POLICY_INVALID");
    std::set<std::string> seen,valid_public_keys;
    for(const BundleSignature& signature:bundle.signatures()){
        if(signature.key_id.empty()||!seen.insert(signature.key_id).second)continue;
        if(signature.algorithm!="ed25519")continue;
        SignatureContext context{bundle.id(),bundle.version(),bundle.digest(),signature.key_id,signature.algorithm,signature.signed_at};
        Result<PublicKey> key=options.keys->find_key(signature.key_id,context);if(!key)continue;
        Result<bool> trusted=options.trust->trust(context);
        if(!trusted||!trusted.value())continue;
        if(verify_ed25519(key.value(),bundle.signing_payload(),signature.signature_base64))
            valid_public_keys.insert(hex_key(key.value().bytes));
    }
    std::size_t required=1;
    if(policy.kind==SignaturePolicyKind::Threshold)required=policy.threshold;
    else if(policy.kind==SignaturePolicyKind::All)required=bundle.signatures().size();
    if(required==0||valid_public_keys.size()<required)
        throw std::runtime_error("SIGNATURE_THRESHOLD_NOT_MET: "+bundle.id());
}
}

void Keyring::add(std::string id,PublicKey key){
    KeyRecord record;record.key_id=std::move(id);record.key=std::move(key);add(std::move(record));
}
void Keyring::add(KeyRecord record){
    if(record.key_id.empty()||record.key.bytes.size()!=32)
        throw std::invalid_argument("invalid Ed25519 key");
    for(const std::string& scope:record.namespace_scopes)
        if(scope.empty()||scope.front()=='.'||scope.back()=='.')
            throw std::invalid_argument("invalid key namespace scope");
    if(records_.count(record.key_id))throw std::invalid_argument("duplicate key id");
    records_[record.key_id]=std::move(record);
}
void Keyring::revoke(const std::string& id){
    const auto found=records_.find(id);
    if(found==records_.end())throw std::invalid_argument("cannot revoke unknown key");
    found->second.revoked=true;
}
Result<std::string> Keyring::snapshot_json()const{
    Json::Value root(Json::objectValue);root["format"]="dage-keyring";root["format_version"]=1;
    root["keys"]=Json::Value(Json::arrayValue);
    for(const auto& item:records_){
        const KeyRecord& record=item.second;Json::Value key(Json::objectValue);
        key["key_id"]=record.key_id;key["algorithm"]="ed25519";key["public_key_hex"]=hex_key(record.key.bytes);
        key["namespaces"]=Json::Value(Json::arrayValue);
        for(const std::string& scope:record.namespace_scopes)key["namespaces"].append(scope);
        key["revoked"]=record.revoked;root["keys"].append(std::move(key));
    }
    return Result<std::string>::success(write_json(root));
}
Result<Keyring> Keyring::from_snapshot_json(const std::string& text){
    try{
        const Json::Value root=parse_json(text);
        if(root.get("format","").asString()!="dage-keyring"||
           root.get("format_version",0).asInt()!=1||!root["keys"].isArray())
            return Result<Keyring>::failure(make_error("trust","KEYRING_INVALID","invalid keyring header"));
        Keyring keyring;std::set<std::string> ids;
        for(const Json::Value& value:root["keys"]){
            if(!value.isObject()||value.get("algorithm","").asString()!="ed25519"||
               !value["namespaces"].isArray())
                return Result<Keyring>::failure(make_error("trust","KEY_RECORD_INVALID","invalid key record"));
            KeyRecord record;record.key_id=value.get("key_id","").asString();
            if(!ids.insert(record.key_id).second||
               !parse_key_hex(value.get("public_key_hex","").asString(),record.key.bytes))
                return Result<Keyring>::failure(make_error("trust","KEY_RECORD_INVALID","duplicate or invalid key"));
            for(const Json::Value& scope:value["namespaces"]){
                if(!scope.isString())return Result<Keyring>::failure(
                    make_error("trust","KEY_RECORD_INVALID","namespace scope must be a string"));
                record.namespace_scopes.push_back(scope.asString());
            }
            record.revoked=value.get("revoked",false).asBool();
            keyring.add(std::move(record));
        }
        return Result<Keyring>::success(std::move(keyring));
    }catch(const std::bad_alloc&){throw;}
    catch(const std::exception& exception){
        return Result<Keyring>::failure(make_error("trust","KEYRING_INVALID",exception.what()));
    }
}
Result<PublicKey> Keyring::find_key(const std::string& id,const SignatureContext& context)const{
    const auto found=records_.find(id);
    if(found==records_.end())return Result<PublicKey>::failure(make_error("trust","UNKNOWN_KEY","unknown key: "+id));
    const KeyRecord& record=found->second;
    if(record.revoked)return Result<PublicKey>::failure(make_error("trust","KEY_REVOKED","key is revoked: "+id));
    if(!record.namespace_scopes.empty()){
        bool matches=false;for(const std::string& scope:record.namespace_scopes)
            if(namespace_match(context.bundle_id,scope)){matches=true;break;}
        if(!matches)return Result<PublicKey>::failure(make_error("trust","KEY_SCOPE_DENIED","key is outside Bundle namespace: "+id));
    }
    return Result<PublicKey>::success(record.key);
}
Result<bool> AllowKnownKeysTrustPolicy::trust(const SignatureContext&)const{return Result<bool>::success(true);}

MemoryBundleRepository::MemoryBundleRepository(std::string source):source_(std::move(source)){}
void MemoryBundleRepository::add(std::string id,std::string version,std::shared_ptr<const ResourceProvider> provider){
    if(!provider)throw std::invalid_argument("bundle provider must not be null");
    bundles_[std::move(id)][std::move(version)]=std::move(provider);
}
Result<std::vector<std::string>> MemoryBundleRepository::available_versions(const std::string& id,bool)const{
    const auto found=bundles_.find(id);if(found==bundles_.end())return Result<std::vector<std::string>>::failure(make_error("resolver","BUNDLE_NOT_FOUND","bundle not found: "+id));
    std::vector<std::string> versions;for(const auto& item:found->second)versions.push_back(item.first);return Result<std::vector<std::string>>::success(std::move(versions));
}
Result<std::shared_ptr<const ResourceProvider>> MemoryBundleRepository::get(const std::string& id,const std::string& version,bool)const{
    const auto bundle=bundles_.find(id);if(bundle==bundles_.end())return Result<std::shared_ptr<const ResourceProvider>>::failure(make_error("resolver","BUNDLE_NOT_FOUND","bundle not found: "+id));
    const auto found=bundle->second.find(version);if(found==bundle->second.end())return Result<std::shared_ptr<const ResourceProvider>>::failure(make_error("resolver","VERSION_NOT_FOUND","bundle version not found: "+id+"@"+version));
    return Result<std::shared_ptr<const ResourceProvider>>::success(found->second);
}
std::string MemoryBundleRepository::source()const{return source_;}

class ResolvedBundleGraph::Impl {
public:
    std::string root_id,lock;BundleLoadMode mode=BundleLoadMode::Development;
    std::map<std::string,std::unique_ptr<Bundle>> bundles;std::vector<std::string> order;
};
ResolvedBundleGraph::ResolvedBundleGraph(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}ResolvedBundleGraph::~ResolvedBundleGraph()=default;
ResolvedBundleGraph::ResolvedBundleGraph(ResolvedBundleGraph&&)noexcept=default;ResolvedBundleGraph& ResolvedBundleGraph::operator=(ResolvedBundleGraph&&)noexcept=default;
const Bundle& ResolvedBundleGraph::root()const noexcept{return *impl_->bundles.find(impl_->root_id)->second;}
const Bundle* ResolvedBundleGraph::find(const std::string&id)const noexcept{auto found=impl_->bundles.find(id);return found==impl_->bundles.end()?nullptr:found->second.get();}
std::vector<std::string> ResolvedBundleGraph::topological_order()const{return impl_->order;}const std::string& ResolvedBundleGraph::lock_json()const noexcept{return impl_->lock;}
Result<Value> ResolvedBundleGraph::merged_config(const ConfigMergeOptions& options)const{
    Value merged=Value::object();
    for(const std::string& id:impl_->order){
        const auto found=impl_->bundles.find(id);if(found==impl_->bundles.end())continue;
        Result<Value> next=deep_merge_config(merged,found->second->config(),options);
        if(!next)return next;
        merged=std::move(next.value());
    }
    return Result<Value>::success(std::move(merged));
}
BundleLoadMode ResolvedBundleGraph::load_mode()const noexcept{return impl_->mode;}
Result<BundleRollbackPlan> ResolvedBundleGraph::plan_rollback_to(const ResolvedBundleGraph& target)const{
    const Bundle& from=root();const Bundle& to=target.root();
    if(from.id()!=to.id())return Result<BundleRollbackPlan>::failure(
        make_error("rollback","ROLLBACK_ROOT_MISMATCH","rollback target has a different root Bundle id"));
    Version from_version,to_version;
    if(!parse_version(from.version(),from_version)||!parse_version(to.version(),to_version))
        return Result<BundleRollbackPlan>::failure(
            make_error("rollback","ROLLBACK_VERSION_INVALID","root Bundle versions must be valid SemVer core versions"));
    if(compare(to_version,from_version)>=0)return Result<BundleRollbackPlan>::failure(
        make_error("rollback","ROLLBACK_TARGET_NOT_OLDER","rollback target root version must be older than the current version"));
    if(target.lock_json().empty())return Result<BundleRollbackPlan>::failure(
        make_error("rollback","ROLLBACK_TARGET_UNLOCKED","rollback target must contain an exact lockfile"));
    BundleRollbackPlan plan;plan.root_bundle_id=from.id();plan.from_version=from.version();plan.to_version=to.version();
    plan.from_digest=from.digest();plan.to_digest=to.digest();plan.target_lock_json=target.lock_json();
    plan.target_load_mode=target.impl_->mode;
    plan.target_verified=target.impl_->mode==BundleLoadMode::Verified||target.impl_->mode==BundleLoadMode::Offline;
    std::set<std::string> ids;
    for(const auto& item:impl_->bundles)ids.insert(item.first);
    for(const auto& item:target.impl_->bundles)ids.insert(item.first);
    for(const std::string& id:ids){
        const Bundle* old_bundle=find(id);const Bundle* new_bundle=target.find(id);
        if(old_bundle&&new_bundle&&old_bundle->version()==new_bundle->version()&&old_bundle->digest()==new_bundle->digest())continue;
        BundleVersionChange change;change.bundle_id=id;
        change.kind=!old_bundle?BundleVersionChangeKind::Added:(!new_bundle?BundleVersionChangeKind::Removed:BundleVersionChangeKind::Changed);
        if(old_bundle){change.from_version=old_bundle->version();change.from_digest=old_bundle->digest();}
        if(new_bundle){change.to_version=new_bundle->version();change.to_digest=new_bundle->digest();}
        plan.changes.push_back(std::move(change));
    }
    return Result<BundleRollbackPlan>::success(std::move(plan));
}

BundleResolver::BundleResolver(BundleResolverOptions options):options_(std::move(options)){}
Result<std::unique_ptr<ResolvedBundleGraph>> BundleResolver::resolve(const ResourceProvider& root_provider)const{
    try{return Result<std::unique_ptr<ResolvedBundleGraph>>::success(resolve_unchecked(root_provider));}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception& exception){
        std::string message=exception.what();std::string code="RESOLUTION_FAILED";const std::size_t colon=message.find(':');
        if(colon!=std::string::npos)code=message.substr(0,colon);else if(!message.empty())code=message;
        return Result<std::unique_ptr<ResolvedBundleGraph>>::failure(make_error("resolver",code,message));
    }
}
std::unique_ptr<ResolvedBundleGraph> BundleResolver::resolve_unchecked(const ResourceProvider& root_provider)const{
    const bool locked_mode=options_.mode!=BundleLoadMode::Development;
    const bool verified=options_.mode==BundleLoadMode::Verified||options_.mode==BundleLoadMode::Offline;
    const bool offline=options_.mode==BundleLoadMode::Offline;
    std::map<std::string,Locked> locks=parse_lock(options_.lock_json);
    if(locked_mode&&options_.lock_json.empty())throw std::runtime_error("LOCKFILE_REQUIRED");
    auto graph=std::make_unique<ResolvedBundleGraph::Impl>();
    graph->mode=options_.mode;
    std::unique_ptr<Bundle> root=load_bundle_from_provider(root_provider);graph->root_id=root->id();
    std::map<std::string,std::string> selected;std::set<std::string> active;
    std::function<void(std::unique_ptr<Bundle>,const std::string&)> visit;
    visit=[&](std::unique_ptr<Bundle> bundle,const std::string& source){
        const std::string id=bundle->id();if(active.count(id))throw std::runtime_error("DEPENDENCY_CYCLE: "+id);
        auto existing=selected.find(id);if(existing!=selected.end()){if(existing->second!=bundle->version())throw std::runtime_error("DEPENDENCY_CONFLICT: "+id);return;}
        if(locked_mode){const auto lock=locks.find(id);if(lock==locks.end())throw std::runtime_error("LOCK_ENTRY_REQUIRED: "+id);
            if(lock->second.version!=bundle->version()||lock->second.digest!=bundle->digest())throw std::runtime_error("LOCK_MISMATCH: "+id);
            if(!lock->second.source.empty()&&lock->second.source!=source)throw std::runtime_error("LOCK_SOURCE_MISMATCH: "+id);}
        if(verified)verify_signatures(*bundle,options_);
        active.insert(id);selected[id]=bundle->version();
        const std::vector<BundleDependency> dependencies=bundle->dependencies();
        graph->bundles[id]=std::move(bundle);
        for(const BundleDependency& dependency:dependencies){
            if(active.count(dependency.id))throw std::runtime_error("DEPENDENCY_CYCLE: "+dependency.id);
            auto already=selected.find(dependency.id);if(already!=selected.end()){
                if(!satisfies(already->second,dependency.constraint))
                    throw std::runtime_error("DEPENDENCY_CONFLICT: "+dependency.id);
                continue;}
            if(!options_.repository)throw std::runtime_error("BUNDLE_REPOSITORY_REQUIRED");
            std::string version;
            if(locked_mode){const auto lock=locks.find(dependency.id);if(lock==locks.end())throw std::runtime_error("LOCK_ENTRY_REQUIRED: "+dependency.id);
                version=lock->second.version;if(!satisfies(version,dependency.constraint))throw std::runtime_error("LOCK_CONSTRAINT_MISMATCH: "+dependency.id);}
            else{Result<std::vector<std::string>> versions=options_.repository->available_versions(dependency.id,false);if(!versions)throw std::runtime_error(versions.error().code+": "+versions.error().message);
                Version best;bool found=false;for(const std::string& candidate:versions.value()){Version parsed;if(parse_version(candidate,parsed)&&satisfies(candidate,dependency.constraint)&&(!found||compare(parsed,best)>0)){best=parsed;found=true;}}
                if(!found)throw std::runtime_error("NO_COMPATIBLE_VERSION: "+dependency.id);
                version=best.text;}
            Result<std::shared_ptr<const ResourceProvider>> provider=options_.repository->get(dependency.id,version,offline);
            if(!provider)throw std::runtime_error(provider.error().code+": "+provider.error().message);
            std::unique_ptr<Bundle> resolved=load_bundle_from_provider(*provider.value());
            if(resolved->id()!=dependency.id||resolved->version()!=version)
                throw std::runtime_error("REPOSITORY_IDENTITY_MISMATCH: "+dependency.id+"@"+version);
            visit(std::move(resolved),options_.repository->source());
        }
        active.erase(id);graph->order.push_back(id);
    };
    visit(std::move(root),"root");
    Json::Value lock(Json::objectValue);lock["format"]="dage-lock";lock["lock_version"]=1;lock["bundles"]=Json::Value(Json::objectValue);
    for(const auto& item:graph->bundles){Json::Value entry(Json::objectValue);entry["version"]=item.second->version();entry["digest"]=item.second->digest();
        entry["source"]=item.first==graph->root_id?"root":(options_.repository?options_.repository->source():"");entry["signatures"]=Json::Value(Json::arrayValue);
        for(const BundleSignature& signature:item.second->signatures()){Json::Value s(Json::objectValue);s["algorithm"]=signature.algorithm;s["key_id"]=signature.key_id;entry["signatures"].append(s);}
        lock["bundles"][item.first]=entry;}
    graph->lock=write_json(lock);
    return std::unique_ptr<ResolvedBundleGraph>(new ResolvedBundleGraph(std::move(graph)));
}
} // namespace dage
