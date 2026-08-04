#include <napi.h>
#include "dage/dage.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
class Api {
public:
    typedef dage_status_t (*Create)(const dage_engine_options_t*,dage_engine_handle*);
    typedef void (*DestroyEngine)(dage_engine_handle);
    typedef dage_status_t (*Load)(dage_engine_handle,dage_string_view_t,dage_workflow_handle*);
    typedef void (*DestroyWorkflow)(dage_workflow_handle);
    typedef dage_status_t (*CreateRun)(dage_engine_handle,dage_workflow_handle,dage_run_handle*);
    typedef void (*DestroyRun)(dage_run_handle);
    typedef dage_status_t (*Execute)(dage_run_handle,dage_string_view_t,char*,size_t,size_t*);
    typedef dage_status_t (*Cancel)(dage_run_handle,dage_string_view_t);
    typedef dage_status_t (*RegisterAsync)(dage_engine_handle,dage_string_view_t,
        dage_async_executor_callback_t,void*,dage_userdata_destroy_t);
    typedef dage_status_t (*Complete)(dage_executor_completion_handle,dage_status_t,
        const dage_owned_buffer_t*);
    typedef void (*Abandon)(dage_executor_completion_handle);
    typedef uint8_t (*IsCancelled)(dage_executor_completion_handle);
    typedef dage_status_t (*CommitEffect)(dage_executor_completion_handle);

    Api():library_(NULL),create(NULL),destroy_engine(NULL),load(NULL),destroy_workflow(NULL),
        create_run(NULL),destroy_run(NULL),execute(NULL),cancel(NULL),register_async(NULL),
        complete(NULL),abandon(NULL),is_cancelled(NULL),commit_effect(NULL){
        const char* configured=std::getenv("DAGE_LIBRARY");
#if defined(_WIN32)
        library_=LoadLibraryA(configured?configured:"libdage.dll");
#define DAGE_SYMBOL(name) GetProcAddress(static_cast<HMODULE>(library_),name)
#else
        library_=dlopen(configured?configured:"libdage.so",RTLD_NOW);
#define DAGE_SYMBOL(name) dlsym(library_,name)
#endif
        if(library_){
            create=symbol<Create>("dage_engine_create");
            destroy_engine=symbol<DestroyEngine>("dage_engine_destroy");
            load=symbol<Load>("dage_engine_load");
            destroy_workflow=symbol<DestroyWorkflow>("dage_workflow_destroy");
            create_run=symbol<CreateRun>("dage_run_create");
            destroy_run=symbol<DestroyRun>("dage_run_destroy");
            execute=symbol<Execute>("dage_run_execute");
            cancel=symbol<Cancel>("dage_run_cancel_with_reason");
            register_async=symbol<RegisterAsync>("dage_engine_register_async_executor");
            complete=symbol<Complete>("dage_executor_complete");
            abandon=symbol<Abandon>("dage_executor_abandon");
            is_cancelled=symbol<IsCancelled>("dage_executor_completion_is_cancelled");
            commit_effect=symbol<CommitEffect>("dage_executor_completion_commit_effect");
        }
    }
    ~Api(){
#if defined(_WIN32)
        if(library_)FreeLibrary(static_cast<HMODULE>(library_));
#else
        if(library_)dlclose(library_);
#endif
    }
    bool valid()const{return create&&destroy_engine&&load&&destroy_workflow&&create_run&&
        destroy_run&&execute&&cancel&&register_async&&complete&&abandon&&is_cancelled&&commit_effect;}
    template<typename T>T symbol(const char* name){return reinterpret_cast<T>(DAGE_SYMBOL(name));}
    void* library_;Create create;DestroyEngine destroy_engine;Load load;DestroyWorkflow destroy_workflow;
    CreateRun create_run;DestroyRun destroy_run;Execute execute;Cancel cancel;RegisterAsync register_async;
    Complete complete;Abandon abandon;IsCancelled is_cancelled;CommitEffect commit_effect;
};
Api& api(){static Api instance;return instance;}

