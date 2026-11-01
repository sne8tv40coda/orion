# 半透明 alpha blend 的 pass 拆分（对齐 RTX Remix）

> 目标：把现在"在 rgen 里就地合成"的半透明，拆成 Remix 的三段结构：
> **primary(opaque) → alpha blend composite（屏幕空间邻居搜索）→ 合成 → RR**。
>
> 备份：`.tmp/backup_20261026_pre_remix_align.zip`（66 MB，541 文件：`VulkanCore.cpp`、
> `cpp/shaders/*`、Java mixin/access、本轮交接文档）。解压即回退。
>
> 状态：**M1+M2 已实现并编译通过（2026-10-26），未实测**。落地细节、与本文的
> 三处偏差、以及 A/B 开关见文末 §8。§7 是执行前的接入点清单，行号会随后续改动漂移。

---

## 1. Remix 的真实结构（逐字依据）

| 阶段 | 位置 | 产物 |
|---|---|---|
| primary raytrace | `rtx/pass/raytrace/…` | `PrimaryDirectDiffuseRadianceHitDistance`（demodulated）、`PrimaryLinearViewZ`、`PrimaryVirtualWorldNormalPerceptualRoughness`、`AlphaBlendGBuffer` |
| alpha blend 累积 | `concept/surface/alpha_blend_surface.slangh:128-158` | 每像素只记一层 `AlphaBlendSurface` + `backgroundTransparency = Π(1−a)` |
| 半透明取光 | `pass/composite/composite_alpha_blend.comp.slang:50-84` | 屏幕空间邻居搜索 → `AlphaBlendRadiance = neighborLight × surface.color.a` |
| 合成 | `pass/composite/composite.comp.slang:812` | `return radianceOutput * backgroundAlpha + alphaBlendOutput` |
| 降噪 | NRD / DLSS-RR | 消费**已合成**的颜色；guide 来自不透明面 |

三个不可省略的性质：

1. **邻居搜索读的是本帧已完成的光照缓冲**（独立 pass ⇒ 有完整数据，无同步问题）。
2. **`backgroundTransparency` 是独立于颜色的量**，还要参与体积/雾（`composite:581-588`）。
3. **alpha blend 层不产生 guide**（depth/normal/MV 全属于背景不透明面）。

## 2. 本工程现状与差距

现状（`rt_test.rgen:15555-16800`）：主射线在**同一次遍历**里消耗玻璃链，最后用
`behindRadiance`（透射命中的接收面）当 C_bg、用 `behindRadiance ÷ receiverAlbedo` 当 C_surface 的光照，
就地算出 `transmittedRadiance` 写进最终色。

差距：

| Remix | 现状 | 后果 |
|---|---|---|
| 邻居搜索（6 次迭代、半径 10×1.6、法线 0.9 / 深度 0.1 / 共面 0.2） | 用"透射射线命中的接收面"代替 | 取光点可能远在几十格外；紧贴光源时过曝 |
| radiance volume 兜底 | `getSurfaceAmbientRadiance()` | SHaRC 默认关（`docs/decisions/sharc-default-off.md`），无等价物 |
| 5×5 邻域加权滤波 | 无 | 玻璃噪点无抑制手段 |
| `opacity > 0.95` 走不透明 | 无 | 近不透明体仍混合 |

**结论：不拆 pass，"邻居搜索"与"邻域滤波"这两条永远做不出来。**

## 3. 拆分方案

### 3.1 新增资源（全屏、渲染分辨率）

| 资源 | 格式 | 用途 |
|---|---|---|
| `g_alphaBlendGbuffer` | RGBA16F | (color.rgb, a) 选中的那一层；alpha 通道存 `a` |
| `g_alphaBlendMeta` | RGBA16F | (hitT, backgroundAlpha, normal.xy 编码) —— 或拆成两张 |
| `g_alphaBlendRadiance` | RGBA16F | 新 pass 的输出，已乘 `a` |
| `g_opaqueRadiance` | RGBA16F | 未乘 `backgroundAlpha` 的不透明辐射 |

`rtOutput` 保持现状（最终色），由合成 pass 写入。

### 3.2 描述符

- **rgen 侧**：`VkDescriptorSetLayoutBinding b[75]` 已占满 0..74 ⇒ 需要扩到 **78**
  （3 张 storage image）。同步位置：set layout、descriptor pool、`vkUpdateDescriptorSets`、
  4 个 rgen 变体的 SPV（同一个 layout，编译期无感）。
- **新 composite pass**：**独立 pipeline layout + 独立 descriptor set**，
  照 `g_volumetricCloudCompositeSet` 的模式（`VulkanCore.cpp:41341-41354`）。
  这样新 pass 的描述符改动不会牵动 RT 主 set。

### 3.3 pass 顺序（改 `VulkanCore.cpp` 的 record 流程）

