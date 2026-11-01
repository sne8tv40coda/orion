#ifndef LOCAL_LIGHT_PROFILE_GLSL
#define LOCAL_LIGHT_PROFILE_GLSL

// =============================================================================
// 本地光源档案表（Local Light Profile）
//
// 2026-10-31：取消"所有发光方块共用 emission/15 同一口径"的做法。
// 每个档案自带三件东西：发射色（色温或直接给定）、满级亮度、基础传播距离。
// 设置界面里的两个滑杆从此只当**整体系数**：
//   「本地光源照射强度」→ 乘在 localLightProfileBrightness() 上（0..4）
//   「本地光范围系数」  → 乘在 localLightProfileRange() 上（0.25..2.0），
//                         乘积最后仍被 @VPT_LOCAL_MAX_DISTANCE 封顶 30 格。
//
// id 由 SectionCompilerMixin.classifyLocalLightType() 写进体素字的
// bits 6..10（5 bit，0..31）。**本表顺序必须与那边的映射逐项一致。**
//
// color：已按峰值归一到 1.0。色温档来自黑体辐射（Tanner Helland 拟合，
//        < 2600K 往白方向去饱和 8%~22%，否则火焰的蓝分量过低、上屏发暗红）；
//        非热辐射光源（灵魂火/红石/传送门/紫水晶…）没有色温，直接给发射色。
// lumens：emission = 15 时的相对发光强度（1.0 ≈ 旧口径的满级亮度）。
//        实际亮度 = lumens × (emission / 15) × 用户滑杆。
// range：基础传播距离（格），已按各光源的物理尺度取过；全部 ≤ 30。
// pointLike：1 = 点状光源（火把/蜡烛，用半径 0.5 的球面近似），
//            0 = 整块光源（荧石/岩浆/海晶灯，用 2x2 立方体面近似）。
// =============================================================================

const uint LOCAL_LIGHT_PROFILE_COUNT = 32u;
const uint LOCAL_LIGHT_PROFILE_MASK = 31u;

// 四张平行表（不合成 struct：glslang 不支持"struct 数组 + 数组构造器"这种
// 常量初始化，而分离的 vec3[]/float[] 构造器是 GLSL 里最稳的写法）。
// 每行注释给出该档对应的光源与色温，改表时请连注释一起改。

const vec3 kLocalLightProfileColor[32] = vec3[32](
    // 0  fallback/未分类发光块        3000K
    vec3(1.0000, 0.7193, 0.4766),
    // 1  torch 火把               2000K
    vec3(1.0000, 0.6387, 0.2625),
    // 2  lantern 灯笼             2200K
    vec3(1.0000, 0.6421, 0.2890),
    // 3  campfire 营火            1900K
    vec3(1.0000, 0.6230, 0.2200),
    // 4  fire 火焰                1850K
    vec3(1.0000, 0.6149, 0.2200),
    // 5  soul_torch 灵魂火把        tint
    vec3(0.2200, 0.7200, 1.0000),
    // 6  soul_lantern 灵魂灯笼      tint
    vec3(0.2400, 0.7600, 1.0000),
    // 7  soul_campfire 灵魂营火     tint
    vec3(0.2600, 0.7900, 1.0000),
    // 8  soul_fire 灵魂火          tint
    vec3(0.2000, 0.7000, 1.0000),
    // 9  redstone_torch 红石火把    tint
    vec3(1.0000, 0.1600, 0.0600),
    // 10 redstone_lamp 红石灯      tint
    vec3(1.0000, 0.3300, 0.1300),
    // 11 lava 岩浆                1600K
    vec3(1.0000, 0.5708, 0.2200),
    // 12 magma_block 岩浆块        1450K
    vec3(1.0000, 0.5408, 0.2200),
    // 13 glowstone 荧石           3400K
    vec3(1.0000, 0.7642, 0.5677),
    // 14 sea_lantern 海晶灯        6200K
    vec3(1.0000, 0.9798, 0.9541),
    // 15 shroomlight 菌光体        2500K
    vec3(1.0000, 0.6840, 0.3908),
    // 16 jack_o_lantern 南瓜灯     2000K
    vec3(1.0000, 0.6387, 0.2625),
    // 17 end_rod 末地烛            tint
    vec3(0.8800, 0.9200, 1.0000),
    // 18 beacon 信标              tint
    vec3(0.9000, 0.9500, 1.0000),
    // 19 conduit 潮涌核心           tint
    vec3(0.3000, 0.8600, 1.0000),
    // 20 nether_portal 地狱门      tint
    vec3(0.7200, 0.3000, 1.0000),
    // 21 glow_lichen 发光地衣       tint
    vec3(0.4600, 0.8600, 0.5200),
    // 22 amethyst 紫水晶簇          tint
    vec3(0.6400, 0.4400, 1.0000),
    // 23 candle 蜡烛              1850K
    vec3(1.0000, 0.6149, 0.2200),
    // 24 crying_obsidian 哭泣黑曜石  tint
    vec3(0.5400, 0.3200, 1.0000),
    // 25 respawn_anchor 重生锚     tint
    vec3(0.7400, 0.4200, 1.0000),
    // 26 furnace_lit 熔炉点燃       1900K
    vec3(1.0000, 0.6230, 0.2200),
    // 27 froglight 蛙明灯          2900K
    vec3(1.0000, 0.7071, 0.4509),
    // 28 brewing_stand 酿造台      1900K
    vec3(1.0000, 0.6230, 0.2200),
    // 29 cave_vines 洞穴藤蔓        tint
    vec3(0.7800, 0.8400, 0.5000),
    // 30 brown_mushroom 棕色蘑菇    tint
    vec3(0.8600, 0.8000, 0.6200),
    // 31 enchanting_table 附魔台   tint
    vec3(0.6000, 0.4600, 1.0000)
);

