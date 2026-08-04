# DAGE Pipeline JSON 协议

> 协议版本：0.2.0（pre-1.0，不兼容旧原型）  
> 适用项目：DAGE（Directed Agent Graph Engine）

## 1. 概述

DAGE Pipeline 是一种使用 JSON 描述 Agent 工作流的低代码协议。一个工作流由多个节点构成，每个节点可以调用 LLM、工具、数据转换器、子工作流或人工审批，并通过 `next`、`on_error`、条件、重试、循环和并行关系连接。

协议目标：

- 对 LLM 友好；
- 对人类友好；
- 对 C++ Runtime 友好；
- 可自动导出关系图；
- 支持局部修改和版本管理；
- 不保存 UI 坐标。

## 2. 最小格式

```json
{
  "format": "dage-workflow",
  "format_version": "0.2.0",
  "name": "Hello DAGE",
  "entry": "generate",

  "nodes": {
    "generate": {
      "type": "llm",
      "executor": "writer",
      "next": "finish"
    },

    "finish": {
      "type": "end"
    }
  }
}
```

## 3. 完整顶层结构

```json
{
  "format": "dage-workflow",
  "format_version": "0.2.0",
  "id": "research_agent",
  "name": "Research Agent",
  "description": "Research and answer a question.",
  "entry": "plan",

  "input": {
    "question": "string"
  },

  "output": {
    "answer": "string"
  },

  "limits": {
    "max_steps": 40,
    "timeout_ms": 180000,
    "max_parallel": 4,
    "max_loop_iterations": 3
  },

  "defaults": {
    "timeout_ms": 30000,
    "retry": {
      "max_attempts": 1
    }
  },

  "nodes": {}
}
```

## 4. 顶层字段

### `version`

- 类型：string
- 必填：是
- 使用语义化版本

### `id`

稳定工作流 ID，格式：

```text
[a-z][a-z0-9_]*
```

### `name`

面向人类的显示名称。

### `description`

工作流用途说明。

### `entry`

入口节点 ID，必须存在且启用。

### `input`

工作流输入接口。

### `output`

工作流输出接口。

### `limits`

全局资源限制。

支持：

```text
max_steps
timeout_ms
max_parallel
max_loop_iterations
max_retries
max_output_bytes
```

### `defaults`

节点默认配置。

### `nodes`

节点对象，键即节点 ID。节点内部不重复保存 `id`。

## 5. 类型系统

简写类型：

```text
any
null
boolean
integer
number
string
object
array
string[]
number[]
object[]
```

可选类型在末尾添加 `?`：

```text
string?
object[]?
```

复杂约束使用 JSON Schema：

```json
{
  "output_schema": {
    "type": "object",
    "required": ["score"],
    "properties": {
      "score": {
        "type": "number"
      }
    }
  }
}
```

同一工作流推荐统一使用简写或完整 Schema。

## 6. 节点通用格式

```json
{
  "search": {
    "name": "Search sources",
    "doc": "Find relevant evidence.",

    "type": "tool",
    "executor": "web_search",
    "enabled": true,
    "effects": {
      "kind": "external_read",
      "replay": "safe"
    },

    "input": {
      "queries": "${nodes.plan.output.queries}"
    },

    "config": {
      "max_results": 10
    },

    "output": {
      "results": "object[]"
    },

    "timeout_ms": 30000,

    "retry": {
      "max_attempts": 2,
      "delay_ms": 1000,
      "backoff": "exponential"
    },

    "next": "write",

    "on_error": [
      {
        "to": "repair_search",
        "when": "${error.retryable}"
      },
      {
        "to": "human_review",
        "otherwise": true
      }
    ],

    "tags": ["research", "external"]
  }
}
```

### Effect 与 Replay

`effects.kind` 描述副作用位置：

- `pure`
- `local_state`
- `external_read`
- `external_write`
- `irreversible`

`effects.replay` 描述重试、恢复和历史重放安全性：

- `safe`
- `idempotent`
- `at_most_once`
- `manual`
- `forbidden`

