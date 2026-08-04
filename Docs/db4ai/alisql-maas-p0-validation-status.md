# AliSQL DB4AI MaaS P0 验证状态

更新日期：2026-08-03。本文是当前 `ai_maas` 分支的证据清单，不以设计文档、
mock 或成功编译替代真实云端验收。

## 已验证

| 范围 | 证据 | 结果 |
|---|---|---|
| Debug 服务端 | `cmake --build build-control-audit --target mysqld ai_file_audit-t ai_model_registry-t --parallel 8` | 通过 |
| 规范响应 | `runtime_output_directory/ai_types-t` | reasoning-only、length finish 和空 final 均拒绝 |
| Profile 解析 | `runtime_output_directory/ai_model_registry-t` | active/capability、实例默认、1024 维 bge-m3、端点、Debug plaintext 策略和凭据 fail-closed 均覆盖 |
| Huawei Adapter | `runtime_output_directory/ai_huawei_maas_adapter-t` | embedding/chat payload、final-only、超时/输出限制、HTTP 403/429/404 分类、协议不匹配和 request ID 覆盖 |
| Runtime options | `runtime_output_directory/ai_runtime-t` | 白名单选项和私有厂商参数拒绝覆盖 |
| 文件审计 | `runtime_output_directory/ai_file_audit-t` + `rds.ai_maas_governance` | 出站前 `STARTED`、终态、JSON Lines 脱敏字段、文件安全权限与全局动态开关覆盖 |
| VECTOR 编码 | `runtime_output_directory/ai_vector_codec-t` | native float VECTOR 编码、维度和非有限数拒绝覆盖 |
| SQL/MTR 契约 | `cd build-control-audit && perl mysql-test/mysql-test-run.pl --suite=rds ai_maas_contract ai_maas_embedding ai_maas_analysis ai_maas_governance ai_maas_rag` | 5 个用例通过；覆盖 SQL arity、NULL 无 egress、`AI_INVOKE`、实例默认 Profile、维度失败和脱敏元数据 |
| 控制面权限 | `rds.ai_maas_model_admin` | 普通客户端即使拥有 `AI_ADMIN` 和表 DML/DDL 权限也不能直接修改控制表；仅 `dbms_ai` 可发布、更新和停用 Profile，复制 applier 覆盖见 `ai_maas_model_admin_rpl` |
| 受控 RAG | `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_rag` | Debug 离线 fixture 经实际 `AI_EMBEDDING` 写入 VECTOR INDEX；tenant/业务/embedding-space 过滤、schema contract 拒绝与来源回传覆盖 |
| 分析与只读诊断 | `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_analysis` | JSON input、analyze/diagnose mode 和私有 provider options 拒绝覆盖；fixture 不读取 secret、不发起 HTTP |
| Release 凭据门禁 | `rds.ai_maas_model_admin_release`（隔离 component keyring 的 Release MTR） | 先发布可读的 fake `SECRET_REF`，移除后 `update_model()` 失败且原 active 版本保持不变；未知引用的 `register_model()` 失败且不发布新行 |

默认 Debug 测试均使用 mock transport 或精确的 `mtr/fixture-*` 离线 Profile；它们通过
`dbms_ai` 注册、不会加载 keyring，也不会发送真实 HTTP 请求。Release MTR 使用隔离的
component keyring 和假的测试数据验证发布前探测及 fail-closed，不等同于目标环境 keyring 的
成功验收。

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

- 审计写入本地受控文件；日志采集、保留、磁盘满告警和运维平台查询由 TaurusDB 日志平台
  提供，不在 P0 SQL 面交付。
- `mysql.taurusdb_ai_model_config` 是内部控制表，不支持客户直接 DML/DDL；Profile 发布、更新
  和停用必须经 `dbms_ai`，其变更进入 binlog。生产发布还需在目标环境以实际 keyring 引用
  验证 reader 可读、轮换及权限配置。
- embedding-space、distance metric 和 index compatibility 已在 RAG schema/查询
  示例中强制过滤；服务器尚未把它们做成 Profile 元数据和通用写入时强制校验。
- 百炼、Bedrock、方舟 Adapter 仅由 canonical Adapter 边界预留，P0 只实现华为
  MaaS HTTPS JSON embedding 与 V2 Chat。
- `PLAINTEXT_DEV` 仅限 Debug 本地联调且不提供静态加密、轮换或生产安全保证；Release
  构建拒绝该 kind，生产凭据仍需要 keyring `SECRET_REF`。
- PolarDB MySQL 的比较只陈述本分支可验证的 AliSQL 证据，未进行外部实测，不做
  兼容性或性能等价声明；详见
  [`alisql-vs-polardb-ai-capability.md`](alisql-vs-polardb-ai-capability.md)。

## 交付前复验命令

```text
cmake --build build-control-audit --target mysqld ai_types-t ai_model_registry-t \
  ai_huawei_maas_adapter-t ai_file_audit-t ai_runtime-t ai_vector_codec-t --parallel 8
build-control-audit/runtime_output_directory/ai_types-t
build-control-audit/runtime_output_directory/ai_model_registry-t
build-control-audit/runtime_output_directory/ai_huawei_maas_adapter-t
build-control-audit/runtime_output_directory/ai_file_audit-t
build-control-audit/runtime_output_directory/ai_runtime-t
build-control-audit/runtime_output_directory/ai_vector_codec-t
cd build-control-audit && perl mysql-test/mysql-test-run.pl --suite=rds \
  ai_maas_contract ai_maas_governance ai_maas_rag ai_maas_analysis vidx_func
```

## Release 明文门禁复验

以 `-DCMAKE_BUILD_TYPE=Release` 配置一个仓库外临时构建目录（并指定本仓库
`extra/boost` 和系统 curl），构建 `mysqld`。在 `--skip-networking` 的临时 datadir
初始化该服务器，通过受控管理路径发布仅含 `UNHEX('00')` fixture 的
`PLAINTEXT_DEV` embedding Profile 并授予 `AI_INVOKE`，然后执行：

```text
SELECT AI_EMBEDDING('release-gate');
```

期望在调用前得到 `DB4AI credential is unavailable`，不配置真实 endpoint 或密钥，
并在验证结束后关闭临时服务器、删除 datadir 和构建目录。
