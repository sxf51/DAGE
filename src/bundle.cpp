#include "bundle_internal.hpp"
#include "sha256.hpp"

#include <json/json.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dage {
namespace {
Error bundle_error(const std::string& code,const std::string& message){
    Error error;error.category="bundle";error.code=code;error.message=message;return error;
}
bool utf8_byte_less(const std::string& left,const std::string& right){
    return std::lexicographical_compare(left.begin(),left.end(),right.begin(),right.end(),
        [](char a,char b){return static_cast<unsigned char>(a)<static_cast<unsigned char>(b);});
}
Json::Value parse_json(const std::string& text){
    Json::CharReaderBuilder builder;builder["collectComments"]=false;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;std::string errors;if(!reader->parse(text.data(),text.data()+text.size(),&root,&errors))throw std::runtime_error(errors);return root;
}
void append_string(std::string& output,const std::string& value){
    static const char hex[]="0123456789abcdef";output.push_back('"');
    for(unsigned char c:value){
        switch(c){case '"':output+="\\\"";break;case '\\':output+="\\\\";break;
        case '\b':output+="\\b";break;case '\f':output+="\\f";break;case '\n':output+="\\n";break;
        case '\r':output+="\\r";break;case '\t':output+="\\t";break;
        default:if(c<0x20){output+="\\u00";output.push_back(hex[c>>4]);output.push_back(hex[c&15]);}
            else output.push_back(static_cast<char>(c));}
    }output.push_back('"');
}
void append_canonical(std::string& output,const Json::Value& value){
    if(value.isNull()){output+="null";return;}
    if(value.isBool()){output+=value.asBool()?"true":"false";return;}
    if(value.isInt64()){output+=std::to_string(value.asInt64());return;}
    if(value.isUInt64()){output+=std::to_string(value.asUInt64());return;}
    if(value.isDouble()){
        const double number=value.asDouble();if(!std::isfinite(number))throw std::runtime_error("NON_FINITE_JSON_NUMBER");
        if(number==0.0){output+="0";return;}
        char buffer[64];const auto converted=std::to_chars(
            buffer,buffer+sizeof(buffer),number,std::chars_format::scientific,std::numeric_limits<double>::max_digits10);
        if(converted.ec!=std::errc())throw std::runtime_error("JSON_NUMBER_ENCODING_FAILED");
        output.append(buffer,converted.ptr);return;
    }
    if(value.isString()){append_string(output,value.asString());return;}
    if(value.isArray()){output.push_back('[');for(Json::ArrayIndex i=0;i<value.size();++i){
        if(i)output.push_back(',');
        append_canonical(output,value[i]);}output.push_back(']');return;}
    if(value.isObject()){output.push_back('{');bool first=true;std::vector<std::string> keys=value.getMemberNames();
        std::sort(keys.begin(),keys.end(),utf8_byte_less);for(const std::string& key:keys){
        if(!first)output.push_back(',');
        first=false;append_string(output,key);output.push_back(':');append_canonical(output,value[key]);}
        output.push_back('}');return;}
    throw std::runtime_error("UNSUPPORTED_JSON_VALUE");
}
std::string canonical_json(const Json::Value& value){
    std::string output;append_canonical(output,value);return output;
}
std::string text_of(const ResourceBytes& bytes){return std::string(bytes.begin(),bytes.end());}
void validate_config_tree(const Json::Value& value,std::size_t depth,std::size_t max_depth){
    if(depth>max_depth)throw std::runtime_error("CONFIG_MAX_DEPTH");
    if(value.isObject()){
        if(value.get("format","").asString()=="dage-workflow")
            throw std::runtime_error("WORKFLOW_CONFIG_MERGE_FORBIDDEN");
        for(const std::string& key:value.getMemberNames())validate_config_tree(value[key],depth+1,max_depth);
    }else if(value.isArray())for(const Json::Value& item:value)validate_config_tree(item,depth+1,max_depth);
}
Json::Value merge_config_json(const Json::Value& base,const Json::Value& overlay,
                              const ConfigMergeOptions& options,std::size_t depth){
    if(depth>options.max_depth)throw std::runtime_error("CONFIG_MAX_DEPTH");
    if(base.isObject()&&overlay.isObject()){
        Json::Value merged=base;
        for(const std::string& key:overlay.getMemberNames()){
            const Json::Value& value=overlay[key];
            if(value.isNull()&&options.nulls==ConfigNullMerge::Delete){merged.removeMember(key);continue;}
            merged[key]=merged.isMember(key)?merge_config_json(merged[key],value,options,depth+1):value;
        }
        return merged;
    }
    if(base.isArray()&&overlay.isArray()&&options.arrays==ConfigArrayMerge::Append){
        Json::Value merged=base;for(const Json::Value& value:overlay)merged.append(value);return merged;
    }
    return overlay;
}
}

