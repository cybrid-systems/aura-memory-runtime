# Aura EDSL 探针

用 aura CLI 运行（`AURA_PATH=<aura>/lib aura --load <file>.aura`），输出每个 probe 的实际结果。aura d44d1077e 上默认（生产 STRICT）配置下只有 `--load` 能跑：直接 `aura <file>.aura` 时 `macro_hygiene.aura` 和 `rule_edsl_errors.aura` 整体报 `tree-walker fallback forbidden`（aura#4864；735135de6 上卫生宏直接运行是正常的，属回归），用 `--load` 或 `AURA_PIPELINE_STRICT=0` 都正常。不进 ctest：这些脚本记录的是上游当前行为，不是本仓库的断言。

| 文件 | 内容 | 结论（main a029752df，dev:v1.1.1；括号里是 76dc312cd 时的结果；735135de6 复验见表下） |
|---|---|---|
| `macro_hygiene.aura` | `define-hygienic-macro` 的变量捕获、`set!` 交换、遮蔽 `if`，展开性能 | 全部符合卫生语义；2 万次展开约 51ms，手写代码约 49ms；参数个数错误现在报 `arity mismatch: macro …` |
| `macro_arity_eval.aura` | 宏参数个数、`eval` 未绑定名字 | 已修：参数多或少都报 `arity mismatch`（aura#4834）；`eval` 未绑定名字抛 `unbound variable`（aura#4833）（之前不检查参数个数，eval 静默返回 `()`） |
| `rule_edsl_errors.aura` | `std/rule` 输入校验、错误信息、2000 条 `rule:define` 耗时 | 已修：拼错关键字（带 did-you-mean）、奇数参数、非字符串 pattern 都报错，未知规则报 `no such rule`；2000 条约 212ms（之前静默接受，约 3.4s，O(n²)，aura#4835） |

735135de6 复验（dev:v1.1.1）：三个探针语义结果与上表相同，默认配置和 `STRICT=0` 一致（`rule_edsl_errors` 只能在 `STRICT=0` 下跑，aura#4848）。性能回归：`macro_hygiene` 的 2 万次卫生宏调用从约 51ms 变成约 71s（手写代码仍约 51ms），2000 次调用 5ms → 1762ms，随调用次数超线性增长（aura#4857）。`rule:define` 2000 条约 211ms，不变。

d44d1077e 复验（dev:v1.1.1，`STRICT=0`）：三个探针语义结果与上表相同。卫生宏性能回归已修：2 万次调用约 53ms（手写约 51ms）；但同一文件里只要出现过 `defmacro`，卫生宏调用仍超线性变慢（2000 次 744ms，4000 次 5.6s，aura#4857 已补复现）。`rule:define` 2000 条约 199ms。`macro_arity_eval` 在默认配置下直接运行也正常。

另外：`std/INDEX` 不再宣称支持 `syntax-rules`；`define-syntax` / `syntax-rules` 未绑定时会提示改用 `define-hygienic-macro`（aura#4836，已修）。`defmacro` 是过程式、非卫生的宏（按设计），需要卫生时用 `define-hygienic-macro`。
