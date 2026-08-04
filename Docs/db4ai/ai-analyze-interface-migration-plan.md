# AI_ANALYZE 接口迁移实施计划

**目标：** 将文本生成接口冻结并实现为 `AI_ANALYZE(model_name, prompt [, options_json])`，移除旧
`task_text + input_value + mode` 契约，迁移离线 MTR、真实 MaaS SQL 和设计文档。

**已确认的接口决策：**

- `model_name`、`prompt` 必填；第三参数可省略。
- `options_json` 仅接受 `max_output_tokens`、`timeout_ms`；不接受 `mode`、`output_format`、
  `return_sources`、Provider 私有字段或模型选择字段。
- 函数返回 `utf8mb4` 文本；不再生成 JSON envelope，也不回显调用方的 RAG 来源元数据。
- 不保留旧三参数签名兼容层。旧调用必须以 `ER_WRONG_ARGUMENTS` 失败，防止同一 arity 被静默解释为
  新语义。
- RAG、SQL 结果分析和 DBA 诊断由调用方完成数据查询、脱敏、访问控制和 prompt 拼装；服务端不执行
  SQL、工具调用或自动修复。

## 执行账本

| 阶段 | 内容 | 状态 | 证据 |
|---|---|---|---|
| 1 | 冻结契约、梳理现有调用点和风险 | 完成 | 本计划；`sql/ai/item_ai_func.cc`、`ai_runtime.*` |
| 2 | 先写新接口 MTR 并确认旧接口失败 | 完成 | `ai_maas_analysis.test` 覆盖新/旧签名与 options |
| 3 | 实现 SQL 层、Runtime 和 fixture | 完成 | `mysqld` 与 `ai_runtime-t` 编译；GUnit 通过 |
| 4 | 迁移真实 MaaS 脚本和产品文档 | 完成 | 三个真实脚本、MTR 和设计/示例文档已更新 |
| 5 | 多轮代码/数据库/AI/文档评审和修复 | 进行中 | 第一轮发现 8 项一致性/测试问题，已修复；等待第二轮复核 |
| 6 | 全量构建、RDS MTR、合入推送 | 进行中 | Debug 全量构建通过；7 个 RDS 用例通过；待二轮复核后合入 |

## Task 1：新 SQL 契约与离线测试（测试先行）

**修改：**

- `mysql-test/suite/rds/t/ai_maas_analysis.test`
- `mysql-test/suite/rds/r/ai_maas_analysis.result`

**验证行为：**

```sql
SELECT AI_ANALYZE('mtr/fixture-chat', 'Summarize the supplied evidence.');
SELECT AI_ANALYZE('mtr/fixture-chat', 'Summarize the supplied evidence.',
                  JSON_OBJECT('max_output_tokens', 256, 'timeout_ms', 60000));
```

- 两参数和三参数新调用返回 fixture 的非空文本。
- 空模型、空 prompt、非对象 options、零值或超范围数值、未知 options 在权限/审计/凭据/HTTP 前失败。
- 旧 `AI_ANALYZE(task, input_json, JSON_OBJECT('model_name', ...))` 失败；该测试使用已注册 fixture，
  防止误判为“模型不存在”。

**Red：** 先只替换用例和 result，运行

```bash
cd build-debug/mysql-test
./mtr --suite=rds ai_maas_analysis
```

预期现有实现因强制第三参、从 options 取 `model_name` 或仍接受旧字段而失败。

## Task 2：SQL 层和 Runtime 最小实现

**修改：**

- `sql/ai/item_ai_func.cc`
- `sql/ai/ai_runtime.h`
- `sql/ai/ai_runtime.cc`
- `sql/ai/ai_runtime_server.cc`
- 必要时同目录单元测试。

**接口：**

```cpp
struct Ai_analyze_options {
  uint32_t max_output_tokens{0};
  uint32_t timeout_ms{0};
};

Ai_error Ai_runtime::Analyze(THD *thd, const std::string &model_name,
                             const std::string &prompt,
                             const Ai_analyze_options &options,
                             std::string *final_content) const;
```

- `resolve_type()` 接受 2 或 3 个字符串参数；第 3 参数可以是 JSON。
- `val_str()` 从第 1 参数取得模型名、第 2 参数取得 prompt；缺失 options 时使用默认 options；只有第
  3 参数通过 `ParseAnalyzeOptions()`。
- `ParseAnalyzeOptions()` 拒绝未知字段并只解析两个正整数上限。
- Runtime 用模型名解析 `TEXT_GENERATION` Profile，固定通用 system policy，把 prompt 原样作为
  user message；返回 Adapter 的最终文本。