`tool` 和 `custom` 必须显式声明 `effects`。`llm` 默认
`external_read/safe`；`transform`、`condition` 和 `noop` 默认 `pure/safe`。

```json
{
  "type": "tool",
  "executor": "create_order",
  "effects": {
    "kind": "external_write",
    "replay": "idempotent",
    "idempotency_key": "${run.id}:${node.id}",
    "compensation": "cancel_order"
  }
}
```

`pure` 只能配 `safe`；`external_write` 不能配 `safe`；`irreversible` 只能配 `manual` 或
`forbidden`。`idempotent` 必须提供 `idempotency_key`，或设置
`executor_guarantees_idempotency: true`。Shadow Run 默认不执行 `external_write`；
未确认完成的 `at_most_once` 节点不得自动恢复执行。

## 7. 节点字段

### `name`

显示名称。默认使用节点 ID。

### `doc`

说明字段，不参与执行。

### `type`

支持：

```text
llm
tool
transform
condition
parallel
join
subflow
human
start
end
noop
custom
```

### `executor`

宿主注册的执行器名称。

### `enabled`

默认 `true`。禁用节点不可作为入口，并在候选中跳过。

### `input`

节点运行时输入映射。

### `config`

静态配置。

### `output`

输出结构简写。

### `output_schema`

完整输出 Schema。

### `timeout_ms`

单次节点执行超时，单位毫秒，必须大于零。

### `retry`

重试策略。

### `next`

节点成功后的后继。

### `on_error`

节点最终失败后的后继。

### `tags`

分类标签，不影响执行。

## 8. 节点类型

### `llm`

```json
{
  "plan": {
    "type": "llm",
    "executor": "planner",

    "input": {
      "question": "${workflow.input.question}"
    },

    "config": {
      "prompt_template": "research_plan_v1",
      "temperature": 0.1
    },

    "output": {
      "need_search": "boolean",
      "queries": "string[]"
    }
  }
}
```

LLM 节点推荐使用结构化输出，长提示词应外置。

### `tool`

调用宿主工具。

### `transform`

执行确定性转换，如排序、筛选、映射和格式化。

### `condition`

显式条件判断节点。简单条件也可以直接写在前驱 `next` 中。

### `parallel`

启动多个并行分支。

### `join`

合并并行结果。

### `subflow`

调用子工作流。

### `human`

等待人工审批或输入。

### `start`

可选显式入口。

### `end`

结束工作流。

### `noop`

不调用外部执行器，用于标记或汇合。

### `custom`

宿主扩展类型。

## 9. `next`

### 字符串

```json
"next": "write"
```

### 对象

```json
{
  "next": {
    "to": "write"
  }
}
```

### 候选数组

```json
{
  "next": [
    {
      "to": "search",
      "when": "${output.need_search}",
      "label": "Search required"
    },
    {
      "to": "write",
      "otherwise": true,
      "label": "Skip search"
    }
  ]
}
```

数组顺序具有执行语义，不得自动排序。

## 10. 后继对象字段

```json
{
  "to": "target_node",
  "when": "${condition}",
  "otherwise": false,
  "label": "Readable label",
  "mode": "normal",
  "enabled": true,
  "max_hits": 3,

  "loop": {
    "id": "revision_loop",
    "max_iterations": 3,
    "on_exhausted": "human_review"
  },

  "set": {
    "retry_count": "${state.retry_count + 1}"
  }
}
```

字段：

- `to`：目标节点。
- `when`：条件表达式。
- `otherwise`：兜底候选。
- `label`：图和日志中的可读标签。
- `mode`：`normal` 或 `jump_back`。
- `enabled`：是否启用。
- `max_hits`：本次运行最多命中次数。
- `loop`：循环声明。
- `set`：转移前更新状态。

## 11. 后继选择语义

1. 按数组顺序遍历；
2. 跳过禁用边；
3. 跳过不存在或禁用节点；
4. 求值 `when`；
5. 选择第一个条件为真的候选；
6. 无条件命中时选择 `otherwise`；
7. 无后继则当前路径自然结束。

错误示例：

