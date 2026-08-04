# DAGE 技术设计与开发维护文档

> 项目名称：DAGE  
> 英文全称：Directed Agent Graph Engine  
> 中文昵称：大哥  
> 技术栈：C++17、JSON、稳定 C ABI、多语言胶水层  
> 文档版本：0.2.0

## 1. 项目定位

DAGE 是一个可嵌入其他应用的 Agent 工作流编排运行时。工作流由 JSON 描述，节点可以调用 LLM、外部工具、本地函数、数据转换器、子工作流或人工审批，并通过 `next`、`on_error`、条件分支、重试、超时、有界循环、并行与汇合关系连接。

DAGE 内部使用 C++17 实现，对外提供稳定 C ABI。Python、Java、C#、Rust 和 Node.js 等语言通过胶水层调用 C ABI，不直接依赖 C++ ABI。

核心口号：

> 工作流有问题，找大哥。

## 2. 目标与非目标

### 2.1 第一阶段目标

1. 加载 JSON 工作流。
2. 规范化合法简写。
3. 静态验证节点、引用、类型和图结构。
4. 编译为不可变 Workflow IR。
5. 注册并调用 Executor。
6. 支持串行执行、条件分支、`next` 和 `on_error`。
7. 支持 timeout、retry、cancel 和有界 loop。
8. 支持运行状态、诊断和事件。
9. 导出 Mermaid 和 Graphviz DOT。
10. 提供 C++ API、稳定 C ABI 和 Python 示例绑定。
11. 提供 validate、format、export 和 run CLI。

### 2.2 后续目标

> 实现状态：0.2.0 已引入 Bundle、ResourceProvider 和不可变 IR；分布式 Worker 和动态插件仍属于非目标。

- parallel / join
- subflow
- checkpoint / resume
- human approval
- revision / rollback
- 领域化 Patch
- LLM 自动优化
- 运行历史回放
- 动态插件
- 分布式 Worker

### 2.3 第一阶段非目标

- 图形化编辑器
- BPMN 全量兼容
- 任意脚本执行
- 分布式调度
- 动态下载插件
- 复杂数据库
- 无限制线程池
- 与特定 LLM 厂商绑定
- 同时完成所有语言绑定

## 3. 总体架构

```text
┌──────────────────────────────────────────┐
│ Language Bindings                        │
│ Python / Java / C# / Rust / Node.js      │
├──────────────────────────────────────────┤
│ Stable C ABI                             │
│ opaque handles + plain C data types      │
├──────────────────────────────────────────┤
│ Optional C++ RAII Wrapper                │
├──────────────────────────────────────────┤
│ DAGE C++17 Core                          │
│ Parser / Normalizer / Validator          │
│ Compiler / Runtime / Expression / Export │
└──────────────────────────────────────────┘
```

核心数据流：

```text
workflow.json
  → Parser
  → Normalizer
  → Validator
  → Compiler
  → Workflow IR
      ├─ Runtime
      ├─ Mermaid Exporter
      ├─ DOT Exporter
      ├─ Diagnostics
      └─ Revision/Patch
```

## 4. 核心设计原则

### 4.1 JSON 面向人和 LLM

- 使用标准 JSON。
- 节点 ID 是 `nodes` 对象的键。
- 节点定义与 `next`、`on_error` 放在一起。
- 不使用数组下标作为节点引用。
- 不保存 UI 坐标。
- 不保存密钥。
- 语义唯一，默认值明确。
- 支持局部修改和稳定格式化。

### 4.2 IR 面向运行时

运行时不得反复遍历原始 JSON。加载流程固定为：

```text
Parse → Normalize → Validate → Compile → Execute
```

编译后：

- 节点 ID 转为数字索引；
- Executor 名称转为注册表索引；
- 条件表达式预编译为 AST；
- 输入模板预编译；
- `next` 和 `on_error` 转为边表。

### 4.3 权限由宿主管理

工作流只能引用 Executor 名称。宿主决定：

- 是否注册；
- 是否允许；
- 访问范围；
- 密钥；
- 沙箱；
- 时间、内存、调用次数和成本限制。

工作流不得提升自身权限。

### 4.4 循环必须有界

任何图环都必须声明：

- `id`
- `max_iterations`
- 可选 `on_exhausted`