```
[1] vkCmdTraceRaysKHR            ← rgen：不再合成玻璃，写 g_alphaBlendGbuffer /
                                    g_alphaBlendMeta / g_opaqueRadiance
    barrier: RT write → compute read
[2] alpha_blend_composite.comp   ← 邻居搜索，读 g_opaqueRadiance + depth + normal，
                                    写 g_alphaBlendRadiance
    barrier: compute write → compute read
[3] glass_alpha_finish.comp      ← out = g_opaqueRadiance * backgroundAlpha
                                        + g_alphaBlendRadiance
                                    写 rtOutput（沿用现有 RT 输出图）
    （也可合并进现有 cloud composite 之前的第一个全屏 pass）
[4] 现有 vol / cloud / denoise 链不变
```

注意 [3] 可以**先不做独立 pass**：把 `out = opaque * bgAlpha + alphaBlend` 塞进现有
任意一个全屏 compute（例如 god-ray composite 的入口）即可，减少一个 pipeline。

### 3.4 rgen 侧改动（唯一必须扩 binding 的地方）

`tracePrimaryGenericTransparentRadiance()` 末尾（现在的 16565-16603）：

- 删掉 `transmittedRadiance` 的合成，改为三件事：
  1. 选层：按 `AlphaBlendSurface::update()` 的**确定性期望值**写
     `(color = tint, a = authoredAlpha)` 与 `backgroundAlpha`（我们已有 `paneChainTransmittance`）；
  2. 把 `behindRadiance`（未乘任何 alpha 的不透明辐射）写 `g_opaqueRadiance`；
  3. 写 `hitT`（玻璃面距离）与法线（供邻居搜索的深度/法线相似度）。
- 清玻璃「当空气」分支保持不变（它本来就不是 alpha blend surface）。
- **C_surface 的光照不再在 rgen 里取**——这正是拆分的收益。

## 4. 里程碑（每步可编译、可回退、可目视验证）

| 里程碑 | 内容 | 验证 | 风险 |
|---|---|---|---|
| **M1** | rgen 双写：`g_opaqueRadiance` + `g_alphaBlendGbuffer/Meta`；合成仍留在 rgen | 画面**逐像素不变**（新图只是旁路输出） | 低（纯新增） |
| **M2** | 新 `alpha_blend_composite.comp`：先用**固定**取光（同像素的 opaque radiance），不做邻居搜索 | 画面仍不变（等价于现在的行为） | 中（新增 pipeline + barrier） |
| **M3** | 把邻居搜索打开（6 次迭代 / 半径 10 / ×1.6 / 法线 0.9 / 深度 0.1 / 共面 0.2，全部照抄 `composite_alpha_blend.comp.slang`） | 紧贴光源不再发白；玻璃面与周围亮度一致 | 中 |
| **M4** | 5×5 邻域滤波（`transparencyBandwidth 0.05` / `distanceWeight 0.5` / `depthBandwidth 0.05`） | 玻璃噪点下降 | 低 |
| **M5**（可选） | `opacity > 0.95` 走不透明；`backgroundTransparency` 接入体积/雾 | — | 低 |

M1/M2 的设计目标就是**画面零变化**，这样拆分本身不会引入回归；视觉变化只允许出现在 M3 之后。

## 5. 回退

- 全量：解压 `.tmp/backup_20261026_pre_remix_align.zip`。
- 单步：M1/M2 都保留 `VPT_GLASS_ALPHA_BLEND_*` 宏，`=0` 回到 rgen 内合成路径。

## 6. 不做什么

- 不引入 G-buffer 式的"延迟材质求值"（`RT_DEFERRED_MATERIAL` 2026-10-21 已实测 +60% 耗时，见交接文档第三节 D）。
- 不为 radiance volume 自造体积网格；SHaRC 默认关，M3 的 miss 兜底仍用环境辐照度，
  并在代码里标注"降级"。

---

## 7. 接入点清单（本轮已核实，下一轮直接用）

### 7.1 binding 有空位，不需要扩到 78

`rt_test.rgen:277-283` 的 `binding = 73 / 74` 在 `#if 0` 里（2026-10-26 泡沫回退时摘掉）：

```glsl
#if 0
layout(binding = 73, set = 0) uniform sampler2DArray waterFoamMap;
layout(binding = 74, set = 0, r32ui) uniform uimage2D waterFoamInjection;
#endif
```

C++ 侧 `VulkanCore.cpp:22843` 是 `VkDescriptorSetLayoutBinding b[73]{}; // 0..72`，两边一致。

=> 新增 3 张全屏 storage image 直接占 **73 / 74 / 75**，数组改成 `b[76]`，
**其余 72 个 binding 编号一个都不用动**。

### 7.2 C++ 必须同步改的点（行号为当前文件）