const float kLocalLightProfileLumens[32] = float[32](
    1.0000,  //  0 fallback/未分类发光块
    1.0000,  //  1 torch 火把
    1.1200,  //  2 lantern 灯笼
    1.3000,  //  3 campfire 营火
    1.2000,  //  4 fire 火焰
    0.7800,  //  5 soul_torch 灵魂火把
    1.0000,  //  6 soul_lantern 灵魂灯笼
    1.1800,  //  7 soul_campfire 灵魂营火
    0.9500,  //  8 soul_fire 灵魂火
    0.5200,  //  9 redstone_torch 红石火把
    1.0500,  // 10 redstone_lamp 红石灯
    1.9000,  // 11 lava 岩浆
    0.5000,  // 12 magma_block 岩浆块
    1.4500,  // 13 glowstone 荧石
    1.3500,  // 14 sea_lantern 海晶灯
    1.3500,  // 15 shroomlight 菌光体
    1.1000,  // 16 jack_o_lantern 南瓜灯
    1.2000,  // 17 end_rod 末地烛
    2.2000,  // 18 beacon 信标
    1.5500,  // 19 conduit 潮涌核心
    1.5000,  // 20 nether_portal 地狱门
    0.3600,  // 21 glow_lichen 发光地衣
    0.4000,  // 22 amethyst 紫水晶簇
    0.5500,  // 23 candle 蜡烛
    0.5800,  // 24 crying_obsidian 哭泣黑曜石
    1.1000,  // 25 respawn_anchor 重生锚
    0.8500,  // 26 furnace_lit 熔炉点燃
    1.3500,  // 27 froglight 蛙明灯
    0.2800,  // 28 brewing_stand 酿造台
    0.3200,  // 29 cave_vines 洞穴藤蔓
    0.0600,  // 30 brown_mushroom 棕色蘑菇
    0.3600  // 31 enchanting_table 附魔台
);

const float kLocalLightProfileRange[32] = float[32](
    12.0000,  //  0 fallback/未分类发光块
    11.0000,  //  1 torch 火把
    13.0000,  //  2 lantern 灯笼
    15.0000,  //  3 campfire 营火
    13.0000,  //  4 fire 火焰
    8.0000,  //  5 soul_torch 灵魂火把
    10.0000,  //  6 soul_lantern 灵魂灯笼
    12.0000,  //  7 soul_campfire 灵魂营火
    10.0000,  //  8 soul_fire 灵魂火
    6.0000,  //  9 redstone_torch 红石火把
    13.0000,  // 10 redstone_lamp 红石灯
    26.0000,  // 11 lava 岩浆
    7.0000,  // 12 magma_block 岩浆块
    18.0000,  // 13 glowstone 荧石
    18.0000,  // 14 sea_lantern 海晶灯
    17.0000,  // 15 shroomlight 菌光体
    14.0000,  // 16 jack_o_lantern 南瓜灯
    14.0000,  // 17 end_rod 末地烛
    28.0000,  // 18 beacon 信标
    20.0000,  // 19 conduit 潮涌核心
    18.0000,  // 20 nether_portal 地狱门
    5.0000,  // 21 glow_lichen 发光地衣
    6.0000,  // 22 amethyst 紫水晶簇
    7.0000,  // 23 candle 蜡烛
    8.0000,  // 24 crying_obsidian 哭泣黑曜石
    11.0000,  // 25 respawn_anchor 重生锚
    9.0000,  // 26 furnace_lit 熔炉点燃
    16.0000,  // 27 froglight 蛙明灯
    4.0000,  // 28 brewing_stand 酿造台
    5.0000,  // 29 cave_vines 洞穴藤蔓
    2.0000,  // 30 brown_mushroom 棕色蘑菇
    6.0000  // 31 enchanting_table 附魔台
);

