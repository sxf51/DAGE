#include "dage/dage.hpp"
#include "dage/bundle.hpp"
#include "dage/resource_provider.hpp"
#ifdef DAGE_HAS_BUNDLE_TOOLS
#include "dage/tools/resource_providers.hpp"
#endif
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    std::ostringstream s; s << f.rdbuf(); return s.str();
}
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: dage <validate|format|mermaid|dot|run|patch|resume> WORKFLOW [ARG]\n"
                     "       dage <bundle-validate|bundle-inspect|bundle-run> BUNDLE WORKFLOW_ID [INPUT_JSON]\n";
        return 2;
    }
    try {
        dage::Engine engine;
        engine.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input){
            return dage::ExecutionResult::ok(input);
        });
        const std::string command=argv[1];
        if(command=="bundle-validate"||command=="bundle-inspect"||command=="bundle-run"){
#ifndef DAGE_HAS_BUNDLE_TOOLS
            throw std::runtime_error("bundle commands require DAGE_BUILD_BUNDLE_TOOLS");
#else
            if(argc<4)throw std::runtime_error("bundle command requires BUNDLE_DIR and WORKFLOW_ID");
            std::unique_ptr<dage::ResourceProvider> provider;
            if(std::filesystem::is_directory(argv[2]))
                provider.reset(new dage::tools::DirectoryResourceProvider(argv[2]));
            else provider.reset(new dage::tools::ZipResourceProvider(std::filesystem::path(argv[2])));
            std::unique_ptr<dage::Bundle> bundle=engine.load_bundle(*provider);
            std::unique_ptr<dage::Workflow> workflow=engine.load_workflow(*bundle,argv[3]);
            if(command=="bundle-validate"){
                std::cout<<"valid "<<bundle->id()<<"@"<<bundle->version()<<" "<<bundle->digest()<<"\n";return 0;
            }
            if(command=="bundle-inspect"){
                std::cout<<"canonical_manifest\n"<<bundle->canonical_manifest()<<"\n"
                         <<"canonical_unsigned_manifest\n"<<bundle->canonical_unsigned_manifest()<<"\n"
                         <<"signing_payload\n"<<bundle->signing_payload()
                         <<"digest\n"<<bundle->digest()<<"\n";return 0;
            }
            std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
            dage::ExecutionResult result=run->execute(dage::Value::parse(argc>4?argv[4]:"{}"));
            if(!result.success){std::cerr<<result.error.code<<": "<<result.error.message<<"\n";return 1;}
            std::cout<<result.output.to_json(true);return 0;
#endif
        }
        const std::string text = read_file(argv[2]);
        if (std::string(argv[1]) == "validate") {
            std::vector<dage::Diagnostic> d = engine.validate(text);
            bool error=false;
            for(std::size_t i=0;i<d.size();++i){std::cout<<d[i].code<<" "<<d[i].path<<": "<<d[i].message<<"\n";error|=d[i].severity==dage::Severity::Error;}
            if(!error)std::cout<<"valid\n";
            return error?1:0;
        }
        std::unique_ptr<dage::Workflow> w=engine.load(text);
        if(command=="format")std::cout<<w->normalized_json();
        else if(command=="mermaid")std::cout<<w->export_mermaid();
        else if(command=="dot")std::cout<<w->export_dot();
        else if(command=="run"){
            std::unique_ptr<dage::Run> run=engine.create_run(*w);
            dage::ExecutionResult r=run->execute(dage::Value::parse(argc>3?argv[3]:"{}"));
            if(!r.success){std::cerr<<r.error.code<<": "<<r.error.message<<"\n";return 1;}
            std::cout<<r.output.to_json(true);
        }else if(command=="patch"){
            if(argc<4)throw std::runtime_error("patch requires a patch JSON file");
            std::unique_ptr<dage::Workflow> changed=engine.apply_patch(*w,read_file(argv[3]));
            std::cout<<changed->normalized_json();
        }else if(command=="resume"){
            if(argc<5)throw std::runtime_error("resume requires CHECKPOINT_FILE HUMAN_OUTPUT_JSON");
            std::unique_ptr<dage::Run> run=engine.restore_run(*w,read_file(argv[3]));
            dage::ExecutionResult r=run->resume(dage::Value::parse(argv[4]));
            if(!r.success){std::cerr<<r.error.code<<": "<<r.error.message<<"\n";return 1;}
            std::cout<<r.output.to_json(true);
        }else throw std::runtime_error("unknown command");
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
