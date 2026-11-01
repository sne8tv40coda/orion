# VulkanCore.cpp 单文件拆分（2026-11-01）

> 过程记录。契约性内容（模块地图、改动流程）在 [`cpp/backend/README.md`](../../cpp/backend/README.md)。

## 动机

`cpp/VulkanCore.cpp` 达到 **71,151 行**：658 个非 JNI 函数 + 197 个 JNI 导出 +
约 500 个文件级全局变量 + 87 个类型。代价：

- 每次改动整文件重编（单 TU），本地增量迭代以分钟计；
- 编辑器/IDE 索引与 git diff 基本不可用；
- 长期靠 `VulkanCore.cpp.bak_<tag>_<日期>` 手工拷贝当版本管理（仓库里堆了
  103 个，共约 700 万行冗余），git 本身才是版本管理。

## 拆分策略：机械拆出，函数体逐字不变

整个拆分由脚本（`.zcode/split_tools/split_backend.py`）完成，**不改任何函数体、
不改任何字符串字面量与注释**。行为保持等价靠四条支柱：

1. **共享内部头 `OrionInternal.h`**：所有类型、constexpr、extern 声明、原型
   按原文件相对顺序排列——原文件里"先定义后使用"的次序在头文件里原样成立。
2. **全局变量集中在 `State.cpp`** 并保持原声明顺序。这是**静态初始化次序**
   的关键：原来所有变量在同一 TU 内按文本序初始化（含 `rtLogFile` 这类
   构造时调函数、两个自安装器对象）；集中到一个 TU 后次序逐位复现。
   函数在动态初始化期间读到的变量状态与拆分前一致。
3. **宏劫持语义搬进 `TrackingHooks.h`**：原来 `#define vkAllocateMemory
   rtxTrackedAllocateMemory` 等两批宏（共 15 个入口、文件里定义了两次）作用于
   `#undef` 区之外的全部文本。拆分后除 Tracking.cpp（记账实现，要调真实入口）
   外每个模块都包含该头，语义逐 TU 等价。曾踩坑：887-890 行的 4 个宏
   （`vkFreeCommandBuffers` 等）起初漏搬，会让记账静默漏记——靠逐宏核对原文抓回。
4. **`#if 0` 死代码块随所在模块原样搬**（4 处顶层的：4115 / 36152 / 44766
   （水泡沫旧实现 835 行）/ 57899；函数体内的死块随函数走），不做"顺手清理"，
   保持拆分 diff 纯机械。

## 过程中修过的拆分器错误（给未来同类操作留证据）

- OMM/SER 扩展 shim（54-169 行）里的 `typedef` 被拆出 `#ifndef` 保护导致
  C2011 重定义 → shim 区整块 verbatim 输出；
- 前向声明分类误伤 `static std::vector<T> g_x(N);` 构造初始化变量 →
  改为对照"已知函数名集合"判定；
- `#define` 之外的 gap 注释里出现 `=` / `(` 时截坏 extern 声明 →
  统一"gap 与块文本分离，变换只作用于块文本"；
- 多变量声明 `static uint32_t a=0,b=0;` 截断时丢第二个声明符 →
  按顶层逗号逐声明符处理（模板尖括号计入深度防 `std::pair<int,int>` 误拆）；
- 带内联方法体的类型（8 个，如 `LocalLightGridCellCoordHash`、
  `RtxSectionBuildBufferPool`）方法体调用原型函数/全局变量 → 头文件分区排序：
  声明区 → 原型 → extern → 这类类型 → 引用它们的 extern；
- `static const` 常量直接降级为 `extern const` 会破坏 `if constexpr` /
  数组维度 / range-for（`RT_PIPELINE_SPV_FILES`）→ 字面类型提升为
  `inline constexpr`，`std::chrono` 等非字面类型保留 extern const + State 定义；
- `static inline` 函数拆分后若定义 TU 从不调用它，MSVC 不发射 → 链接失败
  （`hdrOutputActive`）→ 定义与原型都去 `inline`。

## 一致性验证

- 基线 DLL（拆分前源码构建，6,048,768 B）vs 拆分后（6,044,672 B）：
  `dumpbin /EXPORTS` **197 个 JNI 导出逐一相同**；
- 全量编译 + 链接 0 error（迭代约 12 轮，全部错误为拆分器机械错误，无一涉及
  原代码逻辑）；
- 初始化/析构次序、记账宏作用域、死代码块逐项核对原文。

## 副产物清理

- 103 个 `VulkanCore.cpp.bak_*` → `cpp/_archive/vulkancore-backups/`
  （`.gitignore` 的 `*.bak_*` 规则本就覆盖，不进仓库）；
- `cpp/shaders/VulkanCore.cpp`（beta 0.42 时代的陈旧拷贝，2.8 万行，
  放错目录且会误导 grep）→ 删除，git 历史可溯（commit 9eedc08）；
- `cpp/VulkanCore.cpp` 本体 → 删除（内容已在 backend/ 里逐字保存；
  拆分前快照另存于 `.zcode/split_tools/VulkanCore.cpp.pre_split`）。

## 明确不做的（留给后续）

- 不开 `/GL`+`/LTCG`：会改变代码生成，与"纯机械拆分"混在一起说不清归因。
  若未来需要跨 TU 内联，单独开一轮并单独 A/B。
- 不借机删除 `#if 0` 死代码、不合并重复定义、不改命名 —— 全部留给后续独立
  提交，保持本轮 diff 可审计。
- `recordFrameCommand`（5,216 行）仍在 Frame.cpp 单函数里，是下一个值得拆的
  对象；`rebuildWorldGeometry`（3,840 行）同理。