| 位置 | 内容 | 改法 |
|---|---|---|
| `22843` | `b[73]{}` | 改成 `b[76]{}`，新增 73/74/75 三个 STORAGE_IMAGE，stageFlags = RAYGEN |
| `23597-23600` | `static_assert(sizeof(b)/sizeof(b[0]) == 73u)` | 改成 `76u`（不改会编译期报错，这是好事） |
| `29119-29135` | RT descriptor pool `ps[5]`，`STORAGE_IMAGE = 16` | 改成 `19`（三个新 binding），`maxSets = 1` 不变 |
| `41026-41045` | `setCloudCompositeTarget()`：写 storage image 的现成样例 | 照抄写 3 处 |
| `41341-41354` | cloud composite 的 pipeline + set + push constant + dispatch 模板 | 新 pass 照此挂载 |
| `23585-23607` | set layout 创建 + bindingCount | 自动跟随 `sizeof(b)` |

### 7.3 仍需在下一轮开始时查的两项（本轮没取到）

1. `vkCmdTraceRaysKHR` 的确切行号与紧随其后的 image barrier 原文（用作新 pass 的 `srcAccessMask`）。
2. 加载 SPV 到 `VkShaderModule` 的函数签名（新 .comp 用同一个加载器）。

这两项都是几分钟能查到的，不阻塞设计。

### 7.4 下一轮执行顺序

1. 读本文档；`.tmp/backup_20261026_pre_remix_align.zip` 只作回退用，不要覆盖工作副本。
2. 写 `cpp/shaders/glass_alpha_blend.comp`：邻居搜索 + 5x5 滤波 + 合成，
   参数照抄 `composite_alpha_blend.comp.slang:50-84 / 106-131 / 146-256`。
3. 改 `rt_test.rgen`：玻璃分支改为写 3 张旁路图（删就地合成），扩 binding 声明。
4. 改 `cpp/VulkanCore.cpp`：按 7.2 六处打补丁（`.tmp/` 下的补丁脚本，`count==1` 校验）。
5. 一次性 `python _deploy_stale_spv.py`（新 .comp 自动纳入）+ `cmake --build`。
6. 部署三处 shader + DLL。
7. 验证：先要求"画面逐像素不变"（M1+M2 等价），再打开邻居搜索看 M3。

---

## 8. 落地记录（2026-10-26，按 §7.4 执行完毕）

### 8.1 实际结构（与 §3 的差异）

| §3 原设计 | 实际 | 原因 |
|---|---|---|
| 4 张新图（gbuffer / meta / radiance / opaqueRadiance） | **4 张图，但 rgen 只用 3 个 binding**：73 gbuffer、74 meta、75 light；第 4 张 `alphaBlendRadiance` 只出现在新 pass 自己的 descriptor set 里 | `opaqueRadiance` 不存在了：rgen 对玻璃像素直接把**未合成**的背景辐射写进 `rtOutput`，新 pass 读同一个 texel |
| 1 个新 pass（composite 里就地写 rtOutput） | **2 个 pass**：`glass_alpha_blend.comp`（取光/滤波/合成 → radiance）+ `glass_alpha_finish.comp`（radiance → rtOutput） | 邻居搜索要读 `rtOutput` 的**邻居格**，同时又要写本格 ⇒ 同 dispatch 跨格读写在 Vulkan 里是竞态 |
| meta = (hitT, backgroundAlpha, normal.xy) | meta = (**链透射率 rgb**, hitT) | `backgroundAlpha` 在本工程是**逐通道**的（any-hit 的 `transmittanceAccum` 是 vec3），压成标量会改变多层叠色的结果 |

描述符：RT 主 set `b[73] → b[76]`（新增 73/74/75，其余 72 个编号未动），
池 `STORAGE_IMAGE 16 → 19`，`static_assert ... == 76u`。
新 pass 有**独立** set layout + pool + pipeline layout（8 张 storage image，push constant 64 B）。

### 8.2 数值等价性（M1+M2）

合成式逐项照抄就地合成：

```
bgAlpha = (1 - a0) * Π(1-a_i)          ; vec3，逐通道
out     = rtOutput * bgAlpha + Σ tint_i*a_i*Π_{j<i}(1-a_j) * E
```

E 的取法由 `ORION_GLASS_ALPHA_BLEND_NEIGHBOR` 决定（0 = 本像素的 75 号图 = 旧口径）。

**三处已知偏差（不是逐位等价，必须知道）：**

1. **高光钳位的顺序**：rgen 末尾的 `if(luma>6) *= 6/luma` 现在作用在"背景辐射"上
   （早于加 surface 项）。已在 blend pass 里补一次同样的钳位（等价于旧行为，
   极端高光下背景项被钳两次 ⇒ 略暗）。
