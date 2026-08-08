# TaurusDB MySQL 对接华为云 MaaS 设计检视意见答复

> **文档角色：评审问题、论证和决策过程记录，不是规范性设计来源。** 决策生效后必须同步进入
> [`taurusdb-maas-p0-committer-design.md`](taurusdb-maas-p0-committer-design.md)；未实现事项必须进入
> [`taurusdb-maas-commercialization-backlog.md`](taurusdb-maas-commercialization-backlog.md)。

**答复日期：** 2026-08-08

**文档状态：** Committer / 技术负责人评审答复稿

**关联设计：** [taurusdb-maas-p0-committer-design.md](taurusdb-maas-p0-committer-design.md)

**关联缺口：** [taurusdb-maas-commercialization-backlog.md](taurusdb-maas-commercialization-backlog.md)

## 1. 总体结论

本轮评审结论为 **REVISE**。当前代码已完成华为云 MaaS 文本向量和文本生成的基本调用闭环，
但模型发现与退市、POC 用量限制、同步调用中断、Token 对账、内容安全、备份恢复和权限委托等
事项仍需在商用前补齐。

2026-08-08 已按代码提交 `6307be989878b06d0b6b2aa2ac108ff5ab1241f0` 重新完成代码—设计—测试
复核：Debug `mysqld` 和 `merge_small_tests-t` 构建通过，AI GUnit 26 个、Huawei Adapter GUnit 10 个、
DB4AI MTR 10 个全部通过。该结果确认当前基线稳定，但不改变 **REVISE** 结论；具体未完成项及测试缺口
已经逐项写入商用化 Backlog 的“2026-08-08 代码—设计差距复核”。

本答复遵循以下边界：

1. “当前实现”只描述 `ai_maas` 分支已经存在的行为；“目标方案”不等同于已经交付。
2. 内核不根据 MaaS 返回的模型名称猜测能力、维度和协议；自动注册必须经过 TaurusDB 兼容目录。
3. 不静默替客户切换模型，特别是向量模型；模型升级必须由客户确认并完成数据迁移。
4. POC 期使用 Provider 无关的内核运行时限制和管控累计配额，不为临时配额向客户 SQL 或模型系统表
   增加永久字段；Provider Endpoint 限流只作为可选保护。
5. 数据库审计用于实例内调用归因，MaaS 统计和计费记录是费用对账的权威来源。

## 2. 检视意见逐项答复

### 2.1 MaaS 模型列表接口能否支持管控面自动注册

**评审问题**

管控面开启 TaurusDB MaaS 功能时，希望获取最新模型列表并自动注册。需要确认公开接口能否区分
文本生成、向量等模型，并返回向量维度和调用协议。

**事实与当前实现**

华为云 MaaS 提供 `GET /v2/models` 模型列表接口，返回内容主要是模型 `id`、对象类型和创建时间。
公开响应中的 `object=model` 不能区分文本生成、Embedding、Rerank、图片或视频能力，也没有向量维度、
Endpoint 协议、停服时间等 TaurusDB 自动注册所需的完整元数据。因此，公开模型列表只能证明当前
凭据“可以看见某个模型”，不能单独作为数据库 Profile 的生成依据。

目标数据库的 `mysql.ai_model_config` 保存能力、Provider 模型名、受控 Endpoint 和可选的 Embedding
默认维度；内核不会通过模型名称猜测这些信息，也不以默认维度限制客户显式维度请求。

**方案决策**

管控面采用“双目录取交集”的方式注册模型：

1. 调用 MaaS `/v2/models`，取得当前账号和 Region 可见的模型 ID。
2. 读取 TaurusDB 维护的兼容目录。兼容目录包含已验证的 capability、可选 `default_dimension`、Endpoint/协议版本、
   Adapter 兼容版本和验证状态。
3. 只注册“当前可见模型 ID ∩ TaurusDB 兼容目录”的模型；未知模型不自动注册。
4. 兼容目录由 TaurusDB 发布流程维护，并推动 MaaS 后续提供内部结构化的能力、维度和生命周期元数据。

这样既能跟随 MaaS 模型变化，又不会因模型命名、协议或响应格式变化把未经验证的模型暴露给客户。

**待实现与验收**

- 管控面实现模型列表同步、兼容目录和差异记录。
- 覆盖“可见且兼容、可见但未知、兼容但当前不可见”三类用例。
- 模型列表接口异常时保持已有配置，不删除模型，也不自动切换客户业务。

### 2.2 模型下线识别与客户迁移

**评审问题**

模型迭代快且会下线，如何识别；识别后是自动升级还是禁用并提示客户。

**事实与当前实现**

MaaS 已发布模型退市公告，且替代模型在能力、API、价格和效果上可能不同。当前系统表具备
`ACTIVE`、`DISABLED`、`RETIRED` 状态，但尚未形成公告订阅、模型目录对账和客户迁移闭环。

**方案决策**

1. 识别来源按优先级依次为：MaaS 内部结构化生命周期通知或管控兼容目录、`/v2/models` 周期对账、
   真实健康探测和持续的 404/模型不可用错误聚合。
2. 管控面记录预警时间、计划退市时间和推荐替代模型，并向客户展示影响范围。
3. MaaS 已确认模型永久下线或到达公告停服时间后，管控面调用
   `CALL dbms_ai.delete_model(model_name, capability)`。该接口是逻辑删除，不物理删除配置：当前实现会把
   该逻辑模型/能力唯一当前行原位置为 `RETIRED` 并递增 `config_version`，Registry 随后不再加载，后续
   调用在数据库本地失败。
4. `update_model()` 不能传入任意 `status`；它的固定语义是原位更新唯一当前行、设置 `ACTIVE` 并递增
   `config_version`。因此模型下线不能使用 `update_model()`，但重新启用或回退发布可以使用该接口。
5. 单次超时、429、5xx、网络错误或短时模型不可用不代表永久下线，不能自动设置 `RETIRED`。如果需要在
   调查期间临时阻断调用，应由后续 `disable_model()` 设置 `DISABLED`；当前尚无该显式接口。
6. 默认不静默升级。文本生成模型可在客户确认并通过 smoke/效果评估后进行一键切换。
7. 向量模型不得透明替换。新旧模型生成的向量不属于同一 Embedding space，必须重新生成已有向量、
   重建索引并完成检索效果验证后再切换。

**待实现与验收**

- 补充 `dbms_ai.disable_model()` 或等价管理能力，用于临时停用；永久下线继续使用已实现的
  `delete_model()` 将状态设置为 `RETIRED`。
- 验证“预警、临时禁用、永久下线 RETIRED、本地拒绝、客户确认迁移、回退”完整链路。
- 将退市通知时效和管控巡检周期纳入运维验收。

### 2.3 POC 阶段 Token 或调用次数限制

**评审问题**

灰度期间需要限制个别客户的大量调用和费用；正式商用按 Token 计费后取消临时限制，并保持兼容。

**事实与当前实现**

当前内核硬编码了单语句最多 32 次 AI 调用、实例最多 32 个并发调用。评审后确认这两个值不应冻结为
P0 产品规格：语句次数限制会让合法的批量向量更新在已经产生外部费用后中途失败；专用并发计数会增加
额外状态、失败边界和运维语义，当前 POC 尚无容量证据证明必须引入。两项硬编码均作为代码待删除项。

MaaS 自定义 Endpoint 支持 RPM/TPM 限流和启停，但它依赖特定 Provider、Region 和 Endpoint 生命周期，
且存在数量限制。阿里百炼、火山方舟和 AWS Bedrock 等 Provider 的限流接口与资源模型也不相同，
因此 MaaS Endpoint 限流不能作为 TaurusDB 公共模型管理和 POC 限制的基础契约。

**方案决策**

POC 第一版在 mysqld Runtime 层实现 Provider 无关的实例级调用频率限制，不修改模型系统表，
也不要求创建 MaaS 自定义 Endpoint：

1. 新增全局动态实例参数 `rds_ai_maas_max_calls_per_minute`；`0` 表示不限制且为数据库默认值，正整数
   表示启用限制，负数、`NULL` 或越界值拒绝且不改变旧配置。不支持 SESSION 级覆盖。
2. 参数由 TaurusDB 管控参数系统持久化并作为权威配置，在实例启动、节点重启、节点替换和故障切换时随
   实例配置重新下发。mysqld 中的变量值和令牌桶只是运行时副本；普通 `SET GLOBAL` 本身不等于持久化。
   生产环境不依赖节点本地 `my.cnf`，避免节点重建后丢失或各节点不一致。
   不再新增 `dbms_ai` 限流设置/取消接口，避免实例参数和管理包形成两个配置入口。
