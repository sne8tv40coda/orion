# 2026-11-06 优化记录：构建提速 + 几何/显存 + VLI

本文档只记录**实机验证过、且已合入代码**的改动，以及**验证后判定无效/危险并回退**的尝试。
每条都给了证据来源（日志字段或测量口径），便于以后复核或回退。

基线：`1e12949`（2026-11-02/03，含 TU 拆分）。
测试平台：RTX 4070 Laptop（8 GB），16 区块视距，DLSS-RR + FG，内部分辨率 707×376 / 输出 2122×1129。

---

## 0. 架构级重构（本会话开始前已完成，已在上一个提交）

- 7.1 万行单文件 `cpp/VulkanCore.cpp` → `cpp/backend/` 下 **36 个 TU** + `OrionInternal.h`（约 5.6k 行：include/类型/原型/全局 extern）+ `State.cpp`（约 2k 行：约 500 个全局变量定义）。拆分契约见 `cpp/backend/README.md`，函数体逐字未变。
- **本会话为其补上的构建侧收尾**：`cpp/CMakeLists.txt` 增加 `/MP` + `target_precompile_headers(OrionInternal.h)`，`bc7enc.cpp` 用 `SKIP_PRECOMPILE_HEADERS`（它不 include 该头，否则 C1010）。

| 指标 | 拆分后（无 PCH） | 现在 |
|---|---|---|
| 全量构建 | ~60 s | **8.5 s** |
| 单 TU 增量 | ~2.3 s | **1.3 ~ 2.4 s** |
| 导出符号 | 201 | **201（Compare-Object 零差异）** |

---

## 1. 手部层指纹判等（`Entities.cpp`）

**问题**：`handChanged = !g_handFrameVertices.empty()` —— 只要第一人称手里有东西（常态）就置脏，经 `Frame.cpp` 的 `g_entityRebuildRequested` **绕过** 10 帧静默窗直接调 `rebuildWorldGeometry(24)`，整轮世界几何重建退化成每帧执行。

**改法**：复用一直存在但从未使用的 `g_handCachedHash` / `g_handCachedHashValid`，改为哈希 + 尺寸判等；手部层消失时删表项。

**证据**：`[HAND LAYER] frames=300 changed=0 unchanged=300` 连续 15 个窗口（4500 帧全命中）。

| 指标 | 改前 | 改后 |
|---|---|---|
| CPU `geometry` 段 avg | 1.62 ms | 1.32 ms |
| CPU `geometry` 段 max | **13.00 ms** | **1.64 ms** |
| CPU `geometry` 段 p90 | 2.70 ms | 1.46 ms |

---

## 2. 显存治理：修「区块死活加载不出来」（`Sections.cpp` / `WorldGeometry.cpp`）

**现象**：`meshSec=6062` 但 `blas=2038`，远景大片区块在 RT 里不可见；日志刷 `page capacity exhausted`。

**根因链**（全部有日志指纹）：

```text
budgetMB 由 7180 动态缩到 6671 → ratio 越过 highWater(0.72) → over=1
over 的退出条件是 ratio < lowWater(0.62)，但驱逐只释放 BLAS（asBlasMB≈144MB / usedMB≈5077）
⇒ ratio 只能降 2.8%，永远到不了 0.62 ⇒ over 永久为真
⇒ 每 4 帧继续踢段 ⇒ nonResident 4637 且持续增长 ⇒ 视野内 90 格外的段全被踢走
```

**四处改动**：

| # | 改动 | 效果（实测） |
|---|---|---|
| 1 | `vramGovernorHighWater` 0.72 → **0.80** → **0.82**（仍低于注释记录的 DLSS `eWarnOutOfVRAM` 线 0.849） | ratio 0.76~0.81 不再误触发 over |
| 2 | `evict` 增加 `nonResident` 饱和保护（默认 3000，`ORION_NONRESIDENT_GUARD`） | `nonResident` 4637 → **0**，`blas` 2038 → **4427** |
| 3 | `over` 在「驱逐已饱和」时也退出 | 整局 `overEnters=0` |
| 4 | `evict` 帧节流 4 帧（`ORION_VRAM_EVICT_INTERVAL`）+ 候选容器 `static` 复用 | `evict` avg 5.03 → 3.76 ms，max 15.43 → **8.63 ms** |

