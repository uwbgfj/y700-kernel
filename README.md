# y700-kernel

用 GitHub Actions 编译联想 Y700 四代（TB322FC）内核，包含：
- GKI android15-6.6.139 源码
- AOSP clang r510928
- KernelSU（FinaSU）→ 保留 root
- 自研内嵌驱动 aa_builtin（判定篡改）

产物：`Image`（需再签名为 boot.img 才能刷入）