3. Runtime 在参数、权限和 Profile 本地校验之后、审计 STARTED 和 MaaS 出站之前申请调用许可。使用
   单实例令牌桶：容量为 N，以 N/60 秒的速率补充，使用单调时钟。其精确定义是平均速率 N 次/分钟、
   短时最多突发 N 次，而不是任意滑动 60 秒窗口都严格不超过 N。
4. 每个实际远端请求计数一次；本地参数、权限、模型等校验失败不计数。取得许可后如审计 STARTED 失败且
   确认没有出站，应归还许可；一旦 Transport 开始外呼，无论成功、失败、超时或取消都不归还。
5. 限制对 `AI_EMBEDDING()`、`AI_ANALYZE()`、STORED 生成列和未来 Provider/能力统一生效；更新值立即影响
   新请求，不取消在途请求。设置为 0 只取消频率限制；完全禁止调用仍使用 `rds_ai_maas=OFF`。
6. 管控面以同一实例参数配置包向目标节点下发限制，并保证调用频率参数先于总开关生效。当前仅主节点交付；
   未来多调用节点需要拆分额度或引入共享限流，不能把相同 N 简单下发到每个可调用节点。
7. MaaS 自定义 Endpoint RPM/TPM 仅作为 Huawei Provider 的可选第二层保护，不写入
   `mysql.ai_model_config`，也不作为跨 Provider 的公共接口。
8. 该限制只能抑制瞬时调用速率，不能准确限制 POC 生命周期、每日/月度 Token 或总费用。累计配额仍由
   管控面结合数据库审计和 MaaS 统计完成，达到总量后关闭 `rds_ai_maas`；正式商用可把调用次数限制设置为 0，
   不需要升级系统表或修改客户 SQL。
9. P0 删除每语句 32 次和实例并发 32 的硬限制，不新增对应系统变量。风险由调用频率、单次超时、
   `KILL QUERY`、`AI_INVOKE` 权限和总开关共同控制；大批量向量生成由应用分批提交。商用容量测试如证明
   存在慢请求堆积，再把并发保护作为独立优化项评审，不预先固化实现。

**为什么使用实例参数，而不使用 `dbms_ai` 设置**

设计阶段评估过三种方案，并非遗漏 `dbms_ai`：

1. **`dbms_ai` 只修改内存。** 接口直观，但 mysqld 重启、节点重启或节点替换后配置丢失；管控面必须额外
   保存并重新调用过程，过程返回成功也不能证明实例生命周期内持续有效，因此不采用。
2. **`dbms_ai` 写系统表并由 Runtime 加载。** 可以持久化，但调用频率是实例运行策略，不是模型 Profile；
   为一个简单整数增加系统表字段或新表，会引入数据字典升级、事务/binlog/复制、缓存刷新和恢复语义，成本过高。
3. **全局动态实例参数，由管控参数系统持久化。** 该方案复用 TaurusDB 已有的参数发布、权限、变更审计、
   重启恢复和节点替换下发能力；Runtime 只读取一个内存值并维护令牌桶，不需要修改模型表。这是最终选择。

也评估过让 `dbms_ai` 包装同一个系统变量，但这会形成“管控参数页面”和“SQL 管理过程”两个写入口，产生
权限边界、最终值、持久化状态和变更审计不一致的问题。因此不提供 `dbms_ai` 限流设置/取消接口：
`dbms_ai` 继续管理模型 Profile，实例运行策略统一由 TaurusDB 参数系统管理。

**待实现与验收**

- 实现 `rds_ai_maas_max_calls_per_minute`、令牌桶、运行状态计数和稳定的本地限流错误分类。
- 验证默认 0、0 与正数动态切换、并发不超发、本地失败不计数、审计失败归还、已出站失败仍计数。
- 验证管控参数在重启、节点替换和故障切换后的恢复顺序、商用不限流切换，以及多 Provider 使用同一
  Runtime 限制点。
- 另行明确 POC 累计调用/Token 阈值、统计延迟和超额误差；不得把内存频率限制表述为总费用上限。

#### 2.3.1 系统变量命名冻结

P0 系统变量统一使用 `rds_ai_maas` 命名空间：保留 `rds_ai_maas`、
`rds_ai_maas_max_calls_per_minute`，将当前代码中的 `rds_api_key`、`ai_invoke_audit`、
`ai_invoke_audit_log_file` 分别重命名为 `rds_ai_maas_huawei_api_key`、`rds_ai_maas_audit`、
`rds_ai_maas_audit_log_file`。华为 Key 名称显式包含 `huawei`，避免未来多 Provider 共用一个含义模糊的
API Key 参数。当前尚未对外发布，接口冻结时直接切换，不保留旧变量别名。

P0 不增加 `rds_ai_maas_statement_max_wait_ms`。单次调用超时由 Embedding/Analyze 第三个
`options_json.timeout_ms` 控制；P0 依靠实例调用频率、单次最大超时、`KILL QUERY`、权限和总开关控制风险，
不增加每语句次数、专用实例并发或累计等待参数。产品必须明确多行 SQL 的总等待可能按调用次数放大并建议
分批提交；后续只有目标环境证明上述边界不足时，才重新评审累计预算或并发保护。

### 2.4 MaaS 超大返回值的内存和展示风险

**评审问题**

模型是否可能返回超大内容，是否造成 mysqld 内存问题或 SQL 客户端展示失败。

**事实与当前实现**

当前 HTTP Transport 在 mysqld 内部对单次 HTTP Response Body 设置 1 MiB 字节硬上限。该限制不是
libcurl 或 MaaS 的接口参数，也不会发送给 MaaS；libcurl 每收到一块响应数据，Transport 都会在写入
`response.body` 前检查累计大小，超过上限立即中止接收并返回 `RESPONSE_TOO_LARGE`。该上限覆盖成功和
失败响应中的完整 Body，包括 `content`、`reasoning_content`、usage、JSON 包装及其他元数据，并同时
作用于 Embedding 和文本生成调用；HTTP Header 不计入该 1 MiB。

该限制只能约束 mysqld 实际接收的字节数，不能阻止 MaaS 在远端继续生成内容，也不能保证超限请求不
产生 Token 和费用。HTTP 超限后 SQL 整体失败，不返回截断的半成品，审计终态记录
`RESPONSE_TOO_LARGE`。

`AI_ANALYZE()` 当前调用 `set_data_type_string(1024 * 1024, utf8mb4)` 声明返回类型；该参数的含义是
最大字符数，换算为 utf8mb4 的 SQL 元数据最大字节长度可接近 4 MiB，并不是 1 MiB 运行时硬限制。
当前代码在为 SQL 结果分配缓冲区前也没有独立检查 `final_content.size()`。由于 Huawei Adapter 的
`content` 来自已经受 1 MiB Body 上限保护的响应，当前真实调用链路的最终内容被间接限制在 1 MiB
以内；但是类型元数据不能作为安全边界，未来新增 Adapter、Fixture 或其他结果来源时也可能绕过这种
间接约束。

单次响应虽然有界，但同一时刻还会存在 libcurl 缓冲区、原始响应字符串、RapidJSON 解析树、提取后的
`content` 和 SQL 返回缓冲区，实际峰值内存会高于 1 MiB；多个数据库连接同时调用会进一步放大实例内存。
Header 中单独保存的 `x-request-id` 当前也没有内核自定义长度限制。不同客户端和驱动对接近 1 MiB 的
单元格还可能存在 packet、结果缓冲或展示限制。

当前 SQL option `max_output_tokens` 省略时，Adapter 不发送输出 Token 参数，由 MaaS/模型采用默认值；
显式指定时当前代码只接受 1～32768，并发送为 `max_tokens`。这是当前 P0 实现保护，不是已经冻结的
跨模型、跨 Provider 公共契约。华为 MaaS V2 Chat 文档明确说明，不同模型支持的最大输出长度不同；
`max_tokens` 的默认值也不同：Kimi-K2.6、openPangu-2.0-Pro 和 openPangu-2.0-Flash 为 32K，其他模型
为 4K。华为 MaaS V2 Chat 文档还提供
`max_completion_tokens`，用于限制包含可见内容和推理内容在内的完整生成预算，更适合作为服务端输出
保护，但它的单位是 Token，不能替代数据库侧的响应字节上限；切换前还必须逐个验证已交付文本模型的
参数兼容性。统一发送 4096 会覆盖模型自身默认值；对深度思考模型，推理内容还可能先消耗大部分预算，
增加 `finish_reason=length` 和 SQL 失败的概率。

