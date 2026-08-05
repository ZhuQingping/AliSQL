# AliSQL DB4AI 控制面迁移与文件审计实施计划

目标：将现有 AliSQL DB4AI 原型从 tenant/系统表审计模型迁移到 TaurusDB P0 的实例级模型
Profile 和两阶段文件审计模型，使主节点与只读节点均可调用 MaaS 而不写 AI 审计系统表。

范围仅包含 HLD 的“控制面迁移”和“新的审计方案”。Embedding、Chat 协议、RAG 产品封装、
异步任务、Provider 扩展和完整 HTTP Trace 不在本阶段实现。

## 约束

- Runtime 只读取 `mysql.ai_model_config`，不再读取
  `alisql_ai_tenant_account`、`alisql_ai_tenant_binding` 或
  `alisql_ai_call_audit`。
- `AI_INVOKE` 是唯一调用权限；`AI_ADMIN` 用于 Profile 管理。不提供
  `AI_AUDIT_INFO()` 文件读取。
- `ai_invoke_audit` 是默认 ON 的动态 GLOBAL 布尔变量；不注册 SESSION 作用域。
- 审计文件由启动参数 `--ai-invoke-audit-log-file` 配置；未配置时位于 datadir，普通 SQL
  会话不得修改。
- 记录 JSON Lines。HTTPS 出站前必须安全落盘 `AI_CALL_STARTED`；失败则禁止出站。终态失败
  返回审计错误，已有开始记录按 `call_id` 视为 `UNKNOWN`。
- 审计中不得出现 API Key、Authorization、credential reference、完整 prompt、完整响应、
  原始 embedding 或 provider 原始错误正文。
- 旧系统表不自动删除：先创建新表和迁移数据，待运行时不再依赖旧表并完成备份验证后，由
  独立运维变更删除。

## 阶段 1：两阶段审计接口和失败语义

文件：`sql/ai/ai_audit.h`、`sql/ai/ai_audit.cc`、`sql/ai/ai_runtime_server.cc`、新建
`unittest/gunit/ai_audit-t.cc`、`unittest/gunit/CMakeLists.txt`。

1. 先写失败 GUnit：Start 失败时 adapter 不得执行；Start 与 Complete 共享 call_id；provider
   成功但 Complete 失败时返回 `k_audit_unavailable`。
2. 将表审计接口抽象为 `Start()` 和 `Complete()`，保持 Runtime 唯一顺序：Start、provider
   dispatch、Complete。
3. 运行 GUnit 及 `rds.ai_maas_embedding`、`rds.ai_maas_analysis`。验收为 Start 失败时
   dispatch 计数为零，终态失败不被误报为未调用。

## 阶段 2：实例级 Profile 和升级路径

文件：`scripts/mysql_system_tables.sql`、`scripts/mysql_system_tables_fix.sql`、
`sql/ai/ai_model_registry.h`、`sql/ai/ai_model_registry.cc`、`sql/ai/item_ai_func.cc`、
`unittest/gunit/ai_model_registry-t.cc`、`ai_maas_contract.test/.result`。

1. 先写失败测试：仅新表中的 ACTIVE Profile 可解析；无 `AI_INVOKE` 在打开 Profile 前拒绝；
   每 capability 一个 ACTIVE 默认模型；Profile 停用后后续调用失败。
2. 创建 `mysql.ai_model_config`，包含 HLD 的 Profile、凭据、版本、维度、生成限制
   和生命周期字段及默认模型索引。
3. 在 upgrade SQL 中迁移可转换的旧 Profile 到实例级 Profile，保留旧表，不迁移 tenant/
   account 绑定；Debug `PLAINTEXT_DEV` 只在 Debug 路径保留。
4. Resolver 按 `model_name + capability + ACTIVE` 或 capability 默认模型读取新表，删除
   tenant/binding 查找，保留冻结的 `Id/config_version`。
5. 删除 `AI_AUDIT_INFO()` 的系统表扫描与 SQL 注册；MTR fixture 改为新 Profile 表和
   动态权限/生命周期断言。

验收：运行时对三个旧 AI 系统表零读写；显式和默认模型均可解析；停用立即生效；旧表仍可
作为升级回退备份。

## 阶段 3：文件 Sink 和全局开关

文件：`sql/ai/ai_audit.h`、`sql/ai/ai_audit.cc`、`sql/sys_vars.cc`、必要时
`sql/sys_vars.h`/`sql/mysqld.cc`、`sql/ai/ai_runtime_server.cc`、
`ai_maas_governance.test/.result`、`unittest/gunit/ai_audit-t.cc`。

1. 先写失败测试：默认开启；仅 GLOBAL 可设置；普通 `AI_INVOKE` 用户不能关闭；关闭后不
   写新调用；已开始调用即使开关关闭仍写终态。
2. 注册 `ai_invoke_audit` GLOBAL 变量，默认 ON，使用 `SYSTEM_VARIABLES_ADMIN` 保护；
   注册启动时只读的 `ai_invoke_audit_log_file`。
3. 实现带进程内互斥的追加文件 Sink。开始事件在 write、flush、安全落盘成功后才返回。
4. 用 RapidJSON 生成 JSON Lines：时间、call_id、实例/节点、账号、客户端 IP、能力、模型、
   版本、Endpoint hash、状态、耗时、HTTP、分类错误、request ID 和 Token。
5. Complete 追加失败时返回审计错误，并只向错误日志写脱敏告警。
6. MTR 使用临时 datadir 文件验证字段、敏感字段缺失、Start fail-closed、终态 UNKNOWN
   和 GLOBAL 开关；全程不访问 MaaS。

## 阶段 4：回归、升级和真实环境验证

文件：`ai_maas_embedding.test/.result`、`ai_maas_analysis.test/.result`、
`ai_maas_rag.test/.result`、`scripts/db4ai_maas_smoke.sh`、
`scripts/db4ai_maas_real_embedding_rag_smoke.sh`、HLD 状态说明。

1. 用文件审计替代旧审计表断言，覆盖无权限、模型停用、成功、失败和 Start 写入失败。
2. 真实冒烟脚本不再读取审计表；继续要求 `DB4AI_RUN_REAL_MAAS=1`。
3. 编译 Debug `mysqld` 和 DB4AI GUnit，运行整个 `rds.ai_maas_*` 离线套件。
4. 仅在显式授权的验证实例迁移 `huawei/bge-m3`、`huawei/glm-5.2` Profile，并运行一次
   真实 Embedding 与 Chat smoke，记录脱敏结果和审计文件检查。
5. 更新 HLD 当前实现状态及旧表删除的运维前置条件。

## 提交与清理

每阶段独立提交，只包含该阶段源码、测试和必要文档。交付并确认提交后，执行
`git worktree remove .worktrees/maas-control-audit`，再删除 `maas-control-audit` 分支。
不会删除主工作区、`alisql-install` 或用户已有未提交文件。