class Bundle::Impl {
public:
    std::string id,version,digest,manifest,unsigned_manifest,signing_payload;
    Value config=Value::object();
    std::map<std::string,std::string> workflows;
    std::vector<BundleDependency> dependencies;
    std::vector<BundleSignature> signatures;
};
Bundle::Bundle(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}Bundle::~Bundle()=default;
Bundle::Bundle(Bundle&&) noexcept=default;Bundle& Bundle::operator=(Bundle&&) noexcept=default;
const std::string& Bundle::id()const noexcept{return impl_->id;}const std::string& Bundle::version()const noexcept{return impl_->version;}
const std::string& Bundle::digest()const noexcept{return impl_->digest;}const std::string& Bundle::canonical_manifest()const noexcept{return impl_->manifest;}
const std::string& Bundle::canonical_unsigned_manifest()const noexcept{return impl_->unsigned_manifest;}
const std::string& Bundle::signing_payload()const noexcept{return impl_->signing_payload;}
const std::vector<BundleDependency>& Bundle::dependencies()const noexcept{return impl_->dependencies;}
const std::vector<BundleSignature>& Bundle::signatures()const noexcept{return impl_->signatures;}
const Value& Bundle::config()const noexcept{return impl_->config;}
std::vector<std::string> Bundle::workflow_ids()const{std::vector<std::string> out;for(const auto& item:impl_->workflows)out.push_back(item.first);return out;}