- 删除 mode/RAG/diagnose JSON 包装、调用方来源回显和基于 mode 的输出过滤代码；不影响
  Embedding、权限、Profile、凭据、审计、HTTPS、响应上限和 Provider response 校验。
- fixture 仅根据 prompt 生成确定性的普通文本，不使用 prompt 内容改变安全策略。

**Green：** 运行 Task 1 MTR；再编译最小服务端目标和 AI GUnit 集合。

## Task 3：调用点、真实验证脚本和产品文档迁移

**修改：**

- `mysql-test/suite/rds/t/ai_maas_rag.test` 及 result（若存在旧 Analyze 调用）
- `scripts/db4ai_maas_smoke.sql`
- `scripts/db4ai_maas_real_embedding_rag_smoke.sql`
- `scripts/db4ai_maas_generation_model_comparison.sql`
- `Docs/db4ai/README.md`
- `Docs/db4ai/taurusdb-maas-p0-committer-design.md`
- 与公开接口冲突的 `Docs/db4ai` 设计/示例文档。

**真实用例格式：**

```sql
SELECT AI_ANALYZE(
  'huawei/glm-5.2',
  '仅根据以下订单证据总结环比变化、风险和下一步建议。\n\n证据：' ||
  JSON_PRETTY(JSON_OBJECT(...)),
  JSON_OBJECT('max_output_tokens', 512, 'timeout_ms', 60000));
```

RAG 脚本必须在 SQL 中先做向量 Top-K、tenant/标签过滤，再将“问题 + 已选片段 + 来源标识”拼入
prompt；文档明确这是调用方责任，P0 不验证来源真实性。

## Task 4：设计稿的案例和可视化重构

**修改：**

- `Docs/db4ai/taurusdb-maas-p0-committer-design.md`
- 必要时创建 `Docs/db4ai/assets/` 下的 SVG；仅使用可审查、版本可控的 SVG/Markdown，不提交位图。

**要求：**

- “典型应用场景”至少给出三套端到端、可执行的案例：客户支持知识库 RAG、订单经营分析、慢 SQL
  DBA 辅助诊断；每套包含输入数据来源、SQL、返回结果如何被应用消费、权限/费用边界和对应真实/
  离线测试。
- 调研案例用 Aurora、Databricks、PolarDB 的官方链接标明“友商模式 -> TaurusDB 取舍”，不复制其
  Provider JSON。
- 架构图分为“控制面”和“数据面”，视觉上采用统一配色、编号、清晰图例和少量连接线；时序图按调用
  生命周期展示本地拒绝、审计、凭据、出站、响应和终态。图中实线表示代码数据流，虚线表示部署依赖。
- 文档必须把当前已实现、此次实现、未来门禁分栏，避免把计划当成已交付。

## Task 5：检视、验证与交付

- 每次代码任务后分别进行 SQL 契约/内核、AI 协议安全、测试质量的独立评审；发现问题回到原任务
  修复并复审。
- 最后一轮对照本文逐项审核：接口、MTR、真实脚本、设计文档和图中名称一致。
- 在 Debug build 中编译 `mysqld` 与相关 AI GUnit；运行所有 `rds.ai_maas_*` MTR（包含更新后的
  analysis/rag）和文档格式检查。真实 MaaS 请求只保留脚本，不自动发起。
- 保留本文件及最终设计稿的“变更/检视记录”；合入 `ai_maas` 后移除本次隔离 worktree。

## 检视记录

### 第一轮（实现、测试、文档独立检视）

- **API/内核：** 确认新签名、参数顺序、fixture、审计和凭据顺序正确；补充 NULL 短路与重复 JSON
  key 拒绝，拒绝空第三参数。
- **测试/脚本：** 发现 `contract`、`rag` result 仍是旧契约，已用 MTR `--record` 更新并人工复核；
  将真实 smoke 的生成超时由 15 秒统一为 60 秒。
- **文档/AI：** 清理旧 JSON envelope、来源回显、DBA 输出过滤和系统表审计时态；来源改为检索 SQL
  单独返回；所有关键 Mermaid 图替换为统一风格的可审查 SVG。

### 验证证据（2026-08-05）

```text
cmake --build build-debug -j 8                                      # 通过
build-debug/runtime_output_directory/ai_runtime-t                   # 6/6 通过
cd build-debug/mysql-test && ./mtr --suite=rds \
  ai_maas_contract ai_maas_embedding ai_maas_analysis ai_maas_governance \
  ai_maas_rag ai_maas_model_admin ai_maas_model_admin_rpl           # 7 个 RDS + shutdown report 通过
```