> 关键认识：**驱逐不释放显存**（页号/槽位被复用），所以用它做「降 ratio」的手段本身无效；over 的滞回带假设「驱逐能降 used」是设计缺陷。

---

## 3. Section role 表「变化才写」（`Sections.cpp`）

**问题**：`writeSectionRoleSlot` 每帧对全部段（~6000）各做一次**离散 4B 写**，而 `g_sectionRoleMapped` 是主机可见（write-combined）映射 —— 离散写每次刷一次 WC 缓冲。

**改法**：主机侧影子数组比较（缓存内读），命中则整帧零写入；影子以映射指针为代际（重建时整体失效重填）。

**证据**：`ord` 段 2.00 ms → **0.19 ms**（`preCmd` 7.64 − evict 3.50 − inst 3.95）。

---

## 4. VLI（体素光注入）八项

| # | 问题 | 改法 | 效果 |
|---|---|---|---|
| 1 | `applyVLISectionVoxelsLocked` 每段调一次 `rebuildVPTCandidateNeighborhoodLocked`，而它是 5×5×5=125 个邻域 Section、每个再内层扫 125 ⇒ 单段约 15625 次哈希查找 | 批量路径改为**按 pageIndex 去重**收集，循环结束统一重建一次 | `apply` 324.84 → **0.23 ms**；`[RT VLI SLOW]` 215 条 → **2 条**，325 → **29 ms** |
| 2 | `refreshVPTEmitterSectionFromLocalLights`（Java 逐 Section 回调）持 `g_vliMutex` 做同样的 125×125 | 锁内只收集受影响页，帧内 `recordVLIUpdates` 统一重建（`s_hasDeferredVptNeighborhoodPages` 原子标志避免空闲帧白拿锁） | `wait` 8~17 ms → **0.0001 ms**，`site=workerApply` 从锁热榜消失 |
| 3 | 池增长 2x：一局 2 次，每次锁内整池迁移 + `vkQueueWaitIdle` 17~22 ms | 步长 **2x → 4x**（1024→3253 一步到位） | 一局 **1 次**，23 ms 一次性 |
| 4 | 池顶到上限后新 Section 被**永久丢弃**（只在 `g_vramGovernor.over` 时才允许淘汰最远页） | 判据从「显存是否超预算」改为**「最远页是否比新段更远」**（`evictFarthestVLIPageForIncomingLocked`） | 池不再死钉上限；`vliLivePages 3248 / freePages 5` 持续回收 |
| 5 | 上述丢弃日志每帧刷（实测 1211 次 / 2.5 min） | 限频每 600 条一条 | 1211 → **6** |
| 6 | 淘汰每次在锁内**全表扫** 3253 页 | 等距抽样上限 256（`VLI_EVICT_SCAN_LIMIT`） | `wait` 7~10 → **0** |
| 7 | `chunkUnloadScan` 单次持锁 **42 ms** | 探针查明 98% 在 `g_vliSections.erase()`（节点内联 `VLISectionCpuPage`，180 µs/次，锁内析构大块）⇒ **先 move 出大对象、锁外析构** | 3043 次列卸载、8059 页摘除，**一次都没超过 1 ms**，该 site 从 `[VLI LOCK]` 彻底消失 |
| 8 | 淘汰路径同样的 erase 代价（一局 5887 次） | 同样 move-out 到 `thread_local` 容器 + 每帧锁外 drain | `pending` max 3.65 → **2.69 ms** |

### 探针实测（第 7 项的依据）

```text
[VLI UNLOAD BREAK] sections=7 totalMs=1.2809
  tableMs=0.0049   mapEraseMs=1.2597   hashForgetMs=0.0087
  tableSlots=8192  livePages=2772
```

即：**不是**哈希表删除、**不是**嵌套取锁，而是 `unordered_map::erase` 析构大对象。

---

## 5. 其他运行期小改