std::unique_ptr<Bundle> load_bundle_from_provider(const ResourceProvider& provider){
    Result<std::vector<std::string>> listed=provider.list_resources();if(!listed)throw std::runtime_error(listed.error().code+": "+listed.error().message);
    std::vector<std::string> paths=listed.value();std::sort(paths.begin(),paths.end(),utf8_byte_less);
    std::set<std::string> seen;for(const std::string& raw:paths){Result<std::string> path=normalize_resource_path(raw);
        if(!path)throw std::runtime_error(path.error().code+": "+path.error().message);
        if(path.value()!=raw)throw std::runtime_error("NON_NORMALIZED_PATH: "+raw);
        if(!seen.insert(raw).second)throw std::runtime_error("DUPLICATE_RESOURCE: "+raw);
    }
    Result<ResourceBytes> manifest_bytes=provider.read_resource("manifest.json");
    if(!manifest_bytes)throw std::runtime_error(manifest_bytes.error().code+": "+manifest_bytes.error().message);
    Json::Value manifest=parse_json(text_of(manifest_bytes.value()));
    if(manifest.get("format","").asString()!="dage-bundle")throw std::runtime_error("INVALID_BUNDLE_FORMAT");
    if(manifest.get("format_version","").asString()!="0.2.0")throw std::runtime_error("UNSUPPORTED_BUNDLE_FORMAT");
    if(!manifest["id"].isString()||!manifest["version"].isString()||!manifest["workflows"].isObject())
        throw std::runtime_error("INVALID_BUNDLE_MANIFEST");
    auto impl=std::make_unique<Bundle::Impl>();impl->id=manifest["id"].asString();impl->version=manifest["version"].asString();
    if(manifest.isMember("config")&&!manifest["config"].isObject())throw std::runtime_error("INVALID_BUNDLE_CONFIG");
    const Json::Value manifest_config=manifest.get("config",Json::Value(Json::objectValue));
    validate_config_tree(manifest_config,0,64);const std::string encoded_config=canonical_json(manifest_config);
    if(encoded_config.size()>16U*1024U*1024U)throw std::runtime_error("CONFIG_MAX_OUTPUT_BYTES");
    impl->config=Value::parse(encoded_config);
    impl->manifest=canonical_json(manifest);
    if(manifest.isMember("dependencies")&&!manifest["dependencies"].isObject())throw std::runtime_error("INVALID_DEPENDENCIES");
    for(const std::string& id:manifest["dependencies"].getMemberNames()){
        BundleDependency dependency;dependency.id=id;dependency.constraint=manifest["dependencies"][id].asString();
        if(dependency.constraint.empty())throw std::runtime_error("INVALID_DEPENDENCY_CONSTRAINT: "+id);
        impl->dependencies.push_back(std::move(dependency));
    }
    if(manifest.isMember("signatures")&&!manifest["signatures"].isArray())throw std::runtime_error("INVALID_SIGNATURES");
    for(const Json::Value& value:manifest["signatures"]){
        BundleSignature signature;signature.algorithm=value.get("algorithm","").asString();signature.key_id=value.get("key_id","").asString();
        signature.signature_base64=value.get("signature","").asString();signature.signed_at=value.get("signed_at","").asString();
        if(signature.algorithm.empty()||signature.key_id.empty()||signature.signature_base64.empty())throw std::runtime_error("INVALID_SIGNATURE");
        impl->signatures.push_back(std::move(signature));
    }
    Json::Value unsigned_manifest=manifest;unsigned_manifest.removeMember("signatures");
    impl->unsigned_manifest=canonical_json(unsigned_manifest);
    std::ostringstream digest_input;digest_input<<"manifest.json\n"<<sha256_digest(impl->unsigned_manifest)<<"\n";
    std::map<std::string,std::string> contents;
    for(const std::string& path:paths){
        if(path=="manifest.json")continue;
        Result<ResourceBytes> content=provider.read_resource(path);if(!content)throw std::runtime_error(content.error().code+": "+content.error().message);
        contents[path]=text_of(content.value());digest_input<<path<<"\n"<<sha256_digest(contents[path])<<"\n";
    }
    for(const std::string& workflow_id:manifest["workflows"].getMemberNames()){
        const std::string raw_path=manifest["workflows"][workflow_id].asString();Result<std::string> path=normalize_resource_path(raw_path);
        if(!path||path.value()!=raw_path)throw std::runtime_error("INVALID_WORKFLOW_PATH: "+raw_path);
        const auto content=contents.find(path.value());if(content==contents.end())throw std::runtime_error("RESOURCE_NOT_FOUND: "+path.value());
        impl->workflows[workflow_id]=content->second;
    }
    impl->signing_payload=digest_input.str();impl->digest=sha256_digest(impl->signing_payload);
    return std::unique_ptr<Bundle>(new Bundle(std::move(impl)));
}

Result<Value> deep_merge_config(const Value& base,const Value& overlay,const ConfigMergeOptions& options){
    try{
        if(!options.max_depth||!options.max_output_bytes)
            return Result<Value>::failure(bundle_error("INVALID_CONFIG_MERGE_OPTIONS","Merge limits must be positive."));
        Json::Value left=parse_json(base.to_json(false)),right=parse_json(overlay.to_json(false));
        if(!left.isObject()||!right.isObject())
            return Result<Value>::failure(bundle_error("CONFIG_ROOT_NOT_OBJECT","Business config roots must be objects."));
        validate_config_tree(left,0,options.max_depth);validate_config_tree(right,0,options.max_depth);
        Json::Value merged=merge_config_json(left,right,options,0);const std::string encoded=canonical_json(merged);
        if(encoded.size()>options.max_output_bytes)
            return Result<Value>::failure(bundle_error("CONFIG_MAX_OUTPUT_BYTES","Merged config exceeds max_output_bytes."));
        return Result<Value>::success(Value::parse(encoded));
    }catch(const std::bad_alloc&){throw;}
    catch(const std::exception& ex){return Result<Value>::failure(bundle_error(ex.what(),ex.what()));}
}

Result<std::string> bundle_workflow_json(const Bundle& bundle,const std::string& workflow_id){
    const auto found=bundle.impl_->workflows.find(workflow_id);
    if(found==bundle.impl_->workflows.end())return Result<std::string>::failure(bundle_error("WORKFLOW_NOT_FOUND","bundle workflow not found: "+workflow_id));
    return Result<std::string>::success(found->second);
}
} // namespace dage
