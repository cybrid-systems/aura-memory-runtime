# Aura EDSL 探针

用 aura CLI 直接运行（`AURA_PIPELINE_STRICT=0 AURA_PATH=<aura>/lib aura <file>.aura`；默认的生产 STRICT 配置下宏定义一律被拒，见 aura#4846），输出每个 probe 的实际结果。不进 ctest：这些脚本记录的是上游当前行为，不是本仓库的断言。

| 文件 | 内容 | 结论（main a029752df，dev:v1.1.1；括号里是 76dc312cd 时的结果） |
|---|---|---|
| `macro_hygiene.aura` | `define-hygienic-macro` 的变量捕获、`set!` 交换、遮蔽 `if`，展开性能 | 全部符合卫生语义；2 万次展开约 51ms，手写代码约 49ms；参数个数错误现在报 `arity mismatch: macro …` |
| `macro_arity_eval.aura` | 宏参数个数、`eval` 未绑定名字 | 已修：参数多或少都报 `arity mismatch`（aura#4834）；`eval` 未绑定名字抛 `unbound variable`（aura#4833）（之前不检查参数个数，eval 静默返回 `()`） |
| `rule_edsl_errors.aura` | `std/rule` 输入校验、错误信息、2000 条 `rule:define` 耗时 | 已修：拼错关键字（带 did-you-mean）、奇数参数、非字符串 pattern 都报错，未知规则报 `no such rule`；2000 条约 212ms（之前静默接受，约 3.4s，O(n²)，aura#4835） |

另外：`std/INDEX` 不再宣称支持 `syntax-rules`；`define-syntax` / `syntax-rules` 未绑定时会提示改用 `define-hygienic-macro`（aura#4836，已修）。`defmacro` 是过程式、非卫生的宏（按设计），需要卫生时用 `define-hygienic-macro`。