// 1 = 点状（球面近似），0 = 整块（立方体面近似）。
const float kLocalLightProfilePointLike[32] = float[32](
    0.0,  //  0 fallback/未分类发光块
    1.0,  //  1 torch 火把
    1.0,  //  2 lantern 灯笼
    1.0,  //  3 campfire 营火
    1.0,  //  4 fire 火焰
    1.0,  //  5 soul_torch 灵魂火把
    1.0,  //  6 soul_lantern 灵魂灯笼
    1.0,  //  7 soul_campfire 灵魂营火
    1.0,  //  8 soul_fire 灵魂火
    1.0,  //  9 redstone_torch 红石火把
    0.0,  // 10 redstone_lamp 红石灯
    0.0,  // 11 lava 岩浆
    0.0,  // 12 magma_block 岩浆块
    0.0,  // 13 glowstone 荧石
    0.0,  // 14 sea_lantern 海晶灯
    0.0,  // 15 shroomlight 菌光体
    0.0,  // 16 jack_o_lantern 南瓜灯
    1.0,  // 17 end_rod 末地烛
    0.0,  // 18 beacon 信标
    0.0,  // 19 conduit 潮涌核心
    0.0,  // 20 nether_portal 地狱门
    1.0,  // 21 glow_lichen 发光地衣
    1.0,  // 22 amethyst 紫水晶簇
    1.0,  // 23 candle 蜡烛
    0.0,  // 24 crying_obsidian 哭泣黑曜石
    0.0,  // 25 respawn_anchor 重生锚
    0.0,  // 26 furnace_lit 熔炉点燃
    0.0,  // 27 froglight 蛙明灯
    1.0,  // 28 brewing_stand 酿造台
    1.0,  // 29 cave_vines 洞穴藤蔓
    1.0,  // 30 brown_mushroom 棕色蘑菇
    0.0  // 31 enchanting_table 附魔台
);

uint localLightProfileIndex(uint typeCode)
{
    return typeCode & LOCAL_LIGHT_PROFILE_MASK;
}

// 发射色（已含色温/色调）。0 = 未分类 ⇒ 暖白兜底。
vec3 localLightProfileColor(uint typeCode)
{
    return kLocalLightProfileColor[localLightProfileIndex(typeCode)];
}

// emission = 15 时的相对发光强度。
float localLightProfileLumens(uint typeCode)
{
    return kLocalLightProfileLumens[localLightProfileIndex(typeCode)];
}

// 基础传播距离（格）。未分类档没有档案语义，按原版光级线性给。
float localLightProfileBaseRange(uint typeCode, float emissionLevel)
{
    if(localLightProfileIndex(typeCode) == 0u)
    {
        return clamp(emissionLevel * 2.0, 2.0, 30.0);
    }
    return kLocalLightProfileRange[localLightProfileIndex(typeCode)];
}

// 亮度因子：替代原来的 (emission / 15)。
// 每档的"满级有多亮"由档案决定，emission 只负责档内的比例（红石火把 7 级
// 就该比营火 15 级暗）。未分类档退回旧口径，模组方块不会突然变死黑或爆亮。
float localLightProfileBrightness(uint typeCode, float emissionLevel)
{
    float level01 = clamp(emissionLevel / 15.0, 0.0, 1.0);
    if(localLightProfileIndex(typeCode) == 0u)
    {
        return level01;
    }
    return localLightProfileLumens(typeCode) * level01;
}

// 光源几何：true = 点状（球面近似），false = 整块（立方体面近似）。
bool localLightProfilePointLike(uint typeCode)
{
    return kLocalLightProfilePointLike[localLightProfileIndex(typeCode)] > 0.5;
}

#endif