**方案决策**

1. `AI_ANALYZE` 不设置数据库统一的默认生成 Token 数。用户省略 `max_output_tokens` 时，Adapter 不发送
   Provider 输出长度参数，使用所选模型的默认值，避免数据库把 32K 等模型默认预算统一压缩为 4096。
2. 用户显式指定 `max_output_tokens` 时只要求是正整数；不把当前 32768 固化为 SQL 公共接口上限，也不
   静默截断用户值。具体上限由 MaaS 按模型校验；TaurusDB 兼容目录能够取得并维护相应元数据时，可在
   管控或 Adapter 层提前拒绝。参数超限响应必须转换为明确的模型参数错误。当前代码的 32768 本地上限
   需要按该目标改造。
3. V2 Chat Adapter 在完成存量模型兼容性验证后，将 Provider 无关的 `max_output_tokens` 映射为 Huawei
   `max_completion_tokens`；其他 Provider 由各自 Adapter 映射，不向 SQL 暴露 Provider 私有字段。
   Provider 侧 Token 限制只作为第一层保护，数据库继续保留固定的 HTTP Body 1 MiB 字节硬上限。
4. 定义独立的 `final_content` 1 MiB 字节硬上限，在分配 SQL 返回缓冲区前显式校验，不依赖 SQL 类型
   元数据或当前 Adapter 的间接约束；超限时整次 SQL 失败，不截断后返回。
5. Provider 返回 `finish_reason=length` 时，继续按 `INCOMPLETE_OUTPUT` 处理并拒绝返回截断内容；错误信息
   需明确说明达到模型输出上限，避免与网络、认证等普通 Provider 失败混淆。
6. 为 HTTP Body 超限提供稳定的 SQL 错误分类。当前内部和审计已经使用 `RESPONSE_TOO_LARGE`，SQL 层仍
   共用 `ER_NOT_SUPPORTED_YET`；商用前应新增专用 errno/SQLSTATE，或者明确冻结现有 errno、错误文本和
   审计分类的兼容契约。
7. 对需要保存的 `x-request-id` 设置固定的小字节上限，建议 256 字节；超长值不进入审计和诊断上下文，
   避免 Response Body 限制之外的响应元数据持续占用内存。
8. 将 libcurl 缓冲、响应体、RapidJSON 解析树、最终字符串和 SQL 结果缓冲区纳入同一内存模型，对最小
   商用实例规格执行单请求和阶梯并发压力测试。验收前根据实测冻结 AI 调用内存预算；至少要求无 OOM、
   无崩溃、无持续内存增长，压力解除后 RSS 回落到已冻结的基线容差内。
9. 验证 mysql client、JDBC 和常用连接池对最大允许结果的完整接收、字符集和展示行为；客户端自身
   packet 或缓冲设置不足时应给出明确配置要求，不能把客户端截断误认为模型结果。

**待实现与验收**

- 移除当前 32768 本地固定上限：验证省略 `max_output_tokens` 时不发送 Provider 长度参数；显式正整数
  不静默截断，并由 MaaS 按具体模型校验；兼容目录具有上限元数据时可提前拒绝。
- 完成 `max_completion_tokens` 映射及各已交付文本模型的参数兼容性验证，覆盖 4K/32K 默认值和深度
  思考模型；参数超限返回明确错误。
- 完成最终 `content` 字节上限、`x-request-id` 长度上限、超限 SQL 错误契约和审计终态。
- 覆盖 `1 MiB-1`、恰好 1 MiB、`1 MiB+1`，单块超限和多块累计超限，以及 HTTP 200/非 200
  超大 Body；确认超限前最多保留 1 MiB，SQL 不返回部分结果。
- 覆盖 `content` 较小但 `reasoning_content` 或其他 JSON 元数据导致完整 Body 超限、
  `finish_reason=length`、最终 content 独立超限、UTF-8 多字节、JSON 转义和深层 JSON。
- 覆盖单请求、阶梯并发和连续多轮调用的峰值 RSS、内存回落和泄漏检查，并在最小商用实例规格上冻结
  可量化的 AI 内存预算和基线容差。
- 覆盖 mysql client、JDBC 和常用连接池对接近上限结果的字节完整性、字符集和展示验证。

### 2.5 同步调用与多行处理耗时

**评审问题**

当前同步模型调用时间不可控，多行调用可能非常慢；后续是否考虑小模型或本地 CPU 推理。

**事实与当前实现**

`AI_EMBEDDING()` 和 `AI_ANALYZE()` 都是同步 SQL 函数：SQL 执行线程必须等待网络和模型响应。对多行表达式
求值时，每行都可能触发一次远端调用，整体耗时和费用近似线性增长，还会长期占用数据库工作线程。
当前超时按单次 MaaS 调用计算，不是整条 SQL 的累计超时。评审后不再限制一条语句的调用总次数；如果
每次调用都接近其超时但成功，批量 SQL 的整体等待仍可能放大到不可接受的量级，因此应用必须分批提交。

任意一次调用超时都会产生 SQL 错误并终止当前语句，后续 AI 调用不再执行。此前已经成功或已经被 MaaS
接收的调用、Token、费用及文件审计不能随 SQL 回滚；InnoDB 语句的数据修改可以回滚，但外部调用不是
数据库事务的一部分。SELECT 客户端即使已经收到部分行，也必须把最终报错的结果集视为不完整并丢弃。

**方案决策**

1. P0 明确只适合交互式、小批量和有界数据处理，不用于高并发 OLTP、无界全表扫描和在线批量灌库。
2. 数据导入、存量向量重算等大批量任务由应用或后续异步任务框架分批执行。
3. 超时按能力区分：连接超时目标为 10 秒；`AI_EMBEDDING` 单次总超时默认 60 秒；`AI_ANALYZE` 默认
   300 秒。两类函数均允许通过 `options_json.timeout_ms` 显式调整，最大 1800000 毫秒；该选项只控制
   本地 libcurl，不发送给 MaaS。
4. P0 不增加语句累计等待系统变量，也不保留每语句 32 次和实例并发 32 的硬限制；超时和取消均不自动
   重试。产品说明多行 SQL 的总耗时可能接近“调用次数 × 单次超时”，并建议批量任务分批提交。
5. 后续如部署 CPU 版 `bge-m3`，应以独立本地推理服务和新的 Adapter 接入，不把模型运行时直接加载进
   mysqld，避免模型内存、线程和故障影响数据库内核。

**待实现与验收**

- 文档和产品界面提示同步语义、适用数据规模和费用风险。
- 实现连接 10 秒、Embedding 默认 60 秒、Analyze 默认 300 秒，以及两类函数最大 1800 秒的显式单次超时。
- 增加多行调用的耗时、线程占用、单次超时和限流测试；验证一次失败后后续调用不执行、先前外呼
  不回滚，并核对 Proxy/客户端查询超时不早于数据库目标值。

### 2.6 MaaS 卡住时能否中断

**评审问题**

模型请求卡住后，用户执行 `KILL QUERY` 是否能及时停止调用。

**事实与当前实现**

当前 libcurl 设置 5 秒连接超时和 30 秒请求总超时；`AI_ANALYZE` 可显式设置单次 `timeout_ms`，但当前
上限为 60000 毫秒；`AI_EMBEDDING` 没有 timeout option，使用 30 秒总超时。Transport 没有接收 THD
中断状态，也没有注册 libcurl progress 回调，因此 `KILL QUERY` 不能保证立即终止正在等待的 HTTP 请求，
最晚需要等待本次请求超时。若在没有取消能力时直接把最大超时提高到 30 分钟，SQL worker、事务锁、
执行线程、连接和审计 STARTED 都可能被占用 30 分钟，不满足商用要求。

**方案决策**

1. Runtime 向 Transport 传入可取消令牌；libcurl 使用 `CURLOPT_XFERINFOFUNCTION` 周期检查
   `THD::killed`，命中后主动终止传输。
2. 完成取消能力后再开放 Analyze 30 分钟显式上限；默认仍为 5 分钟，30 分钟不是所有调用的默认值。
3. 审计终态增加或映射为稳定的 `CANCELLED` 分类，区分超时、网络失败和用户取消。
4. 保留单次总超时，不设置每语句次数、MaaS 专用实例并发或语句累计等待系统变量；调用频率、权限和
   总开关作为 POC 的其他控制点。
