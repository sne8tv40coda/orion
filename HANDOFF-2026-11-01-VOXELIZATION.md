# HANDOFF 2026-11-01：体素化改造（把次级射线的遍历从 BVH 切到体素 DDA）

> 本文档自包含，给接手的人/智能体用。判据与历史数据在
> `docs/decisions/rt-cost-attribution.md` 与 `docs/decisions/rt-optimization-report-20261031.md`。
> 这里只写：**要做什么、现在有什么、坑在哪、怎么验证**。

---

## 零、一句话状态

- `trace` 成本已拆完：**材质求值 40% / 主射线非材质 40% / GI 弹射遍历 20%**。
- 已落地：payload 124 B → **64 B**、SER pin → mode 6、emissive 无损跳过。
- **体素化还没开工**，本文档是它的施工图。

---

## 一、为什么是体素化（数据依据，同局 A/B 实测）

| 射线类 | 非材质（遍历） | 材质求值 | 结论 |
|---|---|---|---|
| **反射** | **0.06 ms** | 6.23 ms | ❌ 体素化无意义（钱不在遍历） |
| **GI 弹射** | **4.35 ms** | 1.67 ms | ✅ **主目标** |
| **主射线** | 8.85 ms（含遍历 + 太阳软影 + 本地光 + raygen 固定开销） | — | ⚠ 混着固定开销，风险高，暂不做 |

⇒ **只对次级射线（GI / 远场反射）做体素化，主射线继续走 BVH。**

---

## 二、现有体素设施（已存在，不必新建）

| 设施 | 位置 | 说明 |
|---|---|---|
| **体素字** | section 16³ = **4096 体素**（每方块 1 体素） | class 位 0-1（1 = 满方块 / 3 = 部分遮挡）、**光源档案 id（5 bit）**、染色玻璃位 `bit 26`、部分透射 `bit 27-31` |
| **体素 DDA** | `rt_test.rgen:4242` `traceVPTEmitterDDA(origin, direction, out blockerAlbedo, ...)` | 目前**只用于本地光**二级弹射/可见性，返回遮挡与光源信息 |
| **体素光照传播** | `cpp/shaders/rt_vpt_irradiance.comp` | 9³ 探针 × 6 方向的方向性缓存；`vli` 段中位 **0.00 ms** |
| **消融档 16** | `perfL10DisableVoxelDda()` | 量过 DDA 整体 **1.09 ms** |

---

## 三、三个必须先知道的事实（避免重复踩坑）

### 3.1 GI **已经不做 POM**（2026-10-25 起）

```glsl
// rt_test.rgen:7911
const uint RT_STATE_GI_NO_POM_BIT = 1u << 17u;
// rgen:14354 / 24932 —— 只有 GI 射线播种这一位
hit.state |= RT_STATE_GI_NO_POM_BIT;
// rt_test.rchit:2111
g_giRaySkipPOM = (payload.state & RT_STATE_GI_NO_POM_BIT) != 0u;
```

⇒ `getPOMDepthScale()` 直接 `return 0.0`，GI 命中的法线本来就是**平面面法线**。
**体素化在 GI 上丢不了 POM**（已经没有可丢的）。

⚠ 相关口径修正：`noPom` 档（stage 18）实测的 1.29 ms 只反映**主射线 + 反射**的 POM。

### 3.2 体素字里**没有 albedo** —— 这是最大障碍

体素只有 class / 光源 id / 玻璃位，**命中后拿不到材质颜色**。而 BVH 命中会走
`resolveHitMaterial()` 去图集采精确纹素。

三条出路与代价：

| 做法 | 代价 |
|---|---|
| 体素字加 albedo 索引（指向"方块平均色表"） | 显存 + 一次查表；GI 里的纹理细节变方块平均色 |
| 命中后仍回图集采一次 | 省不掉最贵的那次采样，等于白做 |
| 只对远场用体素（近处仍 BVH） | 复杂度↑，画质影响最小 |

**视觉风险**：纹理级细节在 GI 链里（半分辨率 → 时域 → DLSS-RR 上采样）基本被抹平，看不出来；
但**大面积纯色偏移**（体素平均色偏亮/偏暗）是能看出来的，必须标定。

### 3.3 体素 DDA vs BVH 的**单射线速度比没有测过**

这是整件事的先行指标。如果 DDA 不比 BVH 快 2× 以上，全部方案都该终止。

---

## 四、施工顺序

### 步骤 0（先做这个，30 分钟）：量 DDA 速度比

**做法**：加一个消融档（编号接着现有 34）——反射或 GI 射线**强制走体素 DDA**，
只填一个近似结果（不求画面正确），只量 `trace`。

**判据**：