```json
{
  "next": [
    "write",
    {
      "to": "human_review",
      "when": "${output.risky}"
    }
  ]
}
```

`write` 无条件成立，会遮挡后续分支。

## 12. `otherwise`

- 每个候选列表最多一个；
- 必须放最后；
- 不得同时设置 `when`；
- 仅当前面均未命中时生效。

## 13. 条件表达式

```json
"when": "${output.score >= 0.8 and output.safe == true}"
```

支持运算符：

```text
==
!=
>
>=
<
<=
and
or
not
```

支持函数：

```text
exists(value)
empty(value)
length(value)
contains(container, value)
starts_with(string, prefix)
ends_with(string, suffix)
```

禁止：

- JavaScript
- Python
- Shell
- 文件访问
- 网络访问
- 动态函数
- 赋值

## 14. 引用语法

```text
${workflow.input.question}
${input.question}
${output.score}
${error.category}
${nodes.search.output.results}
${nodes.search.error.message}
${state.revision_count}
${run.id}
${run.step}
```

同一含义不得提供多个别名。

整个字符串仅包含一个引用时，应保留原始值类型；嵌入普通文本时，结果为字符串。

## 15. `on_error`

```json
{
  "on_error": [
    {
      "to": "repair_output",
      "when": "${error.category == 'invalid_output'}"
    },
    {
      "to": "wait_and_retry",
      "when": "${error.category == 'rate_limit'}"
    },
    {
      "to": "human_review",
      "otherwise": true
    }
  ]
}
```

处理顺序：

1. 节点失败；
2. 根据 `retry` 重试；
3. 重试耗尽；
4. 构造标准错误；
5. 按顺序选择 `on_error`；
6. 无匹配则工作流失败。

## 16. Retry

```json
{
  "retry": {
    "max_attempts": 3,
    "delay_ms": 1000,
    "backoff": "exponential",
    "max_delay_ms": 10000,
    "on": [
      "timeout",
      "rate_limit",
      "temporary_error"
    ]
  }
}
```

- `max_attempts` 包含首次执行。
- 支持 `fixed`、`linear`、`exponential`。
- `max_attempts: 1` 表示不重试。

## 17. 标准错误对象

```json
{
  "category": "invalid_output",
  "code": "SCHEMA_VALIDATION_FAILED",
  "message": "Required field 'queries' is missing.",
  "node": "plan",
  "attempt": 2,
  "retryable": true,
  "details": {}
}
```

标准类别：

```text
timeout
cancelled
invalid_input
invalid_output
executor_not_found
permission_denied
rate_limit
temporary_error
tool_error
model_error
condition_error
resource_limit
internal_error
```

## 18. Loop

所有图循环必须显式声明：

```json
{
  "to": "revise",
  "when": "${output.score < 0.8}",

  "loop": {
    "id": "answer_revision",
    "max_iterations": 3,
    "on_exhausted": "human_review"
  }
}
```

验证器拒绝未声明环。

## 19. JumpBack

```json
{
  "to": "repair_context",
  "mode": "jump_back",
  "max_hits": 2
}
```

语义：

1. 记录返回节点；
2. 进入修复路径；
3. 修复路径自然结束；
4. 返回原节点；
5. 重新判断原节点的后继。

## 20. State

```json
{
  "to": "search",
  "set": {
    "search_attempt": "${state.search_attempt + 1}"
  }
}
```

State 只保存小型控制信息，大型数据保存在节点输出或外部存储。

## 21. Parallel

```json
{
  "collect_sources": {
    "type": "parallel",

    "branches": [
      "search_web",
      "search_files"
    ],

    "join": "merge_sources",

    "policy": {
      "mode": "all",
      "fail": "collect"
    }
  }
}
```

模式：

```text
all
any
min_success
```

失败策略：

```text
fast
wait
collect
```

## 22. Join

```json
{
  "merge_sources": {
    "type": "join",
    "executor": "merge_sources",

    "input": {
      "web": "${nodes.search_web.output.results}",
      "files": "${nodes.search_files.output.results}"
    }
  }
}
```