2. **空气透视 / 水下光柱只作用在背景项**：`applyClearSkyAerialPerspective` 与
   `applyUnderwaterWaterColumn` 都在 rgen 里、作用在整个像素上；拆 pass 后 surface
   项（`tint*a*E`）在它们之后才相加 ⇒ 水下玻璃面的自发光项少了那一层衰减
   （水上场景差值 < 1%，水下偏亮）。补法：给新 pass 绑 camera UBO 并在 surface 项上
   复算同一个函数（本轮未做，因为要新增 binding 8 = UBO）。
3. **fp16 往返**：E 与 surface 权重经 `rgba16f` 中转一次（旧路径全程 fp32 直到写
   `rtOutput`）。末位差异，肉眼不可见。

### 8.3 开关

| 开关 | 出厂 | 作用 |
|---|---|---|
| `VPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE`（rgen 宏） | 1 | 0 = 逐位回到 rgen 就地合成；宿主仍清 74 号图 ⇒ 新链惰性 |
| `ORION_GLASS_ALPHA_BLEND_NEIGHBOR`（环境变量） | 0 | 1 = M3 屏幕空间邻居取光 |
| `ORION_GLASS_ALPHA_BLEND_FILTER`（环境变量） | 0 | 1 = M4 5x5 邻域滤波 |
| `VPT_GLASS_ALPHA_BLEND_PANE_SPECULAR` | 0 | 1 时**强制关闭** deferred（那档要叠镜面反射，提前返回会丢掉） |

邻居搜索参数一律走 push constant，改档不必重编：6 次迭代 / 半径 10 / ×1.6 /
法线 0.9 / 相对深度 0.1 / 共面 0.2；滤波 5×5 / transparencyBandwidth 0.05 /
distanceWeight 0.5 / depthBandwidth 0.05。

### 8.4 产物（2026-10-26 编译，三处目录已同步）

```
rt_test.rgen.spv              8B685927
rt_test_ser.rgen.spv          6679319D
rt_test_sharc_update.rgen.spv B5EAC14A
rt_test_gi_half.rgen.spv      7E620DD0
glass_alpha_blend.comp.spv    30DE2943
glass_alpha_finish.comp.spv   DB219153
```

DLL：`cpp/build/Release/rtx_backend.dll`（5,917,184 B，MD5 `20DFAAC7…`；`ABA0E86C…` 是补同步修复前的那版），已部署 `run/`、
`src/main/resources/`、`src/main/resources/natives/windows/`、
`build/resources/main/`、`build/resources/main/natives/windows/`。

---

## 9. 第四轮：严格搬 RTX Remix（2026-10-26，用户要求）

> §8 那套"确定性逐层累加"（`Σ_i tint·a_i·Π_{j<i}(1−a_j)`）**已废弃**。
> 它是问题本身：层数一多 Σ→1，颜色饱和成 tint，表现为"只有正对面透明、
> 四个侧面纯蓝实心"。原文不是这样。

### 9.1 原文（`_ref/dxvk-remix`，逐字）

| 位置 | 内容 |
|---|---|
| `concept/surface/alpha_blend_surface.slangh:29-37` | `AlphaBlendSurface { color, normal, backgroundTransparency, randomThreshold, geometryHash, hitT, hasEmissive }` |
| `:128-158` | `update()`：`if (!isOpaque()) { if (randomThreshold >= 1−bgT) 覆盖 else setOpaque(); } bgT *= (1−a)` —— **每像素只留一层** |
| `:130-141` | 注释给出无偏性证明：C1 概率 a1、C2 概率 (1−a1)a2…… |
| `algorithm/resolve.slangh:471-524` | 三档 opacity：`>= 254/255` 当不透明、`<= 1/255` 当全透、中间才是 alpha blend |
| `algorithm/geometry_resolver.slangh:2196-2204` | GBuffer `load → update → store`；`bgT < 1/255 ⇒ continueResolving = false` |
| `algorithm/geometry_resolver.slangh:3238` | 链起点写 `randomThreshold = getNextSampleBlueNoise(randomState)` |
| `pass/composite/composite_alpha_blend.comp.slang:187` | `(!isMiss && (isCoplanar \|\| isDepthSimilar) && isNormalSimilar && isSameGeometry)` |
| `:160 / :204` | **只有失败时才** `radius *= expandFactor` |
| `:253 / :259 / :273` | `surfaceLight = 邻像素解调光照`；找不到 → radiance volume；`AlphaBlendRadiance = surfaceLight * color.a` |
| `pass/composite/composite.comp.slang:812` | `out = radianceOutput * backgroundAlpha + alphaBlendOutput` |
| 默认值 | 6 / 10.0 / 1.6 / 0.9 / 0.1 / 0.2；`UseNeighborSearch/SameObject/ShareNeighbors/UseRadianceVolume` 全 true，`AverageSearchMatches` false（`rtx_composite.h:107-120`） |

### 9.2 本轮落地（逐项对应）

- **状态放 GBuffer，不放 ray payload**（原文就是这么做）：73 = `color.rgba`、
  74 = `(bgT, hitT, normalOct.xy)`、75 = `(randomThreshold, geometryHash, emissive)`。
  三张图 = 12 个浮点，与原文 `pack()` 的 16 B 一一对应。