验证器拒绝未声明的环。

### 4.5 单一语义来源

Runtime、Exporter、CLI 和绑定层共享同一个 Workflow IR，不允许各自重新解释协议。

## 5. C++17 语言规范

### 5.1 应使用

- RAII
- `std::unique_ptr`
- 仅在真实共享时使用 `std::shared_ptr<const T>`
- 移动构造与移动赋值
- `enum class`
- `nullptr`
- `override` / `final`
- `std::function`
- Lambda
- `std::chrono`
- `std::mutex`
- `std::lock_guard`
- `std::condition_variable`
- `std::atomic`

### 5.2 语言基线

允许使用 C++17 标准库，包括 `std::variant`、`std::string_view`、`std::make_unique`、
structured bindings 和 `if constexpr`。Core 不依赖 concepts、coroutines 等 C++20 特性。
公共 C ABI 不暴露任何 C++ 标准库类型。

### 5.3 所有权规则

- `std::unique_ptr<T>`：唯一所有权。
- `std::shared_ptr<const T>`：共享不可变对象。
- `T&`：必存在的非拥有引用。
- `T*`：可空非拥有引用。
- 不用裸 `new/delete` 管理业务对象。
- 公共 API 文档标注 owned、borrowed、transferred。

## 6. 异常与错误

内部异常可用于：

- 构造失败；
- JSON 解析失败；
- 内存分配失败；
- 不可恢复内部错误。

普通节点失败使用显式 `ExecutionResult`，不使用异常控制流程。

异常不得穿过 C ABI：

```cpp
extern "C" dage_status_t dage_workflow_load_file(
    dage_engine_handle engine,
    const char* path,
    dage_workflow_handle* out)
{
    try {
        return dage::c_api::load_workflow(engine, path, out);
    }
    catch (const std::bad_alloc&) {
        return DAGE_STATUS_OUT_OF_MEMORY;
    }
    catch (const std::exception&) {
        return DAGE_STATUS_INTERNAL_ERROR;
    }
    catch (...) {
        return DAGE_STATUS_UNKNOWN_ERROR;
    }
}
```

标准错误：

```cpp
struct Error {
    std::string category;
    std::string code;
    std::string message;
    std::string node_id;
    std::uint32_t attempt;
    bool retryable;
    Value details;
};
```

## 7. 推荐仓库结构

```text
dage/
├── CMakeLists.txt
├── README.md
├── CHANGELOG.md
├── CONTRIBUTING.md
├── SECURITY.md
├── include/dage/
│   ├── c_api/
│   │   ├── dage.h
│   │   ├── types.h
│   │   ├── status.h
│   │   ├── engine.h
│   │   ├── workflow.h
│   │   ├── run.h
│   │   ├── value.h
│   │   ├── diagnostics.h
│   │   ├── executor.h
│   │   └── export.h
│   └── cpp/
│       ├── dage.hpp
│       ├── engine.hpp
│       ├── workflow.hpp
│       ├── run.hpp
│       ├── value.hpp
│       └── error.hpp
├── src/
│   ├── core/
│   ├── parser/
│   ├── normalizer/
│   ├── validator/
│   ├── compiler/
│   ├── runtime/
│   ├── expression/
│   ├── value/
│   ├── executor/
│   ├── export/
│   ├── persistence/
│   ├── c_api/
│   └── util/
├── bindings/
│   ├── python/
│   ├── java/
│   ├── csharp/
│   ├── rust/
│   └── node/
├── tools/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── c_api/
│   ├── abi/
│   ├── bindings/
│   ├── fixtures/
│   ├── invalid_workflows/
│   └── golden/
├── examples/
│   ├── cpp/
│   ├── c/
│   ├── python/
│   └── workflows/
└── docs/
    ├── technical_guide.md
    ├── pipeline_protocol.md
    ├── architecture.md
    ├── cpp_api.md
    ├── c_api.md
    ├── bindings.md
    ├── executor_guide.md
    ├── threading.md
    ├── testing.md
    └── decisions/
```

## 8. 模块职责

### Parser

负责 JSON 读取、UTF-8、大小与深度限制、语法诊断和 JSON 路径。不得处理节点语义。

### Normalizer

将：

