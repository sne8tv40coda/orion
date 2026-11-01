# backend/ — rtx_backend 的 C++ 源码

2026-11-01 规范化拆分：此前整个 Vulkan 后端是**单文件 `cpp/VulkanCore.cpp`
（约 7.1 万行、658 个函数、197 个 JNI 导出、约 500 个文件级全局变量）**，任何一次
改动都要整文件重编（约 1 分钟），编辑器与 git diff 都不可用。现在按领域拆成
32 个编译单元 + 1 个全局状态单元 + 2 个内部头文件。

本目录所有 `.cpp` 由**拆分脚本机械拆出，函数体逐字不变**——拆分本身不引入任何
行为变更（验证方式见下文"一致性验证"）。头文件与模块的**架构契约**如下。

## 文件地图

| 文件 | 内容 | 大致行数 |
|---|---|---|
| `OrionInternal.h` | 共享内部头：include、扩展 shim、类型、constexpr 常量、全局变量 extern 声明、全部函数原型 | ~5.8k |
| `TrackingHooks.h` | `vkAllocateMemory` 等 15 个 Vulkan 入口 → `rtxTracked*` 记账劫持宏 | ~50 |
| `State.cpp` | **全部**文件级全局变量定义（保持原声明顺序）+ 2 个自安装器对象 | ~2.2k |
| `Platform.cpp` | 路径解析、RTLog、崩溃转储、phase 探针、泄漏探针、设备初始化停机点 | ~1.1k |
| `PipelineCache.cpp` | RT 管线缓存 blob 的装载/持久化/清理 | ~0.7k |
| `Perf.cpp` | Perf-L10 仪表：A/B 消融档位、GPU/CPU 时间戳、ray stats、帧预算 | ~1.8k |
| `Settings.cpp` | `ORION_*` 环境闸门 getter、`populate*Params` UBO 填充 | ~1.0k |
| `Memory.cpp` | `findMemoryType`、`createBuffer`、池化缓冲、CameraUBO | ~0.5k |
| `Tracking.cpp` | `rtxTracked*` 记账实现（**唯一**不包含 TrackingHooks.h 的模块） | ~0.6k |
| `Textures.cpp` | 图集 JNI、PBR 图集 sprite-mip / BC7、纹理上传 | ~2.0k |
| `Shaders.cpp` | SPV 装载、shader module、buffer device address | ~0.2k |
| `As.cpp` | 加速结构/微地图函数指针、OMM 探针与 JNI | ~0.5k |
| `Geometry.cpp` | Section 顶点打包、几何捕获线程、chunk 上传 JNI、去重摘要 | ~2.3k |
| `Sections.cpp` | Section 显存池、VRAM governor、retirement、BLAS/TLAS、顶点地址表 | ~3.6k |
| `Lights.cpp` | 动态光/局部光 GPU 缓冲与描述符 | ~1.2k |
| `Sharc.cpp` | SHaRC 辐射缓存、RR guide/GBuffer 图像、半分辨率 GI | ~2.3k |
| `Vli.cpp` | VLI 体素光照页池、VPT 发射源目录、`recordVLIUpdates` | ~4.0k |
| `Nrd.cpp` | NRD 集成、DI confidence、direct atrous、RR guide relax | ~4.5k |
| `Shadow.cpp` | 太阳阴影近/远场、地形高度图 | ~1.2k |
| `Clouds.cpp` | 云噪声、v3/v4 资源、体积云管线与合成、热重载 | ~3.7k |
| `VolRestir.cpp` | 体积 ReSTIR 链 | ~1.8k |
| `Sky.cpp` | Bruneton 预计算、天空散射、星图/末地天图 JNI | ~1.1k |
| `Water.cpp` | 水面 FFT/焦散/泡沫/水体模拟、实体水波 JNI | ~3.7k |
| `Glass.cpp` | 玻璃 alpha-blend 通道 | ~1.5k |
| `PostFx.cpp` | SDR/HDR 后处理、自动曝光、HDR 编码、FG 图像、bloom | ~3.7k |
| `RtPipeline.cpp` | RT 描述符布局、SBT、主管线重建/回收 | ~1.2k |
| `Streamline.cpp` | SL 代理/初始化、swapchain JNI、DLSS-FG/Reflex/PCL/HDR | ~6.3k |
| `JniSettings.cpp` | 约 160 个设置类 JNI setter/getter | ~2.3k |
| `Device.cpp` | `initDeviceHandlesNative`（设备句柄接管 + 主管线创建） | ~0.5k |
| `Frame.cpp` | `updateCamera`、`recordFrameCommand`（每帧全部 pass 的录制） | ~5.8k |
| `WorldGeometry.cpp` | `rebuildWorldGeometry`（世界几何批量重建） | ~3.8k |
| `Lod.cpp` | Voxy / Distant Horizons LOD 四边形、图集、覆盖图 | ~1.1k |
| `Entities.cpp` | 实体批 JNI、实体 PBR 回退资源 | ~0.5k |
| `Particles.cpp` | 粒子批 JNI、雨丝纹理 | ~0.2k |
| `Cleanup.cpp` | `cleanup` JNI（全部资源拆除） | ~1.0k |

