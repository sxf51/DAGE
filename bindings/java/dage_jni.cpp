#include "dage/dage.h"
#include <jni.h>
#include <limits>
#include <string>
#include <vector>

static std::string utf8(JNIEnv* env,jstring value){
    if(!value)return std::string();
    jclass string_class=env->FindClass("java/lang/String");
    jmethodID get_bytes=string_class?env->GetMethodID(
        string_class,"getBytes","(Ljava/lang/String;)[B"):NULL;
    jstring encoding=env->NewStringUTF("UTF-8");
    jbyteArray bytes=get_bytes&&encoding?static_cast<jbyteArray>(
        env->CallObjectMethod(value,get_bytes,encoding)):NULL;
    if(!bytes||env->ExceptionCheck())return std::string();
    const jsize size=env->GetArrayLength(bytes);std::string result(static_cast<std::size_t>(size),'\0');
    if(size)env->GetByteArrayRegion(bytes,0,size,reinterpret_cast<jbyte*>(&result[0]));
    return result;
}
static jstring java_utf8(JNIEnv* env,const char* data,std::size_t size){
    if(size>static_cast<std::size_t>(std::numeric_limits<jsize>::max()))return NULL;
    jbyteArray bytes=env->NewByteArray(static_cast<jsize>(size));if(!bytes)return NULL;
    if(size)env->SetByteArrayRegion(bytes,0,static_cast<jsize>(size),
                                    reinterpret_cast<const jbyte*>(data));
    if(env->ExceptionCheck())return NULL;
    jclass string_class=env->FindClass("java/lang/String");
    jmethodID constructor=string_class?env->GetMethodID(
        string_class,"<init>","([BLjava/lang/String;)V"):NULL;
    jstring encoding=env->NewStringUTF("UTF-8");
    return constructor&&encoding?static_cast<jstring>(
        env->NewObject(string_class,constructor,bytes,encoding)):NULL;
}
struct JavaAsyncExecutor {
    JavaVM* vm;
    jobject bridge;
    jclass context_class;
    jmethodID context_constructor;
    jmethodID start;
};
static JNIEnv* callback_env(JavaVM* vm,bool* attached){
    *attached=false;JNIEnv* env=NULL;
    jint status=vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
    if(status==JNI_EDETACHED){
        if(vm->AttachCurrentThread(reinterpret_cast<void**>(&env),NULL)!=JNI_OK)return NULL;
        *attached=true;
    }else if(status!=JNI_OK)return NULL;
    return env;
}
static dage_status_t java_async_execute(
    const dage_execution_context_t* context,dage_string_view_t input,
    dage_executor_completion_handle completion,void* userdata){
    JavaAsyncExecutor* callback=static_cast<JavaAsyncExecutor*>(userdata);bool attached=false;
    JNIEnv* env=callback_env(callback->vm,&attached);if(!env)return DAGE_STATUS_EXECUTION_ERROR;
    dage_status_t status=DAGE_STATUS_OK;
    jobject java_context=callback->context_constructor?env->NewObject(
        callback->context_class,callback->context_constructor,reinterpret_cast<jlong>(completion),
        java_utf8(env,context->run_id.data,context->run_id.size),
        java_utf8(env,context->node_id.data,context->node_id.size),
        java_utf8(env,context->node_type.data,context->node_type.size),
        static_cast<jint>(context->attempt),
        java_utf8(env,context->idempotency_key.data,context->idempotency_key.size),
        java_utf8(env,context->run_mode.data,context->run_mode.size),
        static_cast<jlong>(context->deadline_remaining_ms)):NULL;
    if(!java_context||!callback->start||env->ExceptionCheck()){
        env->ExceptionClear();status=DAGE_STATUS_EXECUTION_ERROR;
    }else{
        env->CallVoidMethod(callback->bridge,callback->start,java_context,
                            java_utf8(env,input.data,input.size));
        if(env->ExceptionCheck()){env->ExceptionClear();status=DAGE_STATUS_EXECUTION_ERROR;}
    }
    if(attached)callback->vm->DetachCurrentThread();
    return status;
}
static void destroy_java_async(void* userdata){
    JavaAsyncExecutor* callback=static_cast<JavaAsyncExecutor*>(userdata);bool attached=false;
    JNIEnv* env=callback_env(callback->vm,&attached);
    if(env){env->DeleteGlobalRef(callback->bridge);
        env->DeleteGlobalRef(callback->context_class);}
    if(attached)callback->vm->DetachCurrentThread();
    delete callback;
}
static jstring output(JNIEnv* env,dage_status_t(*function)(dage_workflow_handle,char*,size_t,size_t*),
                      dage_workflow_handle workflow){
    size_t required=0;if(function(workflow,NULL,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL)return NULL;
    std::vector<char> data(required);if(function(workflow,&data[0],data.size(),&required)!=DAGE_STATUS_OK)return NULL;
    return java_utf8(env,&data[0],required?required-1:0);
}
extern "C" {
JNIEXPORT jlong JNICALL Java_io_dage_Engine_nativeCreate(JNIEnv*,jclass){
    dage_engine_handle engine=NULL;return dage_engine_create(NULL,&engine)==DAGE_STATUS_OK?reinterpret_cast<jlong>(engine):0;
}
JNIEXPORT void JNICALL Java_io_dage_Engine_nativeDestroy(JNIEnv*,jclass,jlong handle){
    dage_engine_destroy(reinterpret_cast<dage_engine_handle>(handle));
}
JNIEXPORT jlong JNICALL Java_io_dage_Engine_nativeLoad(JNIEnv* env,jclass,jlong handle,jstring json){
    std::string text=utf8(env,json);dage_string_view_t view={text.data(),text.size()};dage_workflow_handle workflow=NULL;
    return dage_engine_load(reinterpret_cast<dage_engine_handle>(handle),view,&workflow)==DAGE_STATUS_OK?reinterpret_cast<jlong>(workflow):0;
}
JNIEXPORT jlong JNICALL Java_io_dage_Engine_nativeCreateRun(
    JNIEnv*,jclass,jlong engine,jlong workflow){
    dage_run_handle run=NULL;
    return dage_run_create(
        reinterpret_cast<dage_engine_handle>(engine),
        reinterpret_cast<dage_workflow_handle>(workflow),&run)==DAGE_STATUS_OK
        ?reinterpret_cast<jlong>(run):0;
}
JNIEXPORT void JNICALL Java_io_dage_Engine_nativeRegisterAsyncExecutor(
    JNIEnv* env,jclass,jlong engine,jstring name,jobject bridge){
    std::string executor_name=utf8(env,name);
    JavaAsyncExecutor* callback=new JavaAsyncExecutor();
    jclass local_context=env->FindClass("io/dage/AsyncExecutionContext");
    jclass bridge_class=env->GetObjectClass(bridge);
    if(env->GetJavaVM(&callback->vm)!=JNI_OK){
        delete callback;
        jclass type=env->FindClass("io/dage/DageException");
        if(type)env->ThrowNew(type,"Unable to access Java VM for async Executor");
        return;
    }
    callback->bridge=env->NewGlobalRef(bridge);
    callback->context_class=local_context?static_cast<jclass>(
        env->NewGlobalRef(local_context)):NULL;
    callback->context_constructor=callback->context_class?env->GetMethodID(
        callback->context_class,"<init>",
        "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;"
        "Ljava/lang/String;J)V"):NULL;
    callback->start=bridge_class?env->GetMethodID(
        bridge_class,"start","(Lio/dage/AsyncExecutionContext;Ljava/lang/String;)V"):NULL;
    if(!callback->bridge||!callback->context_class||!callback->context_constructor||
       !callback->start||env->ExceptionCheck()){
        if(env->ExceptionCheck())env->ExceptionClear();
        if(callback->bridge)env->DeleteGlobalRef(callback->bridge);
        if(callback->context_class)env->DeleteGlobalRef(callback->context_class);
        delete callback;
        jclass type=env->FindClass("io/dage/DageException");
        if(type)env->ThrowNew(type,"Unable to initialize Java async Executor bridge");
        return;
    }
    dage_string_view_t view={executor_name.data(),executor_name.size()};
    dage_status_t status=dage_engine_register_async_executor(
        reinterpret_cast<dage_engine_handle>(engine),view,&java_async_execute,
        callback,&destroy_java_async);
    if(status!=DAGE_STATUS_OK){
        env->DeleteGlobalRef(callback->bridge);
        env->DeleteGlobalRef(callback->context_class);delete callback;
        jclass type=env->FindClass("io/dage/DageException");
        if(type){std::string message="DAGE status "+std::to_string(static_cast<int>(status));
            env->ThrowNew(type,message.c_str());}
    }
}
JNIEXPORT jstring JNICALL Java_io_dage_Engine_nativeLastError(JNIEnv* env,jclass,jlong handle){
    size_t required=0;dage_last_error(reinterpret_cast<dage_engine_handle>(handle),NULL,0,&required);
    std::vector<char> data(required?required:1);dage_last_error(reinterpret_cast<dage_engine_handle>(handle),&data[0],data.size(),&required);
    return java_utf8(env,&data[0],required?required-1:0);
}
JNIEXPORT jstring JNICALL Java_io_dage_Workflow_nativeMermaid(JNIEnv* env,jclass,jlong handle){
    return output(env,dage_workflow_export_mermaid,reinterpret_cast<dage_workflow_handle>(handle));
}
JNIEXPORT void JNICALL Java_io_dage_Workflow_nativeDestroy(JNIEnv*,jclass,jlong handle){
    dage_workflow_destroy(reinterpret_cast<dage_workflow_handle>(handle));
}
JNIEXPORT jstring JNICALL Java_io_dage_Run_nativeExecute(
    JNIEnv* env,jclass,jlong handle,jstring input){
    std::string text=utf8(env,input);dage_string_view_t view={text.data(),text.size()};
    size_t required=0;dage_status_t status=dage_run_execute(
        reinterpret_cast<dage_run_handle>(handle),view,NULL,0,&required);
    if(status!=DAGE_STATUS_BUFFER_TOO_SMALL){
        jclass type=env->FindClass("io/dage/DageException");
        if(type){std::string message="DAGE status "+std::to_string(static_cast<int>(status));
            env->ThrowNew(type,message.c_str());}
        return NULL;
    }
    std::vector<char> data(required);
    status=dage_run_execute(reinterpret_cast<dage_run_handle>(handle),view,
                            data.empty()?NULL:&data[0],data.size(),&required);
    if(status!=DAGE_STATUS_OK){
        jclass type=env->FindClass("io/dage/DageException");
        if(type){std::string message="DAGE status "+std::to_string(static_cast<int>(status));
            env->ThrowNew(type,message.c_str());}
        return NULL;
    }
    return java_utf8(env,data.empty()?"":&data[0],required?required-1:0);
}
JNIEXPORT void JNICALL Java_io_dage_Run_nativeCancel(
    JNIEnv* env,jclass,jlong handle,jstring reason){
    std::string text=utf8(env,reason);dage_string_view_t view={text.data(),text.size()};
    dage_status_t status=dage_run_cancel_with_reason(
        reinterpret_cast<dage_run_handle>(handle),view);
    if(status!=DAGE_STATUS_OK){
        jclass type=env->FindClass("io/dage/DageException");
        if(type){std::string message="DAGE status "+std::to_string(static_cast<int>(status));
            env->ThrowNew(type,message.c_str());}
    }
}
JNIEXPORT void JNICALL Java_io_dage_Run_nativeDestroy(JNIEnv*,jclass,jlong handle){
    dage_run_destroy(reinterpret_cast<dage_run_handle>(handle));
}
JNIEXPORT jboolean JNICALL Java_io_dage_AsyncExecutionContext_nativeIsCancelled(
    JNIEnv*,jclass,jlong completion){
    return dage_executor_completion_is_cancelled(
        reinterpret_cast<dage_executor_completion_handle>(completion))?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_io_dage_AsyncExecutionContext_nativeCommitEffect(
    JNIEnv* env,jclass,jlong completion){
    dage_status_t status=dage_executor_completion_commit_effect(
        reinterpret_cast<dage_executor_completion_handle>(completion));
    if(status!=DAGE_STATUS_OK){
        jclass type=env->FindClass("io/dage/DageException");
        if(type)env->ThrowNew(type,"Unable to commit async Executor effect");
    }
}
JNIEXPORT void JNICALL Java_io_dage_AsyncExecutionContext_nativeComplete(
    JNIEnv* env,jclass,jlong completion,jstring output){
    std::string text=utf8(env,output);dage_owned_buffer_t buffer={
        text.data(),text.size(),NULL,NULL};
    dage_status_t status=dage_executor_complete(
        reinterpret_cast<dage_executor_completion_handle>(completion),
        DAGE_STATUS_OK,&buffer);
    if(status!=DAGE_STATUS_OK){
        jclass type=env->FindClass("io/dage/DageException");
        if(type)env->ThrowNew(type,"Unable to complete async Executor");
    }
}
JNIEXPORT void JNICALL Java_io_dage_AsyncExecutionContext_nativeFail(
    JNIEnv*,jclass,jlong completion,jstring){
    dage_executor_complete(
        reinterpret_cast<dage_executor_completion_handle>(completion),
        DAGE_STATUS_EXECUTION_ERROR,NULL);
}
JNIEXPORT void JNICALL Java_io_dage_AsyncExecutionContext_nativeAbandon(
    JNIEnv*,jclass,jlong completion){
    dage_executor_abandon(reinterpret_cast<dage_executor_completion_handle>(completion));
}
}