- `"next": "write"`
- `"next": {"to":"write"}`
- 简写类型
- 缺失默认值

转换为唯一标准形式。

### Validator

分层检查：

1. 语法和字段；
2. 节点与 Executor 引用；
3. 图结构；
4. 类型；
5. 权限与资源策略。

### Compiler

将规范化配置编译为 Workflow IR，包括节点索引、边、表达式 AST、输入模板和 LoopDescriptor。

### Runtime

负责 Run 生命周期、节点执行、输出验证、transition、retry、timeout、loop、state、cancel 和 event。

### Expression Engine

第一版只支持：

```text
== != > >= < <= and or not
exists empty length contains starts_with ends_with
```

禁止任意代码、文件和网络访问。

### Exporter

从 IR 导出 Mermaid 和 DOT，不读取原始 JSON。

## 9. 核心类建议

```cpp
namespace dage {

class Value;
class Error;
class Diagnostics;
class WorkflowSource;
class NormalizedWorkflow;
class Workflow;
class WorkflowCompiler;
class WorkflowValidator;
class Expression;
class ExecutorRegistry;
class ExecutionContext;
class Run;
class Engine;

}
```

公共 C++ 类建议使用 Pimpl：

```cpp
class Engine {
public:
    Engine();
    ~Engine();

    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
```

## 10. Value 设计

C++17 提供 `std::variant`。公共业务失败统一使用 `Result<T, Error>`，避免异常承担普通控制流：

1. Pimpl 包装内部 JSON Value；
2. Tagged union；
3. 多态节点值。

第一版推荐 Pimpl 或稳定包装，公共接口不暴露第三方 JSON 类型。

```cpp
class Value {
public:
    enum class Type {
        Null, Boolean, Integer, Double, String, Array, Object
    };

    Value();
    ~Value();
    Value(const Value&);
    Value& operator=(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(Value&&) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
```

## 11. C ABI

### Opaque Handle

```c
typedef struct dage_engine_t* dage_engine_handle;
typedef struct dage_workflow_t* dage_workflow_handle;
typedef struct dage_run_t* dage_run_handle;
typedef struct dage_value_t* dage_value_handle;
typedef struct dage_diagnostics_t* dage_diagnostics_handle;
```

### 状态码

```c
typedef enum dage_status_t {
    DAGE_STATUS_OK = 0,
    DAGE_STATUS_INVALID_ARGUMENT = 1,
    DAGE_STATUS_OUT_OF_MEMORY = 2,
    DAGE_STATUS_PARSE_ERROR = 3,
    DAGE_STATUS_VALIDATION_ERROR = 4,
    DAGE_STATUS_EXECUTION_ERROR = 5,
    DAGE_STATUS_CANCELLED = 6,
    DAGE_STATUS_NOT_FOUND = 7,
    DAGE_STATUS_BUFFER_TOO_SMALL = 8,
    DAGE_STATUS_INTERNAL_ERROR = 100,
    DAGE_STATUS_UNKNOWN_ERROR = 101
} dage_status_t;
```

已发布值不得重新编号。

### 可扩展结构体

```c
typedef struct dage_engine_options_t {
    uint32_t struct_size;
    uint32_t api_version;
    const dage_allocator_t* output_allocator;
    uint64_t max_workflow_bytes;
    uint32_t max_json_depth;
    uint32_t max_nodes;
    uint64_t max_edges;
    uint64_t max_expression_bytes;
    uint64_t max_literal_bytes;
    uint64_t max_compiled_ir_bytes;
    void* reserved[2];
} dage_engine_options_t;
```

零值限制使用运行时默认值。所有 Workflow 入口共享这些解析和编译预算；超限返回
`DAGE_STATUS_RESOURCE_EXHAUSTED`。公共头文件是结构布局的唯一权威来源，发布前由
64 位布局测试固定字段偏移。

### 字符串

```c
typedef struct dage_string_view_t {
    const char* data;
    size_t size;
} dage_string_view_t;
```

统一 UTF-8。

### 生命周期

```c
dage_status_t dage_engine_create(
    const dage_engine_options_t* options,
    dage_engine_handle* out);

void dage_engine_destroy(dage_engine_handle engine);
```

### 输出缓冲区

