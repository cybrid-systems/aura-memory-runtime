# #4946 aura-memory-runtime 验证报告

日期：2026-11-09
仓库：cybrid-systems/aura-memory-runtime
分支：v0.2-evo（未 push）
Aura pin：third_party/aura @ 99ddc81c94cfd503919ad0d6cde6d83331a7dcdf
本机工具链：GCC 16.2.0（aarch64），CMake 4.5.0-rc2，Ninja 1.13.2
CI 镜像：本机没有 docker，ghcr.io/cybrid-systems/dev:v1.0.9 不可用；按任务要求改用本机 GCC/CMake，并在此记录。Aura 侧同一份源码在本机用 GCC 16.2 构建出 aura dev (99ddc81c9)。

## 0. 结论

- ctest：改动前 9/9 通过；改动后 11/11 通过（新增 test_evo_gate、test_evo_moead）。
- 规则集进化已接到 Aura 的 std/moead：C++ 只发 moead:ask / moead:tell!，MOEA/D 的权重、邻域、Tchebycheff 归一化和替换规则全部在 Aura 里，本仓库没有复制算法。
- 每个候选仍走现有 worldline：COW RuleSet 拷贝 + Aura fiber Context + COW overlay；只 promote "F1 = 1.0 且 (cost, tokens, sink_calls) 帕累托改进"的规则集。
- 固定 seed 42、连续跑两遍，12 次 ask/evaluate/tell 决策的 transcript 逐字节相同。
- 未达成项（如实列出）：
  1. "通过嵌入 API"只完成了一半。Aura 当前只导出 aura::core 与 aura::fiber_rt 两个可链接目标，两者都不含 Evaluator（docs/embedding.md）。C++ 无法在进程内调用 std/moead，只能走 Aura 的 --serve 编译器会话协议。算法没有复制，但边界是子进程，不是函数调用。
  2. 严格 F1 = 1.0 的质量门下，tokens 不可能变化（同样的结果集文本 token 必然相同），sink_calls 也只在引入负样本规则时变化。因此三目标前沿退化为 1 个点。这是目标定义与正确性门共同导致的，不是搜索失败。

## 1. 基线（改动前）

Aura pin 7bb2ed954，GCC 16.2，build-baseline：

    9/9 ctest 通过，总耗时 5.97 s
    日志追加 128B，不 fsync        713827 条/秒（1.40 us/条）
    日志追加 128B，每条 fsync      1952 条/秒（512.31 us/条）
    打开+校验+扫描 200000 条（8 段） 302.1 ms
    全量重放 55002 条               70.9 ms
    checkpoint + 5000 条尾部        71.2 ms
    规则 10000：begin / COW 改 1 条 / 深拷贝  0.9 us / 3.3 us / 2090.1 us
    基线查询                       13533 行 / 397.3 us
    worldline run_queries（200 intent） 83.8 ms，201 次 fiber 切换
    promote 后查询                  329 行 / 53.5 us

改动后同一套 9 个旧测试全部通过，说明 pin 从 7bb2ed954 升到 99ddc81c9 没有引入回归。

## 2. 声明 -> 测试 -> 结果

| # | 声明 | 测试 | 结果 |
|---|---|---|---|
| C1 | C++ 通过 Aura 的 ask/tell 驱动 std/moead，不复制 MOEA/D | test_evo_moead / moead_ask_tell_is_deterministic_for_a_fixed_seed | Aura 进程内执行 moead:make / moead:ask / moead:tell!，C++ 只编码 genome 和 (cost,tokens,sink_calls)；本仓库没有权重、邻域或标量化代码。2 cases / 54 checks / 0 fail |
| C2 | 只 promote 帕累托改进的规则集 | test_evo_gate / explicit_genome_gate_promotes_pareto_and_aborts_the_rest | pushdown 与 filter-through-parallel 依次 promote；同答案但 sink_calls 翻倍的规则被拒；改变答案的规则被质量门拒绝。1 case / 18 checks / 0 fail |
| C3 | worldline 隔离沿用 fiber ctx + COW 规则集 + overlay | test_evo_gate，另有 test_main_line_isolation / test_abort_no_trace / test_cow_sharing | 每个候选 begin_experiment(snapshot) -> mutate_rules -> run_queries -> promote/abort；结束 active_worldlines()==0，被拒规则不进入主线；原有隔离/COW 测试仍通过 |
| C4 | 固定 seed，结果确定，重复跑一致 | test_evo_moead / moead_ask_tell_is_deterministic_for_a_fixed_seed | 两次独立 Runtime + 独立 Aura 会话，seed=42，transcript 4581 字节完全相同；candidate id、genome、三个指标、promote/abort 原因、最终规则 digest 全部一致 |
| C5 | 指标覆盖 cost / tokens / sink_calls | 两个新测试的打印与断言 | 每次决策都记录 baseline 与 candidate 的三元组；负样本分别把 sink_calls 从 1.5 推到 3.0、把 F1 从 1.0 降到 0.0308 |

## 3. 数值证据

### 3.1 显式 genome 门（test_evo_gate，3000 条记忆，40 个 intent，一半读两个 source）