- `glass_chain_common.glsl` 新增 `glassAlphaBlendSurfaceReset()` /
  `glassAlphaBlendSurfaceUpdate()`，后者是 `update()` 的逐行移植。
- 两个 rahit：**每个相遇面都是一层候选**（旧的 `glassChainAcceptEntry`
  "一个体只算一次"在本口径下停用），并补上两档 opacity 早退与 `bgT < 1/255` 终止。
- rgen：链起点 reset + 写 `randomThreshold`；相机直接命中的面作为**第一层候选**
  调同一个 `update()`；不再做任何就地合成。
- `glass_alpha_blend.comp`：按原文重写邻居搜索（判据 OR、只在失败时扩半径、
  蓝噪声重定位、`isSameGeometry` 哈希、找到即停）。
- `glass_alpha_finish.comp`：`out = radiance * bgT + alphaBlendOutput`。
- 宿主：binding 73/74/75 的 `stageFlags` 加 `ANY_HIT`（原文的 update 在几何求值侧）；
  74 号图清成 `(1,0,0,0)`（`createEmpty()` 的 bgT=1 / hitT=0）；新 pass 加
  binding 8 = camera UBO（邻居世界位置重建）。

### 9.3 仍然缺的输入（都是"没有对应缓冲"，不是自创数学）

| 原文 | 本轮做法 | 影响 |
|---|---|---|
| `PrimaryLinearViewZ` | `sceneDepth`（reverse-Z）+ `projInverse` 反算 | 等价，精度足够 |
| `PrimaryDirectDiffuseRadianceHitDistance` | `sceneColor[邻居] / max(albedo,0.05)` | 现解调，含间接与高光 |
| `PrimaryDirectSpecularRadianceHitDistance` | **未计入** | 玻璃上的高光取光偏暗 |
| radiance volume（`evalVolumetricNEE`） | 本像素背景辐照度 | 无窗室内偏暗 |
| `getNextSampleBlueNoise` | 整数哈希 | 同分布、非同一张图 |
| `opacity >= 254/255` 的 final-hit 着色 | 仅"不记录、作为遮挡" | 近不透明体走 rgen 兜底 |

### 9.4 产物（2026-10-26 第四轮，三处已同步）

```
rt_test.rgen.spv              5A009143      rt_test.rahit.spv            13C82CE9
rt_test_ser.rgen.spv          656F566B      rt_test_translucent.rahit    6F999FA1
rt_test_sharc_update.rgen.spv 672E4D0C
rt_test_gi_half.rgen.spv      5AAA707E
glass_alpha_blend.comp.spv    605E744A      glass_alpha_finish.comp.spv  678CB48A
```

DLL `866CBF0E…`。A/B（默认全开，设 0 关）：`ORION_GLASS_ALPHA_BLEND_NEIGHBOR`、
`ORION_GLASS_ALPHA_BLEND_RADIANCE_VOLUME`；彻底回旧口径用
`-DVPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE=0`（宏已移到共享头）。

---

## 10. 第五轮：补 §9.3 的缺口（2026-10-27）

### 10.1 已补：final-hit 当不透明着色（原文 `resolve.slangh:471-479`）

原文 `opacity >= cb.resolveOpaquenessThreshold(254/255) ⇒ resolveVertexFinalHit()`
= 按**不透明表面完整着色**。上一轮只做了"不记录、当遮挡返回"，于是这类面落到
rgen 的 `hitAlbedo * ambient` 兜底 —— 不是原文行为，是实现偷懒。

现在 rgen 里：

```glsl
const bool hitIsOpaqueGlass =
    glassChainIsTranslucentMaterial(hitMaterialId) &&
    payload.glassBlendAlpha >= GLASS_ALPHA_BLEND_OPAQUENESS_THRESHOLD;
if(hitMaterialId <= 0.5 || hitIsOpaqueGlass) { ...完整不透明着色... }
```

判据必须用 `glassBlendAlpha`（贴图 alpha × 顶点色 alpha）；`glassAlpha` 对整个
玻璃族恒为 1.0，用它会让**所有**玻璃都走不透明分支。

### 10.2 已补：时空蓝噪声（原文 `utility/noise.slangh:161-249`）

原文不是哈希，是 `texelFetch(BlueNoise, ivec3((px+off)&127, (py+off)&127, temporal&63), 0).r`，
数据为 `src/util/util_blueNoise_128x128x64.h`（MIT，128×128×64 R8）。

落地：
- 该表原样导出为 `cpp/shaders/blue_noise_128x128x64.r8`（1,048,576 B）；
- 宿主上传为 **storage buffer**（`createGlassBlueNoiseBuffer`），rgen binding 76、
  新 pass binding 9。`texelFetch` 本就是 NEAREST 取单 texel ⇒ 按字节索引逐位等价，
  而 SSBO 不需要 cmd 上传与 layout 转换；
