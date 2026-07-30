# AliSQL DB4AI MaaS P0 验证状态

更新日期：2026-07-30。本文是当前 `ai_maas` 分支的证据清单，不以设计文档、
mock 或成功编译替代真实云端验收。

## 已验证

| 范围 | 证据 | 结果 |
|---|---|---|
| Debug 服务端 | `cmake --build build-debug --target mysqld --parallel 8` | 通过 |
| 规范响应 | `runtime_output_directory/ai_types-t` | reasoning-only、length finish 和空 final 均拒绝 |
| Profile 解析 | `runtime_output_directory/ai_model_registry-t` | active/capability/tenant 优先级、1024 维 bge-m3、端点、Debug plaintext 策略和凭据 fail-closed 均覆盖 |
| Huawei Adapter | `runtime_output_directory/ai_huawei_maas_adapter-t` | embedding/chat payload、final-only、超时/输出限制、HTTP 403/429/404 分类、协议不匹配和 request ID 覆盖 |
| Runtime options | `runtime_output_directory/ai_runtime-t` | 白名单选项和私有厂商参数拒绝覆盖 |
| Persistent audit | `runtime_output_directory/ai_audit-t` + `rds.ai_maas_contract` | dispatch 前 call id、独立完成更新、token/request/status telemetry 和无敏感字段的投影覆盖 |
| VECTOR 编码 | `runtime_output_directory/ai_vector_codec-t` | native float VECTOR 编码、维度和非有限数拒绝覆盖 |
| SQL/MTR 契约 | `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract` | SQL arity、NULL 无 egress、`AI_INVOKE`、tenant account→Profile 优先级、Debug plaintext 精确 Profile 读取后本地协议拒绝、维度失败、无凭据失败、脱敏 model metadata 覆盖 |
| 审计授权读取 | `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_governance` | 无权限拒绝、viewer tenant 隔离、AI_ADMIN 跨 tenant、limit 和敏感字段缺失覆盖 |
| Release 明文门禁 | 临时 Release `mysqld` 隔离实例，含 `PLAINTEXT_DEV` fixture 的 `AI_EMBEDDING` | 调用前返回 `DB4AI credential is unavailable`；未发生 HTTP 请求 |

默认测试均使用 mock transport、不存在的 `SECRET_REF` 或不回显的 Debug fixture；
plaintext fixture 仅触发 Adapter 的本地端点协议拒绝，不会发送真实 HTTP 请求。

## 真实 MaaS 的 opt-in 检查

脚本 [`scripts/db4ai_maas_smoke.sh`](../../scripts/db4ai_maas_smoke.sh) 只在
生产运维人员已经为服务器配置 keyring `SECRET_REF`，或本地 Debug 运维人员在已授权
实例中配置临时 `PLAINTEXT_DEV` Profile、批准 endpoint、tenant Profile 和 `AI_INVOKE`
后执行。它仅输出向量维度和生成长度，不输出 key、prompt、completion 或 embedding。
2026-07-30 已在隔离的本地 Debug mysqld 完成一次显式授权检查：临时
`PLAINTEXT_DEV` embedding 与 generation Profile 仅在该实例中存在，执行
`scripts/db4ai_maas_smoke.sh` 后得到向量维度 `1024` 和非空生成长度 `38`。检查前已
完成全部离线 GUnit/MTR；检查后已删除临时 Profile、binding 和测试 schema，并移除
临时数据目录。该结果只验证当前凭据的 MaaS 可达性和协议兼容性，不构成生产部署验收。

## 明确未完成的生产门禁

- `mysql.alisql_ai_call_audit` 已有独立、持久的创建/完成生命周期和
  `AI_AUDIT_VIEWER` 查询面；审计保留、重试和运营仪表盘仍未完成。
- `AI_ADMIN` 已注册，尚没有受该权限保护的 Profile 管理 SQL 管理面；当前系统表
  应仅由受控运维流程修改。
- embedding-space、distance metric 和 index compatibility 已在 RAG schema/查询
  示例中强制过滤；服务器尚未把它们做成 Profile 元数据和通用写入时强制校验。
- 百炼、Bedrock、方舟 Adapter 仅由 canonical Adapter 边界预留，P0 只实现华为
  MaaS HTTPS JSON embedding 与 V2 Chat。
- `PLAINTEXT_DEV` 仅限 Debug 本地联调且不提供静态加密、轮换或生产安全保证；Release
  构建拒绝该 kind，生产凭据仍需要 keyring `SECRET_REF`。
- PolarDB MySQL 的比较只陈述本分支可验证的 AliSQL 证据，未进行外部实测，不做
  兼容性或性能等价声明。

## 交付前复验命令

```text
cmake --build build-debug --target mysqld ai_types-t ai_model_registry-t \
  ai_huawei_maas_adapter-t ai_audit-t ai_runtime-t ai_vector_codec-t --parallel 8
build-debug/runtime_output_directory/ai_types-t
build-debug/runtime_output_directory/ai_model_registry-t
build-debug/runtime_output_directory/ai_huawei_maas_adapter-t
build-debug/runtime_output_directory/ai_audit-t
build-debug/runtime_output_directory/ai_runtime-t
build-debug/runtime_output_directory/ai_vector_codec-t
cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract ai_maas_governance
```

## Release 明文门禁复验

以 `-DCMAKE_BUILD_TYPE=Release` 配置一个仓库外临时构建目录（并指定本仓库
`extra/boost` 和系统 curl），构建 `mysqld`。在 `--skip-networking` 的临时 datadir
初始化该服务器，插入仅含 `UNHEX('00')` fixture 的 `PLAINTEXT_DEV` embedding Profile、
tenant binding、`root@localhost` tenant account 与 `AI_INVOKE` 授权，然后执行：

```text
SELECT AI_EMBEDDING('release-gate');
```

期望在调用前得到 `DB4AI credential is unavailable`，不配置真实 endpoint 或密钥，
并在验证结束后关闭临时服务器、删除 datadir 和构建目录。