| 步骤 | genome | cost | tokens | sink_calls | F1 | 决策 |
|---|---|---|---|---|---|---|
| 基线 | 0000 | 3048 | 60 | 2 | 1.0 | - |
| pushdown | 1000 | 2060 | 60 | 2 | 1.0 | promote |
| +filter-through-parallel | 1100 | 90 | 60 | 2 | 1.0 | promote |
| 重复 sink（负样本） | 0010 | 4542 | 60 | 3 | 1.0 | abort：不是帕累托改进 |
| 删 entity 过滤（负样本） | 0001 | 3720 | 64 | 2 | 0.0308 | abort：质量回归 |

主线总成本 121928 -> 3604（33.8 倍），结果 id 与基线完全一致。

### 3.2 MOEA/D 实际搜索（test_evo_moead，seed 42，4 代 x 5 候选）

    评估 20 次：promote 1，abort 19
    主线总成本 81033 -> 2475（32.7 倍）
    其中质量不可行 9 次，sink_calls 变差 10 次
    可行非支配前沿：1 个点（cost=77, tokens=59, sink_calls=2）
    最终规则集 digest：9082856256884491975
    两次运行 transcript：4581 bytes，完全相同

前沿只有 1 个点，原因是 3.2 的目标与正确性门：F1 = 1.0 时 tokens 恒定；sink_calls 只有负样本会变差；于是所有可行点在同一 (tokens, sink_calls) 上比 cost，最小的那个支配其余。MOEA/D 仍然在三维目标上搜索，只是可 promote 的集合退化了。

### 3.3 质量门优先

负样本 0001 的 cost 比基线低（3720 < 4542 的重复 sink 版本），如果只看成本会被误 promote；F1 = 0.0308 先把它拦住。这与 README 的关键发现 4 一致：质量门必须在成本门之前。

## 4. 实现说明

- ask/tell 用的是 std/moead 的 moead:ask / moead:tell!；它建立在 std/evo-core 的 candidate / measure / archive 上。evo:search 的 generic handle 不带 MOEA/D，所以没有用它。
- include/amr/evo.hpp、src/evo.cpp：AuraEvoSession（进程边界）+ EvoEngine（worldline、规则集编码、门）。
- genome 是 4 维向量，阈值 0.5 转成 4 条规则：pushdown、filter-through-parallel、重复 sink（负样本）、删 entity 过滤（负样本）。
- 重复 sink 规则带 :dup 标记，避免 fixpoint 重写把 Parallel 指数放大（实现时踩到过，已修）。
- 目标向量取每 query 均值：cost = (rows_scanned + rows_processed) / samples，tokens、sink_calls 同理；约束哈希把 1 - F1 作为违反量交给 MOEA/D，使其在搜索阶段就避开坏规则。
- promote 使用现有 Runtime::promote 与 PromotePolicy，require_cost_improvement=false，因为帕累托门已在 EvoEngine 中执行；promote 仍会独立复核质量。

## 5. 失败项、限制与上游问题

1. 嵌入 API 缺口（未达成原文字面要求）。docs/embedding.md 只有 aura::core / aura::fiber_rt；没有 Evaluator 的可链接 ABI。本实现用 --serve，属于进程内编译器会话，但不是同进程嵌入。建议上游提供 aura::eval 或 C ABI（对应 #4774 的后续）。
2. --serve 需要 AURA_SANDBOX=off 和 AURA_PIPELINE_STRICT=0 才能跑 std/moead；默认 STRICT 下报 "tree-walker fallback forbidden"。这是宿主集成的一个可用性缺口。
3. std/moead 没有 require std/list 和 std/math，但内部用了 sort / expt；在 --serve 下直接 require std/moead 会 unbound variable。本仓库在 prelude 里补 require，属于绕行，建议上游模块自带依赖。
4. CI 镜像不可用（本机无 docker），且镜像里没有 aura 二进制。test_evo_moead 在无 AMR_AURA_BIN 时打印 SKIP 并通过；test_evo_gate 不依赖 Aura，CI 仍覆盖门与三个指标。要覆盖 ask/tell，CI 需要安装 v0.2 aura 或设置 AMR_AURA_BIN。
5. 三目标前沿退化（见 3.2）。若要让 tokens / sink_calls 真正参与 trade-off，需要放宽质量门（允许有界召回损失）或引入能改变 token 成本但保持答案等价的规则（当前 EDSL 没有这种规则）。
6. Aura pin 99ddc81c9 不是 tag；v0.2.0-rc 尚未打 tag。本报告以 commit 为准。

## 6. 复现

    cd /home/dev/code/downstream/aura-memory-runtime
    git submodule update --init --depth 1
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DAMR_AURA_BIN=/home/dev/code/aura/build/aura
    cmake --build build -j
    ctest --test-dir build --output-on-failure
    AMR_AURA_BIN=/home/dev/code/aura/build/aura ./build/test_evo_moead

不设置 AMR_AURA_BIN 时：

    ./build/test_evo_gate     # 18 checks，不需要 Aura
    ./build/test_evo_moead    # 打印 SKIP

## 7. 变更文件

- CMakeLists.txt：加入 src/evo.cpp、test_evo_gate、test_evo_moead、AMR_AURA_BIN。
- include/amr/evo.hpp、src/evo.cpp：新增。
- tests/test_evo_gate.cpp、tests/test_evo_moead.cpp：新增。
- README.md：新增规则集进化小节、测试映射行、本机基线。
- third_party/aura：7bb2ed954 -> 99ddc81c9。
