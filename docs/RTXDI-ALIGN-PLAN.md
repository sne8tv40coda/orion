# RTXDI 完全对齐实施计划

> 目标：把当前 **SVGF 路线**的 DI 管线（`rt_di_prepare → reproject → moments → confidence → atrous`）
> 换成 **ReSTIR 路线**（reservoir + temporal/spatial reuse），SVGF 降为后备滤波。
>
> 现状差异（已核实）：`rt_di_confidence.comp:23` 自己写着"本项目没有上一帧 reservoir 与 BVH"，
> 全套方差来自 `svgf/direct_moments.comp` 的移植。

---

## 一、目标架构（5 个 pass）

| # | pass | 粒度 | 输入 | 输出 |
|---|---|---|---|---|
| 1 | `rt_di_presample`（**新增**） | 每像素 | `local_light_grid` + 深度/法线 | reservoir A |
| 2 | `rt_di_temporal`（**新增**） | 每像素 | reservoir A + prev reservoir + prevViewProj | reservoir B |
| 3 | `rt_di_spatial`（**新增**） | 每像素 | reservoir B + 邻域 | reservoir A |
| 4 | `rt_di_shade`（**改造** `rt_di_prepare`） | 每像素 | reservoir A + 可见性 | direct radiance |
| 5 | `rt_di_atrous`（**保留**） | 每像素 | direct radiance | 输出 |

现有 `rt_di_reproject` / `rt_di_moments` / `rt_di_confidence` 在 ReSTIR 跑通前**保留**（开关切换），
跑通后再评估是否删除——ReSTIR 的输出方差低，通常只需要 2~3 次 atrous，不需要 moments/confidence 全套。

---

## 二、Reservoir 数据结构（16 B）

```
x: lightIndex  16b   0 = 太阳（方向光）, 1..N = local_light_grid 下标
   | age        8b   已累积帧数
   | flags      8b   bit0 = 有可见性结果, bit1 = 太阳样本
y: weightSum    f32  Σ w_i × target_i
z: W            f32  1 / (M × pdf)
w: M            u32  已合并样本数（temporal 时 clamp）
```

- 缓冲区：`StorageBuffer`，`w*h*16B`（707×376 ≈ 4.2MB）×2 ping-pong。
- 与体积光 reservoir 编码保持一致，便于将来共用调试工具。
- 需要注意 **per-pixel 而非 per-froxel**，重投影走 2D 屏幕空间（`prevViewProj * worldPos`）。

---

## 三、关键实现点（容易踩的地方）

### 1. WRS 选光（阶段 2）

```glsl
// 候选来自 local_light_grid 邻域（现有 8 格走访）
// target = emission × 距离衰减 × NdotL × shadowPDF
// 关键：必须记录 pdf，否则 weightSum 无法无偏化
float pdf = selectWeight / totalWeight;   // WRS 的隐式 pdf
reservoir.x = packed(lightIndex, age, flags);
reservoir.y = selectWeight / pdf;         // weightSum（无偏）
reservoir.z = 1.0 / (M * pdf);            // W
```

**注意**：我们现在是"逐灯衰减相加"，改成"选一盏"后，**总能量必须靠 W 补回来**，
否则画面会整体变暗（这是最容易误判成"回归"的地方）。

### 2. Temporal reuse（阶段 3）

- 重投影：`prevClip = prevViewProj * vec4(worldPos, 1)` → NDC → UV（注意与 MV 的 UV/NDC 口径一致，别再犯 `mvecScale` 那个错）。
- 合并规则（ReSTIR 标准）：
  ```
  m = min(prev.M, TEMPORAL_M_CLAMP)      // Remix 用 2
  w_prev = prev.weightSum * prev.W * m * target(prev.sample)
  w_cur  = cur.weightSum  * cur.W  * target(cur.sample)
  M = m + 1
  W = (w_prev + w_cur) / (M * target(chosen) / cur.pdf)
  ```
- **必须有 disocclusion 判据**（深度/法线/MV 任一超阈值就丢弃历史），否则拖影。

### 3. Spatial reuse（阶段 4）

- 邻域半径 8~16 像素，`spatialReuseMaxSampleCount` 对齐 Remix 的 8。
- 合并时用 `target` 加权，`M` 累加，**不做 clamp**（Remix 只在 temporal 做）。

### 4. Shading（阶段 5）

- 从 reservoir 取出样本后只发 **1 条可见性射线**（不是逐灯多条）。
- 这就是 RTXDI 的核心收益：**每像素 1 次阴影射线**，而我们现在是 8 盏灯各一条。

---

## 四、分阶段验收（每阶段单独编译 + 单独测量）

| 阶段 | 改动 | 验收判据 |
|---|---|---|
| **1** | reservoir buffer + UBO 参数 + 空写入 | 编译通过、行为不变（开关默认关） |
| **2** | WRS 选光写入 reservoir + 直接 shading | `[DI]` 日志里 reservoir 的 lightIndex 分布合理；**整体亮度与旧路径一致**（能量不丢） |
| **3** | temporal reuse | 静止时噪声明显下降；快速转头无拖影（disocclusion 生效） |
| **4** | spatial reuse | 再降一档噪声；密集灯区（红石阵列）尤其明显 |
| **5** | 切换 shading 路径 + 减 atrous 次数 | `trace` 下降（阴影射线从 N 条降到 1 条）；`atrous` 次数 5→2 |

**每阶段都必须能单独回退**（环境变量或开关），不允许一次性替换整条管线。

---

## 五、与现有系统的接口

| 需要什么 | 现成的 | 备注 |
|---|---|---|
| 候选光源 | ✅ `local_light_grid`（binding 17/18） | 32B/灯 + 哈希网格，已有邻域走访 |
| 太阳方向/强度 | ✅ `cam` UBO | — |
| 上一帧 view-proj | ✅ 已有（MV 在用） | **口径必须与 MV 一致** |
| 深度/法线 | ✅ `depthImage` / G-buffer | 用于 disocclusion |
| TLAS（可见性） | ✅ `topLevelAS` | — |
| 蓝噪声/抖动 | ⚠️ 需确认 | WRS 与 spatial reuse 都需要 |

---

## 六、风险清单

1. **能量守恒**：从"逐灯相加"到"选一盏 + W 补偿"，任何 pdf 算错都表现为整体变暗或变亮。**每阶段都要和旧路径对比同位置亮度**。
2. **口径一致性**：重投影的 UV/NDC 与 MV 必须同口径（`mvecScale` 那次已经踩过）。
3. **与 SHARC 的交互**：SHARC 在间接路径 query，DI 在直接光路径，理论上不冲突；但如果阶段 5 改了 ray 预算，要重新确认 SHARC 的 `stale` 表现。
4. **不要一次替换**：`ORION_DIRECT_LIGHT_MODE` 已有 0/1 两档，新管线走第三档（建议 `=2`），跑通后再考虑切换默认。

---

## 七、当前状态

- 阶段 1 **未开始**。
- 现有 DI（SVGF 路线）：`rt_di_prepare / reproject / moments / confidence / atrous` 全部可用，`ORION_DIRECT_LIGHT_MODE=1` 启用。
- 建议开工顺序：阶段 1 → 2 → 3 → 4 → 5，每阶段一次编译、一局测量。
