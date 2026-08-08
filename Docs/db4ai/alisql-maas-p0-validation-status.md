# AliSQL DB4AI MaaS P0 验证状态

> **文档角色：指定代码提交的验证证据，不定义目标接口或设计。** 目标契约见
> [`taurusdb-maas-p0-committer-design.md`](taurusdb-maas-p0-committer-design.md)，未闭环事项见
> [`taurusdb-maas-commercialization-backlog.md`](taurusdb-maas-commercialization-backlog.md)。

**更新日期：** 2026-08-08

**代码提交：** `6307be989878b06d0b6b2aa2ac108ff5ab1241f0`

**分支：** `ai_maas`

## 1. 本次重新验证结果

| 范围 | 命令或入口 | 结果 |
| --- | --- | --- |
| Debug 服务端构建 | `cmake --build build-debug --target mysqld --parallel 8` | 通过；存在 1 条与本特性无关的既有编译 Warning。 |
| 合并单测构建 | `cmake --build build-debug --target merge_small_tests-t --parallel 8` | 通过。 |
| AI 相关 GUnit | `merge_small_tests-t --gtest_filter='Ai*:*Ai*'` | 7 个 suite、26 个测试通过。 |
| Huawei Adapter GUnit | `merge_small_tests-t --gtest_filter='HuaweiMaaSTest.*'` | 10 个测试通过。 |
| DB4AI MTR | 见下方命令 | 10 个特性用例全部通过；测试框架报告 11/11，含 `shutdown_report`；耗时 34 秒。 |

MTR 复验命令：

```bash
cd build-debug
perl mysql-test/mysql-test-run.pl --suite=rds \
  ai_maas_analysis ai_maas_contract ai_maas_embedding ai_maas_governance \
  ai_maas_model_admin ai_maas_model_admin_release ai_maas_model_admin_rpl \
  ai_maas_rag ai_maas_rag_mixed_rpl ai_maas_rds_api_key
```

当前 10 个 MTR 覆盖：SQL 参数个数和 NULL 短路、Embedding、Analyze、动态权限、总开关、模型管理、
两阶段审计、VECTOR/RAG、STORED 生成列、MIXED 转 ROW、模型配置复制，以及当前 `rds_api_key` 的 SQL
不可见/不可设置行为。

## 2. 本次通过结果不能证明的能力

默认 MTR 使用 `mtr/fixture-*` 精确离线 Profile。fixture 会绕过真实凭据和 HTTP，因此本次 10 个 MTR
通过不等于下列能力已经闭环：

- Huawei 请求中的 `dimensions`、`max_completion_tokens` 等目标协议字段；
- libcurl 连接/上传/等待/下载阶段的目标超时与 `KILL QUERY`；
- SCC/Secret 密文凭据、真实 TLS/网络、MaaS 限流、内容安全、Token 计费或模型退市；
- 主备切换、节点替换、备份恢复、日志平台、目标 Region 和长稳容量。

已有真实 smoke 脚本覆盖 `bge-m3` Embedding、`glm-5.2` Analyze、向量写入/索引/STORED/RAG，并提供
多个文本生成模型的人工对比入口。历史开发环境曾成功调用 `bge-m3` 和 `glm-5.2`；本次文档差距复核
没有重新产生付费 MaaS 请求，因此该历史证据不应用于关闭目标代码提交上的协议或商用门禁。

## 3. 已确认的当前实现差距

以下是事实，不是新增产品规格：

1. Runtime 仍硬编码每语句最多 32 次、实例最多 32 个在途 AI 调用；目标设计要求删除，并新增按分钟
   调用频率参数。
2. libcurl 当前默认连接超时 5 秒、总超时 30 秒；Analyze 最大 60 秒；Embedding 尚无 `timeout_ms`；
   `KILL QUERY` 尚不能中断 libcurl。
3. `AI_EMBEDDING()` 返回元数据仍固定为 `VECTOR(1024)`；Huawei 请求不发送 `dimensions`，并会本地拒绝
   非 1024 的 BGE-M3 结果。
4. `AI_ANALYZE()` 当前发送 `max_tokens`，而目标契约是 `max_completion_tokens`；reasoning/cached Token
   的 Provider 嵌套明细尚未正确解析。
5. 模型表仍是多版本多行结构；尚无 `disable_model()`；update 不能重新启用 DISABLED/RETIRED 行。
6. 系统变量仍为 `rds_api_key`、`ai_invoke_audit`、`ai_invoke_audit_log_file`；目标统一为
   `rds_ai_maas_*` 命名。
7. root 当前不能按目标仅转授 `AI_INVOKE`；多数失败仍映射为通用 `ER_NOT_SUPPORTED_YET`。
8. HTTP Response Body 已有 1 MiB 硬上限，但最终 content 和 Provider request id 尚缺独立长度边界。

详细实现任务、优先级和关闭证据见 Backlog 的“2026-08-08 代码—设计差距复核”。

## 4. 下一轮自动化测试入口

目标实现落地时至少新增以下自动化，不能只更新现有 result：

1. 删除两个 32 硬限制后，大于 32 次顺序调用成功；按分钟限频的默认 0、动态升降、并发准确性和拒绝零出站。
2. Embedding/Analyze 目标默认和最大超时，连接/上传/等待/下载阶段 `KILL QUERY`，取消后的审计终态。
3. 维度省略、显式透传、Provider 拒绝、成功维度偏差 Warning、实际 VECTOR 维度和固定列写入失败。
4. 单行模型表 register/update/disable/delete 状态机、版本递增、并发快照和数据字典升级。
5. 新系统变量名、旧名消失、root 仅转授 `AI_INVOKE`、稳定 SQLSTATE/errno 和脱敏错误信息。
6. `1 MiB-1`、恰好 1 MiB、`1 MiB+1` 的 HTTP Body/final content，超长 request id，嵌套 Token usage。
7. ROW `FULL/MINIMAL/NOBLOB`、STORED 正文/非正文更新、备机零外呼的复制矩阵。

## 5. 商用前集成验证

以下只能在 TaurusDB 集成或目标云环境完成：

- SCC/Secret Key 下发、轮换、吊销和节点一致性；
- 每个上线 Region 的 DNS、TLS、Policy Route/NAT/EIP、安全组、Endpoint 和模型配额；
- 日志轮转、采集、保留、权限、磁盘水位、告警和 STARTED 未闭合追溯；
- 新建/升级/混部/回退/备份恢复、主备切换和节点替换；
- 真实 MaaS 协议、401/403/404/429/5xx、内容安全、Token 对账、性能容量和长稳。

任一项缺少环境或外部依赖时，应在 Backlog 保持“未开始/验证中”，不能以离线 MTR 或单次 Demo 成功
标记为已关闭。
