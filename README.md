# aura-memory-runtime

**Agent-Native Query & Memory Runtime: Stage 1 可运行验证原型 (C++26, 基于 [Aura](https://github.com/cybrid-systems/aura))**

这个仓库用来验证《Agent-Native Query & Memory Runtime 完整设计方案》的 Stage 1。设计里每一条可证伪的主张，都对应一个会失败的测试。这些测试和基准在 CI 中运行，CI 使用的 GCC 16 镜像和 Aura 是同一个。

```
Intent ──intent_to_plan──▶ EDSL 计划 (Retrieve/Filter/Join/Rank/Assemble/CostHint/Parallel)
                               │  optimize(plan, rules)   ← 规则 = 数据（S-expr AST，分 tier）
                               ▼
                         execute(plan, store) ──▶ Metrics(cost, tokens, sink_calls …)
                               ▲
  worldline (Aura fiber ctx + COW 规则集 + Aura overlay) ── run_queries/feedback ── promote / abort
                               │
              append-only log (CRC32C, 分段封存) ──replay──▶ hot state (+ checkpoint)
```

## 构建与运行

```bash
git clone --recursive https://github.com/cybrid-systems/aura-memory-runtime
cd aura-memory-runtime
./scripts/dev.sh          # 在 ghcr.io/cybrid-systems/dev:v1.0.9 里执行 cmake+ninja → ctest → bench
```

CI 工作流在 `ci/github-actions-ci.yml`。当前推送用的 token 没有 `workflow` scope，因此还没放进 `.github/workflows/`。有权限的人执行 `mkdir -p .github/workflows && git mv ci/github-actions-ci.yml .github/workflows/ci.yml` 即可启用。

构建要求 GCC 16 和 `-std=c++26 -fcontracts`。原因是 Aura 头文件用了 `pre(...)` / `contract_assert`。Clang 和更老的 GCC 都无法编译。

## 设计主张 → 测试映射

| # | 设计主张 | 测试（`tests/`） | 怎么证伪 | 结果 |
|---|---|---|---|---|
| 1 | 主线只看到已 promote 的规则版本 | `test_main_line_isolation`: `main_sees_only_promoted_rules` | 两个并发 worldline 来自同一快照，A 加入 pushdown 规则。检查三件事：主线的计划和结果在 promote 之前保持不变；B 看不到 A 的规则；A promote 之后主线才看到，B 仍停在自己的快照 | ✅ |
| 1b | 每个 worldline 运行在独立的 fiber 上下文中 | `worldline_runs_on_its_own_aura_fiber_context` | 在 fiber 内读取 Aura 的 `fiber_ctx::active().owner_fiber`，它必须等于 worldline id。fiber 本地状态不能串到别的 worldline。`run_queries` 每跑完一条 query 就 yield 一次 | ✅ |
| 2 | abort 或未 promote 的 worldline 不留任何痕迹，崩溃恢复之后也一样 | `test_abort_no_trace`（5 个用例） | 比较 abort 后主线 digest；让进程带着开放的 worldline 消失后再 reopen，验证它被丢弃并且丢弃动作写进了日志；checkpoint 时 worldline 还开着的情况；在 promote 记录写入过程中每隔 7 字节注入一次崩溃，验证结果是全有或全无；被拒绝的 promote | ✅ |
| 3 | 半截写入被截断，已确认的前缀不丢 | `test_torn_writes`（2910 个检查） | 在最后 3 条记录范围内逐字节截断；在 header 或 payload 中翻转任意一个字节；已封存段损坏时 fail-closed；`fork` 出子进程，fsync 并 ack 之后发 SIGKILL，验证所有已 ack 的 seq 都在；对 runtime 的 `put_memory` 逐字节注入崩溃 | ✅ |
| 4 | 热状态等于日志重放 | `test_hot_state_replay` | 4 个随机种子，每个 1500 次混合操作（写入、删除、实验、promote/abort、管理员改规则、checkpoint，16KB 分段）。要求 live digest、纯重放、checkpoint+尾部、全量重放四者两两相等，查询结果也相同；任何一个 checkpoint 损坏都要能回退；checkpoint 领先于日志时必须忽略它 | ✅ |
| 5 | COW 共享未修改的规则节点（实测） | `test_cow_sharing` | 1000 条规则中修改 1 条。worldline 的 6000 个节点里 **5994 个（99.90%）与主线共享**，新增的 6 个节点恰好是新规则本身。顶层规则 id 有 999/1000 是共享的。Aura PCV 的 `with_set_cow_total` 只增加 1 | ✅ |
| 6 | 规则改写在实验中降低成本指标，并被 promote | `test_promote_improves` | pushdown 规则让平均成本从 **3399.8 行降到 98.0 行（34.7 倍）**，质量（与基线结果的 F1）为 1.000，因此被 promote；主线总成本从 203986 降到 5883，结果完全相同，重启后依然保持。删掉 ts 过滤的坏规则质量只有 0.208，被拒绝；不命中任何计划的规则被拒绝，理由是 "no cost improvement" | ✅ |
| + | 规则是可以查询的数据，EDSL 语义正确 | `test_rules_engine` | S-expr 往返；模式绑定；按 tier 查询规则；optimize 到达不动点并且幂等；never-auto 安全规则会屏蔽 secret；Join/Relate 和 token 预算生效 | ✅ |
| + | tier、上限与冲突 | `test_tiers_and_limits` | never-auto 规则既不能改也不能删，实验也不能新建 never-auto 规则；needs-confirm 需要 `confirmed=true`；超过活跃 worldline 上限时抛出 `WorldlineLimit`；两个 worldline 改同一规则时先提交者赢，后者被 abort；主线在快照之后改了同名规则时 promote 冲突，改无关规则不冲突；来自未来的快照会被拒绝 | ✅ |

最近一次本地运行（dev:v1.0.9，GCC 16.2）：**8/8 个 ctest 通过，约 3290 个检查，0 个失败**（Aura pin 升到 7bb2ed954、dev:v1.1.1 后：9/9 通过，新增 `test_soak_worldlines`）。

## 组件

| 模块 | 文件 | 要点 |
|---|---|---|
| EDSL AST | `include/amr/ast.hpp` | 节点池采用 FlatAST 风格的 `NodeId`，只追加不修改。子节点使用 **Aura `PersistentChildVector<NodeId>`**。提供 S-expr 打印和解析，以及结构相等与结构哈希 |
| 规则即数据 | `include/amr/rules.hpp` | `(Rule :name :tier :prio (Pattern …) (Rewrite …))`。模式语言支持 `?x`、`_`、`"?v"`、`"_"`（必须存在）、`"~"`（必须不存在）、`:as`。模板没有覆盖项时直接复用原节点，这是共享的来源。规则集也可以用模式来查询 |
| Intent → plan | `include/amr/plan.hpp` | 生成 `CostHint(Assemble(Rank(Join?(Filter*(Retrieve | Parallel(Retrieve…))))))`。执行器统计 `rows_scanned` / `rows_processed` / tokens / sink_calls / hint_violations |
| append log | `include/amr/log.hpp` | 32B 头：magic `AMRL`、version、type、len、seq、worldline，加 header CRC32C，后接 payload 和 payload CRC32C。`Seal` 记录携带整段的 CRC。非末尾段损坏或未封存时 fail-closed；末尾段截断到最后一条有效记录。支持 fsync/fdatasync 和故障注入 |
| runtime | `include/amr/runtime.hpp` | 日志先写，然后 `apply_record`。live 路径和恢复路径共用同一个状态转移函数。`WlPromote` 自包含（基线版本加全部 mutation），`WlMutate` 只用于审计。checkpoint 流程是 tmp→fsync→rename→fsync(dir)，保留 2 份 |
| fiber | `include/amr/fiber.hpp` | ucontext fiber。每次 resume 时 attach **Aura `fiber_ctx::Context`**，方式与 Aura `serve/fiber.cpp` 相同 |

## Aura 的复用情况：哪些可以直接用，哪些缺失

直接从子模块编译链接（`third_party/aura` @ `69d3600`，没有复制代码）：

| Aura 组件 | 用途 | 评价 |
|---|---|---|
| `core/persistent_child_vector.hh` | EDSL 子节点和规则集的 COW 存储 | ✅ O(1) 拷贝、结构共享，有 `with_set_cow_total` 指标。⚠️ 见 F1、F2 |
| `compiler/fiber_cow_overlay.hh` | worldline 写集、先提交者赢的冲突检测、rollback、purge | ✅ 语义正好对应 promote 冲突。⚠️ 见 F4、F5 |
| `core/fiber_context.{hh,cpp}` | 给每个 worldline 一个 Aura fiber 上下文（`owner_fiber`） | ✅ 可以独立链接 |
| `core/contract_handler.cpp` | C++26 contract 违约处理器 | ✅ 必须一起链接，否则会出现 `handle_contract_violation` undefined |

没能用上，需要自己补的部分：

- **F1：PCV 不能同时实例化两种元素类型。** 类内 friend 模板 `share_storage<U>` 会在每次实例化时重复定义，GCC 16 报 `redefinition`。因此只能用 `PCV<uint32_t>`，节点改为放进 NodeId 池。（上游：cybrid-systems/aura#4769）
- **F2（上游已修，cybrid-systems/aura#4770）：PCV `with_set() const` 曾在 `use_count()==1` 时原地修改接收者**（#2140 引入的快速路径）。当前 pin 上接收者保持不变，`test_cow_sharing.aura_pcv_with_set_leaves_unique_receiver_untouched` 断言这一点；`RuleSet::with_upsert` 的本地拷贝保留为防御。
- **F3：`serve/fiber.{h,cpp}` 的 `Fiber` 无法单独链接**，它依赖 Evaluator、GC、mailbox 等。Aura 也没有可嵌入的库目标，规则和 workspace 引擎只能在解释器里跑。所以本仓库自己写了一个 ucontext fiber，但沿用 Aura 的 Context。（上游：cybrid-systems/aura#4774）
- **F4：overlay 的冲突基线是 `begin()` 时刻的 registry seq，而不是历史快照**，所以从旧快照开出的 worldline 会漏掉“快照之后、begin 之前”发生的提交。本仓库额外加了一层 `rule_modified_at` 检查（陈旧快照守卫）。（上游：cybrid-systems/aura#4771）
- **F5：overlay 的 `commit()` 没有 prepare / validate-only 阶段。** 冲突扫描、apply 和写 commit log 在一次调用里完成，没法做到“先校验，再持久化写日志，再发布”。目前只能在 commit 之后写 `WlPromote`；如果日志写入失败（不是进程崩溃），registry 里会留下幽灵 claim。（上游：cybrid-systems/aura#4771）
- **F6：overlay 只存在于内存**，没有持久化和重放。这对 Stage 1 没有影响，因为恢复时开放的 worldline 一律丢弃。
- **F7：`ast:snapshot` 是整份 workspace 的 unparse 加深拷贝**，只有源码完全相同时才共享（#4598 phase 3），不是结构化 COW，所以不能直接充当 O(1) worldline 分叉。本仓库在 C++ 层用 PCV 实现了 O(1) 分叉。（上游：cybrid-systems/aura#4772）
- **F8：CI 镜像 `dev:v1.0.9` 里的 `/usr/local/bin/ld.lld` 无法运行**，缺少 `libicui18n.so.70`，所以只能用 bfd ld。（上游：cybrid-systems/aura#4773）

## 基准（`bench/bench.cpp`，dev:v1.0.9 容器，本地 x86_64）

| 基准 | 数值 |
|---|---|
| 日志追加，128B，不 fsync | 72.9 万条/秒（1.37 µs/条） |
| 日志追加，128B，每条 fdatasync | 4175 条/秒（240 µs/条） |
| 打开、校验并扫描 20 万条记录（8 段） | 311 ms |
| 全量重放恢复，55002 条记录 | 63 ms |
| checkpoint 加 5000 条尾部恢复 | 66 ms（见风险 R3） |
| `begin_experiment` / COW 修改 1 条规则 / 全量文本深拷贝，规则数 = 10 | 0.9 µs / 3.0 µs / 1.9 µs |
| 同上，规则数 = 1000 | 0.9 µs / 2.9 µs / 291 µs |
| 同上，规则数 = 10000 | **1.0 µs** / 5.1 µs / **4356 µs** |
| 基线规则下每条查询的平均成本和延迟 | 13533 行 / 962 µs |
| promote pushdown 之后 | **329 行 / 88 µs** |
| `intent_to_plan` + `optimize`（4 条规则） | 7.6 µs |

worldline 分叉的开销与规则数无关，规则从 10 条增加到 10000 条时都在约 1 µs。深拷贝则随规则数线性增长。

## 关键发现

1. 主线隔离和不留痕迹可以只靠两条纪律来保证：**先写日志再 apply**，以及**只有 `WlPromote` 会改变主线**（`WlMutate` 仅用于审计）。崩溃恢复不需要任何 undo 逻辑，丢弃开放的 worldline 就够了。
2. 要把 promote 做成全有或全无，`WlPromote` 必须**自包含**，也就是携带全部 mutation，而不是引用之前的 `WlMutate`。否则截断时可能出现“只合并了一半”的情况。
3. 冲突检测需要两层：Aura overlay 负责 worldline 之间的先提交者赢，`rule_modified_at` 负责主线在快照之后的修改（F4）。
4. 质量门必须放在成本门之前。在宽时间窗下，删掉 ts 过滤的坏规则在 Rank-by-recency 之下会显得无害，质量仍是 1.0，成本也略有下降，这时会被误 promote。只有在近时间窗的负载下它的质量才掉到 0.208。所以**反馈负载的覆盖面决定了 gate 是否可信**，这一点也和 aura-memory 的 held-out gate 结论一致。
5. Join/Relate 的语义（把关联行带进上下文，还是只做半连接过滤）必须在 EDSL 层写死。本原型选择前者。
6. Aura 自带的 EDSL 设施（宏、`std/rule`）的验证探针在 `tests/aura_edsl/`：`define-hygienic-macro` 的卫生语义和展开性能都达标；宏参数个数不检查、`eval` 吞掉未绑定名字、`std/rule` 不校验输入且 `rule:define` 为 O(n²)，已分别报到上游 aura#4833–#4836。本仓库的规则 EDSL 是 C++ 实现，不依赖这些设施。

## 未解决的风险

- **R1：单进程、单 writer**，没有多进程锁和组提交（group commit）。每条 fsync 的吞吐只有约 4k/s。
- **R2：节点池只追加不回收**，长时间跑的实验会让它一直增长，需要 epoch 或引用计数 GC。`test_soak_worldlines` 实测（1500 个 worldline 生命周期，Aura 7bb2ed954 pin）：每个生命周期新增约 87 个节点、约 45 KB 常驻内存，线性增长不回落；日志重放和重新打开后的状态 digest 与内存状态一致。
- **R3：checkpoint 序列化的是完整热状态**，在这个基准里并不比重放快（66 ms 对 63 ms），只有日志中删除和覆盖操作较多时才有优势。需要增量 checkpoint 或 LSM 化。
- **R4：质量度量是与基线结果的 F1**，只适用于“等价改写”类规则。真正改变语义的规则需要人工标注的 held-out 集。
- **R5：fiber 是协作式的**，`run_queries` 在单线程内交替执行，并没有利用多核（Aura #4707 也有类似问题）。
- **R6：模式语言没有谓词和否定子树**，tier 只在规则名的粒度上执行。
- **R7：F2、F4、F5 都依赖 Aura 上游修复**。现在的绕行（本地拷贝、额外的陈旧快照检查）是额外的维护负担。

## 复用与致谢

- **cybrid-systems/aura**：PCV、fiber COW overlay、fiber context、contract handler 通过子模块直接复用。
- **`/workspace/aura-memory`（及 `aura-memory-progress.md`）**：Aura 语言实现的记忆系统，worldline 分叉是 O(1) 的持久化 store checkpoint，配合 `ast:snapshot/restore`，并且有“严格更优才保留”的 gate 和 held-out gate。本仓库沿用了“worldline = O(1) 快照 + gate 决定 promote”的模型。它记录的解释器层上游 bug（#4611/#4629/#4625/#4669 等）是 Stage 1 规则引擎改在 C++ 层实现、而不跑在解释器上的原因。
- **`/workspace/aura-kernel-sink`**：identify → plan → .so → `mutate:rebind` → score/heal 的热路径闭环，启发了“Retrieve 的 pushed 字段 = sink 片段”和成本反馈的设计。
- **fiber8 / mem7 / edsl8 报告**：对 Aura fiber、mutation、EDSL 成熟度的评审结论，本仓库据此选择复用哪些组件（overlay 与 fiber_ctx 可用，serve Fiber 不可用）。

## License

Apache-2.0