class Completion {
public:
    explicit Completion(dage_executor_completion_handle value):handle_(value){}
    bool cancelled(){
        std::lock_guard<std::mutex> lock(mutex_);
        return !handle_||api().is_cancelled(handle_)!=0;
    }
    bool commit(){
        std::lock_guard<std::mutex> lock(mutex_);
        return handle_&&api().commit_effect(handle_)==DAGE_STATUS_OK;
    }
    void complete(const std::string& json){
        dage_executor_completion_handle handle=take();if(!handle)return;
        dage_owned_buffer_t output={json.data(),json.size(),NULL,NULL};
        api().complete(handle,DAGE_STATUS_OK,&output);
    }
    void fail(){
        dage_executor_completion_handle handle=take();if(handle)
            api().complete(handle,DAGE_STATUS_EXECUTION_ERROR,NULL);
    }
    void abandon(){
        dage_executor_completion_handle handle=take();if(handle)api().abandon(handle);
    }
private:
    dage_executor_completion_handle take(){
        std::lock_guard<std::mutex> lock(mutex_);
        dage_executor_completion_handle value=handle_;handle_=NULL;return value;
    }
    std::mutex mutex_;dage_executor_completion_handle handle_;
};
struct Invocation {
    std::shared_ptr<Completion> completion;
    std::string run_id,node_id,node_type,idempotency_key,run_mode,input;
    uint32_t attempt=0;uint64_t deadline=0;
};
struct Registration { Napi::ThreadSafeFunction tsfn; };

std::string stringify(Napi::Env env,const Napi::Value& value){
    Napi::Object json=env.Global().Get("JSON").As<Napi::Object>();
    Napi::Value result=json.Get("stringify").As<Napi::Function>().Call(json,{value});
    if(env.IsExceptionPending()){env.GetAndClearPendingException();
        throw std::runtime_error("Executor output serialization failed");}
    if(!result.IsString())throw std::runtime_error("Executor output is not JSON serializable");
    return result.As<Napi::String>().Utf8Value();
}
void settle_success(Napi::Env env,const std::shared_ptr<Completion>& completion,const Napi::Value& value){
    try{completion->complete(stringify(env,value));}catch(...){completion->fail();}
}
void dispatch_invocation(Napi::Env env,Napi::Function callback,Invocation* raw){
    std::unique_ptr<Invocation> invocation(raw);
    std::shared_ptr<Completion> completion=invocation->completion;
    Napi::Object context=Napi::Object::New(env);
    context.Set("runId",invocation->run_id);context.Set("nodeId",invocation->node_id);
    context.Set("nodeType",invocation->node_type);context.Set("attempt",invocation->attempt);
    context.Set("idempotencyKey",invocation->idempotency_key);context.Set("runMode",invocation->run_mode);
    context.Set("deadlineRemainingMs",Napi::Number::New(env,static_cast<double>(invocation->deadline)));
    context.Set("isCancelled",Napi::Function::New(env,[completion](const Napi::CallbackInfo& info){
        return Napi::Boolean::New(info.Env(),completion->cancelled());
    }));
    context.Set("commitEffect",Napi::Function::New(env,[completion](const Napi::CallbackInfo& info){
        if(!completion->commit())Napi::Error::New(info.Env(),"Unable to commit effect").ThrowAsJavaScriptException();
        return info.Env().Undefined();
    }));
    try{
        Napi::Object json=env.Global().Get("JSON").As<Napi::Object>();
        Napi::Value input=json.Get("parse").As<Napi::Function>().Call(
            json,{Napi::String::New(env,invocation->input)});
        if(env.IsExceptionPending()){env.GetAndClearPendingException();completion->fail();return;}
        Napi::Value value=callback.Call({context,input});
        if(env.IsExceptionPending()){env.GetAndClearPendingException();completion->fail();return;}
        if(value.IsPromise()){
            Napi::Object promise=value.As<Napi::Object>();
            Napi::Function resolve=Napi::Function::New(env,[completion](const Napi::CallbackInfo& info){
                settle_success(info.Env(),completion,info.Length()?info[0]:info.Env().Undefined());
                return info.Env().Undefined();
            });
            Napi::Function reject=Napi::Function::New(env,[completion](const Napi::CallbackInfo& info){
                completion->fail();return info.Env().Undefined();
            });
            promise.Get("then").As<Napi::Function>().Call(promise,{resolve,reject});
            if(env.IsExceptionPending()){env.GetAndClearPendingException();completion->fail();}
        }else settle_success(env,completion,value);
    }catch(...){completion->fail();}
}
dage_status_t async_executor(const dage_execution_context_t* context,dage_string_view_t input,
                             dage_executor_completion_handle handle,void* userdata){
    Registration* registration=static_cast<Registration*>(userdata);
    std::unique_ptr<Invocation> invocation(new Invocation());
    invocation->completion=std::make_shared<Completion>(handle);
    invocation->run_id.assign(context->run_id.data,context->run_id.size);
    invocation->node_id.assign(context->node_id.data,context->node_id.size);
    invocation->node_type.assign(context->node_type.data,context->node_type.size);
    invocation->idempotency_key.assign(context->idempotency_key.data,context->idempotency_key.size);
    invocation->run_mode.assign(context->run_mode.data,context->run_mode.size);
    invocation->input.assign(input.data,input.size);invocation->attempt=context->attempt;
    invocation->deadline=context->deadline_remaining_ms;
    napi_status status=registration->tsfn.NonBlockingCall(
        invocation.get(),[](Napi::Env env,Napi::Function callback,Invocation* value){
            dispatch_invocation(env,callback,value);
        });
    if(status!=napi_ok)return DAGE_STATUS_EXECUTION_ERROR;
    invocation.release();return DAGE_STATUS_OK;
}
void destroy_registration(void* userdata){
    std::unique_ptr<Registration> registration(static_cast<Registration*>(userdata));
    registration->tsfn.Release();
}