## 23. Subflow

```json
{
  "research": {
    "type": "subflow",

    "config": {
      "workflow": "research.json",
      "entry": "plan"
    },

    "input": {
      "question": "${workflow.input.question}"
    }
  }
}
```

父工作流只能读取子工作流公开输出。

## 24. Human

```json
{
  "approve": {
    "type": "human",
    "executor": "approval_request",

    "input": {
      "draft": "${nodes.write.output.answer}"
    },

    "output": {
      "approved": "boolean",
      "feedback": "string?"
    }
  }
}
```

必须支持挂起、恢复、取消和超时。

## 25. End

```json
{
  "finish": {
    "type": "end",

    "input": {
      "answer": "${nodes.write.output.answer}"
    }
  }
}
```

End 不得有 `next`。

## 26. 注释与扩展字段

标准 JSON 不支持注释。使用：

```json
{
  "doc": "Human-readable explanation.",
  "input_doc": "Input description."
}
```

扩展字段必须使用 `x_`：

```json
{
  "x_owner": "agent_team"
}
```

未知且非 `x_` 字段应产生警告或错误。

## 27. 完整示例

```json
{
  "format": "dage-workflow",
  "format_version": "0.2.0",
  "id": "research_agent",
  "name": "Research Agent",
  "entry": "plan",

  "input": {
    "question": "string"
  },

  "output": {
    "answer": "string",
    "sources": "object[]"
  },

  "limits": {
    "max_steps": 40,
    "timeout_ms": 180000,
    "max_parallel": 3,
    "max_loop_iterations": 3
  },

  "nodes": {
    "plan": {
      "name": "Plan research",
      "type": "llm",
      "executor": "planner",

      "input": {
        "question": "${workflow.input.question}"
      },

      "output": {
        "need_search": "boolean",
        "queries": "string[]"
      },

      "next": [
        {
          "to": "collect_sources",
          "when": "${output.need_search}",
          "label": "Research"
        },
        {
          "to": "write",
          "otherwise": true,
          "label": "Write directly"
        }
      ]
    },

    "collect_sources": {
      "type": "parallel",
      "branches": ["search_web", "search_files"],
      "join": "merge_sources",
      "policy": {
        "mode": "all",
        "fail": "collect"
      }
    },

    "search_web": {
      "type": "tool",
      "executor": "web_search",

      "input": {
        "queries": "${nodes.plan.output.queries}"
      },

      "output": {
        "results": "object[]"
      },

      "retry": {
        "max_attempts": 2,
        "delay_ms": 1000,
        "backoff": "exponential"
      },

      "next": "merge_sources",
      "on_error": "merge_sources"
    },

    "search_files": {
      "type": "tool",
      "executor": "file_search",

      "input": {
        "question": "${workflow.input.question}"
      },

      "output": {
        "results": "object[]"
      },

      "next": "merge_sources",
      "on_error": "merge_sources"
    },

    "merge_sources": {
      "type": "join",
      "executor": "merge_sources",

      "input": {
        "web": "${nodes.search_web.output.results}",
        "files": "${nodes.search_files.output.results}"
      },

      "output": {
        "sources": "object[]"
      },

      "next": "write"
    },

    "write": {
      "type": "llm",
      "executor": "writer",

      "input": {
        "question": "${workflow.input.question}",
        "sources": "${nodes.merge_sources.output.sources}"
      },

      "output": {
        "answer": "string"
      },

      "next": "review"
    },

    "review": {
      "type": "llm",
      "executor": "reviewer",

      "input": {
        "answer": "${nodes.write.output.answer}"
      },

      "output": {
        "approved": "boolean",
        "score": "number",
        "feedback": "string"
      },

      "next": [
        {
          "to": "finish",
          "when": "${output.approved and output.score >= 0.8}",
          "label": "Approved"
        },
        {
          "to": "revise",
          "otherwise": true,
          "label": "Revise",
          "loop": {
            "id": "answer_revision",
            "max_iterations": 3,
            "on_exhausted": "human_review"
          }
        }
      ]
    },

    "revise": {
      "type": "llm",
      "executor": "writer",

      "input": {
        "answer": "${nodes.write.output.answer}",
        "feedback": "${nodes.review.output.feedback}"
      },

      "output": {
        "answer": "string"
      },

      "next": "review"
    },

    "human_review": {
      "type": "human",
      "executor": "approval_request",

      "input": {
        "answer": "${nodes.revise.output.answer}"
      },

      "output": {
        "approved": "boolean",
        "answer": "string?"
      },

      "next": [
        {
          "to": "finish",
          "when": "${output.approved}"
        },
        {
          "to": "rejected",
          "otherwise": true
        }
      ]
    },

    "finish": {
      "type": "end",
      "input": {
        "answer": "${nodes.revise.output.answer}",
        "sources": "${nodes.merge_sources.output.sources}"
      }
    },

    "rejected": {
      "type": "end",
      "input": {
        "answer": "",
        "sources": []
      }
    }
  }
}
```

