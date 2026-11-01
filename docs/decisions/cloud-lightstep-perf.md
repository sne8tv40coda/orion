# 体积云光步降档（2026-10-28「对齐 + 性能优先」轮）

原文件：`cpp/shaders/cloud_v5_core.glsl`（`HPCloudConfig.lightErosion / lightReuse`、
`hpEvaluateSunLuminance` 的光步、`hpEvaluateCloudStep`、`hpTraceVolumetricRay` 的命中分支）。

## 1. 成本账：为什么先动光步

整个 v5 云 pass 的绝对大头是**光步**（`hpEvaluateSunLuminance`），不是视线步。按每个
**命中**采样点算一次体积采样的次数（weather 2D / base 3D / erosion 3D）：

| 环节 | 采样次数 | 说明 |
| --- | --- | --- |
| 视线步（simple 门控） | 2 | `simpleMode=true`：weather + base |
| 视线步（命中后补做） | 3 | + erosion |
| **光步**（lightSteps=8） | **24** | 8 步 × (weather + base + erosion) |

即光步约占命中像素体积采样量的 60%~80%，而它的产出（太阳项 / 自阴影 OD / phi_fwd）
变化尺度远小于视线步。

参考实现（HanPi，Unity HDRP）在这一步靠 **3D 噪声的 mip 链**把远光步换成粗层：
`EvaluateCloudProperties(..., mipOffset = j/(N-1)*3, ...)`。本仓两张体积
（base 128³ / erosion 64³）目前 `mipLevels=1`（`VulkanCore` 的 `createVolume`），
所以那条 `textureLod` 的 mip 参数**一直是空转**，光步 8 次全部按全分辨率全价付。
补齐 mip 链要动 DLL（纹理创建 + 3D blit 生成 mip + 视图 maxLod），属于下一轮；
本轮先在着色器侧降档。

## 2. 两个开关

| 字段 | 默认 | 语义 | 参考行为 |
| --- | --- | --- | --- |
| `lightErosion` | **0** | 0 = 光步用 `simpleMode=true`（跳过 erosion 采样） | `false`（全算） |
| `lightReuse` | **2** | 每 N 个命中步重算一次光步，中间复用太阳项 / OD / phi_fwd | 每步都算 |

`lightReuse` 的实现是把 `hpEvaluateCloud` 拆成两半：`hpEvaluateCloudStep`
（散射积分，公式逐行不变）与保留的 `hpEvaluateCloud`（参考 1:1 入口，内部自己算光步）。
trace 只走 `hpEvaluateCloudStep` + 缓存。相邻命中步相距 `stepSmall ≈ stepL = stepLarge*0.25`
（云内约 9~30 格），太阳项在这个尺度上几乎不变。

## 3. 实测

预览器 `--model v5 --game-save`，1280×720，**关掉校验层**
（`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`；开着校验层时 CPU 端开销
把差异淹没，同一变体两次能差 2 倍）后 200 帧 × 3 次取均值：

| 变体 | 200 帧均值 | 相对 |
| --- | --- | --- |
| 改动前 | 6737 ms | 1.00× |
| 仅 `lightErosion=0` | 5095 ms | 1.39× |
| 仅 `lightReuse=2` | 6381 ms | 1.11× |
| 两者同开（**出厂**） | 5076 ms | 1.33×（扣掉约 1 s 启动后 ≈1.4×） |

`lightReuse` 单独只有 1.11×、和 `lightErosion` 叠加后不额外加速 ⇒ 说明跳掉 erosion
之后光步已不再是瓶颈（剩下的开销回到视线步）。为不白拿"偏离参考"的代价，也可以把
`lightReuse` 设回 1，代价是丢掉那 1.11×。

外观代价（同一脚本、PPM 逐像素抽样）：

| 视角 | `lightErosion=0` + `lightReuse=2` | 仅 `lightReuse=2` |
| --- | --- | --- |
| 界面默认（平视） | mean 1.96/255，p95 9.0，p99 17.7，Δ亮度 −1.9 | mean 0.13，p99 1.3 |
| 仰视（pitch 45） | mean 3.84，p95 14.0，p99 21.7，Δ亮度 −3.8 | mean 0.18，p99 1.3 |

- `lightReuse=2` 基本**看不出差别**（p99 = 1.3/255）——相邻步差异远小于 8 位量化。
- `lightErosion=0` 让云**略暗**（自阴影 OD 偏高约 1/3）：`simpleMode` 里
  billowy/wispy 归零 ⇒ `eroded = baseShape`（不再被侵蚀压低）⇒ 光步看到的密度更大。
  这是"跳过侵蚀"的必然偏差，不是 bug。想要参考语义就把 `lightErosion` 设 1。

## 4. 被否决的一项

参考的 `GetCloudVolumeIntersection_Light` 还有半句"太阳射线被行星挡住 ⇒ 无直射光"。
本轮移植后又删掉了：本仓两个消费者传进来的 `sunDir` 在太阳落到地平线下时**已经被
翻转成月亮方向**（`volumetric_clouds_v5.comp`：`lightDirection = sunElevation >= 0 ? sun : -sun`），
`sunDir.y` 恒 ≥ 0 ⇒ 该分支在游戏里永不触发；而在预览器（不翻转）里它会在夜间触发，
造成"预览暗、游戏亮"的分叉（实测 `--sun-alt -6` 亮度差 +4/255，方向还与直觉相反：
AMBIENT 因为没有 self-shadow OD 反而更亮）。对齐参考的前提是两侧语义一致，这里不一致。

## 5. 复现

```powershell
$env:VK_LOADER_LAYERS_DISABLE="VK_LAYER_KHRONOS_validation"
& .\cpp\build\Release\cloudviewer.exe --model v5 --shot out/a.ppm --frames 4 --game-save
& .\cpp\build\Release\cloudviewer.exe --model v5 --shot out/b.ppm --frames 4 --game-save --pitch 45
# 变体对照：把 shaders 目录整份复制到 .tmp/…，只改 hpDefaultConfig 里的两个默认值后
#   用 "-I<dir>" 单独编译 cloud_bevy_preview.comp，再 --shader-dir <dir> 跑同一个 shot。
python .tmp/cloudalign/cmp.py out/a.ppm out/a2.ppm   # mean/p50/p95/p99 + 亮度差
```

## 6. 下一步（按收益排序）

1. **给 base / erosion 体积补 mip 链**（DLL）：让参考的 `mipOffset` 真正生效，
   光步可以既快又不掉细节，是上面两个开关的"正解"。
2. 光步只在前 K 步采样 erosion（近场自阴影需要细节，远场只需要平均 OD）——
   介于 `lightErosion=0/1` 之间，需要一次 A/B 定 K。
3. 受光层次（HANDOFF §还没做完 第 1 条）与 detail 分布标定属于纯质量项，
   在性能预算确定后再做。