```c
dage_status_t dage_workflow_export_mermaid(
    dage_workflow_handle workflow,
    char* buffer,
    size_t buffer_size,
    size_t* required_size);
```

由库分配的内存只能由库释放。

## 12. Executor

C++：

```cpp
using ExecutorFunction = std::function<ExecutionResult(
    const ExecutionContext&,
    const Value&)>;
```

C ABI：

```c
typedef dage_status_t (*dage_executor_callback_t)(
    dage_execution_context_handle context,
    dage_value_handle input,
    dage_value_handle* output,
    dage_error_handle* error,
    void* userdata);
```

注册必须包含 `userdata` 销毁回调。

第一阶段只支持同步 Executor，但接口设计不得阻碍未来异步扩展。

## 13. 多语言绑定

- Python：CFFI 或 CPython Extension，高级 Pythonic Wrapper。
- Java：JNI、`AutoCloseable`、异常映射。
- C#：P/Invoke、`SafeHandle`、Dispose、delegate 生命周期。
- Rust：bindgen + 安全 Wrapper、Drop、Result。
- Node.js：Node-API，不阻塞主线程。

所有绑定基于 C ABI。各语言包装层不能重新实现工作流语义。

## 14. 线程安全

初始约定：

- 不同 Engine 可并行。
- Workflow 编译后不可变，可共享。
- 同一个 Run 不允许并发操作。
- `cancel()` 线程安全。
- Executor 注册时声明线程安全属性。
- C ABI handle 默认不保证并发安全。
- 不使用全局可变注册表。

## 15. 构建

```cmake
option(DAGE_BUILD_SHARED "Build shared library" ON)
option(DAGE_BUILD_TESTS "Build tests" ON)
option(DAGE_BUILD_TOOLS "Build CLI tools" ON)
option(DAGE_BUILD_EXAMPLES "Build examples" ON)
option(DAGE_BUILD_BINDINGS "Build bindings" OFF)
option(DAGE_ENABLE_SANITIZERS "Enable sanitizers" OFF)
option(DAGE_WARNINGS_AS_ERRORS "Treat warnings as errors for DAGE-owned targets" OFF)
```

要求：

- C++17；
- C ABI 头可由纯 C 编译；
- 默认隐藏内部符号；
- 动态库只导出预期 `dage_` 符号。

## 16. 测试

### Core

- Value 复制和移动；
- RAII；
- Parser；
- Normalizer；
- Expression；
- Validator；
- Transition；
- Retry；
- Loop；
- Export escaping。

### Runtime

- 单节点；
- 串行；
- 条件；
- Retry 成功与耗尽；
- `on_error`；
- timeout；
- cancel；
- loop；
- end。

### C ABI

- 纯 C 编译；
- NULL 参数；
- create/destroy；
- buffer too small；
- 异常不跨 ABI；
- `struct_size`；
- 导出符号。

### Binding

每种绑定至少有 smoke test。

## 17. 里程碑

0. C++17 工程骨架与 ABI 边界。
1. Value、JSON、Error、Diagnostics。
2. Parser 与 Normalizer。
3. Validator、Expression 与 Compiler。
4. 串行 Runtime。
5. Loop 与 State。
6. 稳定 C ABI。
7. Exporter 与 CLI。
8. Python Binding。
9. Parallel、Join、Subflow、Checkpoint。

## 18. 团队协作

推荐 Conventional Commits：

```text
feat(runtime): implement ordered next selection
fix(c-api): prevent exceptions crossing ABI
docs(protocol): clarify loop semantics
test(validator): add undeclared cycle fixture
```

协议变化更新 `pipeline_protocol.md`；C ABI 变化更新 `c_api.md`；重要决策新增 ADR。

## 19. Definition of Done

- 编译通过；
- 正常、错误和边界测试；
- ASan/UBSan 无问题；
- 公共 API 有注释；
- 所有权明确；
- 文档同步；
- 协议和 ABI 影响已说明；
- Review 完成。

## 20. 成功标准

- 新成员一天内理解架构；
- 半天内实现自定义 Executor；
- 简单工作流 50 行以内；
- 非法引用在运行前发现；
- 所有循环有界；
- 自动导出清晰关系图；
- Python 可调用；
- 核心不依赖具体 LLM 服务；
- 至少两个桌面平台通过 CI。