| 结果 | 结论 |
|---|---|
| DDA 臂 `trace` 下降 ≥ 25% | 继续步骤 1 |
| 下降 < 10% | **终止**，体素化在这台机器上没有意义（DDA 本身也有步进成本） |

### 步骤 1：最小切片 = 只改**远场**反射（推荐先做这个，不是 GI）

理由：它能把一个**已经验证有 1.85 ms 但因画面问题不能上线**的方案变成能上线的版本。

现状（已实测）：反射射线 `tMax` 截到 96 格能省 **1.85 ms**，但 96 格外的反射会变成**天空色**
（画面错误，已回退到 10000）。

改造思路：

```
反射射线：< N 格 → BVH（精确，含图集材质）
          > N 格 → 体素 DDA（近似，只拿到方块级信息）
```

需要改的地方：
1. `dda` 需要支持"从任意 origin 开始、返回首个命中的 距离/法线/方块坐标"
2. 远场命中后的颜色：先用一个粗糙但可标定的来源（体素色表 / 方块平均色）
3. rgen 的反射段（`rt_test.rgen` 光泽循环，`MAX_PATH_TRACE_STEPS = 16`）加分支

**画面上应该看到**：远处倒影变成"糊的方块色"，而不是**消失变天空**——这是它的价值。

### 步骤 2：GI 全走体素（步骤 1 成功后再谈）

GI 的非材质 4.35 ms 是最大一块可动遍历。但 GI 是画质核心，必须：
- 先做**同屏对比图**（体素 GI vs BVH GI），确认差异可接受
- 再谈性能

---

## 五、硬约束（这套工程已经踩过的坑，别重复）

1. **加消融档必须同步三处**：`PERF_L10_ABLATION_STAGE_COUNT`、两处 `% N`、
   rgen 的 `perfL10Stage()` clamp 上限。漏第三处 ⇒ 新档被**静默折成上限值**
   （2026-10-30 档 31 落到 30 = noMatNoGi，量出来的是错的）。
2. **一次只改一个字段/一个变量**，每次改完都验画面。上一轮 normal+albedo 同时压缩导致全屏黑，
   无法区分元凶，白费一轮。
3. **`Copy-Item` 恢复源码会保留备份的旧 mtime** ⇒ 构建脚本判「未过期」不重编 ⇒
   实际部署的仍是坏版本 SPV（审计报成"漂移"）。恢复后必须 `touch` 源文件。
4. **跨局不可比**：同一份代码 base 在同场景能差 15~22 ms，比要测的量还大。
   **只用同局 A/B**（`ORION_PERF_ABLATION=alt:`）。
5. **payload 增大 64 B 就会让驱动 SPIR-V 编译器 LLVM OOM**（36 分钟后 `0xC0000409`）。
   当前 64 B 已贴着边界，任何往 payload 里加字段的想法都要先想清楚。
6. **改画面的改动必须用户拍板**。亮度类副作用在帧时间上完全看不出来。

---

## 六、当前状态与回退点

| 项 | 值 |
|---|---|
| payload | **64 B**（`hit_payload.glsl`：玻璃链 10 字段已删、albedo 已压 unorm8x4、normal 仍是 vec3） |
| 玻璃 | **单片近似**（多层叠加偏淡），等重做；重做时走「独立 payload location=1」或 per-ray SSBO |
| SER | pin = mode 6（`VulkanCore.cpp` 的 `g_perfL10SerMode` / `g_perfL10SerRadToggle`） |
| 消融档 | 17–33 共 17 个，全部仅探针 |
| 部署 | 以 `tools/audit_spv_freshness.py` 的输出为准（应漂移 0） |

回退点（源码备份）：

```
cpp/shaders/*.bak_pre_slim_20261031          玻璃字段删除前（124 B）
cpp/shaders/*.bak_pre_albedopack_20261031    albedo 压缩前（68 B）
cpp/shaders/*.bak_pre_pack_20261031          normal+albedo 压缩前
cpp/shaders/*.bak_pre_normalpack_20261031    64 B 版（验证正常）
```

---

## 七、验证方法（照抄即可）

```powershell
# 一局 A/B（同局交替，避免跨局漂移）
$env:ORION_PERF_ABLATION="alt:0,34"      # 34 = 新加的体素档
cmd /c .tmp\run_ab_xxx.bat               # 任意带 gradlew.bat runClient --offline 的脚本

# 读数据（run/rt_debug.log 只读新增部分）
python .tmp/analyze_ablation.py <byte-offset>
```

**必须**：进世界 → 站定 → 固定视角 → 全屏 → 至少 3 分钟。
失焦会锁 10 fps，窗口时间翻倍，数据作废。

---

## 八、一句话给接手者

**先量 DDA vs BVH 的速度比（步骤 0），不要直接改架构。**
那一个数决定这件事值不值得做——如果 DDA 不比 BVH 快 2×，后面全部作废。
