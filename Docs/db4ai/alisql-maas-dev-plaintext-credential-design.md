# AliSQL DB4AI Debug 明文凭据设计

## 目标

为本地 Debug 构建提供受控的 `PLAINTEXT_DEV` 凭据路径，以便快速验证 Huawei
MaaS embedding 和 V2 Chat。该路径不改变客户 SQL 接口：客户仍只能传逻辑模型名，
不能传 API Key、endpoint 或厂商请求体。

## 范围与边界

- 仅当服务器以 Debug 构建编译时接受 `credential_kind='PLAINTEXT_DEV'`。
- 明文值存放在既有的 `mysql.alisql_ai_model_config.api_key_plaintext`；不新增用户
  可见的 SQL 参数、系统变量或配置文件格式。
- Release 构建对该 kind fail closed，返回 credential unavailable；`SECRET_REF` 仍为
  唯一的生产凭据来源。
- 明文值绝不进入 `AI_MODEL_INFO`、audit、错误文本、测试期望、文档示例或 Git。
- 该开发路径仅用于本地受控实例，不承诺静态加密、轮换或生产可用性。

## 调用数据流

1. Model registry 按 tenant、capability 和 logical model 解析非机密 Profile。
2. Runtime 在 HTTP dispatch 前调用 credential resolver，并传入已冻结的
   `config_id` 与 `config_version`。
3. 对 `SECRET_REF`，resolver 保持现有 keyring reader 路径。
4. 对 Debug `PLAINTEXT_DEV`，resolver 通过受控系统表访问精确读取同一 active
   config row 的 `api_key_plaintext`，验证非空及 kind/config-version 一致后，写入
   自动清零的 `Secure_string`。
5. Adapter 只在单次同步 dispatch 期间借用该短生命周期的 `Secure_string`，不跨调用
   保存凭据；构造临时 Authorization header 后用 `my_cleanse` 清零。HTTP transport
   在完成 curl 调用后也用 `my_cleanse` 清零临时 header 和 curl header 副本。审计只
   记录已存在的 config、HTTP 状态、用量和 provider request id。

`Ai_resolved_model` 和 `Ai_canonical_request` 不持有明文值；HTTP request 仅在同步调用
期间借用 header 的视图，避免请求复制、metadata 渲染或诊断路径意外携带密钥。

## 失败语义

- `PLAINTEXT_DEV` 在 Release、值为空、row 失效、kind/version 不匹配或系统表读取
  失败时都返回 `k_credential_unavailable`，并且不发送网络请求。
- 现有 HTTPS、端点 authority、固定 1024 维 bge-m3、权限和响应完整性校验保持不变。
- Huawei Chat Profile 使用验证脚本确认的
  `https://api.modelarts-maas.com/v2/chat/completions` 与 `glm-5.2` 逻辑模型。

## 验证

- 先新增离线 GUnit：Debug `PLAINTEXT_DEV` 可由 resolver 读取，Release/缺失值
  fail closed，并确认 resolved model/metadata 没有 secret 字段。
- 完整执行既有 DB4AI GUnit 与 `rds.ai_maas_contract`，确保默认测试仍无外网访问。
- 经操作者已授权后，在临时 Debug Profile 中配置明文值，分别运行一次
  `AI_EMBEDDING` 和 `AI_ANALYZE`；输出只保留向量维度、完成状态、HTTP 分类和字符数。