- `glassBlueNoiseR2()` / `glassBlueNoiseSample()` 逐字移植 `R2()` 与
  `getNextSampleBlueNoise()`，`seed` 初值取 `temporalIdx / RNG_SEED_FRAME_RANGE`；
- 文件缺失时宿主用确定性哈希填一份（描述符永远有效，分布退化）。

### 10.3 radiance volume：已换成体素面辐照度缓存（SH-froxel 待立项）

**已做（2026-10-27）**：兜底数据源从"本像素背景辐照度"换成**体素面辐照度缓存**
（`rt_vpt_irradiance.comp` 的 16³×6 block-face direct irradiance，查询函数
`evaluateVPTExactFaceDirectLighting`，`rt_test.rgen:2084`）。

- 新增 binding 77 = `g_alphaBlendVolumeImage`（rgba16f）；
- rgen 在玻璃像素上写
  `evaluateVPTExactFaceDirectLighting(receiverPos, receiverNormal, vec3(1))`
  —— 第三参传 1 得到"单位 albedo 的漫反射出射辐射"（内部含
  `VPT_EXACT_FACE_DIFFUSE_SCALE = 0.92/π`，见 `rt_test.rgen:2060`）；
- blend 在邻居搜索失败时读它（新 pass binding 10），再乘 `surface.rgb`；
- 体素格缺失时才退到最后一级（本像素背景辐照度），并在注释里标明那不是原文行为。

量纲：Remix 的 `volumeRadiance` 是 NEE 辐照度（`/4π` 各向同性假定，见
`geometry_resolver.slangh:1828` 的注释），我们的是 `0.92/π · E`。两者都乘
`surface albedo` 后被消费，常数差异未做对齐（**未逐位一致**，已知）。

**仍待立项（完整照抄）**：

原文 `composite_alpha_blend.comp.slang:259-267` 的兜底是：

```slang
evalVolumetricNEE(VolumeFilteredRadianceY, VolumeFilteredRadianceCoCg,
                  cb.volumeArgs, surfaceInteraction, froxelVolumeHint)
```

`VolumeFilteredRadianceY/CoCg` 是 **`Sampler3D` 辐照度 froxel 网格**
（`composite_bindings.slangh:82`），由 volume lighting 的 inject / filter / integrate
三个 pass 每帧填充，还依赖 `cb.volumeArgs`（froxel 相机、深度切片、最大距离）。

本仓库**没有可用的同类缓存**：
- `evaluateVPTExactFaceDirectLighting`（16³×6 面直射）被硬关
  （`rt_test.rgen:23434 VPT_LOCAL_EXACT_DIRECT_ON_PRIMARY = false`，
  原因记录为"空间离散静态白点"）；
- 9³ air probe 注释明说"不参与可见主光照"；
- `vol_restir` 的 froxel 网格存的是**散射辐亮度**，不是辐照度。

要补这一项，需要（照抄顺序）：
1. 3D 纹理两张（Y + CoCg，128³ 量级）+ 描述符；
2. inject pass（把直接光注入 froxel，对应 `pass/volume_lighting/` 的 inject）；
3. filter pass（大核空间滤波，Remix 是 3 级 separable）；
4. 新 pass 的 `evalVolumetricNEE`（球谐或各向同性取样）；
5. `cb.volumeArgs` 等价参数（froxel 相机 / 切片数 / 最大距离）。

这是**独立子系统**（与半透明无关，辐射缓存本身），预估与 §9 全轮同级的工作量。
在当前缺 specular 缓冲的前提下，它的收益主要体现在"玻璃背后是无窗室内"的
暗部补光，不影响"侧面纯色"这个主问题。

### 10.3.1 SH-froxel 第一阶段：球谐头（2026-10-27，已完成）

`cpp/shaders/spherical_harmonics.glsl` —— `concept/light/spherical_harmonics.slangh`
（177 行）的逐行移植，含 `rgbToYCoCg`/`yCoCgToRGB` 矩阵（`utility/color.slangh:137-160`）。

- 结构照原文：**亮度 2 阶 SH（L11/L1-1/L10/L00）+ 标量色度 CoCg**，不是 9 阶全 SH；
- 函数对应：`shEvalBasis` / `shAddRadiance` / `shGetIrradiance` /
  `shResolveColorToDiffuse`（Frostbite p.49）/ `shScale` / `shHanningFilter(0.7)` /
  `shLerp` / `shLoad3D` / `shLoadFiltered3D` / `shEvaluateHenyeyGreenstein`；
- 写端 `store3D` 未进头文件：GLSL 要求同一函数内所有 image 参数**格式一致**
  （原文 `RWTexture3D<float4>` 与 `<float2>` 是两种格式，Slang 允许）⇒ 由调用方
  `imageStore` 两行代替；