5. 明确外部副作用：本地超时或取消只表示数据库不再等待，MaaS 可能已经接收或完成请求，仍可能产生
   Token 和费用；默认不自动重试。

**待实现与验收**

- 覆盖连接、上传、等待首字节和下载阶段的 `KILL QUERY`。
- 验证取消后的线程、curl handle、响应缓冲区和审计事件均正常释放/闭合。
- 验证 Embedding 60 秒、Analyze 5 分钟默认值以及两类函数 30 分钟显式上限；在取消能力未启用的构建中不得开放
  30 分钟上限。

### 2.7 后续计费所需的 Token 统计

**评审问题**

正式商用按 Token 计费时，当前设计是否能够统计 Token 消耗。

**事实与当前实现**

两阶段审计的终态已经预留 `prompt_tokens`、`completion_tokens`、`reasoning_tokens`、
`cached_tokens` 和 `total_tokens`。华为 MaaS V2 Chat 返回 `prompt_tokens`、`completion_tokens`、
`total_tokens`，但 `reasoning_tokens` 位于 `completion_tokens_details`，`cached_tokens` 位于
`prompt_tokens_details`。当前 Adapter 按顶层字段解析后两项，因此它们可能被错误记录为 0。

**方案决策**

1. 修复嵌套 usage 字段解析，并兼容字段缺失；禁止通过推算替代 Provider 返回值。
2. 数据库审计提供实例、用户、模型和调用级归因；MaaS 统计/账单作为最终计费和争议处理依据。
3. 审计与管控统计通过 Provider 请求 ID、实例 UUID、逻辑模型名和配置版本关联。
4. 对流量丢失、审计失败、超时但 MaaS 已计费等场景建立差异对账规则。

**待实现与验收**

- 增加包含嵌套 usage 的 fixture 和真实 MaaS 回归。
- 核对数据库审计、MaaS 调用统计和账单三个口径，并定义允许误差与补偿流程。

### 2.8 敏感话题和内容安全

**评审问题**

用户通过 `AI_ANALYZE()` 讨论违法违规或敏感话题时是否需要限制，以及由谁限制。

**事实与当前实现**

当前数据库只进行参数、权限、模型和 Endpoint 校验，不具备独立的内容分类器。华为 MaaS 自定义 Endpoint
支持内容安全防护，但开启防护会增加一定调用时延。内容判断发生在请求进入 MaaS 后，不等同于数据库出站前的
DLP 或敏感数据识别。

**方案决策**

1. 生产 Endpoint 必须由管控面开启 MaaS 内容安全防护，客户 SQL 不能替换 Endpoint 或关闭防护。
2. Provider 拒绝、拦截和内容安全错误映射为稳定的数据库错误分类，并记录脱敏审计，不记录完整 prompt。
3. 管控面展示防护状态，未满足安全策略时不得开启 `rds_ai_maas`。
4. 产品规格明确内容安全由 MaaS 策略执行，数据库不承诺识别所有违法违规或敏感内容；启用检测和拦截可能
   增加端到端调用时延，该时延计入 `AI_ANALYZE()` 的同步调用耗时与超时预算。

**待实现与验收**

- 验证内容安全开关、典型拦截、误拦截、错误映射和审计脱敏，并记录开启策略前后的调用时延变化。
- 安全团队确认禁止类别、申诉流程和策略变更责任边界。

### 2.9 系统表、密码等敏感数据是否可能被外发

**评审问题**

用户能否利用 `AI_ANALYZE()` 获取系统表、密码或其他敏感信息，是否需要额外限制。

**事实与当前实现**

当前结论是**部分支持**：内核已经防止 AI 函数隐式取数和凭据、日志泄漏，但不能阻止有权账号主动提交其
可读取的敏感数据。

当前已经实现以下保护：

1. 模型不能主动连接数据库或执行 SQL；`AI_EMBEDDING()`、`AI_ANALYZE()` 只接收调用者显式传入的参数，
   不隐式读取系统表、业务表、当前 SQL、会话历史、系统变量或文件。
2. MySQL 原有表、列、视图和存储程序权限语义仍然生效；AI 函数不能绕过数据读取权限。外部调用还必须通过
   实例级 `AI_INVOKE` 检查，无该权限不会形成 Provider 请求。行级范围由应用查询条件、脱敏视图或
   TaurusDB 既有数据隔离能力约束，不由 AI 函数新增实现。
3. 当前 `rds_api_key`（目标重命名为 `rds_ai_maas_huawei_api_key`）是
   `READ_ONLY + NOT_VISIBLE + NON_PERSIST + SENSITIVE + NOT_IN_BINLOG` 的启动参数，
   普通 SQL 不能查询、设置或持久化；`mysql.ai_model_config` 不保存 API Key，模型选项拒绝凭据字段。
4. AI 调用审计和错误日志不记录 API Key、Authorization、完整 prompt、完整响应或原始向量。

当前没有 DLP、敏感字段识别、prompt 扫描或系统表/业务列 AI denylist。若用户同时拥有敏感数据的读取权限和
`AI_INVOKE`，仍可以通过 `CONCAT()`、JSON 或查询结果把 `mysql.user.authentication_string` 等可读字段
显式拼入 prompt 并发送到 MaaS。这不是 AI 函数绕过 SQL 权限，而是获授权账号的数据出站风险。固定 system
prompt 和 MaaS 内容安全策略也不是数据库出站前 DLP。

**方案决策**

1. 将 `AI_INVOKE` 定义为“允许向外部模型发送数据”的高风险权限，默认不授予普通用户。
2. 只向经过审批的应用账号授予，结合最小 SQL 权限、脱敏视图、行列过滤和数据分类限制可传入内容。
3. AI 函数不得隐式读取系统表、会话历史、SQL 文本、变量或文件；只处理显式参数。
4. API Key、完整 prompt、完整响应和原始向量不得进入错误日志和 AI 调用审计。
5. 当前没有内核 DLP/敏感内容识别能力，文档不得宣称数据库能够自动阻止所有敏感数据外发；需要时由
   管控、安全网关或后续独立 DLP 能力承担。

产品规格只能承诺“AI 函数不隐式取数、不绕过 SQL 权限，凭据和日志正文受隔离”，不能承诺“数据库自动识别并
阻止所有敏感数据外发”。

**待实现与验收**

- 安全评审覆盖系统表、业务表、视图、存储程序、错误日志、审计脱敏，以及“无 `SELECT`、无
  `AI_INVOKE`、只有 `SELECT`、只有 `AI_INVOKE`、两者都有”的权限矩阵。
- 增加包含唯一敏感标记的离线用例：验证无读取权限不能取数、无 `AI_INVOKE` 不出站、API Key 不可读，
  审计和错误日志不出现完整 prompt/response 或敏感标记。
- 安全团队确认当前不提供 DLP 是否满足商用边界；如不满足，由管控、安全网关或后续独立 DLP 承担，不能以
  system prompt 或 MaaS 内容安全替代。

### 2.10 备份恢复到新实例的处理

**评审问题**

已开启 MaaS 的实例恢复为新实例后，模型表、权限、开关、网络、API Key 和审计参数如何处理。

**方案决策**

1. 模型 Profile 和动态权限元数据可随 `mysql` 数据恢复，但必须经过目标版本兼容校验。
2. 华为敏感启动参数（目标名 `rds_ai_maas_huawei_api_key`）、SCC 绑定、EIP/路由、审计文件路径等实例级外部资源不随备份恢复。
3. 新实例恢复后强制以 `rds_ai_maas=OFF` 启动，禁止因为源实例已开启而自动访问外部 MaaS。
4. 管控面在目标 Region 重新完成网络、SCC 凭据、审计路径和模型可用性配置。
5. 管控面验证 Profile 与目标 Region 的模型目录和 Endpoint 兼容后，才开启总开关。
6. 跨 Region 不可用的 Profile 保留用于审计和客户确认，但状态应置为 `DISABLED`，不得静默映射。

**待实现与验收**

- 增加同 Region、跨 Region、不同内核版本和凭据轮换后的恢复测试。
- 验证恢复过程中不会产生外部调用，旧 API Key 不会进入备份或新实例。

### 2.11 SUPER 用户是否自动拥有 AI 权限

**评审问题**

拥有 `SUPER` 的用户是否天然具有 `AI_INVOKE`、`AI_ADMIN`。

**事实与方案决策**