struct RunControl {
    std::mutex mutex;dage_run_handle run=NULL;bool cancel_requested=false;std::string reason;
    void set(dage_run_handle value){
        std::lock_guard<std::mutex> lock(mutex);run=value;
        if(cancel_requested&&run){dage_string_view_t view={reason.data(),reason.size()};api().cancel(run,view);}
    }
    void clear(){std::lock_guard<std::mutex> lock(mutex);run=NULL;}
    void cancel(const std::string& value){
        std::lock_guard<std::mutex> lock(mutex);cancel_requested=true;reason=value;
        if(run){dage_string_view_t view={reason.data(),reason.size()};api().cancel(run,view);}
    }
};
class EngineWrap;
class RunWorker:public Napi::AsyncWorker {
public:
    RunWorker(EngineWrap* owner,Napi::Env env,std::string workflow,std::string input,
              std::shared_ptr<RunControl> control);
    ~RunWorker()override;
    Napi::Promise Promise(){return deferred_.Promise();}
    void Execute()override;
    void OnOK()override;
    void OnError(const Napi::Error& error)override;
private:
    EngineWrap* owner_;Napi::Promise::Deferred deferred_;std::string workflow_,input_,result_;
    std::shared_ptr<RunControl> control_;
};

class EngineWrap:public Napi::ObjectWrap<EngineWrap> {
public:
    static Napi::FunctionReference constructor;
    static void Init(Napi::Env env,Napi::Object exports){
        Napi::Function value=DefineClass(env,"Engine",{
            InstanceMethod("registerAsyncExecutor",&EngineWrap::Register),
            InstanceMethod("startRun",&EngineWrap::StartRun),
            InstanceMethod("close",&EngineWrap::Close)});
        constructor=Napi::Persistent(value);constructor.SuppressDestruct();exports.Set("Engine",value);
    }
    explicit EngineWrap(const Napi::CallbackInfo& info):Napi::ObjectWrap<EngineWrap>(info),
        engine_(NULL),active_(0),closed_(false){
        if(!api().valid()||api().create(NULL,&engine_)!=DAGE_STATUS_OK)
            Napi::Error::New(info.Env(),"Unable to create DAGE Engine").ThrowAsJavaScriptException();
    }
    ~EngineWrap()override{if(engine_)api().destroy_engine(engine_);}
    dage_engine_handle engine()const{return engine_;}
    void WorkerStarted(){active_.fetch_add(1);Ref();}
    void WorkerFinished(){active_.fetch_sub(1);Unref();}
private:
    Napi::Value Register(const Napi::CallbackInfo& info){
        if(closed_||!engine_){Napi::Error::New(info.Env(),"Engine is closed").ThrowAsJavaScriptException();
            return info.Env().Undefined();}
        if(info.Length()!=2||!info[0].IsString()||!info[1].IsFunction()){
            Napi::TypeError::New(info.Env(),"name and callback are required").ThrowAsJavaScriptException();
            return info.Env().Undefined();}
        std::string name=info[0].As<Napi::String>().Utf8Value();
        std::unique_ptr<Registration> registration(new Registration{
            Napi::ThreadSafeFunction::New(info.Env(),info[1].As<Napi::Function>(),
                                          "DAGE async Executor",0,1)});
        dage_string_view_t view={name.data(),name.size()};
        Registration* transferred=registration.release();
        dage_status_t status=api().register_async(engine_,view,async_executor,
                                                   transferred,destroy_registration);
        if(status!=DAGE_STATUS_OK){
            Napi::Error::New(info.Env(),"Registration failed").ThrowAsJavaScriptException();}
        return info.Env().Undefined();
    }
    Napi::Value StartRun(const Napi::CallbackInfo& info){
        if(closed_||!engine_){Napi::Error::New(info.Env(),"Engine is closed").ThrowAsJavaScriptException();
            return info.Env().Undefined();}
        if(info.Length()!=2||!info[0].IsString()||!info[1].IsString()){
            Napi::TypeError::New(info.Env(),"workflow and input JSON are required").ThrowAsJavaScriptException();
            return info.Env().Undefined();}
        std::shared_ptr<RunControl> control=std::make_shared<RunControl>();
        RunWorker* worker=new RunWorker(this,info.Env(),info[0].As<Napi::String>().Utf8Value(),
                                        info[1].As<Napi::String>().Utf8Value(),control);
        Napi::Object operation=Napi::Object::New(info.Env());operation.Set("promise",worker->Promise());
        operation.Set("cancel",Napi::Function::New(info.Env(),[control](const Napi::CallbackInfo& call){
            std::string reason=call.Length()&&call[0].IsString()?
                call[0].As<Napi::String>().Utf8Value():"Node.js AbortSignal cancelled";
            control->cancel(reason);return call.Env().Undefined();
        }));
        WorkerStarted();worker->Queue();return operation;
    }
    Napi::Value Close(const Napi::CallbackInfo& info){
        if(active_.load()!=0){Napi::Error::New(info.Env(),"Engine has active Runs").ThrowAsJavaScriptException();
            return info.Env().Undefined();}
        if(engine_){api().destroy_engine(engine_);engine_=NULL;}closed_=true;return info.Env().Undefined();
    }
    dage_engine_handle engine_;std::atomic<unsigned> active_;bool closed_;
};
Napi::FunctionReference EngineWrap::constructor;

