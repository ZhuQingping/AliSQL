# AliSQL DB4AI MaaS P0 验证状态

更新日期：2026-07-30。本文是当前 `ai_maas` 分支的证据清单，不以设计文档、
mock 或成功编译替代真实云端验收。

## 已验证

| 范围 | 证据 | 结果 |
|---|---|---|
| Debug 服务端 | `cmake --build build-debug --target mysqld --parallel 8` | 通过 |
| 规范响应 | `runtime_output_directory/ai_types-t` | reasoning-only、length finish 和空 final 均拒绝 |
| Profile 解析 | `runtime_output_directory/ai_model_registry-t` | active/capability/tenant 优先级、1024 维 bge-m3、端点和凭据 fail-closed 均覆盖 |
| Huawei Adapter | `runtime_output_directory/ai_huawei_maas_adapter-t` | embedding/chat payload、final-only、超时/输出限制、HTTP 403/429/404 分类、协议不匹配和 request ID 覆盖 |
| Runtime options | `runtime_output_directory/ai_runtime-t` | 白名单选项和私有厂商参数拒绝覆盖 |
| VECTOR 编码 | `runtime_output_directory/ai_vector_codec-t` | native float VECTOR 编码、维度和非有限数拒绝覆盖 |
| SQL/MTR 契约 | `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract` | SQL arity、NULL 无 egress、`AI_INVOKE`、tenant account→Profile 优先级、维度失败、无凭据失败、脱敏 model metadata 覆盖 |

默认测试均使用 mock transport 或不存在的 `SECRET_REF`；不会发送真实 HTTP 请求。

## 真实 MaaS 的 opt-in 检查

脚本 [`scripts/db4ai_maas_smoke.sh`](../../scripts/db4ai_maas_smoke.sh) 只在
运维人员已经为服务器配置 keyring `SECRET_REF`、批准 endpoint、tenant Profile 和
`AI_INVOKE` 后执行。它仅输出向量维度和生成长度，不输出 key、prompt、completion
或 embedding。当前环境没有可用于执行该检查的云端凭据，因此该项是**未执行**，
不是失败或成功。

## 明确未完成的生产门禁

- `mysql.alisql_ai_call_audit` 已有 schema、canonical telemetry 和 in-memory
  sink；独立、持久、可重试的审计 writer 及 `AI_AUDIT_VIEWER` 查询面仍未完成。
- `AI_ADMIN` 已注册，尚没有受该权限保护的 Profile 管理 SQL 管理面；当前系统表
  应仅由受控运维流程修改。
- embedding-space、distance metric 和 index compatibility 已在 RAG schema/查询
  示例中强制过滤；服务器尚未把它们做成 Profile 元数据和通用写入时强制校验。
- 百炼、Bedrock、方舟 Adapter 仅由 canonical Adapter 边界预留，P0 只实现华为
  MaaS HTTPS JSON embedding 与 V2 Chat。
- PolarDB MySQL 的比较只陈述本分支可验证的 AliSQL 证据，未进行外部实测，不做
  兼容性或性能等价声明。

## 交付前复验命令

```text
cmake --build build-debug --target mysqld ai_types-t ai_model_registry-t \
  ai_huawei_maas_adapter-t ai_runtime-t ai_vector_codec-t --parallel 8
build-debug/runtime_output_directory/ai_types-t
build-debug/runtime_output_directory/ai_model_registry-t
build-debug/runtime_output_directory/ai_huawei_maas_adapter-t
build-debug/runtime_output_directory/ai_runtime-t
build-debug/runtime_output_directory/ai_vector_codec-t
cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract
```