不自动拥有。当前实现按动态权限精确检查：执行 AI 函数需要 `AI_INVOKE`，管理模型需要 `AI_ADMIN`；
仅有 `SUPER` 仍会被拒绝。这一行为符合最小权限原则并保持不变。管控面只对 TaurusDB 受控管理员账号
显式授权，不把 `SUPER` 作为隐式旁路。

**待实现与验收**

- 保留“只有 SUPER、只有 AI_INVOKE、只有 AI_ADMIN、两者都有”的权限矩阵测试。

### 2.12 root 能否向普通用户授予 AI 权限

**评审问题**

TaurusDB 的受限 root 是否能够把 `AI_INVOKE`、`AI_ADMIN` 授予普通用户。

**事实与当前差距**

目标权限模型是：root 可以向业务账号授予和回收 `AI_INVOKE`，但不能委托 `AI_ADMIN`。当前升级脚本给
root 注册两项权限时均未授予 Grant Option，因此 root 目前不能完成预期的 `AI_INVOKE` 委托。

**方案决策**

1. 管控面授予 root：`AI_INVOKE WITH GRANT OPTION`。
2. 管控面授予 root：`AI_ADMIN`，但不带 Grant Option。
3. root 可将 `AI_INVOKE` 授予普通用户并回收；不得把 `AI_ADMIN` 扩散给普通用户。
4. `GRANT OPTION ON *.*` 不应绕过动态权限自身的委托边界。

**待实现与验收**

- 修复初始化/升级授权脚本，并覆盖 root 委托、回收、越权授予 AI_ADMIN 和存量实例升级。

### 2.13 图片、视频、Rerank 等能力的系统表扩展性

**评审问题**

当前 `capability` 只支持文本向量和文本生成，后续扩展新模态是否需要数据字典升级。

**事实与当前差距**

当前 `capability` 是仅包含 `TEXT_EMBEDDING`、`TEXT_GENERATION` 的 `ENUM`。新增 Rerank、图片、视频或
音频时必须修改表定义，产生不必要的数据字典升级。`default_dimension` 只对向量能力有意义，
`provider_options` 可以承载非敏感、Provider 特有的扩展配置。

**方案决策**

1. 在接口冻结前将 `capability` 改为 `VARCHAR(64)`，由内核能力注册表和 `dbms_ai` 严格校验允许值。
   不使用 MySQL `TEXT`：它不能不带前缀长度完整参与唯一键，而前缀唯一索引不能保证完整能力值唯一。
2. `default_dimension INT UNSIGNED DEFAULT NULL` 保持可空，只描述省略请求维度时已知的默认输出维度；
   它不是支持维度列表，也不限制客户显式请求其他维度。
3. `provider_options` 保持 JSON，采用 Provider/能力级白名单 Schema，禁止存放凭据和任意透传 HTTP 参数。
4. 新模态需要新的 SQL 契约、Canonical Request/Response 和 Adapter，不把所有能力塞入 `AI_ANALYZE()`。
5. 未被当前内核识别的 capability 可以由管控目录保留，但不能注册为可调用 Profile。
6. 每个 `(model_name, capability)` 只保留一行当前配置，唯一键为
   `uq_ai_model_config(model_name, capability)`；删除包含 `config_version` 的旧三列唯一键及
   `ix_ai_model_active(model_name, capability, status)`。模型数量很少，ACTIVE 列举不需要额外索引。
7. `config_version` 保留为当前行的单调发布代数。update、disable、delete 和回退均原位修改并递增版本；
   系统表不保存历史配置行，回退来源由管控面变更记录保存。

**待实现与验收**

- 完成 ENUM 到 VARCHAR、`dimension` 到 `default_dimension`、多行版本到单行当前配置，以及新唯一键的
  幂等升级脚本和兼容测试。
- 用 `TEXT_RERANK` 作为扩展性设计验证，但不承诺在本版本交付 Rerank SQL 接口。

### 2.14 新实例与存量升级场景

**评审问题**

新创建实例和老实例升级都需要适配该特性。

**方案决策**

1. 新实例：按冻结结构创建模型表。`capability` 使用 `VARCHAR(64)`，维度字段使用可空的
   `default_dimension`，唯一键为 `(model_name, capability)`，不创建 ACTIVE 辅助索引；同时注册动态权限
   和参数，`rds_ai_maas` 默认关闭。客户开启后由管控面配置网络、SCC/API Key、审计、兼容模型并显式授权。
2. 存量升级：使用幂等脚本创建/升级表和动态权限。同一 `(model_name, capability)` 存在多行时，先保留
   `config_version` 最大的行；版本相同则保留 `Id` 最大的行。随后将 ENUM 改为 `VARCHAR(64)`、将
   `dimension` 重命名为 `default_dimension`、删除旧三列唯一键和 ACTIVE 辅助索引，再建立两列唯一键。
   升级同时清理历史模型表中的明文 Key，不自动启用、不自动注册未经验证的模型，也不访问 MaaS。
3. 升级失败必须能够重试；回退时不破坏模型元数据，但旧版本不得误读新 capability 或凭据。
4. 新建、滚动升级、原地升级、备份恢复和降级需要分别验收，不能只用全新初始化数据库替代升级验证。

**待实现与验收**

- 补齐从发布基线版本升级的自动化测试、失败重试和回退测试。
- 核对系统表 DD 版本、动态权限注册顺序和开关默认值。

### 2.15 BGE-M3 默认维度与 AI_EMBEDDING 参数

**评审问题**

华为 MaaS 当前 BGE-M3 默认维度是否能通过接口发现；用户调用时是否必须指定维度。

**事实与当前实现**

MaaS `/v2/models` 不返回向量维度。真实接口验证表明，省略维度时 BGE-M3 返回 1024 维；请求 JSON
使用单数 `dimension` 会被忽略，而 Huawei Adapter 目标协议应把 SQL 的统一选项 `dimension` 映射为
Provider 字段 `dimensions`。当前 BGE-M3 会拒绝显式改变维度，甚至可能拒绝显式传入 1024，因此推荐
省略该选项。现有代码仍把维度作为本地固定断言并强制 1024，尚未实现下述目标契约。

**方案决策**

1. 客户推荐调用 `AI_EMBEDDING('huawei/bge-m3', text)`，不要求了解或重复指定默认维度；数据库不向
   MaaS 发送维度字段，并按响应数组的实际长度生成向量。
2. 模型表使用可空的 `default_dimension`。`NULL` 表示数据库不预设默认维度；非 `NULL` 仅记录已知的
   省略选项时默认输出，不作为本地允许维度列表，也不阻止显式请求其他维度。
3. 客户显式传入 `options_json.dimension=N` 时，数据库只校验 N 是合法的 VECTOR 维度，不根据模型名、
   `default_dimension` 或本地能力目录猜测客户意图；Huawei Adapter 将其映射为请求字段 `dimensions:N`。
4. Provider 拒绝请求或没有返回向量时，SQL 返回 Error。Provider 返回成功但实际维度与 N 不同时，
   `AI_EMBEDDING()` 返回实际向量并产生 Warning，不截断、不填充、不改写向量。Warning 可通过
   `SHOW WARNINGS` 查看，内容同时包含请求维度和实际维度。
5. 把结果写入固定 `VECTOR(N)` 列时仍由列类型执行精确维度校验；实际维度不同则写入失败。函数层的
   Warning 不能绕过列约束，也不能把不同维度的向量静默混入已有向量索引。
6. `AI_EMBEDDING()` 的返回元数据必须能容纳 Provider 实际返回的合法维度，不能继续固定声明为
   `VECTOR(1024)`。不增加 `allowed_dimensions` 字段；不同 Provider 的字段名由 Adapter 映射。

典型调用及诊断方式如下：

```sql
SELECT VECTOR_DIM(AI_EMBEDDING(
  'huawei/bge-m3',
  '测试文本',
  JSON_OBJECT('dimension', 512)
)) AS actual_dimension;
SHOW WARNINGS;
```

如果 Provider 成功返回 1024 维，第一条 SQL 成功并得到 `1024`；随后 Warning 显示请求维度为 512、
实际维度为 1024。若 Provider 拒绝 512 维，则第一条 SQL 直接返回 Error，不产生伪造或降级向量。

**待实现与验收**

- 覆盖省略 dimension、显式维度成功且一致、Provider 拒绝、成功但返回维度不一致并产生 Warning、
  以及写入固定 `VECTOR(N)` 失败等场景。

## 3. 对主体设计的修改清单