| 改动 | 说明 |
|---|---|
| `Glass.cpp`：稀疏掩码 pass 门控 | 采样率 ≤ 0 时整条 pass 早退（默认档 30%，故默认无收益，仅「0=关」时生效） |
| `Device.cpp` / `Sharc.cpp`×2：管线缓存写盘去抖 | 三处同步 `persistSharedPipelineCache()` 改为 `countdown=60`，交给 `maybePersistSharedPipelineCache` 去抖 + 大小去重 |
| `PipelineCache.cpp`：恢复后去重 | 加载成功后设 `g_pipelineCachePersistedBytes = initialData.size()`，消除启动后把**与磁盘完全相同**的 184 MB blob 再写一遍（实测 120 ms 空写） |
| `vol_restir_composite.comp`：`froxelW` 恒等化简 | `(viewZ-near)/maxViewZ ≡ pow(sliceT,e)`，`pow(pow(t,e),1/e) ≡ t` ⇒ 每帧省约 6600 万次 `pow` + `clamp` + 除法 |
| `Sharc.cpp`：update 闸门 / resolve 隔帧 | `ORION_SHARC_RESOLVE_PARITY`（默认 2）；仅在「间接光模式 = SHaRC」时生效，默认档为关 |

---

## 6. 验证后**回退**的尝试（避免以后重复踩）

| 尝试 | 结果 | 原因 |
|---|---|---|
| **段数/距离驱动驱逐**（`ORION_SECTION_MAX_COUNT` 默认 4000） | **负优化，默认改回 0** | 16 区块视野内真实段数就是 ~6000，4000 上限让系统永远超限 ⇒ 每 4 帧全表扫 + 持续驱逐且降不下来：`asRetireN` 1130→2519、`upWaitMs` 12.8→19.2、`fps` 69.6→47.6。机制保留但默认关闭 |
| **VLI 池增长改 fence 延迟回收** | **GPU 挂死，已回退** | 首次触发增长即 `5s timeout reached when waiting for VK semaphore` → `0xC0000409`。恢复 `vkQueueWaitIdle` 同步版后同条件稳定。结论：该路径是 8 个长期驻留 buffer 的整体切换，异步化需要 timeline semaphore 在帧边界注入，收益（一局 2×20 ms）不值风险 |
| **`ORION_CAPTURE_PER_WAKE=1`** | 只是切片 | `capHoldMs` 10.6→2.7，但 `capWaitMs` 10.2→19.5 且加载变慢；尖峰根因是锁内堆分配/缺页，与持锁时长无关 |
| **TLAS UPDATE**（`updateTlas=false` 打开） | **不做** | `[RT GEOMETRY SLOW]` 实测 `tlas` 段 avg 0.24 / max 0.37 ms，收益仅 0.1 ms |

---

## 7. 环境变量开关（本会话新增/使用）

| 变量 | 默认 | 作用 |
|---|---|---|
| `ORION_VRAM_EVICT_INTERVAL` | 4 | evict 帧节流；=1 回到逐帧（A/B） |
| `ORION_NONRESIDENT_GUARD` | 3000 | 被驱逐段数上限，达到后停止驱逐；=0 关闭 |
| `ORION_SECTION_MAX_COUNT` | 0 | 段数驱动驱逐上限；**默认关闭**（见第 6 节） |
| `ORION_SECTION_KEEP_RADIUS` | 384 | 段数驱动时的保留半径（格） |
| `ORION_VLI_BUDGET_MS` | 4 | VLI apply 每帧时间预算 |
| `ORION_VLI_SECTIONS_PER_FRAME` | 2 | VLI apply 每帧段数上限 |
| `ORION_SHARC_RESOLVE_PARITY` | 2 | SHaRC resolve 隔帧 |
| `ORION_VLI_GROW_DEFER_FRAMES` | 30 | 池增长的安静窗推迟上限 |

---

## 8. 已知未解决问题（下一步）

1. **`pending`（VLI apply 循环）avg 2.20 / max 2.69 ms** —— 受 `ORION_VLI_BUDGET_MS=4` 约束，属设计内；想再压可调到 2（代价：局部光照晚 1 帧）。
2. **`rest` 6~7 ms** —— `recordVLIUpdates` 内仍未归因的 CPU（`apply`/`gather`/`upload`/`solve` 合计不到 2 ms）。
3. **退出期 `0xC0000005 at nvoglv64.dll+0xA6639A`** —— 既有问题（`inWorld=0` 路径），与以上改动无关；15:23 / 17:43 / 18:12 / 18:30 / 19:31 / 19:36 / 20:28 全是同一签名，**未修**。
4. **仓库根目录散落 177 个未跟踪 shader 文件**（`.rahit.spv`、`bloom_*.comp`、`camera_ubo.glsl` 等），疑似脚本误拷贝，**未提交**，建议清理。