## 28. 可视化规则

图由语义关系自动布局，不保存坐标。

边来源：

- `next`
- `on_error`
- `branches`
- `join`
- `loop`
- `jump_back`

推荐样式：

- 普通边：实线；
- 条件边：实线 + label；
- 错误边：虚线；
- 循环：回弧或粗线；
- JumpBack：点划线；
- End：双圆；
- Condition：菱形；
- Parallel：分叉；
- Join：汇合。

## 29. 编写最佳实践

1. 节点职责单一。
2. 重要异常分支放在候选前面。
3. 多分支提供 `otherwise`。
4. 不让无条件分支遮挡后续。
5. 循环显式声明。
6. 权限不写入工作流。
7. 长提示词外置。
8. 控制信息使用结构化输出。
9. 确定性任务不用 LLM。
10. 大型流程拆为子工作流。
11. 节点 ID 使用 snake_case。
12. 不依赖对象键顺序。
13. 不排序 `next` 和 `on_error`。
14. 修改后运行静态验证。
15. LLM 优先提交局部 Patch。

## 30. 静态验证

必须检查：

- JSON 合法；
- 协议版本支持；
- Entry 存在；
- Node ID 合法；
- 节点引用存在；
- Executor 注册且允许；
- 不可达节点；
- 多个 `otherwise`；
- Shadowed branch；
- 未声明环；
- End 无后继；
- 类型兼容；
- Timeout 和 Retry 合法；
- Parallel 可达 Join。

## 31. 诊断格式

```json
{
  "severity": "error",
  "code": "UNKNOWN_NODE_REFERENCE",
  "path": "/nodes/review/next/1/to",
  "message": "Node 'revise_answer' does not exist.",
  "suggestion": "Add the node or change the target."
}
```

严重级别：

```text
error
warning
info
```

## 32. LLM 修改规范

模型不应默认重写整个工作流。

```json
{
  "base_digest": "sha256:current-compiled-workflow-digest",
  "base_revision": 12,
  "reason": "Add recovery for invalid search output.",

  "changes": [
    {
      "action": "add_node",
      "node_id": "repair_search",
      "node": {
        "type": "llm",
        "executor": "query_repair"
      }
    },
    {
      "action": "set_on_error",
      "node_id": "search",
      "value": "repair_search"
    }
  ]
}
```

`base_digest` 必须精确匹配被修改的不可变 Workflow。`base_revision` 是可选的附加检查，
不能替代 digest。应用方必须先执行事务性 dry-run，再发布候选 Workflow。

`x_revision` 用于发布顺序和乐观并发检查，但不参与编译 IR 的语义 digest。inverse Patch
恢复原语义时 revision 继续单调增长，不允许倒退。

推荐支持：

```text
add_node
remove_node
replace_node
rename_node
set_field
remove_field
set_next
insert_next
remove_next
set_on_error
enable_node
disable_node
```

## 33. 规范化保存

- UTF-8；
- 两空格缩进；
- 保留候选数组顺序；
- 节点字段按固定顺序；
- 文件末尾换行；
- 相同输入产生稳定输出；
- 不写 UI 坐标。