本轮评审结论已经同步到主体设计
[`taurusdb-maas-p0-committer-design.md`](taurusdb-maas-p0-committer-design.md)。本节不再把已经写入设计文档的
内容重复列为“待补充”，而是记录各章节当前冻结的设计结论，以及代码、管控面和验收尚未闭环的差距。

1. **1.3.3 管控面启用与运维编排：设计已刷新，管控实现待闭环。**
   主体设计已补充 MaaS 可见模型与 TaurusDB 兼容目录取交集、模型退市、网络与 SCC 凭据配置、动态权限、
   POC 调用频率、内容安全、总开关和备份恢复后重新绑定的启停顺序。剩余工作是管控面真正实现目录同步、
   参数持久化下发、部分失败补偿、模型退市通知和恢复实例重新绑定，并提交目标 Region 的联调证据。

2. **2.1.3 数据面、资源保护与 DFX：设计已刷新，部分内核能力待实现。**
   主体设计已描述实例级调用频率限制、连接/单次超时、1 MiB 响应上限、两阶段审计、Token usage、
   Provider request id 和 `KILL QUERY` 取消边界。当前仍需实现并验证调用频率令牌桶、Embedding timeout option、
   libcurl 对 `THD::killed` 的响应、嵌套 Token usage 解析及稳定错误分类；已有响应上限和审计能力仍需做
   并发内存与故障注入验收。

3. **2.2 元数据设计：单行当前配置、能力和默认维度契约已刷新，表结构迁移待完成。**
   主体设计已保留 `ACTIVE/DISABLED/RETIRED` 生命周期，并把 Embedding 字段目标定义为可空的
   `default_dimension INT UNSIGNED DEFAULT NULL`，把 capability 定义为 `VARCHAR(64)`，并以唯一键
   `(model_name, capability)` 保证每个模型能力只有一行当前配置。`config_version` 是当前行发布代数，
   系统表不保存历史行；删除 ACTIVE 辅助索引。当前代码仍使用 ENUM、旧 `dimension`、三列版本唯一键、
   ACTIVE 辅助索引和多行发布，需要完成 bootstrap/升级、`dbms_ai`、Registry、复制和 MTR 的一致改造。

4. **2.3 接口描述：客户接口目标契约已刷新，代码实现待对齐。**
   `AI_EMBEDDING(model_name, text [, options_json])` 省略维度时不发送 Provider 维度字段；显式
   `dimension=N` 表达客户请求意图，Huawei Adapter 映射为 `dimensions:N`。Provider 拒绝时返回 Error；
   Provider 成功但实际维度不同，函数返回实际向量并产生 Warning，固定 `VECTOR(N)` 列不匹配仍拒绝写入。
   当前代码仍有本地 1024/Profile 相等性拦截、固定 `VECTOR(1024)` 返回元数据且请求未发送 `dimensions`，
   需要完成实现、错误/Warning 契约和离线/真实验证。模型管理还需补充用于临时停用的
   `dbms_ai.disable_model()`；永久退市继续使用 `delete_model()` 标记 `RETIRED`。

5. **2.4 参数描述：配置归属已经明确，参数与运行时实现待完成。**
   主体设计已统一 `rds_ai_maas_*` 命名，并删除语句累计等待变量；调用频率使用全局实例参数，由 TaurusDB
   管控参数系统持久化，不增加重复的 `dbms_ai` 写入口。仍需完成三个旧变量重命名、频率参数、权限、
   动态生效、重启/节点替换恢复顺序，以及总开关开启前的管控预检。

6. **2.5 升级回退：设计规则已刷新，升级矩阵待验证。**
   主体设计已覆盖新建实例、存量升级、降级预检、备份恢复、跨 Region 网络/SCC 重新绑定和 root 动态权限
   委托边界。剩余工作是完成幂等升级脚本、失败重试、历史明文 Key 清理、权限注册顺序和新旧版本混部验证。

7. **2.6 故障与亚健康：故障语义已刷新，稳定错误码和告警闭环待完成。**
   主体设计已覆盖模型退市、调用频率/Provider 配额、内容安全拦截、超大响应、超时、用户取消、Token 对账、
   STARTED/terminal 审计异常，以及 Embedding 成功响应维度偏差只产生 Warning 的语义。剩余工作是稳定
   SQL error/SQLSTATE 与 Warning 文本、审计分类、指标和日志平台告警/恢复规则。

8. **第 3 章非功能设计：设计已刷新，目标环境数据待补齐。**
   主体设计已说明同步调用适用范围、敏感数据出站边界、MaaS 内容安全责任、并发内存风险和不承诺性能 SLA。
   商用前仍需在目标 Region 补充容量、长稳、内存回落、网络超时、内容安全附加时延和费用边界数据。

9. **第 5 章测试设计：测试矩阵已刷新，新增用例待落地。**
   主体设计已列出离线 MTR、真实 MaaS、故障、边界、升级和长稳测试。需要新增可控制实际向量维度的 fixture，
   覆盖省略维度、显式维度一致、Provider 拒绝、成功偏差 Warning、`SHOW WARNINGS`、通用 VECTOR 返回类型和
   固定 `VECTOR(N)` 写入失败；同时补齐调用频率、Embedding/Analyze 单次超时、取消、模型退市、内容安全和管控恢复用例。

上述未闭环项继续进入商用化 Backlog；只有代码、自动化测试和目标环境证据均完成后，才能从“设计已刷新”
变更为“已交付”。

## 4. 商用阻断项和优先级

本节与
[`taurusdb-maas-commercialization-backlog.md`](taurusdb-maas-commercialization-backlog.md) 配合使用：本节说明
评审门禁及优先级，Backlog 跟踪责任人、状态和关闭证据。某项只有同时具备代码或管控变更、离线回归、
目标环境验证和风险结论，才能从阻断项中关闭；仅完成设计、Demo 或单次真实调用不表示商用可用。

### 4.1 P0：生产商用前必须完成

1. **生产凭据控制面（对应 G-001）。** 华为 MaaS API Key 必须由 SCC/Secret 服务加密托管，完成受控
   解密、下发、轮换、吊销和节点一致性；不得出现在模型表、SQL、binlog、备份、错误日志或审计中。
   当前 `rds_api_key` 明文启动注入只允许开发验证；接口冻结前重命名为
   `rds_ai_maas_huawei_api_key`，仍不能作为生产交付。

2. **Embedding SQL 接口与模型表冻结。** 将 capability 从 ENUM 改为 `VARCHAR(64)`；完成 `dimension`
   到可空 `default_dimension` 的物理表、bootstrap 和升级迁移；每个 `(model_name, capability)` 只保留一行，
   唯一键为 `uq_ai_model_config(model_name, capability)`，删除旧三列唯一键和 `ix_ai_model_active`。
   `dbms_ai` 改为原位更新并递增 `config_version`，Registry 不再选择多行最大版本。删除 1024/Profile
   本地相等性拦截。省略 dimension 不发送
   Provider 字段；显式 `dimension=N` 由 Huawei Adapter 映射为 `dimensions:N`。Provider 成功但实际
   维度不同，函数返回实际向量并产生稳定 Warning；Provider 拒绝或固定 `VECTOR(N)` 列不匹配仍返回 Error。
   `AI_EMBEDDING()` 返回元数据不能继续固定为 `VECTOR(1024)`。

3. **模型兼容目录与退市闭环。** 管控面只注册 MaaS 当前可见模型与 TaurusDB 已验证兼容目录的交集；
   完成预警、临时 `DISABLED`、永久 `RETIRED`、客户确认迁移和回退。不得根据模型名称猜测能力，也不得
   静默替换向量模型；向量模型升级必须重算向量并重建索引。

4. **同步调用超时、取消和输出边界。** 落地连接超时 10 秒、Embedding 默认总超时 60 秒、Analyze 默认
   300 秒，以及两类函数 `timeout_ms` 最大 1800 秒。`KILL QUERY` 必须能够中止 libcurl 等待、释放
   资源并写正确审计终态；该能力完成前不得开放 30 分钟上限。保持 HTTP Response Body 1 MiB 硬上限；
   `max_output_tokens` 省略时不发送，显式值映射为 `max_completion_tokens`，不使用跨模型固定 32768 上限。