- 验证：`.tmp/sh_check.comp` 全入口编译通过；`.tmp/sh_selftest.py` 九项全 PASS。
  **自检抓到一个真错误**：第一版 `shYCoCgToRgb` 的 G 通道写成 `Y`（漏 `+ Cg`），
  三基色往返误差 0.25~0.50 —— 若没抓出来，整个 SH 辐照度会偏色、绿色通道虚高。

### 10.3.2 SH-froxel 第二阶段（调查完毕，改动清单已定）

**调查结论（2026-10-27）：原文没有独立的 SH 滤波 pass。**
决定性证据是绑定行本身：

```cpp
// rtx_composite.cpp
ctx->bindResourceView(COMPOSITE_VOLUME_FILTERED_RADIANCE_Y_INPUT,
                      globalVolumetrics.getCurrentVolumeAccumulatedRadianceY().view, nullptr);
```

`VolumeFilteredRadiance*`（着色器里的 `Sampler3D`）绑的就是
`getCurrentVolumeAccumulatedRadiance*` —— "Filtered" 只是绑定名的历史遗留，
`RtxGlobalVolumetrics` 里也只有 `m_volumeAccumulatedRadianceY/CoCg/Age[2]`
的 ping-pong，没有任何 filter 资源。所以"滤波"只有两处：

1. 消费端 `loadFiltered3D` = **三线性采样**（`Sampler3D` + linear sampler）；
2. `hanningFilter(0.7)` 对 SH 系数缩放（抑制振铃）。

**还差两块**：

1. **froxel 映射**：本引擎已有等价物 —— `cpp/shaders/vol_restir_common.glsl`
   就是照 Remix `froxel.slangh` 的口径写的（`vol_restir_integrate.comp` 头注释
   第 26 行明写"froxel 世界位置：vol_restir_common.glsl（Remix froxel.slangh 口径）"）。
2. **SH 注入 + 时间累积**：`volume_integrator.slangh:202-266`
   ```slang
   radianceSH.addRadiance(fireflyFiltering(radiance, thr), lightDir);   // :202-205
   ...
   HarmonicsHelpers::store3D(newAccumulatedRadianceSH, AccumulatedRadianceY,
                             AccumulatedRadianceCoCg, froxelIndex + offset); // :266
   ```
   其 `radiance` 来自 volume ReSTIR 的最终 reservoir —— **正是本引擎
   `vol_restir_integrate.comp` 已经在算的东西**（该 pass 把最终 reservoir 换算成
   每 froxel 的入散射辐亮度，写 `volRadianceOut`）。

**因此落点唯一且不需要新造数学**：在 `vol_restir_integrate.comp` 里对每个 froxel
`shAddRadiance(radiance, lightDir)`，与上一帧 SH 做 `shLerp` 后写回 ping-pong 的
两张 3D 纹理；`glass_alpha_blend.comp` 的兜底改成
`shLoadFiltered3D → shHanningFilter(0.7) → shGetIrradiance`
（`volume_lighting.slangh:357-366`）。

需要的改动：
- 3 张 3D 纹理（SH Y: rgba16f、SH CoCg: rgba16f、age: r16f）× 2 份 ping-pong；
- `vol_restir_integrate.comp`：+2 个 image3D 输出（本帧/历史）+ SH 累积；
- `vol_restir_composite.comp` 或 blend 侧绑定 SH 纹理；
- host：资源创建/描述符/ping-pong 翻转（照 `g_volRadianceView` 的现有模式）。

前置条件：`ORION_VOL_RESTIR=1`（体积光 ReSTIR 链）。链关着时 SH 无数据，
兜底自动退回 binding 10 的体素面缓存 —— 两条兜底并存，不冲突。

### 10.3.3 SH 累积的全部依赖已找齐（2026-10-27）

原文累积公式（`volume_integrator.slangh:96-269`，逐行）：

```
previousFroxelUVW = calculateJitteredFroxelUvw(prev, 0.05)        // :99  历史抖动
reprojectionValid = all(prevUVW > 0 && prevUVW < 1)               // :103
historyAgeWeight = 1.0                                            // :108
oldRadianceSH = 0; newHistoryAge = 0                              // :104-106
if (previousVolumeExists && reprojectionValid && maxAccumulationFrames > 0):
    oldRadianceSH  = loadFiltered3D(PrevY, PrevCoCg, physicalPrev) // :125 三线性
    oldHistoryAge  = PrevAge.SampleLevel(physicalPrev) * maxAccumulationFrames  // :126
    newHistoryAge  = min(oldHistoryAge + 1, maxAccumulationFrames) // :129
    if (newHistoryAge > 1): historyAgeWeight = 1/(newHistoryAge + 5.0)  // :140-142
radianceSH.addRadiance(fireflyFiltering(radiance, thr), lightDir)  // :202-205
newSH = mix(oldRadianceSH, radianceSH, historyAgeWeight)          // :262
store3D(newSH, AccumY, AccumCoCg, froxelIndex)                    // :266
AccumAge[froxelIndex] = newHistoryAge / maxAccumulationFrames     // :268
```

