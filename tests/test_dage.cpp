#include "dage/dage.hpp"
#include "dage/bundle.hpp"
#include "dage/bundle_resolver.hpp"
#include "dage/ir.hpp"
#include "dage/resource_provider.hpp"
#include "dage/result.hpp"
#include "dage/extensions/opentelemetry.hpp"
#ifdef DAGE_HAS_BUNDLE_TOOLS
#include "dage/tools/resource_providers.hpp"
#include "dage/tools/bundle_store.hpp"
#include <zlib.h>
#include <fstream>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <openssl/evp.h>

#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ")+#x); } while(0)

class JoiningThreads {
public:
    ~JoiningThreads() {
        for(auto& thread:threads_)if(thread.joinable())thread.join();
    }
    template<class Function>
    void launch(Function&& function) {
        threads_.emplace_back(std::forward<Function>(function));
    }
private:
    std::vector<std::thread> threads_;
};

static bool has_code(const std::vector<dage::Diagnostic>& d,const std::string& code){
    for(std::size_t i=0;i<d.size();++i)if(d[i].code==code)return true;
    return false;
}
static bool has_event(const std::vector<dage::EventEnvelope>& events,const std::string& name){
    for(const auto& event:events)if(event.event==name)return true;
    return false;
}
#ifdef DAGE_HAS_BUNDLE_TOOLS
static void zip_u16(dage::ResourceBytes& out,std::uint16_t value){
    out.push_back(static_cast<std::uint8_t>(value));out.push_back(static_cast<std::uint8_t>(value>>8));
}
static void zip_u32(dage::ResourceBytes& out,std::uint32_t value){
    for(int i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
static dage::ResourceBytes test_zip(const std::vector<std::pair<std::string,std::string>>& files){
    struct Central {std::string name;std::uint32_t crc,compressed,size,offset;dage::ResourceBytes data;};
    std::vector<Central> entries;dage::ResourceBytes out;
    for(const auto& file:files){
        Central entry;entry.name=file.first;entry.size=static_cast<std::uint32_t>(file.second.size());
        entry.crc=static_cast<std::uint32_t>(crc32(0,reinterpret_cast<const Bytef*>(file.second.data()),entry.size));
        entry.data.resize(compressBound(entry.size));z_stream stream{};
        stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(file.second.data()));stream.avail_in=entry.size;
        stream.next_out=entry.data.data();stream.avail_out=static_cast<uInt>(entry.data.size());
        CHECK(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK);
        CHECK(deflate(&stream,Z_FINISH)==Z_STREAM_END);CHECK(deflateEnd(&stream)==Z_OK);
        entry.data.resize(stream.total_out);entry.compressed=static_cast<std::uint32_t>(entry.data.size());
        entry.offset=static_cast<std::uint32_t>(out.size());
        zip_u32(out,0x04034b50);zip_u16(out,20);zip_u16(out,0x0800);zip_u16(out,8);
        zip_u16(out,0);zip_u16(out,0);zip_u32(out,entry.crc);zip_u32(out,entry.compressed);zip_u32(out,entry.size);
        zip_u16(out,static_cast<std::uint16_t>(entry.name.size()));zip_u16(out,0);
        out.insert(out.end(),entry.name.begin(),entry.name.end());out.insert(out.end(),entry.data.begin(),entry.data.end());
        entries.push_back(std::move(entry));
    }
    const std::uint32_t central_offset=static_cast<std::uint32_t>(out.size());
    for(const Central& entry:entries){
        zip_u32(out,0x02014b50);zip_u16(out,0x0314);zip_u16(out,20);zip_u16(out,0x0800);zip_u16(out,8);
        zip_u16(out,0);zip_u16(out,0);zip_u32(out,entry.crc);zip_u32(out,entry.compressed);zip_u32(out,entry.size);
        zip_u16(out,static_cast<std::uint16_t>(entry.name.size()));zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);
        zip_u16(out,0);zip_u32(out,0);zip_u32(out,entry.offset);out.insert(out.end(),entry.name.begin(),entry.name.end());
    }
    const std::uint32_t central_size=static_cast<std::uint32_t>(out.size())-central_offset;
    zip_u32(out,0x06054b50);zip_u16(out,0);zip_u16(out,0);zip_u16(out,static_cast<std::uint16_t>(entries.size()));
    zip_u16(out,static_cast<std::uint16_t>(entries.size()));zip_u32(out,central_size);zip_u32(out,central_offset);zip_u16(out,0);
    return out;
}
static std::string file_text(const std::string& path){
    std::ifstream stream(path,std::ios::binary);CHECK(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream),{});
}
static std::vector<std::uint8_t> hex_bytes(const std::string& text){
    CHECK(text.size()%2==0);std::vector<std::uint8_t> bytes;
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':10+(c>='a'&&c<='f'?c-'a':c-'A');};
    for(std::size_t i=0;i<text.size();i+=2)bytes.push_back(static_cast<std::uint8_t>((digit(text[i])<<4)|digit(text[i+1])));
    return bytes;
}
#endif
class SlowTraceSink final:public dage::TraceSink{
public:void emit(const dage::EventEnvelope&)noexcept override{std::this_thread::sleep_for(std::chrono::milliseconds(4));}
};
#ifdef DAGE_TEST_OPENTELEMETRY_BRIDGE
class TestOtelExporter final:public dage::extensions::OpenTelemetryExporter{
public:void export_span(const dage::extensions::OpenTelemetrySpan& span)noexcept override{spans.push_back(span);}
    std::vector<dage::extensions::OpenTelemetrySpan> spans;
};
#endif
static std::string wf(const std::string& nodes,const std::string& entry="a");
class InlineScheduler final : public dage::Scheduler {
public:
    std::future<void> schedule(std::function<void()> task) override {
        ++scheduled;
        std::promise<void> done;
        try { task(); done.set_value(); }
        catch (...) { done.set_exception(std::current_exception()); }
        return done.get_future();
    }
    int scheduled=0;
};
class RejectingScheduler final : public dage::Scheduler {
public:
    std::future<void> schedule(std::function<void()>) override {
        throw std::runtime_error("queue full");
    }
};
static void infrastructure_tests(){
    dage::Result<int> ok=dage::Result<int>::success(7);
    CHECK(ok&&ok.value()==7);
    dage::Error error;error.category="validation";error.code="BAD";error.message="bad input";
    dage::Result<int> failed=dage::Result<int>::failure(error);
    CHECK(!failed&&failed.error().code=="BAD");

    dage::ThreadPoolOptions options;options.worker_count=2;options.queue_capacity=4;
    dage::ThreadPoolScheduler pool(options);
    CHECK(pool.worker_count()==2&&pool.queue_capacity()==4);
    CHECK(pool.schedule([](){}).wait_for(std::chrono::seconds(1))==std::future_status::ready);

    auto scheduler=std::make_shared<InlineScheduler>();
    dage::Engine engine(scheduler);
    engine.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value&i){return dage::ExecutionResult::ok(i);});
    std::unique_ptr<dage::Workflow> workflow=engine.load(wf(
        "{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\",\"c\"],\"join\":\"j\"},"
        "\"b\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"c\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"echo\",\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\"}}"));
    CHECK(engine.create_run(*workflow)->execute(dage::Value::object()).success);
    // One parent Run drive plus one continuation-native child Run drive per branch.
    CHECK(scheduler->scheduled==3);

    dage::Engine rejecting(std::make_shared<RejectingScheduler>());
    std::unique_ptr<dage::Workflow> rejected=rejecting.load(wf(
        "{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\"],\"join\":\"j\"},"
        "\"b\":{\"type\":\"noop\",\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"missing\"}}"));
    dage::ExecutionResult rejection=rejecting.create_run(*rejected)->execute(dage::Value::object());
    CHECK(!rejection.success&&rejection.error.code=="SCHEDULER_REJECTED");
}
static void bundle_ir_tests(){
    dage::MemoryResourceProvider resources;
    resources.add_text("manifest.json",
        "{\"format\":\"dage-bundle\",\"format_version\":\"0.2.0\","
        "\"id\":\"com.example.demo\",\"version\":\"1.2.3\","
        "\"workflows\":{\"main\":\"workflows/main.json\"}}");
    resources.add_text("workflows/main.json",wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"echo\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\","
        "\"idempotency_key\":\"${run.id}:${node.id}\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\"}}"));
    dage::Engine engine;
    std::unique_ptr<dage::Bundle> bundle=engine.load_bundle(resources);
    CHECK(bundle->id()=="com.example.demo"&&bundle->version()=="1.2.3");
    CHECK(bundle->workflow_ids().size()==1&&bundle->digest().find("sha256:")==0&&bundle->digest().size()==71);
    std::unique_ptr<dage::Workflow> workflow=engine.load_workflow(*bundle,"main");
    const dage::WorkflowIR& ir=workflow->ir();
    CHECK(ir.entry()=="a"&&ir.nodes().size()==2&&ir.digest().find("sha256:")==0);
    const dage::NodeIR* node=ir.find_node("a");
    CHECK(node&&node->type()==dage::NodeType::Tool);
    CHECK(node->effect_kind()==dage::EffectKind::ExternalWrite);
    CHECK(node->replay_policy()==dage::ReplayPolicy::Idempotent);
    CHECK(node->idempotency_key()=="${run.id}:${node.id}");
    CHECK(!dage::normalize_resource_path("../escape"));
    CHECK(!dage::normalize_resource_path("a\\b"));
}
#ifdef DAGE_HAS_BUNDLE_TOOLS
static void bundle_tool_provider_tests(){
    const std::string root=std::string(DAGE_SOURCE_DIR)+"/examples/bundles/basic";
    dage::tools::DirectoryResourceProvider directory(root);
    auto listed=directory.list_resources();CHECK(listed&&listed.value().size()==2);
    CHECK(directory.read_resource("manifest.json")&&!directory.read_resource("../manifest.json"));
    dage::tools::ResourceLimits tight;tight.max_resources=1;
    dage::tools::DirectoryResourceProvider limited(root,tight);
    CHECK(!limited.list_resources()&&limited.list_resources().error().code=="RESOURCE_COUNT_LIMIT");

    const std::string manifest=file_text(root+"/manifest.json");
    const std::string workflow=file_text(root+"/workflows/main.json");
    dage::ResourceBytes bytes=test_zip({{"manifest.json",manifest},{"workflows/main.json",workflow}});
    dage::tools::ZipResourceProvider zip(bytes);
    auto zip_list=zip.list_resources();CHECK(zip_list&&zip_list.value().size()==2);
    dage::Engine engine;std::unique_ptr<dage::Bundle> bundle=engine.load_bundle(zip);
    CHECK(bundle->id()=="org.dage.examples.basic");
    dage::tools::ZipLimits tiny;tiny.max_total_bytes=8;
    bool size_rejected=false;try{dage::tools::ZipResourceProvider rejected(bytes,tiny);}
    catch(const std::exception& ex){size_rejected=std::string(ex.what()).find("ZIP_SIZE_LIMIT")!=std::string::npos;}
    CHECK(size_rejected);
    bool traversal_rejected=false;try{dage::tools::ZipResourceProvider rejected(test_zip({{"../escape", "x"}}));}
    catch(const std::exception& ex){traversal_rejected=std::string(ex.what()).find("ZIP_INVALID_PATH")!=std::string::npos;}
    CHECK(traversal_rejected);
    bool duplicate_rejected=false;try{dage::tools::ZipResourceProvider rejected(
        test_zip({{"manifest.json","a"},{"manifest.json","b"}}));}
    catch(const std::exception& ex){duplicate_rejected=std::string(ex.what()).find("ZIP_DUPLICATE_ENTRY")!=std::string::npos;}
    CHECK(duplicate_rejected);
    dage::ResourceBytes corrupt=bytes;
    const std::string needle="manifest.json";auto position=std::search(corrupt.begin(),corrupt.end(),needle.begin(),needle.end());
    CHECK(position!=corrupt.end());const std::size_t data=static_cast<std::size_t>(position-corrupt.begin())+needle.size();
    corrupt[data]^=0x01;
    dage::tools::ZipResourceProvider corrupt_zip(std::move(corrupt));
    CHECK(!corrupt_zip.read_resource("manifest.json"));

    const std::filesystem::path repository_root=std::filesystem::temp_directory_path()/
        ("dage-repository-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path directory_version=repository_root/"org.dage.examples.basic"/"1.0.0";
    std::filesystem::create_directories(directory_version/"workflows");
    {std::ofstream output(directory_version/"manifest.json",std::ios::binary);output<<manifest;}
    {std::ofstream output(directory_version/"workflows"/"main.json",std::ios::binary);output<<workflow;}
    dage::tools::DirectoryBundleRepository directory_repository(repository_root);
    auto directory_versions=directory_repository.available_versions("org.dage.examples.basic",true);
    CHECK(directory_versions&&directory_versions.value().size()==1&&directory_versions.value()[0]=="1.0.0");
    CHECK(directory_repository.get("org.dage.examples.basic","1.0.0",true));
    const std::filesystem::path zip_root=repository_root/"zip";
    std::filesystem::create_directories(zip_root/"org.dage.examples.basic");
    {std::ofstream output(zip_root/"org.dage.examples.basic"/"1.0.0.zip",std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    dage::tools::ZipBundleRepository zip_repository(zip_root);
    auto zip_versions=zip_repository.available_versions("org.dage.examples.basic",true);
    CHECK(zip_versions&&zip_versions.value().size()==1&&zip_repository.get("org.dage.examples.basic","1.0.0",true));
    CHECK(!zip_repository.get("../escape","1.0.0",true));
    std::filesystem::remove_all(repository_root);
}
static void bundle_golden_vector_tests(){
    const std::string base=std::string(DAGE_SOURCE_DIR)+"/testdata/golden/";
    const dage::Value vector=dage::Value::parse(file_text(base+"bundle_v1.vector.json"));
    dage::tools::DirectoryResourceProvider provider(base+"bundle_v1");
    dage::Engine engine;std::unique_ptr<dage::Bundle> bundle=engine.load_bundle(provider);
    CHECK(bundle->canonical_manifest()==vector.get("canonical_manifest").as_string());
    CHECK(bundle->canonical_unsigned_manifest()==vector.get("canonical_unsigned_manifest").as_string());
    CHECK(bundle->signing_payload()==vector.get("signing_payload").as_string());
    CHECK(bundle->digest()==vector.get("bundle_digest").as_string());
    CHECK(bundle->signatures().size()==1&&
          bundle->signatures()[0].signature_base64==vector.get("signature_base64").as_string());

    dage::BundleResolverOptions development;
    auto unlocked=dage::BundleResolver(development).resolve(provider);CHECK(unlocked);
    dage::PublicKey public_key;public_key.bytes=hex_bytes(vector.get("public_key_hex").as_string());
    dage::Keyring keys;keys.add("golden-ed25519",public_key);dage::AllowKnownKeysTrustPolicy trust;
    dage::BundleResolverOptions verified;verified.mode=dage::BundleLoadMode::Verified;
    verified.lock_json=unlocked.value()->lock_json();verified.keys=&keys;verified.trust=&trust;
    CHECK(dage::BundleResolver(verified).resolve(provider));

    dage::MemoryResourceProvider reordered;
    reordered.add_text("workflows/main.json",file_text(base+"bundle_v1/workflows/main.json"));
    reordered.add_text("assets/prompt.txt",file_text(base+"bundle_v1/assets/prompt.txt"));
    reordered.add_text("manifest.json",file_text(base+"bundle_v1/manifest.json"));
    std::unique_ptr<dage::Bundle> reordered_bundle=engine.load_bundle(reordered);
    CHECK(reordered_bundle->digest()==bundle->digest()&&
          reordered_bundle->signing_payload()==bundle->signing_payload());

    reordered.add_text("assets/prompt.txt","DAGE golden payload tampered\n");
    std::unique_ptr<dage::Bundle> tampered=engine.load_bundle(reordered);
    CHECK(tampered->digest()!=bundle->digest());
    CHECK(!dage::BundleResolver(verified).resolve(reordered));
}
static void bundle_store_tests(){
    const std::string base=std::string(DAGE_SOURCE_DIR)+"/testdata/golden/";
    const dage::Value vector=dage::Value::parse(file_text(base+"bundle_v1.vector.json"));
    dage::tools::DirectoryResourceProvider provider(base+"bundle_v1");
    dage::Engine engine;
    std::unique_ptr<dage::Bundle> bundle=engine.load_bundle(provider);
    const std::filesystem::path temporary=std::filesystem::temp_directory_path()/
        ("dage-bundle-store-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try{
        dage::tools::BundleCache cache(temporary/"cache");
        auto installed=cache.install(provider);
        CHECK(installed&&installed.value()==bundle->digest());
        CHECK(cache.install(provider)&&cache.open(bundle->digest()));
        auto cached=cache.open(bundle->digest());CHECK(cached);
        CHECK(engine.load_bundle(*cached.value())->digest()==bundle->digest());
        CHECK(!cache.open("sha256:BAD"));

        dage::MemoryResourceProvider changed;
        changed.add_text("manifest.json",file_text(base+"bundle_v1/manifest.json"));
        changed.add_text("workflows/main.json",file_text(base+"bundle_v1/workflows/main.json"));
        changed.add_text("assets/prompt.txt","different immutable cache payload\n");
        const std::string changed_digest=engine.load_bundle(changed)->digest();
        CHECK(cache.install(changed).value()==changed_digest);
        {std::ofstream unknown(cache.root()/"sha256"/"README",std::ios::binary);unknown<<"not a cache entry";}

        auto dry=cache.garbage_collect({bundle->digest()});CHECK(dry);
        CHECK(dry.value().candidates==std::vector<std::string>{changed_digest});
        CHECK(dry.value().removed.empty()&&cache.open(changed_digest));
        dage::tools::CacheGcOptions collect;collect.dry_run=false;
        auto removed=cache.garbage_collect({bundle->digest()},collect);CHECK(removed);
        CHECK(removed.value().removed==std::vector<std::string>{changed_digest});
        CHECK(removed.value().reclaimed_bytes>0&&!cache.open(changed_digest));
        CHECK(std::filesystem::exists(cache.root()/"sha256"/"README"));
        CHECK(!cache.garbage_collect({"invalid"}));

        auto unlocked=dage::BundleResolver(dage::BundleResolverOptions{}).resolve(provider);CHECK(unlocked);
        const std::filesystem::path lock_path=temporary/"state"/"dage.lock";
        dage::tools::AtomicLockfileOptions lock_options;lock_options.create_parent=true;
        CHECK(dage::tools::write_lockfile_atomic(lock_path,unlocked.value()->lock_json(),lock_options));
        CHECK(file_text(lock_path.string())==unlocked.value()->lock_json());
        CHECK(!dage::tools::write_lockfile_atomic(lock_path,"{}",lock_options));
        CHECK(file_text(lock_path.string())==unlocked.value()->lock_json());
        lock_options.max_bytes=1;
        CHECK(!dage::tools::write_lockfile_atomic(lock_path,unlocked.value()->lock_json(),lock_options));

        dage::PublicKey public_key;public_key.bytes=hex_bytes(vector.get("public_key_hex").as_string());
        dage::KeyRecord key_record;key_record.key_id="golden-ed25519";key_record.key=public_key;
        key_record.namespace_scopes={"org.dage"};
        dage::Keyring keys;keys.add(key_record);
        dage::SignatureContext denied_context{
            "org.other.bundle","1.0.0","sha256:x","golden-ed25519","ed25519","2026-07-27T00:00:00Z"};
        CHECK(!keys.find_key("golden-ed25519",denied_context));
        lock_options.max_bytes=16*1024*1024;
        const std::filesystem::path keyring_path=temporary/"state"/"keys.json";
        CHECK(dage::tools::write_keyring_atomic(keyring_path,keys,lock_options));
        auto restored_keys=dage::Keyring::from_snapshot_json(file_text(keyring_path.string()));
        CHECK(restored_keys);
        dage::AllowKnownKeysTrustPolicy trust;dage::MemoryBundleRepository repository("cache");
        auto production=dage::tools::verified_production_profile(
            unlocked.value()->lock_json(),repository,restored_keys.value(),trust);
        CHECK(production&&production.value().mode==dage::BundleLoadMode::Verified);
        CHECK(dage::BundleResolver(production.value()).resolve(provider));
        auto offline=dage::tools::verified_production_profile(
            unlocked.value()->lock_json(),repository,restored_keys.value(),trust,{},true);
        CHECK(offline&&offline.value().mode==dage::BundleLoadMode::Offline);
        CHECK(dage::BundleResolver(offline.value()).resolve(provider));
        restored_keys.value().revoke("golden-ed25519");
        CHECK(dage::tools::write_keyring_atomic(keyring_path,restored_keys.value(),lock_options));
        auto revoked_keys=dage::Keyring::from_snapshot_json(file_text(keyring_path.string()));CHECK(revoked_keys);
        dage::SignatureContext golden_context{
            "org.dage.golden","1.2.3",bundle->digest(),"golden-ed25519","ed25519","2026-07-27T00:00:00Z"};
        CHECK(!revoked_keys.value().find_key("golden-ed25519",golden_context));
        dage::SignaturePolicy disabled;disabled.kind=dage::SignaturePolicyKind::Disabled;
        CHECK(!dage::tools::verified_production_profile(
            unlocked.value()->lock_json(),repository,keys,trust,disabled));
        CHECK(!dage::tools::verified_production_profile("{}",repository,keys,trust));
    }catch(...){std::error_code ignored;std::filesystem::remove_all(temporary,ignored);throw;}
    std::filesystem::remove_all(temporary);
}
#endif
static std::shared_ptr<dage::MemoryResourceProvider> bundle_provider(
    const std::string& id,const std::string& version,const std::string& dependencies,
    const std::string& signatures="[]",const std::string& config="{}"){
    auto provider=std::make_shared<dage::MemoryResourceProvider>();
    provider->add_text("manifest.json",
        "{\"format\":\"dage-bundle\",\"format_version\":\"0.2.0\",\"id\":\""+id+
        "\",\"version\":\""+version+"\",\"workflows\":{\"main\":\"workflows/main.json\"},"
        "\"dependencies\":"+dependencies+",\"config\":"+config+",\"signatures\":"+signatures+"}");
    provider->add_text("workflows/main.json",wf("{\"z\":{\"type\":\"end\"}}","z"));
    return provider;
}
static std::string base64(const unsigned char* data,std::size_t size){
    std::string output(4*((size+2)/3),'\0');
    const int written=EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&output[0]),data,static_cast<int>(size));
    output.resize(static_cast<std::size_t>(written));return output;
}
static std::pair<dage::PublicKey,std::string> sign_message(const std::string& message){
    EVP_PKEY_CTX* key_context=EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,nullptr);CHECK(key_context);
    CHECK(EVP_PKEY_keygen_init(key_context)==1);EVP_PKEY* key=nullptr;CHECK(EVP_PKEY_keygen(key_context,&key)==1);
    EVP_PKEY_CTX_free(key_context);
    dage::PublicKey public_key;public_key.bytes.resize(32);std::size_t public_size=public_key.bytes.size();
    CHECK(EVP_PKEY_get_raw_public_key(key,public_key.bytes.data(),&public_size)==1);
    EVP_MD_CTX* sign_context=EVP_MD_CTX_new();CHECK(sign_context&&EVP_DigestSignInit(sign_context,nullptr,nullptr,nullptr,key)==1);
    std::size_t signature_size=0;CHECK(EVP_DigestSign(sign_context,nullptr,&signature_size,
        reinterpret_cast<const unsigned char*>(message.data()),message.size())==1);
    std::vector<unsigned char> signature(signature_size);CHECK(EVP_DigestSign(sign_context,signature.data(),&signature_size,
        reinterpret_cast<const unsigned char*>(message.data()),message.size())==1);
    EVP_MD_CTX_free(sign_context);EVP_PKEY_free(key);signature.resize(signature_size);
    return {public_key,base64(signature.data(),signature.size())};
}
class RejectTrust final:public dage::TrustPolicy{
public:dage::Result<bool> trust(const dage::SignatureContext&)const override{return dage::Result<bool>::success(false);}
};
class OnlineOnlyRepository final:public dage::BundleRepository{
public:
    explicit OnlineOnlyRepository(std::shared_ptr<const dage::ResourceProvider> provider):provider_(std::move(provider)){}
    dage::Result<std::vector<std::string>> available_versions(const std::string&,bool offline)const override{
        if(offline)return dage::Result<std::vector<std::string>>::failure(error());
        return dage::Result<std::vector<std::string>>::success({"1.6.2"});
    }
    dage::Result<std::shared_ptr<const dage::ResourceProvider>> get(const std::string&,const std::string&,bool offline)const override{
        if(offline)return dage::Result<std::shared_ptr<const dage::ResourceProvider>>::failure(error());
        return dage::Result<std::shared_ptr<const dage::ResourceProvider>>::success(provider_);
    }
    std::string source()const override{return "online-only";}
private:
    static dage::Error error(){dage::Error e;e.category="resolver";e.code="OFFLINE_CACHE_MISS";e.message="not cached";return e;}
    std::shared_ptr<const dage::ResourceProvider> provider_;
};
static void resolver_tests(){
    dage::Engine engine;
    auto dep14=bundle_provider("com.example.dep","1.4.0","{}");
    auto dep16=bundle_provider("com.example.dep","1.6.2","{}","[]",
        "{\"model\":{\"temperature\":0.2,\"retries\":2},\"tags\":[\"dep\"],\"remove_me\":true}");
    auto root=bundle_provider("com.example.root","1.0.0","{\"com.example.dep\":\"^1.4.0\"}","[]",
        "{\"model\":{\"temperature\":0.8},\"tags\":[\"root\"],\"remove_me\":null}");
    dage::MemoryBundleRepository repository("registry.example");
    repository.add("com.example.dep","1.4.0",dep14);repository.add("com.example.dep","1.6.2",dep16);
    dage::BundleResolverOptions options;options.repository=&repository;
    dage::BundleResolver resolver(options);auto graph_result=resolver.resolve(*root);CHECK(graph_result);
    std::unique_ptr<dage::ResolvedBundleGraph> graph=std::move(graph_result.value());
    CHECK(graph->find("com.example.dep")&&graph->find("com.example.dep")->version()=="1.6.2");
    CHECK(graph->topological_order().size()==2&&graph->topological_order()[0]=="com.example.dep");
    CHECK(graph->lock_json().find("\"format\" : \"dage-lock\"")!=std::string::npos);
    dage::Result<dage::Value> merged_config=graph->merged_config();CHECK(merged_config);
    CHECK(merged_config.value().get("model").get("temperature").as_double()>0.79&&
          merged_config.value().get("model").get("temperature").as_double()<0.81);
    CHECK(merged_config.value().get("model").get("retries").as_integer()==2);
    CHECK(merged_config.value().get("tags").size()==1&&!merged_config.value().has("remove_me"));

    dage::Value config_base=dage::Value::parse(
        "{\"nested\":{\"a\":1},\"array\":[1],\"nullable\":\"value\",\"typed\":{\"x\":1}}");
    dage::Value config_overlay=dage::Value::parse(
        "{\"nested\":{\"b\":2},\"array\":[2],\"nullable\":null,\"typed\":\"scalar\"}");
    dage::Result<dage::Value> config_default=dage::deep_merge_config(config_base,config_overlay);
    CHECK(config_default&&config_default.value().get("nested").get("a").as_integer()==1);
    CHECK(config_default.value().get("nested").get("b").as_integer()==2);
    CHECK(config_default.value().get("array").size()==1&&!config_default.value().has("nullable"));
    CHECK(config_default.value().get("typed").as_string()=="scalar");
    dage::ConfigMergeOptions merge_options;merge_options.arrays=dage::ConfigArrayMerge::Append;
    merge_options.nulls=dage::ConfigNullMerge::Preserve;
    dage::Result<dage::Value> config_explicit=dage::deep_merge_config(config_base,config_overlay,merge_options);
    CHECK(config_explicit&&config_explicit.value().get("array").size()==2&&
          config_explicit.value().has("nullable")&&config_explicit.value().get("nullable").is_null());
    CHECK(!dage::deep_merge_config(dage::Value::object(),
        dage::Value::parse("{\"embedded\":{\"format\":\"dage-workflow\",\"nodes\":{}}}")));
    dage::ConfigMergeOptions shallow;shallow.max_depth=1;
    dage::Result<dage::Value> too_deep=dage::deep_merge_config(
        dage::Value::object(),dage::Value::parse("{\"a\":{\"b\":{\"c\":1}}}"),shallow);
    CHECK(!too_deep&&too_deep.error().code=="CONFIG_MAX_DEPTH");

    options.mode=dage::BundleLoadMode::Frozen;options.lock_json=graph->lock_json();
    auto frozen=dage::BundleResolver(options).resolve(*root);CHECK(frozen&&frozen.value()->find("com.example.dep")->version()=="1.6.2");
    std::string corrupt_lock=graph->lock_json();const std::size_t digest_pos=corrupt_lock.find("sha256:");
    CHECK(digest_pos!=std::string::npos);corrupt_lock[digest_pos+7]=corrupt_lock[digest_pos+7]=='0'?'1':'0';
    options.lock_json=corrupt_lock;CHECK(!dage::BundleResolver(options).resolve(*root));options.lock_json=graph->lock_json();
    options.mode=dage::BundleLoadMode::Offline;
    options.signature_policy.kind=dage::SignaturePolicyKind::Disabled;
    auto offline=dage::BundleResolver(options).resolve(*root);CHECK(offline&&offline.value()->find("com.example.dep")!=nullptr);
    OnlineOnlyRepository online_only(dep16);options.repository=&online_only;
    CHECK(!dage::BundleResolver(options).resolve(*root));
    options.repository=&repository;
    dage::BundleResolverOptions bad;bad.mode=dage::BundleLoadMode::Frozen;bad.repository=&repository;
    CHECK(!dage::BundleResolver(bad).resolve(*root));

    auto unsigned_root=bundle_provider("com.example.signed","2.0.0","{}");
    std::unique_ptr<dage::Bundle> unsigned_bundle=engine.load_bundle(*unsigned_root);
    const auto signed_value=sign_message(unsigned_bundle->signing_payload());
    const std::string signatures="[{\"algorithm\":\"ed25519\",\"key_id\":\"release-a\",\"signature\":\""+
        signed_value.second+"\",\"signed_at\":\"2026-07-27T00:00:00Z\"}]";
    auto signed_root=bundle_provider("com.example.signed","2.0.0","{}",signatures);
    dage::BundleResolverOptions dev;auto signed_result=dage::BundleResolver(dev).resolve(*signed_root);CHECK(signed_result);
    std::unique_ptr<dage::ResolvedBundleGraph> signed_graph=std::move(signed_result.value());
    dage::Keyring keys;keys.add("release-a",signed_value.first);dage::AllowKnownKeysTrustPolicy trust;
    dage::BundleResolverOptions verified;verified.mode=dage::BundleLoadMode::Verified;verified.lock_json=signed_graph->lock_json();
    verified.keys=&keys;verified.trust=&trust;
    auto verified_result=dage::BundleResolver(verified).resolve(*signed_root);CHECK(verified_result&&verified_result.value()->root().id()=="com.example.signed");
    dage::BundleResolverOptions missing_trust=verified;missing_trust.trust=nullptr;
    CHECK(!dage::BundleResolver(missing_trust).resolve(*signed_root));
    bool duplicate_key_rejected=false;
    try{keys.add("release-a",signed_value.first);}catch(const std::invalid_argument&){duplicate_key_rejected=true;}
    CHECK(duplicate_key_rejected);

    const std::string one_signature="{\"algorithm\":\"ed25519\",\"key_id\":\"release-a\",\"signature\":\""+
        signed_value.second+"\",\"signed_at\":\"2026-07-27T00:00:00Z\"}";
    auto duplicate_root=bundle_provider("com.example.signed","2.0.0","{}",
        "["+one_signature+","+one_signature+"]");
    auto duplicate_graph=dage::BundleResolver(dev).resolve(*duplicate_root);CHECK(duplicate_graph);
    verified.lock_json=duplicate_graph.value()->lock_json();
    verified.signature_policy.kind=dage::SignaturePolicyKind::Threshold;
    verified.signature_policy.threshold=2;
    CHECK(!dage::BundleResolver(verified).resolve(*duplicate_root));
    const std::string alias_signature="{\"algorithm\":\"ed25519\",\"key_id\":\"release-alias\",\"signature\":\""+
        signed_value.second+"\",\"signed_at\":\"2026-07-27T00:00:00Z\"}";
    auto alias_root=bundle_provider("com.example.signed","2.0.0","{}",
        "["+one_signature+","+alias_signature+"]");
    auto alias_graph=dage::BundleResolver(dev).resolve(*alias_root);CHECK(alias_graph);
    keys.add("release-alias",signed_value.first);
    verified.lock_json=alias_graph.value()->lock_json();
    CHECK(!dage::BundleResolver(verified).resolve(*alias_root));

    const auto second_signed_value=sign_message(unsigned_bundle->signing_payload());
    const std::string second_signature="{\"algorithm\":\"ed25519\",\"key_id\":\"release-b\",\"signature\":\""+
        second_signed_value.second+"\",\"signed_at\":\"2026-07-27T00:00:00Z\"}";
    auto threshold_root=bundle_provider("com.example.signed","2.0.0","{}",
        "["+one_signature+","+second_signature+"]");
    auto threshold_graph=dage::BundleResolver(dev).resolve(*threshold_root);CHECK(threshold_graph);
    keys.add("release-b",second_signed_value.first);
    verified.lock_json=threshold_graph.value()->lock_json();
    CHECK(dage::BundleResolver(verified).resolve(*threshold_root));
    verified.signature_policy.kind=dage::SignaturePolicyKind::All;
    CHECK(dage::BundleResolver(verified).resolve(*threshold_root));

    verified.signature_policy={};
    verified.lock_json=signed_graph->lock_json();
    auto tampered=bundle_provider("com.example.signed","2.0.0","{}",signatures);
    tampered->add_text("workflows/main.json",wf("{\"z\":{\"type\":\"end\",\"input\":{\"tampered\":true}}}","z"));
    CHECK(!dage::BundleResolver(verified).resolve(*tampered));
    RejectTrust reject;verified.trust=&reject;CHECK(!dage::BundleResolver(verified).resolve(*signed_root));
    keys.revoke("release-a");verified.trust=&trust;CHECK(!dage::BundleResolver(verified).resolve(*signed_root));

    auto cycle_a=bundle_provider("cycle.a","1.0.0","{\"cycle.b\":\"1.0.0\"}");
    auto cycle_b=bundle_provider("cycle.b","1.0.0","{\"cycle.a\":\"1.0.0\"}");
    dage::MemoryBundleRepository cycles;cycles.add("cycle.a","1.0.0",cycle_a);cycles.add("cycle.b","1.0.0",cycle_b);
    dage::BundleResolverOptions cycle_options;cycle_options.repository=&cycles;
    CHECK(!dage::BundleResolver(cycle_options).resolve(*cycle_a));

    auto root_v2=bundle_provider("com.example.rollback","2.0.0","{}");
    auto root_v1=bundle_provider("com.example.rollback","1.5.0","{}");
    auto current=dage::BundleResolver(dage::BundleResolverOptions{}).resolve(*root_v2);
    auto target=dage::BundleResolver(dage::BundleResolverOptions{}).resolve(*root_v1);
    CHECK(current&&target);
    dage::Result<dage::BundleRollbackPlan> rollback=current.value()->plan_rollback_to(*target.value());
    CHECK(rollback&&rollback.value().root_bundle_id=="com.example.rollback");
    CHECK(rollback.value().from_version=="2.0.0"&&rollback.value().to_version=="1.5.0");
    CHECK(rollback.value().changes.size()==1&&!rollback.value().target_lock_json.empty());
    CHECK(!rollback.value().target_verified);
    CHECK(!target.value()->plan_rollback_to(*current.value()));
    auto other_root=bundle_provider("com.example.other","1.0.0","{}");
    auto other=dage::BundleResolver(dage::BundleResolverOptions{}).resolve(*other_root);
    CHECK(other&&!current.value()->plan_rollback_to(*other.value()));
}
static std::string wf(const std::string& nodes,const std::string& entry){
    return "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\""+entry+"\",\"nodes\":"+nodes+"}";
}
static void value_tests(){
    dage::Value a=dage::Value::parse("{\"x\":[1,true,\"z\"]}");dage::Value b=a;
    CHECK(b.get("x").size()==3);dage::Value c(std::move(b));CHECK(c.has("x"));
    dage::Value o=dage::Value::object();o.set("ok",dage::Value(true));CHECK(o.get("ok").as_bool());
}
static void validation_tests(){
    dage::Engine e;
    CHECK(has_code(e.validate("{"),"INVALID_JSON"));
    CHECK(has_code(e.validate("{\"format\":{},\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"end\"}}}"),
                   "INVALID_FIELD_TYPE"));
    CHECK(has_code(e.validate("{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"nodes\":{\"a\":{\"type\":\"end\"}}}"),"MISSING_ENTRY"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":\"missing\"}}")),"UNKNOWN_NODE_REFERENCE"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":[{\"to\":\"b\",\"otherwise\":true},{\"to\":\"b\",\"otherwise\":true}]},\"b\":{\"type\":\"end\"}}")),"MULTIPLE_OTHERWISE"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":[\"b\",{\"to\":\"c\",\"when\":\"${output.x}\"}]},\"b\":{\"type\":\"end\"},\"c\":{\"type\":\"end\"}}")),"SHADOWED_BRANCH"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":\"b\"},\"b\":{\"type\":\"noop\",\"next\":\"a\"}}")),"UNDECLARED_CYCLE"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"end\",\"next\":\"a\"}}")),"END_HAS_SUCCESSOR"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"x\",\"timeout_ms\":0}}")),"INVALID_TIMEOUT"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"x\"}}")),"MISSING_EFFECTS"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"x\",\"effects\":{\"kind\":\"external_write\",\"replay\":\"safe\"}}}")),"INVALID_EFFECT_REPLAY"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"x\",\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\"}}}")),"MISSING_IDEMPOTENCY_GUARANTEE"));
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\"],\"join\":\"c\"},\"b\":{\"type\":\"noop\"},\"c\":{\"type\":\"join\",\"executor\":\"x\"}}")),"UNREACHABLE_NODE")==false);
    dage::WorkflowResourceLimits limits;
    limits.max_workflow_bytes=96;limits.max_json_depth=4;limits.max_nodes=1;limits.max_edges=1;
    e.set_workflow_resource_limits(limits);
    CHECK(has_code(e.validate(std::string(97,' ')),"WORKFLOW_BYTES_LIMIT"));
    CHECK(has_code(e.validate("[[[[[]]]]]"),"WORKFLOW_DEPTH_LIMIT"));
    limits.max_workflow_bytes=4096;limits.max_json_depth=64;e.set_workflow_resource_limits(limits);
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":\"b\"},\"b\":{\"type\":\"end\"}}")),
                   "WORKFLOW_NODES_LIMIT"));
    limits.max_nodes=4;e.set_workflow_resource_limits(limits);
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"noop\",\"next\":[\"b\",\"c\"]},\"b\":{\"type\":\"end\"},\"c\":{\"type\":\"end\"}}")),
                   "WORKFLOW_EDGES_LIMIT"));
    limits.max_edges=8;limits.max_expression_bytes=5;e.set_workflow_resource_limits(limits);
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"end\",\"input\":\"${abcdef}\"}}")),
                   "WORKFLOW_EXPRESSION_BYTES_LIMIT"));
    limits.max_expression_bytes=1024;limits.max_literal_bytes=16;e.set_workflow_resource_limits(limits);
    CHECK(has_code(e.validate(wf("{\"a\":{\"type\":\"end\",\"input\":\"a-long-literal-value\"}}")),
                   "WORKFLOW_LITERAL_BYTES_LIMIT"));
    dage::Engine ir_limited;dage::WorkflowResourceLimits ir_limits;
    ir_limits.max_compiled_ir_bytes=128;ir_limited.set_workflow_resource_limits(ir_limits);
    bool ir_rejected=false;try{ir_limited.load(wf("{\"a\":{\"type\":\"end\"}}"));}
    catch(const std::runtime_error& ex){
        const std::string message=ex.what();
        ir_rejected=message.find("WORKFLOW_IR_BYTES_LIMIT: observed=")==0&&
                    message.find(" limit=128")!=std::string::npos;
    }CHECK(ir_rejected);
    bool rejected=false;try{limits.max_nodes=0;e.set_workflow_resource_limits(limits);}
    catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
}
static void all_node_format_tests(){
    const char* types[]={"llm","tool","transform","join","human","custom"};
    dage::Engine e;
    for(std::size_t i=0;i<sizeof(types)/sizeof(types[0]);++i){
        const std::string effects=(std::string(types[i])=="tool"||std::string(types[i])=="custom")
            ?",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}":"";
        std::string text=wf("{\"a\":{\"type\":\""+std::string(types[i])+"\",\"executor\":\"echo\""+effects+",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}");
        CHECK(!has_code(e.validate(text),"UNKNOWN_NODE_TYPE"));CHECK(e.load(text).get()!=0);
    }
    CHECK(e.load(wf("{\"a\":{\"type\":\"subflow\",\"config\":{\"workflow\":\"child\"},\"next\":\"z\"},\"z\":{\"type\":\"end\"}}")).get()!=0);
    const char* internal[]={"start","condition","noop"};
    for(std::size_t i=0;i<3;++i)CHECK(e.load(wf("{\"a\":{\"type\":\""+std::string(internal[i])+"\",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}")).get()!=0);
    CHECK(e.load(wf("{\"a\":{\"type\":\"end\"}}")).get()!=0);
    CHECK(e.load(wf("{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\"],\"join\":\"j\"},\"b\":{\"type\":\"noop\"},\"j\":{\"type\":\"join\",\"executor\":\"echo\"}}")).get()!=0);
    e.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value&i){return dage::ExecutionResult::ok(i);});
    const std::string chain=wf(
        "{\"a\":{\"type\":\"start\",\"next\":\"condition_node\"},"
        "\"condition_node\":{\"type\":\"condition\",\"next\":\"llm_node\"},"
        "\"llm_node\":{\"type\":\"llm\",\"executor\":\"echo\",\"next\":\"tool_node\"},"
        "\"tool_node\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},\"next\":\"transform_node\"},"
        "\"transform_node\":{\"type\":\"transform\",\"executor\":\"echo\",\"next\":\"join_node\"},"
        "\"join_node\":{\"type\":\"join\",\"executor\":\"echo\",\"next\":\"subflow_node\"},"
        "\"subflow_node\":{\"type\":\"subflow\",\"config\":{\"workflow\":\"child\"},\"next\":\"human_node\"},"
        "\"human_node\":{\"type\":\"human\",\"executor\":\"echo\",\"next\":\"custom_node\"},"
        "\"custom_node\":{\"type\":\"custom\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"noop_node\"},"
        "\"noop_node\":{\"type\":\"noop\",\"next\":\"end_node\"},"
        "\"end_node\":{\"type\":\"end\",\"input\":{\"all\":true}}}");
    e.register_workflow("child",wf("{\"a\":{\"type\":\"end\",\"input\":{\"child\":true}}}"));
    std::unique_ptr<dage::Workflow> executable=e.load(chain);
    CHECK(e.create_run(*executable)->execute(dage::Value::object()).output.get("all").as_bool());
}
static void runtime_tests(){
    dage::Engine e;e.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value&i){return dage::ExecutionResult::ok(i);});
    std::unique_ptr<dage::Workflow>w=e.load(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"input\":{\"v\":\"${workflow.input.v}\"},\"next\":[{\"to\":\"yes\",\"when\":\"${output.v >= 2}\"},{\"to\":\"no\",\"otherwise\":true}]},\"yes\":{\"type\":\"end\",\"input\":{\"answer\":\"yes\"}},\"no\":{\"type\":\"end\",\"input\":{\"answer\":\"no\"}}}"));
    std::unique_ptr<dage::Run>r=e.create_run(*w);CHECK(r->execute(dage::Value::parse("{\"v\":3}")).output.get("answer").as_string()=="yes");
    std::unique_ptr<dage::Run>r2=e.create_run(*w);CHECK(r2->execute(dage::Value::parse("{\"v\":1}")).output.get("answer").as_string()=="no");
    std::unique_ptr<dage::Workflow> funcs=e.load(wf("{\"a\":{\"type\":\"condition\",\"next\":[{\"to\":\"yes\",\"when\":\"${length(workflow.input.words) == 1 and contains(workflow.input.words, 'x') and starts_with(workflow.input.name, 'DA') and ends_with(workflow.input.name, 'GE')}\"},{\"to\":\"no\",\"otherwise\":true}]},\"yes\":{\"type\":\"end\",\"input\":{\"ok\":true}},\"no\":{\"type\":\"end\",\"input\":{\"ok\":false}}}"));
    CHECK(e.create_run(*funcs)->execute(dage::Value::parse("{\"words\":[\"x\"],\"name\":\"DAGE\"}")).output.get("ok").as_bool());
}
static void error_retry_policy_tests(){
    dage::Engine e;int tries=0;e.register_executor("flaky",[&](const dage::ExecutionContext&,const dage::Value&){
        ++tries;return tries<2?dage::ExecutionResult::fail("temporary_error","TEMP","retry",true):dage::ExecutionResult::ok(dage::Value::object());});
    std::unique_ptr<dage::Workflow>w=e.load(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"flaky\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},\"retry\":{\"max_attempts\":2},\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}"));
    CHECK(e.create_run(*w)->execute(dage::Value::object()).success);CHECK(tries==2);
    e.set_executor_policy([](const std::string&){return false;});
    dage::ExecutionResult denied=e.create_run(*w)->execute(dage::Value::object());CHECK(!denied.success&&denied.error.category=="permission_denied");
    dage::Engine missing;std::unique_ptr<dage::Workflow>mw=missing.load(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"none\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"}}}"));
    CHECK(missing.create_run(*mw)->execute(dage::Value::object()).error.category=="executor_not_found");
    dage::Engine recovery;recovery.register_executor("fail",[](const dage::ExecutionContext&,const dage::Value&){
        return dage::ExecutionResult::fail("tool_error","FAIL","expected");});
    std::unique_ptr<dage::Workflow>rw=recovery.load(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"fail\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},\"on_error\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"recovered\":true}}}"));
    CHECK(recovery.create_run(*rw)->execute(dage::Value::object()).output.get("recovered").as_bool());
}
static void effect_replay_tests(){
    dage::Engine engine;std::string observed_key,observed_mode;
    engine.register_executor("write",[&](const dage::ExecutionContext& context,const dage::Value& input){
        observed_key=context.idempotency_key;observed_mode=context.run_mode;
        if(context.effect_committer)context.effect_committer->commit();
        return dage::ExecutionResult::ok(input);
    });
    const std::string workflow_text=wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"write\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\","
        "\"idempotency_key\":\"${run.id}:${node.id}\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}");
    std::unique_ptr<dage::Workflow> workflow=engine.load(workflow_text);
    dage::RunOptions replay;replay.mode=dage::RunMode::Replay;
    std::unique_ptr<dage::Run> replay_run=engine.create_run(*workflow,replay);
    CHECK(replay_run->execute(dage::Value::object()).success);
    CHECK(observed_mode=="replay"&&observed_key.find(":a")!=std::string::npos);
    CHECK(replay_run->snapshot().get("nodes").get("a").as_string()=="succeeded");

    dage::RunOptions shadow;shadow.mode=dage::RunMode::Shadow;
    dage::ExecutionResult blocked=engine.create_run(*workflow,shadow)->execute(dage::Value::object());
    CHECK(!blocked.success&&blocked.error.code=="EFFECT_REPLAY_BLOCKED");

    CHECK(has_code(engine.validate(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"write\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"at_most_once\"},"
        "\"retry\":{\"max_attempts\":2}}}")),"UNSAFE_RETRY_POLICY"));

    const std::string irreversible_text=wf(
        "{\"a\":{\"type\":\"custom\",\"executor\":\"write\","
        "\"effects\":{\"kind\":\"irreversible\",\"replay\":\"manual\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\"}}");
    std::unique_ptr<dage::Workflow> irreversible=engine.load(irreversible_text);
    CHECK(!engine.create_run(*irreversible)->execute(dage::Value::object()).success);
    dage::RunOptions approved;approved.allow_irreversible=true;
    CHECK(engine.create_run(*irreversible,approved)->execute(dage::Value::object()).success);
}
static void reliability_recovery_tests(){
    auto store=std::make_shared<dage::MemoryStateStore>();dage::Engine engine;engine.set_state_store(store);
    int writes=0;engine.register_executor("write",[&](const dage::ExecutionContext& context,const dage::Value&){
        ++writes;if(context.effect_committer)context.effect_committer->commit();
        dage::Value output=dage::Value::object();output.set("writes",dage::Value(static_cast<std::int64_t>(writes)));
        return dage::ExecutionResult::ok(output);
    });
    std::unique_ptr<dage::Workflow> workflow=engine.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"write\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\","
        "\"idempotency_key\":\"${run.id}:${node.id}\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}"));
    bool injected=false;engine.set_failure_injector([&](const std::string& point,const std::string& node){
        if(!injected&&point=="after_state_save"&&node=="a"){injected=true;return true;}return false;
    });
    std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
    dage::ExecutionResult crashed=run->execute(dage::Value::object());CHECK(!crashed.success&&crashed.error.code=="INJECTED_CRASH");
    const std::string run_id=run->snapshot().get("run_id").as_string();dage::Result<std::string> persisted=store->get(run_id);CHECK(persisted);
    engine.set_failure_injector({});
    std::unique_ptr<dage::Run> restored=engine.restore_run(*workflow,persisted.value());
    bool ownership_conflict=false;try{engine.restore_run(*workflow,persisted.value());}
    catch(const std::exception&){ownership_conflict=true;}CHECK(ownership_conflict);
    CHECK(restored->execute(dage::Value::object()).success);CHECK(writes==1);
    CHECK(restored->selective_rerun("a").success);CHECK(writes==2);

    int attempts=0;dage::Engine retry_engine;retry_engine.register_executor("flaky",[&](const dage::ExecutionContext&,const dage::Value&){
        ++attempts;return dage::ExecutionResult::fail("temporary","RETRY","retry",true);
    });
    std::unique_ptr<dage::Workflow> retry_workflow=retry_engine.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"flaky\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},"
        "\"retry\":{\"max_attempts\":4,\"delay_ms\":1,\"backoff\":\"exponential\",\"jitter_ms\":1}}}"));
    dage::RunOptions limited;limited.retry_budget=1;
    dage::ExecutionResult exhausted=retry_engine.create_run(*retry_workflow,limited)->execute(dage::Value::object());
    CHECK(!exhausted.success&&exhausted.error.code=="RETRY_BUDGET_EXHAUSTED"&&attempts==2);

    dage::Engine fence_engine;fence_engine.register_executor("unsafe",[](const dage::ExecutionContext&,const dage::Value& input){
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> fence_workflow=fence_engine.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"unsafe\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\","
        "\"idempotency_key\":\"${run.id}:${node.id}\"}}}"));
    CHECK(fence_engine.create_run(*fence_workflow)->execute(dage::Value::object()).error.code=="EFFECT_COMMIT_NOT_CONFIRMED");

    auto transition_store=std::make_shared<dage::MemoryStateStore>();dage::Engine transition;
    transition.set_state_store(transition_store);int pure_calls=0;
    transition.register_executor("pure",[&](const dage::ExecutionContext&,const dage::Value& input){
        ++pure_calls;return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> transition_workflow=transition.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"pure\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
        "\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"done\":true}}}"));
    bool transition_crash=true;transition.set_failure_injector([&](const std::string& point,const std::string& node){
        if(transition_crash&&point=="after_state_save"&&node=="a"){transition_crash=false;return true;}return false;
    });
    std::unique_ptr<dage::Run> transition_run=transition.create_run(*transition_workflow);
    CHECK(transition_run->execute(dage::Value::object()).error.code=="INJECTED_CRASH");
    dage::Result<std::string> transition_cp=transition_store->get(
        transition_run->snapshot().get("run_id").as_string());CHECK(transition_cp);
    transition.set_failure_injector({});
    std::unique_ptr<dage::Run> transition_restored=transition.restore_run(*transition_workflow,transition_cp.value());
    CHECK(transition_restored->execute(dage::Value::object()).output.get("done").as_bool());
    CHECK(pure_calls==1);
}
static void state_store_and_async_tests(){
    JoiningThreads executor_threads;
    dage::MemoryStateStore store;
    CHECK(store.compare_exchange("cas-run",0,"first"));
    dage::Result<std::uint64_t> conflict=store.compare_exchange("cas-run",0,"stale");
    CHECK(!conflict&&conflict.error().code=="STATE_CONFLICT");
    dage::Result<dage::StateRecord> record=store.load("cas-run");
    CHECK(record&&record.value().version==1&&record.value().checkpoint=="first");

    dage::Engine engine;
    engine.register_async_executor("async",[&](const dage::ExecutionContext&,const dage::Value& input,
                                               const std::shared_ptr<dage::AsyncExecutorCompletion>& completion){
        executor_threads.launch([input,completion](){
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            completion->complete(dage::ExecutionResult::ok(input));
        });
    });
    std::unique_ptr<dage::Workflow> workflow=engine.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"async\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
        "\"input\":{\"async\":true},\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":\"${nodes.a.output}\"}}"));
    dage::RunOptions options;options.deadline_ms=100;
    std::unique_ptr<dage::Run> async_run=engine.create_run(*workflow,options);
    std::promise<dage::ExecutionResult> async_done;
    async_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){async_done.set_value(value);});
    dage::ExecutionResult result=async_done.get_future().get();
    CHECK(result.success&&result.output.get("async").as_bool());

    dage::ThreadPoolOptions pool_options;pool_options.worker_count=1;pool_options.queue_capacity=8;
    std::shared_ptr<dage::ThreadPoolScheduler> one_worker(new dage::ThreadPoolScheduler(pool_options));
    dage::Engine nonblocking(one_worker);
    std::promise<void> started;
    std::shared_ptr<dage::AsyncExecutorCompletion> held_completion;
    nonblocking.register_async_executor("held",[&](const dage::ExecutionContext&,const dage::Value&,
                                                    const std::shared_ptr<dage::AsyncExecutorCompletion>& completion){
        held_completion=completion;started.set_value();
    });
    std::unique_ptr<dage::Workflow> held_workflow=nonblocking.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"held\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"done\":true}}}"));
    std::unique_ptr<dage::Run> held_run=nonblocking.create_run(*held_workflow);
    std::promise<dage::ExecutionResult> held_done;
    held_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){held_done.set_value(value);});
    started.get_future().wait();
    std::future<void> sentinel=one_worker->schedule([](){});
    CHECK(sentinel.wait_for(std::chrono::milliseconds(100))==std::future_status::ready);
    CHECK(held_completion->complete(dage::ExecutionResult::ok(dage::Value::object())));
    CHECK(held_done.get_future().get().success);

    dage::Engine timed(one_worker);
    std::promise<void> timed_started;
    std::shared_ptr<dage::AsyncExecutorCompletion> late_completion;
    timed.register_async_executor("late",[&](const dage::ExecutionContext&,const dage::Value&,
                                             const std::shared_ptr<dage::AsyncExecutorCompletion>& completion){
        late_completion=completion;timed_started.set_value();
    });
    std::unique_ptr<dage::Workflow> timed_workflow=timed.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"late\",\"timeout_ms\":10,"
        "\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}}}"));
    std::unique_ptr<dage::Run> timed_run=timed.create_run(*timed_workflow);
    std::promise<dage::ExecutionResult> timed_done;
    timed_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){timed_done.set_value(value);});
    timed_started.get_future().wait();
    dage::ExecutionResult timed_result=timed_done.get_future().get();
    CHECK(!timed_result.success&&timed_result.error.code=="NODE_TIMEOUT");
    CHECK(late_completion->cancelled());
    CHECK(late_completion->complete(dage::ExecutionResult::ok(dage::Value::object())));

    dage::Engine retrying(one_worker);std::atomic<int> async_attempts(0);
    retrying.register_async_executor("flaky_async",[&](const dage::ExecutionContext& context,const dage::Value&,
                                                        const std::shared_ptr<dage::AsyncExecutorCompletion>& done){
        CHECK(context.attempt==static_cast<std::uint32_t>(++async_attempts));
        if(context.attempt==1)done->complete(dage::ExecutionResult::fail("temporary","RETRY_ME","retry",true));
        else done->complete(dage::ExecutionResult::ok(dage::Value::object()));
    });
    std::unique_ptr<dage::Workflow> retry_workflow=retrying.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"flaky_async\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
        "\"retry\":{\"max_attempts\":2,\"delay_ms\":2,\"backoff\":\"exponential\",\"jitter_ms\":1},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"retried\":true}}}"));
    std::unique_ptr<dage::Run> retry_run=retrying.create_run(*retry_workflow);
    std::promise<dage::ExecutionResult> retry_done;
    retry_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){retry_done.set_value(value);});
    dage::ExecutionResult retry_result=retry_done.get_future().get();
    CHECK(retry_result.success&&retry_result.output.get("retried").as_bool()&&async_attempts==2);

    dage::Engine async_effect(one_worker);
    async_effect.register_async_executor("write_async",[](const dage::ExecutionContext& context,const dage::Value&,
                                                           const std::shared_ptr<dage::AsyncExecutorCompletion>& done){
        CHECK(context.effect_committer_owner.get()!=nullptr);
        context.effect_committer_owner->commit();
        done->complete(dage::ExecutionResult::ok(dage::Value::object()));
    });
    std::unique_ptr<dage::Workflow> effect_workflow=async_effect.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"write_async\","
        "\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\","
        "\"idempotency_key\":\"${run.id}:${node.id}\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"committed\":true}}}"));
    dage::ExecutionResult effect_result=async_effect.create_run(*effect_workflow)->execute(dage::Value::object());
    CHECK(effect_result.success&&effect_result.output.get("committed").as_bool());

    dage::Engine fan_in(one_worker);
    std::vector<std::shared_ptr<dage::AsyncExecutorCompletion> > branch_completions;
    std::promise<void> branches_started;std::atomic<int> branch_count(0);
    fan_in.register_async_executor("branch",[&](const dage::ExecutionContext&,const dage::Value&,
                                                const std::shared_ptr<dage::AsyncExecutorCompletion>& completion){
        branch_completions.push_back(completion);
        if(++branch_count==2)branches_started.set_value();
    });
    fan_in.register_executor("join",[](const dage::ExecutionContext&,const dage::Value& input){
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> fan_workflow=fan_in.load(
        "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\","
        "\"limits\":{\"max_parallel\":2},\"nodes\":{"
        "\"a\":{\"type\":\"parallel\",\"branches\":[\"b\",\"c\"],\"join\":\"j\"},"
        "\"b\":{\"type\":\"tool\",\"executor\":\"branch\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"c\":{\"type\":\"tool\",\"executor\":\"branch\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"join\",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}}");
    std::unique_ptr<dage::Run> fan_run=fan_in.create_run(*fan_workflow);
    std::promise<dage::ExecutionResult> fan_done;
    fan_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){fan_done.set_value(value);});
    branches_started.get_future().wait();
    std::future<void> fan_sentinel=one_worker->schedule([](){});
    CHECK(fan_sentinel.wait_for(std::chrono::milliseconds(100))==std::future_status::ready);
    CHECK(branch_completions[0]->complete(dage::ExecutionResult::ok(dage::Value::object())));
    CHECK(branch_completions[1]->complete(dage::ExecutionResult::ok(dage::Value::object())));
    CHECK(fan_done.get_future().get().success);

    dage::Engine subflows(one_worker);
    subflows.register_async_executor("child_async",[&](const dage::ExecutionContext&,const dage::Value& input,
                                                       const std::shared_ptr<dage::AsyncExecutorCompletion>& done){
        executor_threads.launch([input,done](){std::this_thread::sleep_for(std::chrono::milliseconds(2));
            done->complete(dage::ExecutionResult::ok(input));});
    });
    subflows.register_workflow("child",wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"child_async\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
        "\"input\":{\"child\":true},\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":\"${nodes.a.output}\"}}"));
    std::unique_ptr<dage::Workflow> subflow_workflow=subflows.load(wf(
        "{\"a\":{\"type\":\"subflow\",\"config\":{\"workflow\":\"child\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":\"${nodes.a.output}\"}}"));
    std::unique_ptr<dage::Run> subflow_run=subflows.create_run(*subflow_workflow);
    std::promise<dage::ExecutionResult> subflow_done;
    subflow_run->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){subflow_done.set_value(value);});
    dage::ExecutionResult subflow_result=subflow_done.get_future().get();
    CHECK(subflow_result.success&&subflow_result.output.get("child").as_bool());
}
static void observable_kernel_tests(){
    auto trace=std::make_shared<dage::MemoryTraceSink>();
    dage::Engine traced;traced.set_trace_sink(trace);
    traced.register_async_executor("waiting",[](const dage::ExecutionContext&,const dage::Value&,
                                                 const std::shared_ptr<dage::AsyncExecutorCompletion>&){
    });
    std::unique_ptr<dage::Workflow> waiting=traced.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"waiting\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}}}"));
    std::unique_ptr<dage::Run> cancelled=traced.create_run(*waiting);
    std::promise<dage::ExecutionResult> cancelled_done;
    cancelled->execute_async(dage::Value::object(),[&](const dage::ExecutionResult& value){cancelled_done.set_value(value);});
    std::this_thread::sleep_for(std::chrono::milliseconds(15));cancelled->cancel("host shutdown");
    dage::ExecutionResult cancelled_result=cancelled_done.get_future().get();
    CHECK(!cancelled_result.success&&cancelled_result.error.category=="cancelled");
    std::vector<dage::EventEnvelope> events=trace->events();
    CHECK(events.size()>=2&&events.front().schema_version==1&&!events.front().run_id.empty());

    dage::Engine quota;quota.register_executor("large",[](const dage::ExecutionContext&,const dage::Value&){
        dage::Value output=dage::Value::object();output.set("payload",dage::Value("too-large"));
        return dage::ExecutionResult::ok(output);
    });
    std::unique_ptr<dage::Workflow> large=quota.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"large\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}}}"));
    dage::RunOptions tiny;tiny.max_output_bytes=4;
    CHECK(quota.create_run(*large,tiny)->execute(dage::Value::object()).error.code=="MAX_OUTPUT_BYTES");

    auto store=std::make_shared<dage::MemoryStateStore>();dage::Engine parallel;
    parallel.set_state_store(store);parallel.set_trace_sink(trace);
    int writes=0;parallel.register_executor("write",[&](const dage::ExecutionContext& context,const dage::Value&){
        ++writes;context.effect_committer->commit();return dage::ExecutionResult::ok(dage::Value::object());
    });
    parallel.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input){
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> graph=parallel.load(wf(
        "{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\"],\"join\":\"j\"},"
        "\"b\":{\"type\":\"tool\",\"executor\":\"write\",\"effects\":{\"kind\":\"external_write\","
        "\"replay\":\"idempotent\",\"idempotency_key\":\"${run.id}:b\"},\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"echo\",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}"));
    bool crash=true;parallel.set_failure_injector([&](const std::string& point,const std::string& node){
        if(crash&&point=="after_parallel_children"&&node=="a"){crash=false;return true;}return false;
    });
    std::unique_ptr<dage::Run> first=parallel.create_run(*graph);
    CHECK(first->execute(dage::Value::object()).error.code=="INJECTED_CRASH");
    const std::string parent_id=first->snapshot().get("run_id").as_string();
    dage::Result<std::string> parent=store->get(parent_id);CHECK(parent);
    parallel.set_failure_injector({});
    std::unique_ptr<dage::Run> resumed=parallel.restore_run(*graph,parent.value());
    CHECK(resumed->execute(dage::Value::object()).success&&writes==1);
    CHECK(has_event(trace->events(),"run_restored"));
    dage::Result<std::vector<std::string>> children=store->list(parent_id+"-p-");
    CHECK(children&&children.value().empty());
}
static void trace_pipeline_and_lease_tests(){
    CHECK(dage::trace_sample_bucket("abc")==2319);
    auto memory=std::make_shared<dage::MemoryTraceSink>();
    auto redacting=std::make_shared<dage::RedactingTraceSink>(memory,std::vector<std::string>{"secret"});
    dage::TraceSamplingOptions sampling_options;sampling_options.rate_basis_points=0;sampling_options.always_sample_errors=true;
    dage::SamplingTraceSink sampled(redacting,sampling_options);
    dage::EventEnvelope event;event.trace_id="trace";event.payload_json="{\"secret\":\"token\",\"ok\":1}";
    sampled.emit(event);CHECK(memory->events().empty());
    event.code="FAILED";sampled.emit(event);CHECK(memory->events().size()==1);
    CHECK(memory->events()[0].payload_json.find("token")==std::string::npos);
    memory->clear();

    auto decision_trace=std::make_shared<dage::MemoryTraceSink>();
    auto decision_leases=std::make_shared<dage::SemaphoreLeaseProvider>(1);
    dage::Engine decisions;decisions.set_trace_sink(decision_trace);decisions.set_resource_lease_provider(decision_leases);
    int decision_attempts=0;
    decisions.register_executor("flaky",[&](const dage::ExecutionContext&,const dage::Value& input){
        if(++decision_attempts==1)return dage::ExecutionResult::fail("temporary","TRY_AGAIN","retry",true);
        return dage::ExecutionResult::ok(input);
    });
    std::unique_ptr<dage::Workflow> decision_workflow=decisions.load(wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"flaky\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},"
        "\"input\":{\"secret\":\"visible-in-inputs\"},\"retry\":{\"max_attempts\":2},"
        "\"next\":[{\"to\":\"yes\",\"when\":\"${nodes.a.output.secret == 'visible-in-inputs'}\"},{\"to\":\"no\",\"otherwise\":true}]},"
        "\"yes\":{\"type\":\"end\",\"input\":{\"ok\":true}},\"no\":{\"type\":\"end\",\"input\":{\"ok\":false}}}"));
    dage::RunOptions capture_inputs;capture_inputs.trace_capture=dage::TraceCapture::Inputs;
    capture_inputs.component_identities["host"]="test-host/1";
    CHECK(decisions.create_run(*decision_workflow,capture_inputs)->execute(dage::Value::object()).success);
    const std::vector<dage::EventEnvelope> decision_events=decision_trace->events();
    CHECK(has_event(decision_events,"run_started")&&has_event(decision_events,"node_input_resolved"));
    CHECK(has_event(decision_events,"retry_scheduled")&&has_event(decision_events,"edge_evaluated"));
    CHECK(has_event(decision_events,"edge_selected")&&has_event(decision_events,"resource_lease_requested"));
    CHECK(has_event(decision_events,"resource_lease_acquired")&&has_event(decision_events,"resource_lease_released"));
    CHECK(!decision_events.front().workflow_digest.empty()&&decision_events.front().payload_json.find("dage/0.2.0")!=std::string::npos);
    CHECK(decision_events.front().payload_json.find("test-host/1")!=std::string::npos);
    bool input_captured=false,condition_hidden=true;
    for(const auto& traced_event:decision_events){
        if(traced_event.event=="node_input_resolved"&&traced_event.payload_json.find("visible-in-inputs")!=std::string::npos)input_captured=true;
        if(traced_event.event=="edge_evaluated"&&traced_event.payload_json.find("${nodes.")!=std::string::npos)condition_hidden=false;
    }
    CHECK(input_captured&&condition_hidden);

    decision_trace->clear();decision_attempts=0;
    dage::RunOptions full_capture;full_capture.trace_capture=dage::TraceCapture::Full;
    dage::ExecutionResult recorded=decisions.create_run(*decision_workflow,full_capture)->execute(
        dage::Value::parse("{\"request\":\"same\"}"));
    CHECK(recorded.success&&decision_attempts==2);
    const std::vector<dage::EventEnvelope> replay_events=decision_trace->events();
    dage::Result<dage::TraceReplayPlan> replay_plan=dage::prepare_trace_replay(
        replay_events,decision_workflow->ir().digest(),decision_workflow->bundle_digest());
    CHECK(replay_plan&&replay_plan.value().workflow_input.get("request").as_string()=="same");
    std::unique_ptr<dage::Run> replay_run=decisions.create_replay_run(*decision_workflow,replay_plan.value());
    dage::ExecutionResult replayed=replay_run->execute(replay_plan.value().workflow_input);
    CHECK(replayed.success&&replayed.output.to_json()==recorded.output.to_json()&&decision_attempts==2);
    std::vector<dage::EventEnvelope> incomplete=replay_events;
    incomplete.pop_back();
    CHECK(!dage::prepare_trace_replay(incomplete,decision_workflow->ir().digest()));

    dage::EvaluationSample baseline_sample;baseline_sample.label="baseline";
    baseline_sample.workflow_digest="sha256:base";
    baseline_sample.result=dage::ExecutionResult::ok(dage::Value::parse("{\"quality\":0.7,\"cost\":0.4}"));
    dage::EvaluationSample candidate_sample;candidate_sample.label="candidate";
    candidate_sample.workflow_digest="sha256:candidate";
    candidate_sample.result=dage::ExecutionResult::ok(dage::Value::parse("{\"quality\":0.8,\"cost\":0.35}"));
    dage::EventEnvelope shadow_started;shadow_started.event="run_started";
    shadow_started.payload_json="{\"run_mode\":\"shadow\"}";candidate_sample.trace.push_back(shadow_started);
    auto field_metric=[](const std::string& field){
        return [field](const dage::EvaluationSample& sample){
            return dage::Result<double>::success(sample.result.output.get(field).as_double());
        };
    };
    dage::MetricDefinition quality;quality.name="quality";quality.weight=2.0;quality.evaluate=field_metric("quality");
    dage::MetricDefinition inverse_cost;inverse_cost.name="inverse_cost";inverse_cost.weight=1.0;
    inverse_cost.evaluate=[](const dage::EvaluationSample& sample){
        return dage::Result<double>::success(-sample.result.output.get("cost").as_double());
    };
    dage::Result<dage::ShadowEvaluationReport> report=dage::compare_shadow_evaluation(
        baseline_sample,candidate_sample,{quality,inverse_cost});
    CHECK(report&&report.value().recommended&&report.value().metrics.size()==2);
    candidate_sample.trace.clear();
    CHECK(!dage::compare_shadow_evaluation(baseline_sample,candidate_sample,{quality}));

    dage::FairResourceLeaseProvider multi({{"cpu",2},{"gpu",1}});
    dage::ResourceRequest multi_request;multi_request.run_id="multi";multi_request.resources={{"cpu",1},{"gpu",1}};
    multi_request.lease_ttl_ms=100;
    dage::Result<std::unique_ptr<dage::ResourceLease>> multi_lease=multi.acquire(multi_request,nullptr);
    CHECK(multi_lease&&multi.available().at("gpu")==0&&multi_lease.value()->fencing_token()>0);
    const std::uint64_t original_expiry=multi_lease.value()->expires_at_unix_ms();
    CHECK(multi_lease.value()->renew(200)&&multi_lease.value()->expires_at_unix_ms()>=original_expiry);
    const std::uint64_t first_fence=multi_lease.value()->fencing_token();multi_lease.value().reset();
    CHECK(multi.available().at("gpu")==1);
    auto next_lease=multi.acquire(multi_request,nullptr);CHECK(next_lease&&next_lease.value()->fencing_token()>first_fence);
    next_lease.value().reset();
    dage::ResourceRequest expiring=multi_request;expiring.lease_ttl_ms=1;
    auto expired=multi.acquire(expiring,nullptr);CHECK(expired);
    const auto expiry_wait_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    while(multi.available().at("gpu")!=1&&std::chrono::steady_clock::now()<expiry_wait_deadline)
        std::this_thread::yield();
    CHECK(multi.available().at("gpu")==1&&!expired.value()->renew(10));

    dage::FairResourceLeaseProvider ordered({{"slots",1}});
    dage::ResourceRequest holder_request;holder_request.run_id="holder";holder_request.fairness_key="holder";
    auto holder=ordered.acquire(holder_request,nullptr);CHECK(holder);
    std::vector<std::string> grant_order;std::mutex grant_mutex;
    auto wait_for_lease=[&](std::string name,int priority){
        dage::ResourceRequest request;request.run_id=name;request.fairness_key=name;request.priority=priority;
        auto granted=ordered.acquire(request,nullptr);CHECK(granted);
        {std::lock_guard<std::mutex> lock(grant_mutex);grant_order.push_back(name);}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    std::thread low(wait_for_lease,"low",0);std::this_thread::sleep_for(std::chrono::milliseconds(3));
    std::thread high(wait_for_lease,"high",10);std::this_thread::sleep_for(std::chrono::milliseconds(3));
    holder.value().reset();low.join();high.join();
    CHECK(grant_order.size()==2&&grant_order[0]=="high");

    auto fairness_holder=ordered.acquire(holder_request,nullptr);CHECK(fairness_holder);grant_order.clear();
    std::thread a1(wait_for_lease,"tenant-a",0);std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread a2(wait_for_lease,"tenant-a",0);std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread b1(wait_for_lease,"tenant-b",0);std::this_thread::sleep_for(std::chrono::milliseconds(3));
    fairness_holder.value().reset();a1.join();a2.join();b1.join();
    CHECK(grant_order.size()==3&&grant_order[0]=="tenant-a"&&grant_order[1]=="tenant-b");

    auto slow=std::make_shared<SlowTraceSink>();dage::AsyncTraceOptions async_options;
    async_options.capacity=1;async_options.full_policy=dage::TraceQueueFullPolicy::DropNewest;
    dage::AsyncTraceSink queued(slow,async_options);
    for(int i=0;i<40;++i)queued.emit(event);
    queued.flush();CHECK(queued.stats().dropped>0&&queued.stats().exported>0);

#ifdef DAGE_TEST_OPENTELEMETRY_BRIDGE
    auto exporter=std::make_shared<TestOtelExporter>();
    auto otel=std::make_shared<dage::extensions::OpenTelemetryTraceSink>(exporter);
    dage::Engine traced;traced.set_trace_sink(otel);
    std::unique_ptr<dage::Workflow> simple=traced.load(wf(
        "{\"a\":{\"type\":\"noop\",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}"));
    CHECK(traced.create_run(*simple)->execute(dage::Value::object()).success);
    otel->flush();CHECK(exporter->spans.size()==2&&!exporter->spans[0].trace_id.empty());
    CHECK(exporter->spans[1].parent_span_id==exporter->spans[0].span_id);
#endif

    auto leases=std::make_shared<dage::SemaphoreLeaseProvider>(1);dage::Engine limited;
    auto parallel_trace=std::make_shared<dage::MemoryTraceSink>();
    limited.set_resource_lease_provider(leases);limited.set_trace_sink(parallel_trace);std::atomic<int> active(0),peak(0);
    limited.register_executor("work",[&](const dage::ExecutionContext& context,const dage::Value& input){
        CHECK(context.resource_lease&&context.resource_lease->fencing_token()>0);
        const int now=++active;int observed=peak.load();while(now>observed&&!peak.compare_exchange_weak(observed,now)){}
        std::this_thread::sleep_for(std::chrono::milliseconds(15));--active;return dage::ExecutionResult::ok(input);
    });
    limited.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input){return dage::ExecutionResult::ok(input);});
    std::unique_ptr<dage::Workflow> parallel=limited.load(wf(
        "{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\",\"c\"],\"join\":\"j\"},"
        "\"b\":{\"type\":\"tool\",\"executor\":\"work\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"c\":{\"type\":\"tool\",\"executor\":\"work\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"echo\",\"next\":\"z\"},\"z\":{\"type\":\"end\"}}"));
    CHECK(limited.create_run(*parallel)->execute(dage::Value::object()).success);
    CHECK(peak.load()==1&&leases->available()==1);
    CHECK(has_event(parallel_trace->events(),"parallel_started")&&has_event(parallel_trace->events(),"parallel_completed"));
}
static void timeout_cancel_tests(){
    dage::Engine e;e.register_executor("slow",[](const dage::ExecutionContext&ctx,const dage::Value&){
        while(!ctx.cancelled->load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return dage::ExecutionResult::fail("cancelled","COOPERATIVE","stopped");
    });
    std::unique_ptr<dage::Workflow>w=e.load(wf("{\"a\":{\"type\":\"tool\",\"executor\":\"slow\",\"effects\":{\"kind\":\"external_read\",\"replay\":\"safe\"},\"timeout_ms\":5}}"));
    dage::ExecutionResult timed=e.create_run(*w)->execute(dage::Value::object());CHECK(!timed.success&&timed.error.category=="timeout");
    std::unique_ptr<dage::Run>cancelled=e.create_run(*w);cancelled->cancel();
    CHECK(cancelled->execute(dage::Value::object()).error.category=="cancelled");
}
static void human_checkpoint_tests(){
    dage::Engine e;e.register_executor("approval",[](const dage::ExecutionContext&,const dage::Value&){
        return dage::ExecutionResult::fail("suspended","AWAITING_APPROVAL","Waiting for a human.");
    });
    std::unique_ptr<dage::Workflow>w=e.load(wf("{\"a\":{\"type\":\"human\",\"executor\":\"approval\",\"next\":[{\"to\":\"yes\",\"when\":\"${output.approved}\"},{\"to\":\"no\",\"otherwise\":true}]},\"yes\":{\"type\":\"end\",\"input\":{\"decision\":\"approved\"}},\"no\":{\"type\":\"end\",\"input\":{\"decision\":\"rejected\"}}}"));
    std::unique_ptr<dage::Run>run=e.create_run(*w);
    dage::ExecutionResult pending=run->execute(dage::Value::object());
    CHECK(!pending.success);CHECK(run->suspended());
    const std::string checkpoint=run->checkpoint();
    std::unique_ptr<dage::Run>restored=e.restore_run(*w,checkpoint);
    CHECK(restored->suspended());
    dage::ExecutionResult done=restored->resume(dage::Value::parse("{\"approved\":true}"));
    CHECK(done.success);CHECK(done.output.get("decision").as_string()=="approved");
    std::unique_ptr<dage::Workflow>other=e.load(wf("{\"a\":{\"type\":\"end\"}}"));
    bool mismatch=false;try{e.restore_run(*other,checkpoint);}catch(const std::exception&){mismatch=true;}
    CHECK(mismatch);
}
static void patch_tests(){
    dage::Engine e;std::unique_ptr<dage::Workflow>w=e.load(wf("{\"a\":{\"type\":\"noop\",\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"version\":1}}}"));
    const std::string patch="{\"base_digest\":\""+w->ir().digest()+"\",\"base_revision\":0,\"changes\":["
        "{\"action\":\"add_node\",\"node_id\":\"middle\",\"node\":{\"type\":\"noop\",\"next\":\"z\"}},"
        "{\"action\":\"set_next\",\"node_id\":\"a\",\"value\":\"middle\"},"
        "{\"action\":\"set_field\",\"node_id\":\"z\",\"field\":\"input\",\"value\":{\"version\":2}},"
        "{\"action\":\"rename_node\",\"node_id\":\"middle\",\"new_node_id\":\"renamed\"}]}";
    std::unique_ptr<dage::Workflow>changed=e.apply_patch(*w,patch);
    dage::ExecutionResult result=e.create_run(*changed)->execute(dage::Value::object());
    CHECK(result.success);CHECK(result.output.get("version").as_integer()==2);
    CHECK(changed->normalized_json().find("\"x_revision\" : 1")!=std::string::npos);
    CHECK(w->normalized_json().find("\"x_revision\"")==std::string::npos);
    dage::PatchDryRunResult dry=e.dry_run_patch(*w,patch);
    CHECK(dry.valid&&!dry.candidate_digest.empty()&&dry.candidate_revision==1);
    CHECK(w->normalized_json().find("\"x_revision\"")==std::string::npos);

    const std::string wrong="{\"base_digest\":\"sha256:wrong\",\"changes\":["
        "{\"action\":\"set_field\",\"node_id\":\"z\",\"field\":\"input\",\"value\":{}}]}";
    dage::PatchDryRunResult conflict=e.dry_run_patch(*w,wrong);
    CHECK(!conflict.valid&&conflict.diagnostics.size()==1&&
          conflict.diagnostics[0].code=="PATCH_BASE_DIGEST_MISMATCH"&&
          conflict.diagnostics[0].path=="/base_digest");
    bool mismatch=false;try{e.apply_patch(*w,wrong);}catch(const std::exception& ex){
        mismatch=std::string(ex.what()).find("PATCH_BASE_DIGEST_MISMATCH")!=std::string::npos;}
    CHECK(mismatch);

    const std::string partial="{\"base_digest\":\""+w->ir().digest()+"\",\"changes\":["
        "{\"action\":\"add_node\",\"node_id\":\"temporary\",\"node\":{\"type\":\"noop\"}},"
        "{\"action\":\"unknown\",\"node_id\":\"a\"}]}";
    dage::PatchDryRunResult partial_result=e.dry_run_patch(*w,partial);
    CHECK(!partial_result.valid&&partial_result.diagnostics[0].code=="PATCH_UNKNOWN_ACTION"&&
          partial_result.diagnostics[0].path=="/changes/1/action");
    CHECK(w->normalized_json().find("temporary")==std::string::npos);

    const std::string malformed="{\"base_digest\":\""+w->ir().digest()+"\",\"changes\":["
        "{},{\"action\":\"unknown\",\"node_id\":\"a\"},"
        "{\"action\":\"set_field\",\"node_id\":\"a\",\"field\":\"\"}]}";
    dage::PatchDryRunResult itemized=e.dry_run_patch(*w,malformed);
    CHECK(!itemized.valid&&itemized.diagnostics.size()==3);
    CHECK(itemized.diagnostics[0].code=="PATCH_ACTION_REQUIRED"&&itemized.diagnostics[0].path=="/changes/0/action");
    CHECK(itemized.diagnostics[1].code=="PATCH_UNKNOWN_ACTION"&&itemized.diagnostics[1].path=="/changes/1/action");
    CHECK(itemized.diagnostics[2].code=="PATCH_FIELD_REQUIRED"&&itemized.diagnostics[2].path=="/changes/2/field");

    const std::string invalid_candidate="{\"base_digest\":\""+w->ir().digest()+"\",\"changes\":["
        "{\"action\":\"set_next\",\"node_id\":\"a\",\"value\":\"missing\"}]}";
    dage::PatchDryRunResult candidate_error=e.dry_run_patch(*w,invalid_candidate);
    CHECK(!candidate_error.valid&&candidate_error.diagnostics[0].code=="UNKNOWN_NODE_REFERENCE"&&
          candidate_error.diagnostics[0].path=="/candidate");

    dage::PatchAnalysis analysis=e.analyze_patch(*w,patch);
    CHECK(analysis.valid&&!analysis.inverse_patch_json.empty());
    CHECK(std::find(analysis.directly_changed_nodes.begin(),analysis.directly_changed_nodes.end(),"a")!=analysis.directly_changed_nodes.end());
    CHECK(std::find(analysis.affected_nodes.begin(),analysis.affected_nodes.end(),"z")!=analysis.affected_nodes.end());
    CHECK(!analysis.checkpoint_resume_compatible);
    dage::WorkflowDiff workflow_diff=e.diff_workflows(*w,*changed);
    CHECK(workflow_diff.before_digest==w->ir().digest()&&workflow_diff.after_digest==changed->ir().digest());
    CHECK(workflow_diff.node_changes.size()==3&&!workflow_diff.checkpoint_resume_compatible);
    CHECK(std::find(workflow_diff.affected_nodes.begin(),workflow_diff.affected_nodes.end(),"z")!=workflow_diff.affected_nodes.end());
    CHECK(workflow_diff.effect_policy_changed);
    std::unique_ptr<dage::Workflow> round_trip=e.apply_patch(*changed,analysis.inverse_patch_json);
    CHECK(round_trip->ir().digest()==w->ir().digest());
    CHECK(round_trip->normalized_json().find("\"x_revision\" : 2")!=std::string::npos);
    dage::WorkflowDiff revision_only=e.diff_workflows(*w,*round_trip);
    CHECK(revision_only.node_changes.empty()&&revision_only.changed_workflow_fields.empty()&&
          revision_only.checkpoint_resume_compatible);

    dage::Engine patch_limited;std::unique_ptr<dage::Workflow> patch_base=patch_limited.load(
        wf("{\"a\":{\"type\":\"end\"}}"));
    dage::WorkflowResourceLimits patch_limits;
    patch_limits.max_compiled_ir_bytes=128;patch_limited.set_workflow_resource_limits(patch_limits);
    const std::string limited_patch="{\"base_digest\":\""+patch_base->ir().digest()+"\",\"changes\":["
        "{\"action\":\"set_field\",\"node_id\":\"a\",\"field\":\"input\",\"value\":{\"x\":1}}]}";
    dage::PatchDryRunResult limited=patch_limited.dry_run_patch(*patch_base,limited_patch);
    CHECK(!limited.valid&&limited.diagnostics.size()==1&&
          limited.diagnostics[0].code=="WORKFLOW_IR_BYTES_LIMIT"&&
          limited.diagnostics[0].path=="/candidate");
}
static void loop_and_export_tests(){
    dage::Engine e;
    std::string text=wf("{\"a\":{\"name\":\"A \\\"quoted\\\"\",\"type\":\"noop\",\"next\":{\"to\":\"a\",\"loop\":{\"id\":\"l\",\"max_iterations\":2,\"on_exhausted\":\"z\"}}},\"z\":{\"type\":\"end\",\"input\":{\"done\":true}}}");
    std::unique_ptr<dage::Workflow>w=e.load(text);dage::ExecutionResult r=e.create_run(*w)->execute(dage::Value::object());
    CHECK(r.success&&r.output.get("done").as_bool());CHECK(w->export_mermaid().find("&quot;")!=std::string::npos);
    CHECK(w->export_dot().find("\\\"quoted\\\"")!=std::string::npos);CHECK(w->normalized_json().find("\"enabled\" : true")!=std::string::npos);
}
static void parallel_test(){
    dage::Engine e;auto replay_trace=std::make_shared<dage::MemoryTraceSink>();e.set_trace_sink(replay_trace);
    std::atomic<int> branch_calls(0);e.register_executor("branch",[&](const dage::ExecutionContext&ctx,const dage::Value&){
        ++branch_calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        dage::Value out=dage::Value::object();out.set("branch",dage::Value(ctx.node_id));return dage::ExecutionResult::ok(out);});
    e.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value&i){return dage::ExecutionResult::ok(i);});
    std::string text="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"limits\":{\"max_parallel\":2},\"nodes\":{\"a\":{\"type\":\"parallel\",\"branches\":[\"b\",\"c\"],\"join\":\"j\"},\"b\":{\"type\":\"tool\",\"executor\":\"branch\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},\"c\":{\"type\":\"tool\",\"executor\":\"branch\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"j\"},\"j\":{\"type\":\"join\",\"executor\":\"echo\",\"input\":{\"b\":\"${nodes.b.output.branch}\",\"c\":\"${nodes.c.output.branch}\"},\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"b\":\"${nodes.j.output.b}\",\"c\":\"${nodes.j.output.c}\"}}}}";
    std::unique_ptr<dage::Workflow>w=e.load(text);dage::RunOptions full;full.trace_capture=dage::TraceCapture::Full;
    const std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    dage::ExecutionResult result=e.create_run(*w,full)->execute(dage::Value::object());
    const long long elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    CHECK(result.success);CHECK(result.output.get("b").as_string()=="b");CHECK(result.output.get("c").as_string()=="c");
    CHECK(elapsed<75&&branch_calls==2);
    auto plan=dage::prepare_trace_replay(replay_trace->events(),w->ir().digest(),w->bundle_digest());CHECK(plan);
    dage::ExecutionResult replayed=e.create_replay_run(*w,plan.value())->execute(plan.value().workflow_input);
    CHECK(replayed.success&&replayed.output.to_json()==result.output.to_json()&&branch_calls==2);

    dage::Engine nested;auto nested_trace=std::make_shared<dage::MemoryTraceSink>();nested.set_trace_sink(nested_trace);
    std::atomic<int> nested_calls(0);
    nested.register_executor("child_echo",[&](const dage::ExecutionContext&,const dage::Value& input){
        ++nested_calls;return dage::ExecutionResult::ok(input);
    });
    nested.register_executor("join_echo",[](const dage::ExecutionContext&,const dage::Value& input){
        return dage::ExecutionResult::ok(input);
    });
    nested.register_workflow("shared_child",wf(
        "{\"a\":{\"type\":\"tool\",\"executor\":\"child_echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
        "\"input\":\"${workflow.input}\",\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":\"${nodes.a.output}\"}}"));
    std::unique_ptr<dage::Workflow> nested_workflow=nested.load(wf(
        "{\"start\":{\"type\":\"parallel\",\"branches\":[\"p1\",\"p2\"],\"join\":\"j\"},"
        "\"p1\":{\"type\":\"subflow\",\"input\":{\"source\":\"p1\"},\"config\":{\"workflow\":\"shared_child\"},\"next\":\"j\"},"
        "\"p2\":{\"type\":\"subflow\",\"input\":{\"source\":\"p2\"},\"config\":{\"workflow\":\"shared_child\"},\"next\":\"j\"},"
        "\"j\":{\"type\":\"join\",\"executor\":\"join_echo\",\"input\":{\"p1\":\"${nodes.p1.output.source}\","
        "\"p2\":\"${nodes.p2.output.source}\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":\"${nodes.j.output}\"}}","start"));
    dage::ExecutionResult nested_result=nested.create_run(*nested_workflow,full)->execute(dage::Value::object());
    CHECK(nested_result.success&&nested_calls==2);
    auto nested_plan=dage::prepare_trace_replay(
        nested_trace->events(),nested_workflow->ir().digest(),nested_workflow->bundle_digest());
    CHECK(nested_plan);
    dage::ExecutionResult nested_replay=nested.create_replay_run(*nested_workflow,nested_plan.value())
        ->execute(nested_plan.value().workflow_input);
    CHECK(nested_replay.success&&nested_replay.output.to_json()==nested_result.output.to_json()&&nested_calls==2);
}
namespace {
struct TestSuite {
    const char* name;
    void (*run)();
};

const std::vector<TestSuite>& test_suites() {
    static const std::vector<TestSuite> suites{
        {"infrastructure", infrastructure_tests},
        {"bundle_ir", bundle_ir_tests},
#ifdef DAGE_HAS_BUNDLE_TOOLS
        {"bundle_tool_provider", bundle_tool_provider_tests},
        {"bundle_golden_vector", bundle_golden_vector_tests},
        {"bundle_store", bundle_store_tests},
#endif
        {"resolver", resolver_tests},
        {"value", value_tests},
        {"validation", validation_tests},
        {"all_node_format", all_node_format_tests},
        {"runtime", runtime_tests},
        {"error_retry_policy", error_retry_policy_tests},
        {"effect_replay", effect_replay_tests},
        {"reliability_recovery", reliability_recovery_tests},
        {"state_store_and_async", state_store_and_async_tests},
        {"observable_kernel", observable_kernel_tests},
        {"trace_pipeline_and_lease", trace_pipeline_and_lease_tests},
        {"timeout_cancel", timeout_cancel_tests},
        {"human_checkpoint", human_checkpoint_tests},
        {"patch", patch_tests},
        {"loop_and_export", loop_and_export_tests},
        {"parallel", parallel_test},
    };
    return suites;
}

int run_suite(const TestSuite& suite) {
    std::cout << "[ RUN      ] " << suite.name << std::endl;
    suite.run();
    std::cout << "[       OK ] " << suite.name << std::endl;
    return 0;
}
}

int main(int argc, char** argv){
    try {
        if (argc == 2) {
            const std::string requested(argv[1]);
            for (const auto& suite : test_suites()) {
                if (requested == suite.name) return run_suite(suite);
            }
            std::cerr << "unknown test suite: " << requested << "\navailable suites:";
            for (const auto& suite : test_suites()) std::cerr << ' ' << suite.name;
            std::cerr << '\n';
            return 2;
        }
        if (argc != 1) {
            std::cerr << "usage: dage_tests [suite]\n";
            return 2;
        }
        for (const auto& suite : test_suites()) run_suite(suite);
        std::cout << "all tests passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