5. **权限和敏感数据出站边界（对应 G-008 及安全评审）。** SUPER 不隐含 `AI_INVOKE` 或 `AI_ADMIN`；
   存量与新建 `root@'%'` 仅可向普通账号授予/回收 `AI_INVOKE`，不能转授 `AI_ADMIN`、总开关或其他系统权限。
   完成 SELECT/视图/存储程序与 `AI_INVOKE` 组合的出站权限测试。数据库不承诺内置 DLP；生产必须启用
   MaaS 内容安全策略并由产品明确责任和附加时延。

6. **审计、Token 对账和生产 DFX（对应 G-004、G-006）。** 两阶段审计必须完成文件轮转、采集、保留、
   访问控制、磁盘水位和告警；STARTED 未闭合可按 `call_id` 推断 `UNKNOWN`。解析
   `prompt_tokens`、`completion_tokens`、`total_tokens` 及可选嵌套明细，关联 Provider request id，并与
   MaaS 统计对账。为权限、模型、凭据、限流、超时、取消、协议、响应超限和审计故障提供稳定的
   SQLSTATE/错误码；Embedding 维度成功偏差提供稳定 Warning 文本。

7. **主备、复制和故障切换（对应 G-002）。** 完成主节点调用、ROW 复制、STORED 生成列、只读节点边界、
   凭据与参数下发、升主及故障切换矩阵。备机不得因应用 row image 重复调用 MaaS；切换后新主节点只有在
   网络、凭据、审计和 Profile 均就绪后才能开启总开关。

8. **升级、回退和备份恢复（对应 G-005）。** 完成新建实例、存量升级、旧模型表迁移、动态权限注册、
   幂等重试、新旧版本混部和降级拦截。备份恢复到新实例后默认保持 `rds_ai_maas=OFF`，重新绑定 Region 网络、
   SCC 凭据、审计和模型 Profile，预检通过后再开启；审计文件不作为事务数据恢复。

9. **目标 Region 网络与模型准入（对应 G-003）。** 每个上线 Region 分别完成 DNS、TLS、Policy Route、
   NAT/EIP、安全组、回流、Endpoint、模型权限和配额验证。网络或模型列表接口异常时不得删除现有 Profile、
   自动切换模型或绕过 Endpoint allowlist。

10. **性能、容量和可靠性（对应 G-007）。** 在目标 Region 完成长稳、并发内存、429、网络故障、磁盘满、
    审计不可写和模型长响应演练，形成容量模型、告警阈值、灰度退出条件及资源回落证据。当前不承诺
    P50/P95/P99、QPS 或吞吐 SLA，但必须证明不会因受控并发和最大响应导致 mysqld OOM 或资源泄漏。

其中第 2 项“SQL 接口、模型表和变量冻结”统一由 `G-009` 跟踪；第 3 项“模型兼容目录与退市闭环”由
`G-012` 跟踪。跨云 Provider 的通用模型治理仍保留在 P1 `G-102`，避免与当前 Huawei P0 退市闭环混淆。

### 4.2 POC 灰度上线前必须完成

1. 完成 Provider 无关的 `rds_ai_maas_max_calls_per_minute`、令牌桶并发准确性、动态更新和稳定限流错误；
   管控面持久化该参数，并保证正整数限制先于总开关生效。正式取消灰度限流时设置为 0，不修改客户 SQL。
2. 明确 POC 周期内累计调用次数或 Token 阈值、统计周期、统计延迟和允许超额误差；达到阈值后由管控面
   关闭 `rds_ai_maas`，不能把进程内每分钟频率限制宣传为总费用上限。
3. 为灰度实例启用 MaaS 内容安全策略、审计采集、预算告警和可回退开关；确认 API Key、模型配额和
   Endpoint 属于隔离的 POC 资源。
4. 产品页面明确同步调用、可能费用、事务回滚不撤销外部调用、超时/取消后远端仍可能计费、模型输出
   不自动执行，以及当前只支持主节点等限制。

### 4.3 P1：不阻断当前华为文本/向量能力商用，但阻断扩展承诺

1. 百炼、火山方舟和 Bedrock 等多 Provider Adapter、认证解析、Endpoint 策略和真实 smoke。
2. 进程级 Registry 缓存、配置版本失效与刷新机制。
3. RAG 应用侧资料解析、切分、ACL、引用展示、反馈运营和向量重建任务。
4. DBA 诊断快照采集、脱敏、人工审批和变更单联动。

### 4.4 后续演进

1. 异步/批量推理任务和本地小模型推理服务 Adapter。
2. Rerank、图片、视频、流式输出、工具调用和 Agent 等独立 SQL 接口。
3. 独立 DLP、按模型/预算的细粒度授权和正式商用计费策略。

## 5. 验证计划

验证计划按“当前基线”和“待关闭门禁”分开记录。计划中列出测试不表示已经通过；每次执行必须记录源码
commit、构建类型、参数、Profile/config version、脱敏命令输出、`call_id`、Provider request id、结果、
失败分类和风险结论。真实调用必须使用专用可撤销凭据，不得把 Key、完整 prompt/response 或向量提交到 Git。

### 5.1 当前已验证基线

1. 2026-08-08 在代码提交 `6307be989878b06d0b6b2aa2ac108ff5ab1241f0` 上重新构建 Debug `mysqld` 和
   `merge_small_tests-t`；AI GUnit 26 个、Huawei Adapter GUnit 10 个通过。执行当前 10 组 MaaS
   MTR/result，SQL 契约、Embedding、Analyze、治理、模型管理、发布/复制、RAG、MIXED/ROW 生成列复制和
   `rds_api_key` SQL 不可见全部通过，测试框架报告 11/11（含 `shutdown_report`）。完成目标改造后必须在
   新 commit 重新执行同等回归；当前通过结果只证明旧行为稳定，不能替代目标契约测试。
2. 已有真实 smoke 脚本覆盖 `bge-m3` Embedding、`glm-5.2` Analyze、向量写入/索引/STORED/RAG，以及
   六个文本生成模型的人工对比入口。当前关闭记录只证明开发环境的 `bge-m3`、`glm-5.2` 基本调用闭环，
   不证明新维度契约、生产凭据、主备、网络、性能或商用 DFX 已通过。
3. 已实现的 1 MiB 输入/HTTP Response Body 边界、Endpoint policy、ROW 复制和两阶段审计，仍需在目标
   commit 补齐最终 content、request id、故障注入和目标环境证据。当前每语句 32 次、实例 32 并发属于
   待删除实现，不作为验证规格；当前 GUnit/MTR 通过也不代表这两个硬限制可以保留。
4. 离线 `mtr/fixture-*` 会绕过真实凭据和 HTTP，因此不覆盖 libcurl 阶段取消、Huawei 请求字段、真实网络、
   TLS、Provider 限流或 MaaS 计费。真实 smoke 的历史成功记录不能替代目标代码提交上的协议和集成复验。

### 5.2 新增离线 MTR 与单元测试

1. **Embedding 维度契约。** 使用可控制请求和响应维度的 fixture 覆盖：省略 dimension；显式维度成功且
   一致；合法维度与 `default_dimension` 不同仍出站；Provider 拒绝；Provider 成功但实际维度不同并返回
   Warning；`VECTOR_DIM()` 返回实际值；`SHOW WARNINGS` 包含请求/实际维度；固定 `VECTOR(N)` 写入不匹配
   返回 Error；非法维度值和未知 option 本地拒绝且零出站。
2. **请求协议和返回类型。** 断言 Huawei JSON 省略维度时没有 `dimensions`，显式请求时只有复数字段
   `dimensions:N`；禁止误用单数 `dimension`。验证 `AI_EMBEDDING()` 通用 VECTOR 返回元数据可以承载
   数据库允许范围内的实际维度，且不截断、不填充、不静默改写向量。
3. **模型元数据和管理。** 断言 `capability VARCHAR(64)`、`default_dimension=NULL/正整数`、唯一键只含
   `(model_name, capability)` 且不存在 `ix_ai_model_active`。覆盖首次 register 版本 1、重复 register 失败、
   update/disable/delete/回退原位更新并递增版本、DISABLED/RETIRED 不可调用、全过程行数保持 1、并发调用
   继续使用已解析快照、直接 DML/DDL 拒绝和 Endpoint allowlist。
4. **HTTP、输出和 Token。** 覆盖 Response Body 的 `1 MiB-1`、恰好 1 MiB、`1 MiB+1`，单块/累计、
   成功/错误响应、UTF-8/JSON 转义、最终 content、`finish_reason=length` 和 request id。省略
   `max_output_tokens` 时请求不含长度参数；显式值映射为 `max_completion_tokens`；解析正常、缺失和嵌套
   usage 字段，不因未知可选明细破坏调用。