RunWorker::RunWorker(EngineWrap* owner,Napi::Env env,std::string workflow,std::string input,
                     std::shared_ptr<RunControl> control)
    :Napi::AsyncWorker(env),owner_(owner),deferred_(Napi::Promise::Deferred::New(env)),
     workflow_(std::move(workflow)),input_(std::move(input)),control_(std::move(control)){}
RunWorker::~RunWorker(){}
void RunWorker::Execute(){
    dage_workflow_handle workflow=NULL;dage_run_handle run=NULL;
    dage_string_view_t document={workflow_.data(),workflow_.size()};
    if(api().load(owner_->engine(),document,&workflow)!=DAGE_STATUS_OK){
        SetError("Workflow load failed");return;}
    struct WorkflowGuard{dage_workflow_handle value;~WorkflowGuard(){api().destroy_workflow(value);}} wg={workflow};
    if(api().create_run(owner_->engine(),workflow,&run)!=DAGE_STATUS_OK){SetError("Run create failed");return;}
    struct RunGuard{dage_run_handle value;std::shared_ptr<RunControl> control;
        ~RunGuard(){control->clear();api().destroy_run(value);}} rg={run,control_};
    control_->set(run);dage_string_view_t input={input_.data(),input_.size()};size_t required=0;
    dage_status_t status=api().execute(run,input,NULL,0,&required);
    if(status!=DAGE_STATUS_BUFFER_TOO_SMALL){SetError(status==DAGE_STATUS_CANCELLED?
        "Run cancelled":"Run execution failed");return;}
    std::vector<char> output(required);status=api().execute(run,input,output.data(),output.size(),&required);
    if(status!=DAGE_STATUS_OK){SetError(status==DAGE_STATUS_CANCELLED?
        "Run cancelled":"Run execution failed");return;}
    result_.assign(output.data(),required?required-1:0);
}
void RunWorker::OnOK(){
    Napi::Object json=Env().Global().Get("JSON").As<Napi::Object>();
    Napi::Value value=json.Get("parse").As<Napi::Function>().Call(
        json,{Napi::String::New(Env(),result_)});
    if(Env().IsExceptionPending()){
        Napi::Error error=Env().GetAndClearPendingException();deferred_.Reject(error.Value());
    }else deferred_.Resolve(value);
    owner_->WorkerFinished();
}
void RunWorker::OnError(const Napi::Error& error){deferred_.Reject(error.Value());owner_->WorkerFinished();}

Napi::Object Init(Napi::Env env,Napi::Object exports){EngineWrap::Init(env,exports);return exports;}
}
NODE_API_MODULE(dage_node,Init)