## 架构契约（改代码前必读）

1. **TrackingHooks.h**：`vkAllocateMemory` / `vkCreateBuffer` 之外的 15 个
   分配与对象创建入口被宏劫持到 `rtxTracked*` 包装（显存与对象总账）。
   除 `Tracking.cpp`（记账实现本身要调真实入口）外**每个模块都包含它**；
   新增模块若调用这些入口，必须包含。**不要**在 OrionInternal.h 里定义这些宏。
2. **State.cpp 持有全部全局变量**，并保持原始声明顺序：拆分前所有变量在同一个
   翻译单元里按文本顺序初始化/析构；集中到 State.cpp 精确复现了这个次序。
   依赖静态初始化顺序的自安装器（terminate handler / 崩溃过滤器）也在这里。
   **新增全局变量请加到 State.cpp**（对应 extern 加到 OrionInternal.h 的
   "全局变量 extern 声明"区），不要散落在业务模块里——那会重新引入跨 TU 初始化
   顺序问题。
3. **OrionInternal.h 分区**（自上而下）：声明区（include/shim/类型/constexpr）→
   bc7enc.h（**无 include guard，全工程只在此包含一次**）→ 函数原型 →
   全局变量 extern → 带内联方法体的类型（它们的方法调用了原型函数，必须位于
   原型之后）→ 引用上述类型的 extern。
4. **`static inline` 函数**：拆分时去掉了定义上的 `inline`（原型也非 inline）。
   单文件时代跨函数内联由编译器在同一 TU 内完成；拆分后如需恢复跨 TU 内联，
   开 `/GL` + `/LTCG`（`CMAKE_INTERPROCEDURAL_OPTIMIZATION`），未默认开启。
5. **JNI 导出名不可改动**（Java 按名字绑定），新增 JNI 函数放对应领域模块即可。

## 一致性验证（拆分当时的凭据）

- 基线：拆分前源码构建的 `rtx_backend.dll`（6,048,768 字节）。
- 拆分后：`cmake --build cpp/build --config Release --target rtx_backend`
  全量编译 + 链接通过；`dumpbin /EXPORTS` 对比 **197 个导出符号逐一相同**；
  新 DLL 6,044,672 字节（-4 KB，TU 重排的正常波动）。
- 全局变量初始化顺序、宏劫持语义、`#if 0` 死代码块（4 处，含水泡沫旧实现
  835 行）均按原文保留。
- 拆分脚本存档于 `.zcode/split_tools/`（含拆分前的 `VulkanCore.cpp` 快照），
  仅供考古，不进构建。

## 新增/修改代码的流程

- 新函数：实现放对应领域模块；原型加进 `OrionInternal.h` 原型区。
- 新 pass：新建 `<Pass>.cpp`，加进 `cpp/CMakeLists.txt` 的 `SOURCES`，
  头部抄任意现有模块的两行 include（`OrionInternal.h` + `TrackingHooks.h`）。
- 模块内 helper 若只被本模块使用，可以加 `static` 保持内部链接。
