# Aura EDSL 探针

用 aura CLI 直接运行（`AURA_PATH=<aura>/lib aura <file>.aura`），输出每个 probe 的实际结果。不进 ctest：这些脚本记录的是上游当前行为，不是本仓库的断言。

| 文件 | 内容 | 结论（main 76dc312cd，dev:v1.1.1） |
|---|---|---|
| `macro_hygiene.aura` | `define-hygienic-macro` 的变量捕获、`set!` 交换、遮蔽 `if`，展开性能 | 全部符合卫生语义；2 万次展开约 51ms，手写代码约 48ms |
| `macro_arity_eval.aura` | 宏参数个数、`eval` 未绑定名字 | 参数个数不检查（aura#4834）；`eval` 未绑定名字静默返回 `()`（aura#4833） |
| `rule_edsl_errors.aura` | `std/rule` 输入校验、错误信息、2000 条 `rule:define` 耗时 | 拼错关键字、奇数参数、非字符串 pattern 都静默接受；2000 条约 3.4s，O(n²)（aura#4835） |

另外：`std/INDEX` 宣称支持 `syntax-rules`，但 `define-syntax` 未绑定（aura#4836）。`defmacro` 是过程式、非卫生的宏（按设计），需要卫生时用 `define-hygienic-macro`。
