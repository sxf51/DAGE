#include "dage/dage.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

struct Provider { std::map<std::string,std::string> files; int* releases=nullptr; };
struct ResolverHost { Provider dependency; bool trust=true; int repository_gets=0,releases=0; };

static std::string text(dage_string_view_t value){return std::string(value.data?value.data:"",value.size);}
static void free_buffer(const char* data,size_t,void*){std::free(const_cast<char*>(data));}
static dage_owned_buffer_t owned(const void* data,size_t size){
    dage_owned_buffer_t output{};char* copy=static_cast<char*>(std::malloc(size));
    if(!copy&&size)return output;
    if(size)std::memcpy(copy,data,size);
    output.data=copy;output.size=size;output.release=&free_buffer;return output;
}
static dage_owned_buffer_t owned(const std::string& value){return owned(value.data(),value.size());}
static dage_status_t list_resources(dage_owned_buffer_t* output,void* userdata){
    const Provider& provider=*static_cast<Provider*>(userdata);std::string json="[";bool first=true;
    for(const auto& item:provider.files){if(!first)json+=",";first=false;json+="\""+item.first+"\"";}
    json+="]";*output=owned(json);return output->data||!output->size?DAGE_STATUS_OK:DAGE_STATUS_OUT_OF_MEMORY;
}
static dage_status_t read_resource(dage_string_view_t path,dage_owned_buffer_t* output,void* userdata){
    const Provider& provider=*static_cast<Provider*>(userdata);auto found=provider.files.find(text(path));
    if(found==provider.files.end())return DAGE_STATUS_NOT_FOUND;
    *output=owned(found->second);
    return output->data||!output->size?DAGE_STATUS_OK:DAGE_STATUS_OUT_OF_MEMORY;
}
static void release_provider(void* userdata){
    Provider* provider=static_cast<Provider*>(userdata);if(provider->releases)++*provider->releases;delete provider;
}
static dage_resource_provider_t descriptor(Provider* provider,bool transferred){
    dage_resource_provider_t result{};result.struct_size=sizeof(result);
    result.list_resources=&list_resources;result.read_resource=&read_resource;
    result.release=transferred?&release_provider:nullptr;result.userdata=provider;return result;
}
static dage_status_t versions(dage_string_view_t id,uint8_t,dage_owned_buffer_t* output,void*){
    if(text(id)!="org.dage.dep")return DAGE_STATUS_NOT_FOUND;
    *output=owned("[\"1.0.0\"]");return DAGE_STATUS_OK;
}
static dage_status_t get_bundle(dage_string_view_t id,dage_string_view_t version,uint8_t,
                                dage_resource_provider_t* output,void* userdata){
    ResolverHost& host=*static_cast<ResolverHost*>(userdata);
    if(text(id)!="org.dage.dep"||text(version)!="1.0.0")return DAGE_STATUS_NOT_FOUND;
    Provider* provider=new Provider(host.dependency);provider->releases=&host.releases;
    *output=descriptor(provider,true);++host.repository_gets;return DAGE_STATUS_OK;
}
static dage_status_t source(dage_owned_buffer_t* output,void*){*output=owned("test-repo");return DAGE_STATUS_OK;}
static std::vector<unsigned char> hex(const std::string& value){
    std::vector<unsigned char> result;auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<value.size();i+=2)result.push_back(static_cast<unsigned char>((digit(value[i])<<4)|digit(value[i+1])));
    return result;
}
static dage_status_t find_key(dage_string_view_t id,dage_string_view_t context,
                              dage_owned_buffer_t* output,void*){
    if(text(id)!="golden-ed25519"||text(context).find("\"bundle_id\":\"org.dage.golden\"")==std::string::npos)
        return DAGE_STATUS_NOT_FOUND;
    const auto key=hex("03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8");
    *output=owned(key.data(),key.size());return DAGE_STATUS_OK;
}
static dage_status_t trust(dage_string_view_t context,uint8_t* trusted,void* userdata){
    ResolverHost& host=*static_cast<ResolverHost*>(userdata);
    *trusted=host.trust&&text(context).find("\"algorithm\":\"ed25519\"")!=std::string::npos?1:0;
    return DAGE_STATUS_OK;
}
static std::string file(const std::string& path){
    std::ifstream input(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{});
}
static std::string output_string(dage_status_t (*fn)(dage_resolved_bundle_graph_handle,char*,size_t,size_t*),
                                 dage_resolved_bundle_graph_handle graph){
    size_t required=0;if(fn(graph,nullptr,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL)return {};
    std::string result(required,'\0');if(fn(graph,&result[0],result.size(),&required))return {};
    result.resize(required-1);return result;
}

int main(){
    dage_engine_handle engine=nullptr;if(dage_engine_create(nullptr,&engine))return 1;
    const std::string base=std::string(DAGE_SOURCE_DIR)+"/testdata/golden/bundle_v1/";
    Provider golden;golden.files["manifest.json"]=file(base+"manifest.json");
    golden.files["workflows/main.json"]=file(base+"workflows/main.json");
    golden.files["assets/prompt.txt"]=file(base+"assets/prompt.txt");
    auto root=descriptor(&golden,false);
    dage_bundle_resolver_options_t development{};development.struct_size=sizeof(development);
    development.mode=DAGE_BUNDLE_LOAD_DEVELOPMENT;development.signature_policy=DAGE_SIGNATURE_DISABLED;
    dage_resolved_bundle_graph_handle graph=nullptr;
    if(dage_bundle_resolve(engine,&root,&development,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&graph))return 2;
    const std::string lock=output_string(&dage_resolved_bundle_graph_lock,graph);
    if(lock.empty()||output_string(&dage_resolved_bundle_graph_root_id,graph)!="org.dage.golden")return 3;
    dage_resolved_bundle_graph_destroy(graph);graph=nullptr;

    ResolverHost host;dage_key_provider_vtable_t keys{};keys.struct_size=sizeof(keys);keys.find_key=&find_key;
    dage_trust_policy_vtable_t policy{};policy.struct_size=sizeof(policy);policy.trust=&trust;
    dage_bundle_resolver_options_t verified{};verified.struct_size=sizeof(verified);
    verified.mode=DAGE_BUNDLE_LOAD_VERIFIED;verified.signature_policy=DAGE_SIGNATURE_AT_LEAST_ONE;
    verified.lock_json={lock.data(),lock.size()};
    if(dage_bundle_resolve(engine,&root,&verified,nullptr,nullptr,&keys,nullptr,&policy,&host,&graph))return 4;
    dage_workflow_handle workflow=nullptr;const char main_id[]="main";
    if(dage_resolved_bundle_graph_load_workflow(engine,graph,{nullptr,0},{main_id,4},&workflow))return 5;
    dage_workflow_destroy(workflow);dage_resolved_bundle_graph_destroy(graph);graph=nullptr;
    host.trust=false;
    if(dage_bundle_resolve(engine,&root,&verified,nullptr,nullptr,&keys,nullptr,&policy,&host,&graph)
       !=DAGE_STATUS_VALIDATION_ERROR)return 6;

    Provider dependency;dependency.files["manifest.json"]=
        "{\"format\":\"dage-bundle\",\"format_version\":\"0.2.0\",\"id\":\"org.dage.dep\","
        "\"version\":\"1.0.0\",\"workflows\":{\"main\":\"workflows/main.json\"},\"dependencies\":{}}";
    dependency.files["workflows/main.json"]=
        "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"z\",\"nodes\":{\"z\":{\"type\":\"end\"}}}";
    host.dependency=dependency;
    Provider parent;parent.files["manifest.json"]=
        "{\"format\":\"dage-bundle\",\"format_version\":\"0.2.0\",\"id\":\"org.dage.root\","
        "\"version\":\"1.0.0\",\"workflows\":{\"main\":\"workflows/main.json\"},"
        "\"dependencies\":{\"org.dage.dep\":\"^1.0.0\"}}";
    parent.files["workflows/main.json"]=dependency.files["workflows/main.json"];
    auto parent_descriptor=descriptor(&parent,false);
    dage_bundle_repository_vtable_t repository{};repository.struct_size=sizeof(repository);
    repository.available_versions=&versions;repository.get=&get_bundle;repository.source=&source;
    development.lock_json={nullptr,0};
    if(dage_bundle_resolve(engine,&parent_descriptor,&development,&repository,&host,
                           nullptr,nullptr,nullptr,nullptr,&graph))return 7;
    dage_resolved_bundle_graph_destroy(graph);dage_engine_destroy(engine);
    if(host.repository_gets!=1||host.releases!=1)return 8;
    return 0;
}