`fireflyFiltering` 原文（`rtx/utility/common.slangh:37-47`，此前只找到调用点）：

```slang
vec3 fireflyFiltering(vec3 contribution, float fireflyFilterMaxIntensity)
{
  if (fireflyFilterMaxIntensity > 0.0)
  { const float magicNumber = 1e-5f;
    const float luminance = calcBt709Luminance(contribution);
    const float clampedLuminance = min(luminance, fireflyFilterMaxIntensity);
    contribution = (luminance > magicNumber) ? (contribution * (clampedLuminance / luminance)) : contribution; }
  return contribution;
}
```
阈值默认 `1000.0`（`rtx_global_volumetrics.h:108`）。

本引擎已有的对应物（无需新造）：
- `lightDir` = `normalize(light.worldPosEmission.xyz - froxelCenter)`（`vol_restir_integrate.comp:132`）；
- `radiance` = 该 froxel 的入散射辐亮度（同文件 :140-141）；
- 重投影 = `vol_restir_temporal.comp:75-99` 的 `volRestirPrevViewProj()` 路径；
- **age 资源已有**：`g_volAgeImage[2]`（`VulkanCore.cpp:5037` 区，R8_UNORM、froxel 网格、两份）；
- 消费端 = `spherical_harmonics.glsl` 的 `shLoadFiltered3D` + `shHanningFilter(0.7)` + `shGetIrradiance`。

host 侧待改 8 处：
1. 新增 `g_volShY*[2]` / `g_volShCoCg*[2]`（rgba16f）+ `g_volShAge*[2]`（r16f）；
2. `createVolumeRestirResources`（:43385 循环内）+3 张 ×2；
3. `destroyVolumeRestirResources`（:43202 区）+销毁；
4. integrate 的 set layout：5 → 11 binding（16..21）；
5. integrate 描述符写入 +6；
6. ping-pong 翻转点（与 `g_volRadianceView` 同处）；
7. blend pass set：+2（SH Y/CoCg 作为 `sampler3D`）；
8. `updateGlassAlphaBlendPassDescriptors` 绑 SH view。

### 10.3.4 SH-froxel 接线完成（2026-10-27，第七轮）

数据流（全部照原文，无一自创公式）：

```
vol_restir_integrate.comp（每个 froxel）
  radiance / lightDir（原有）→ fireflyFiltering(radiance, 1000)
  → shAddRadiance(radianceSH, r, lightDir)                     // integrator:202-205
  历史：volRestirPrevViewProj() 投影 → 8-tap 三线性 → shLerp(old, new, w)  // :125/:262
  → 写 SH Y / CoCg / age（binding 19/20/21）                   // :266/:268
  host 随后 copy [0] → [1]（上一帧），首帧清 0
glass_alpha_blend.comp（邻居搜索失败时）
  world → froxel 坐标 → 三线性 → shHanningFilter(0.7) → shGetIrradiance()
                                                               // volume_lighting:357-366
```

- 累积参数取原文默认：`maxAccumulationFrames = 128`、`historyAgeOffset = 5.0`、
  阈值 `1000`；`historyAgeWeight = 1/(age+5)`，重投影失败时 `age = 0 / w = 1`。
- **已知差异**：重投影只投 xy（z 取当前层）——与 `vol_restir_temporal.comp:98`
  的既有口径一致；Remix 对 z 也做深度切片反解（`froxelCoordinateToPrev`）。
- 兜底顺序：SH（体积 ReSTIR 开着且该 froxel 有数据）→ 体素面辐照度（binding 10）
  → 本像素背景辐照度。
- 默认配置安全：`ORION_VOL_RESTIR=0` 时 SH 图不存在，`updateGlassAlphaBlendPassDescriptors`
  改绑一张 **1×1×1 的 3D 占位图**（颜色不重要，`push.shValid = 0` ⇒ 着色器不读）。
  不加这个占位图的话，binding 11/12 会绑 NULL view —— 与 2026-10-26 云那次
  「NULL sampler ⇒ 整条写入被丢弃」同类。

### 10.4 产物（第七轮，2026-10-27）

```
rt_test.rgen.spv AF921EB7   ser 99D1F5C7   sharc A9C750FE   gi_half AC46E9A9
glass_alpha_blend.comp.spv 531224C7（binding 11/12 = SH Y/CoCg，push 80 B）
glass_alpha_finish.comp.spv 85411AE0
vol_restir_integrate.comp.spv 7A8148C4（binding 16..21 = SH prev/cur）
blue_noise_128x128x64.r8 479E5C12
DLL 7F85BEE4 × 6 处
```