5. **超时和取消。** 通过可注入延迟的 Transport 验证连接 10 秒、Embedding 默认 60 秒、Analyze 默认
   300 秒、两类函数显式最大 1800 秒。延迟期间执行
   `KILL QUERY`，验证及时取消、后续 AI 调用不执行、curl/缓冲释放和 `CANCELLED` 审计终态；
   已经出站的调用和费用不回滚。
6. **调用频率。** 覆盖默认 0、正整数、动态升降、设回 0、并发不超发、本地失败不计数、审计 STARTED
   失败归还许可和已出站失败仍计数；普通用户和 `AI_ADMIN` 均不能绕过管控参数权限。
7. **权限和敏感数据。** 验证 SUPER 不隐含 AI 权限，root 只能转授 `AI_INVOKE`；覆盖无 SELECT、无
   `AI_INVOKE`、仅具其一、两者都有等组合，以及表、系统表、视图和存储程序路径。断言 Key、prompt、
   response 和原始向量不进入 SQL 可见变量、错误日志、审计、binlog 或 MTR result。
8. **审计和复制。** 覆盖 STARTED 写失败零出站、terminal 写失败保留 Provider 原结果、未闭合 UNKNOWN、
   日志轮转边界和磁盘故障注入；覆盖 ROW FULL/MINIMAL/NOBLOB、STORED 正文/非正文更新、备机不重复外呼。

### 5.3 真实 MaaS 协议验证

1. BGE-M3 省略 dimension，应成功返回当前实际 1024 维；显式 512 应确认 Huawei Adapter 发送
   `dimensions=512`，并将当前 Provider 拒绝映射为脱敏 Error。不得要求真实 BGE-M3 制造“HTTP 成功但
   返回不同维度”，因为当前服务会拒绝显式维度；该 Warning 场景只由 5.2 的可控 fixture 稳定覆盖。
2. 对冻结的文本生成 Profile 验证省略/显式 `max_output_tokens`、`max_completion_tokens` 映射、非空最终
   content、finish reason、usage 和 Provider request id。六模型对比只用于兼容性与效果观察，不形成
   模型排名或性能 SLA。
3. 将数据库审计中的模型、配置版本、成功/失败、耗时、Token usage 和 request id 与 MaaS 统计抽样对账；
   允许明细字段缺失，但 `total_tokens` 与主要输入/输出 Token 不得被错误计费或重复累计。
4. 在受控环境验证 401/403、模型不存在/退市、429、5xx、DNS/TLS、连接/总超时和用户取消。RPM/TPM 429、
   内容安全拒绝和 Endpoint 禁用依赖相应 MaaS 配置，无法构造时必须记录外部阻塞，不能以未执行标记通过。
5. 真实长推理验证 Analyze 5 分钟默认值覆盖正常响应；30 分钟上限只在隔离验收环境显式使用，不作为
   性能 SLA。核对 Proxy、客户端和网络设备超时不早于数据库目标值，并确认本地取消后远端仍可能计费。

### 5.4 管控面、主备、升级和恢复验证

1. 验证首次开启、关闭、重复开启的幂等性和部分步骤失败补偿；网络、凭据、审计、Profile、权限和调用频率
   必须先于 `rds_ai_maas=ON` 生效。
2. 验证 SCC/API Key 创建、密文下发、轮换、吊销、旧 Key 失效和节点一致性；任何 SQL、日志、审计、
   binlog、备份和进程启动参数采样均不得出现明文 Key。
3. 覆盖新实例 bootstrap 和存量升级：ENUM 到 `VARCHAR(64)`、`dimension` 到 `default_dimension`、旧多版本
   行按最大 `config_version`（相同版本取最大 `Id`）收敛为单行、删除旧索引并建立新唯一键。验证迁移幂等、
   失败重试、旧表/明文凭据清理、新旧版本混部和降级拦截；分别覆盖存在/不存在 `root@'%'`。
4. 覆盖主节点、只读节点、ROW 复制、节点替换和故障切换；验证备机不重复调用 MaaS，升主后按管控配置
   恢复参数和凭据，并在预检完成前保持总开关关闭。
5. 覆盖同 Region/跨 Region 备份恢复。恢复模型 Profile 和权限元数据，但重新绑定目标网络、SCC 凭据、
   审计和模型准入；审计文件不作为事务数据恢复。
6. 验证模型列表与兼容目录取交集、模型退市预警、临时禁用、永久 RETIRED、客户确认迁移、向量重算/
   索引重建、效果验证和回退；未知模型不得自动注册。
7. 验证 POC 调用频率、累计配额、告警和达到阈值后关闭总开关；重启、节点替换、故障切换后恢复相同
   实例配置，正式商用设为 0 时不再限频。

### 5.5 安全、容量和长稳验证

1. 在目标 Region 以受控输入、固定并发和专用预算执行容量与长稳测试，记录端到端时延、成功率、429、
   mysqld RSS、单次/并发响应峰值、内存回落、CPU、线程、连接、网络和审计文件增长。至少覆盖单请求、
   由低到高的阶梯并发和连续多轮接近 1 MiB 响应，证明无 OOM、泄漏或不可恢复资源占用，并以实测判断
   是否需要后续专用并发保护。
2. 注入 MaaS 不可达、DNS/TLS 错误、网络抖动、审计目录不可写、日志轮转失败、磁盘高水位/满、Provider
   长响应和节点切换，验证 fail closed/fail open 边界、告警去重、自动恢复和灰度退出。
3. 使用无权限、越权、敏感标记和提示注入测试集验证 SQL 数据权限与 `AI_INVOKE` 组合；确认模型只看到
   调用者显式传入且有权读取的数据。当前没有数据库 DLP，应在报告中明确剩余风险，不得宣称能够阻止
   所有敏感数据外发。
4. 在启用 MaaS 内容安全策略的 Endpoint 上验证允许内容、违规内容、误拦截和附加时延；数据库只传播
   Provider 结果和稳定错误分类，不自行承诺识别所有敏感话题。
5. 检查审计采集、轮转、保留、访问控制、磁盘水位、STARTED 未闭合 UNKNOWN、Provider request id 关联、
   Token 对账和告警恢复，确保日志中不存在 Key、完整 prompt/response 或原始向量。

### 5.6 阻断项关闭证据

每个第 4 章阻断项必须提供独立证据包，至少包含：

1. 关联 Backlog ID、责任模块、源码 commit、构建类型和测试环境/Region。
2. 可复跑命令、完整退出码、通过/失败数量及失败分析；不得只提供截图或口头结论。
3. 脱敏的实例参数、模型 Profile/config version、网络/凭据版本标识和权限状态。
4. 代表性成功与失败的 SQL 结果、Warning、`call_id`、Provider request id、审计终态和 MaaS 统计对账。
5. 性能/容量/长稳指标、资源回落、告警和恢复证据，以及未解决风险和适用边界。
6. 安全检查结论：Key、完整业务数据、完整响应和原始向量未进入 Git、SQL 可查询配置、日志或审计。

关闭标准是该项所有必需验证通过、外部依赖满足且没有未接受的 P0 风险。测试未执行、环境不具备、结果
不可复现或只有 Demo 成功时，状态只能保持未开始/验证中，不能标记已关闭。

## 6. 官方资料依据

1. [华为云 MaaS：查询模型列表](https://support.huaweicloud.com/model-call-maas/model-call-029.html)
2. [华为云 MaaS：模型下线公告](https://support.huaweicloud.com/bulletin-maas/bulletin-maas-0002.html)
3. [华为云 MaaS：模型目录与推荐模型](https://support.huaweicloud.com/model-call-maas/usermanual_maas_0008.html)
4. [华为云 MaaS：Embedding API](https://support.huaweicloud.com/model-call-maas/model-call-027.html)
5. [华为云 MaaS：V2 Chat Completion API](https://support.huaweicloud.com/model-call-maas/model-call-019.html)
6. [华为云 MaaS：自定义 Endpoint、限流和内容安全](https://support.huaweicloud.com/model-call-maas/model-call-048.html)
7. [华为云 MaaS：调用统计 API](https://support.huaweicloud.com/api-maas/ShowStatistics.html)
8. [华为云 MaaS：调用统计概述](https://support.huaweicloud.com/call-statistics-maas/maas-modelarts-0074.html)
9. [华为云 MaaS：CES 监控指标](https://support.huaweicloud.com/call-statistics-maas/maas-modelarts-0095.html)
