#ifndef SKY_MODEL_COMMON_GLSL
#define SKY_MODEL_COMMON_GLSL

#include "sky_sun_common.glsl"

// =============================================================================
// 2026-11-27 天空 LUT（skyScatteringTex / skyTransmittanceTex，256×128 equirect）
// 的仰角参数化 —— 修「天空色彩呈阶梯分布、断层」（用户报告）。
//
// 旧映射 v = elevation/π + 0.5 是**线性**的：128 行均分 180°，每行 1.41°。
// 而天空的色彩变化几乎全挤在地平线上下几度（暮光橙带、地平线泛白、Rayleigh
// 光程在 mu→0 处的陡升）；双线性插值在这里只是一串 1.4° 长的折线段，
// 每个转折点的斜率突变经 ACES2 抬对比后读成一圈圈色阶 = "阶梯断层"。
//
// 改为 Hillaire 2020（UE4 Sky-View LUT）的非线性纬度映射：
//   v = 0.5 + 0.5·sign(l)·sqrt(|l| / (π/2))
// 地平线 ±5° 内由 ~7 行增至 ~30 行（采样密度 ≈4×），天顶附近每行放宽到 ~2.8°
// —— 那里天空本来就是平滑渐变，不需要分辨率。零额外采样、零额外显存。
//
// **写端与读端必须同一映射**：写端 sky_atmosphere.comp::directionForPixel，
// 读端 rt_test.rmiss（天空本体 + 日盘透射染色）、rgen_transparent_primary.glsl
// （玻璃解析反射）、rgen_water_receiver.glsl（走查耗尽天空）、
// volumetric_clouds_v5.comp（云的天光）。四处都只调这两个函数。
// star_map_common.glsl / rt_test.rgen 里的 directionToEquirect 仍是线性的，
// 它们服务星图等别的 equirect 纹理，**不要**拿来采天空 LUT。
// A/B 回退：-DSKY_LUT_HORIZON_MAPPING=0（逐位回到线性映射）。
// =============================================================================
#ifndef SKY_LUT_HORIZON_MAPPING
#define SKY_LUT_HORIZON_MAPPING 1
#endif

vec2 skyLutDirectionToUv(vec3 dir)
{
    const float TWO_PI = 6.28318530718;
    const float HALF_PI = 1.57079632679;
    float u = fract(atan(dir.z, dir.x) / TWO_PI);
    float elevation = asin(clamp(dir.y, -1.0, 1.0));
#if SKY_LUT_HORIZON_MAPPING
    float v = 0.5 + 0.5 * sign(elevation) *
        sqrt(clamp(abs(elevation) / HALF_PI, 0.0, 1.0));
#else
    float v = elevation / 3.14159265359 + 0.5;
#endif
    return vec2(u, v);
}

float skyLutElevationFromV(float v)
{
#if SKY_LUT_HORIZON_MAPPING
    const float HALF_PI = 1.57079632679;
    float s = 2.0 * v - 1.0;
    return sign(s) * s * s * HALF_PI;
#else
    return (v - 0.5) * 3.14159265359;
#endif
}

vec3 sunDirection()
{
    const float TWO_PI = 6.28318530718;
    float angle = fract(cam.historyParams.w) * TWO_PI;
    return normalize(vec3(cos(angle), 0.81915204429 * sin(angle),
        0.57357643635 * sin(angle)));
}

// =============================================================================
// 2026-11-16 月亮方向（**单一真源**，纯函数：不引用 cam，任何 TU 都能 include）
//
// 旧实现把"太阳方向取反"当月亮 ⇒ 断言了"月亮永远在太阳正对面"：永远满月、
// 太阳一落它才升起、白天天上永远没有月亮。现实完全不是这样 ——
//   朔（新月）  黄经差   0°  ⇒ 与太阳同升同落，白天就在天上（被阳光淹没）
//   上弦/下弦   黄经差  90°  ⇒ 傍晚在正头顶 / 后半夜才升起
//   望（满月）  黄经差 180°  ⇒ 太阳落下时升起
// 也就是说**太阳在的时候月亮经常也在**，只是相位、方位、亮度不同。
//
// 本工程沿用同一条 35°N 春分日轨（忽略 5.1° 黄白交角），只沿日轨推移相位：
//   月相 0   ⇒ 偏移 +0.5 圈 ⇒ 与太阳正对，与旧 `-sunDirection()` **逐位等价**
//                              （cos(a+π) = -cos(a)、sin(a+π) = -sin(a)）
//   月相 0.5 ⇒ 偏移 +0.0 圈 ⇒ 与太阳重合（新月）
// 月相来源 = CameraUBO.sssParams.z，语义必须与 skyModelMoonPhaseDim() 一致：
// z = 0 满月、z = 0.5 新月（两者同源于同一个 z，不会出现"能量按满月给、
// 月盘却画成月牙"）。
//
// ⚠ 这里**只能**放纯函数。引用 cam.sssParams 的包装函数必须写在调用方 TU 里：
//   cloud_bevy_preview.comp 用的是裁剪版 camera 块（没有 sssParams 字段），
//   在这里引用会让它编译失败（实测 'sssParams' : no such field in structure）。
// =============================================================================
vec3 moonEclipticDirectionAt(float phase01)
{
    const float TWO_PI = 6.28318530718;
    float angle = fract(phase01) * TWO_PI;
    return normalize(vec3(cos(angle), 0.81915204429 * sin(angle),
        0.57357643635 * sin(angle)));
}

// 月亮在日轨上的相位 = 太阳相位 + 月相 + 0.5
float moonDecalPhase01(float sunPhase01, float moonPhase01)
{
    return fract(sunPhase01 + moonPhase01 + 0.5);
}


const vec3 END_SKY_POLE = vec3(0.0, 0.57357643635, -0.81915204429);
const float END_SKY_SPIN_RATE = 2.0;
const float END_EARTH_PHASE_OFFSET = 0.75;
const float END_EARTH_TILT = 0.40840704497;
const float END_EARTH_ANGULAR_RADIUS = 0.4363;

// 黄道（太阳日轨所在的平面）上的单位方向，t 以"圈"为单位。
vec3 endEclipticDirection(float t)
{
    const float TWO_PI = 6.28318530718;
    float a = fract(t) * TWO_PI;
    return normalize(vec3(cos(a), 0.81915204429 * sin(a),
        0.57357643635 * sin(a)));
}

// 地球方向（现在**会动**：2 圈/周期，沿黄道）。
vec3 endEarthDirection()
{
    return endEclipticDirection(
        cam.historyParams.w * END_SKY_SPIN_RATE + END_EARTH_PHASE_OFFSET);
}

// 地球自转轴。两个要求：
//   1) 与地球方向**刚性**同步（都随星空前进），并且**始终垂直**于地球方向
//      ⇒ 视面中心永远落在赤道附近。原实现用常量轴 (0.3978,0.9175,0) 几乎就是
//      世界竖直方向，与地球方向夹角只有 41° ⇒ 视面中心在 48.7°S、最北只看到
//      41°N，玩家永远只能看到"南极 + 南大洋"，认不出是地球。
//   2) 保留 23.4° 轴倾角，看起来像真实行星。
// 构造：黄道上超前 90° 的方向，再朝天极倾斜 23.4°（于是 ⊥ 地球方向，恒成立）。
vec3 endEarthAxis()
{
    float t = cam.historyParams.w * END_SKY_SPIN_RATE +
        END_EARTH_PHASE_OFFSET + 0.25;
    return normalize(cos(END_EARTH_TILT) * endEclipticDirection(t) +
        sin(END_EARTH_TILT) * END_SKY_POLE);
}

#ifndef END_ORB_ENABLE
#define END_ORB_ENABLE 1
#endif

#if END_ORB_ENABLE
const float END_ORB_ANGULAR_RADIUS = 0.4581;
const vec3  END_ORB_GLOW_COLOR = vec3(1.0, 0.62, 0.2);
const float END_ORB_HALO_STRENGTH = 0.9;
const float END_ORB_AURA_STRENGTH = 1.5;
const float END_ORB_HALO_SIGMA = 0.014;
const float END_ORB_AURA_SIGMA = 0.087;
const float END_ORB_IOR = 1.46;
const float END_ORB_MILK = 0.008;
const float END_ORB_PLASMA = 3.5;
const float END_ORB_SMOKE = 0.0;
const float END_ORB_GLYPH_GLOW = 6.0;
const float END_ORB_RUNES = 18.0;
const float END_ORB_BAND_R = 1.035;
const float END_ORB_BAND_H = 0.17;
const int   END_ORB_VOLUME_STEPS = 32;
const float END_ORB_ANIM_RATE = 24.0;

// 光源球方向：2026-10-18 用户定档"末地正中央上空，只自转不公转" ⇒ 固定天顶。
// **六个方向副本（rgen/rmiss/rchit×2/water_caustics + 这里）必须同一向量**，
// 否则可见球与直射光方向分家。球内部的自转仍由 tDay/spinA 驱动（见 rmiss）。
vec3 endOrbDirection()
{
#if 0
    const float TWO_PI = 6.28318530718;
    float a = fract(cam.historyParams.w * 29.5) * TWO_PI;
    return vec3(0.5 * cos(a), 0.86602540378, 0.5 * sin(a));
#else
    return vec3(0.0, 1.0, 0.0);
#endif
}
#endif
const float END_SUN_ANGULAR_RADIUS = 0.0046250;
const float END_SUN_DIAMETER_DEGREES = 0.53;
const float END_SPACE_STRENGTH = 1.25;
const float END_STAR_STRENGTH = 0.15;
const vec3 END_GALAXY_LIGHT_COLOR = vec3(0.62, 0.72, 1.00);
const float END_GALAXY_AMBIENT_LIFT = 0.10;
const float END_EARTH_SRGB = 2.2;
const float END_NIGHT_BASE = 0.006;
const float END_NIGHT_LIGHTS_STRENGTH = 8.0;
const float END_NIGHT_POLAR_FADE = 0.92;
const vec3 END_EARTH_DAYLIGHT = vec3(1.35, 1.32, 1.27);
const float END_DAY_RAMP_TOP = 0.25;
const float END_CLOUD_ALBEDO = 0.80;
const float END_EARTHSHINE_STRENGTH = 0.03;
const float END_SUN_GLOW_SCALE = 0.0;

const float END_CLOUD_SPIN_MULTIPLIER = 1.15;
const float END_CLOUD_OPACITY = 0.90;
const float END_CLOUD_THRESHOLD_LO = 0.06;
const float END_CLOUD_THRESHOLD_HI = 0.71;

// 1 = 太阳完全可见；0 = 被地球完全挡住（月食）。只认**真实**太阳方向：
// 夜里 rmiss/rgen 会把方向翻到"月亮"，那不是日食的几何。
// earthDir 必须由调用方传 endEarthDirection() —— 地球现在是动的，不能当常量。
float endEarthEclipseFactor(vec3 sunDir, vec3 earthDir)
{
    if(abs(cam.weatherParams.z - 3.0) >= 0.25)
    {
        return 1.0;
    }
    float angle = acos(clamp(dot(sunDir, earthDir), -1.0, 1.0));
    return smoothstep(END_EARTH_ANGULAR_RADIUS - END_SUN_ANGULAR_RADIUS,
                      END_EARTH_ANGULAR_RADIUS + END_SUN_ANGULAR_RADIUS, angle);
}

// 地球被照亮的比例：0 = 全食（地球只有暗面朝我们）、1 = 满地。
float endEarthPhase(vec3 sunDir, vec3 earthDir)
{
    return 0.5 * (1.0 - dot(sunDir, earthDir));
}

const vec3 TWILIGHT_DAWN_TINT = vec3(1.00, 0.48, 0.24);
const vec3 TWILIGHT_DUSK_TINT = vec3(1.00, 0.45, 0.08);
const vec3 TWILIGHT_PREDAWN_TINT = vec3(0.62, 0.50, 0.86);

float twilightEveningWeight()
{
    const float TWO_PI = 6.28318530718;
    return smoothstep(0.20, -0.20, cos(fract(cam.historyParams.w) * TWO_PI));
}

// 拂晓窗口：上升侧、太阳高度角 -0.30..-0.02。
float twilightPredawnWeight(float sunElevation)
{
    return (1.0 - twilightEveningWeight()) *
        smoothstep(-0.30, -0.10, sunElevation) *
        (1.0 - smoothstep(-0.10, -0.02, sunElevation));
}

// 曙暮光色相：黄昏橘红、黎明橘粉、拂晓冷紫。
vec3 twilightPalette(float sunElevation)
{
    return mix(
        mix(TWILIGHT_DAWN_TINT, TWILIGHT_DUSK_TINT, twilightEveningWeight()),
        TWILIGHT_PREDAWN_TINT,
        twilightPredawnWeight(sunElevation)
    );
}


// =============================================================================
// 2026-11-13 夜间月光色阶（**单一真源**）
//
// 地形（rt_test.rgen）、云（volumetric_clouds_v3/v5）、体积光（volumetric_light）
// 四处必须用同一份，否则夜里"地偏银、云恒冷蓝"。
// 口径：月亮越高越接近银白，越低越偏深蓝（低仰角大气消光 + 浦肯野效应）。
// ⚠ 本函数**不得**引用 cam.sssParams 之类的"扩展字段"：cloud_bevy_preview.comp
//   用的是裁剪版 camera block，引用不存在的字段会让它编译失败。
// sunElevation 就是 sunDirection().y（0.819·sin(相位)），负值表示太阳在地平线下。
// =============================================================================
vec3 skyModelMoonColor(float sunElevation)
{
    float moonHeight = clamp(-sunElevation, 0.0, 1.0);

    return mix(
        vec3(0.44, 0.56, 1.00),
        vec3(0.74, 0.82, 1.00),
        smoothstep(0.0, 0.55, moonHeight));
}

// 月相调光系数（满月 1.0 / 新月 floor）。调用方只在夜间权重上乘它。
// ⚠ 同上：只有确认该 TU 的 camera block 含 sssParams 时才能调用。
float skyModelMoonPhaseDim(float moonPhase01, float floorValue)
{
    float illuminated = 0.5 + 0.5 * cos(6.28318530718 * clamp(moonPhase01, 0.0, 1.0));
    return mix(floorValue, 1.0, illuminated);
}

// =============================================================================
// 2026-11-19 直射光「太阳 → 月亮」交接带（**单一真源**）
//
// 病灶（用户报告："云在夕阳到夜晚的某一段时间光照会突然跳变"）：
//   旧口径把方向在 sunElevation >= 0 处硬翻（太阳 ↔ 真实月亮），而混色权重
//   dayAmount = smoothstep(-0.03, 0.03, s) 在翻转点只有 0.5 ⇒ 直射能量还剩
//   一半时就整束换了方向 + 换了颜色。满月时方向差 176°、上弦 86°；按 MC
//   20 分钟一天，这一段只有约 12 秒 ⇒ 云的亮度/前向散射/银边/云影方向同时跳。
//
// 修法：交接点下移到太阳沉到地平线下 0.05（≈2.9°），并让两侧能量在该点
//   **同时归零**：
//       太阳项 = smoothstep(-0.05, 0.06, s)      在 s = -0.05 时 = 0
//       月亮项 = 1 - smoothstep(-0.11, -0.05, s)  在 s = -0.05 时 = 0
//   方向/颜色的硬切换发生在"两侧能量都为 0"的那一点 ⇒ 画面上不可见；交接带
//   之外两侧各自连续。白天（s > 0.06）太阳项仍严格 = 1.0、深夜月亮项仍严格
//   = 1.0，所以两端观感与改动前逐位一致。
//
// ⚠ 消费者必须共用本组函数（各写一份必然漂移）：volumetric_clouds_v3/v5、
//   volumetric_light（光柱）、fog_screen_march（体积雾）。
//   地形 rt_test.rgen 不在此列 —— 那里地平线处的直射能量本来就随 |e| 归零，
//   硬翻发生在零能量点，且"太阳落山后云仍被照亮、地面已不受光"是物理正确的。
// =============================================================================
const float SKY_DIRECT_HANDOVER_ELEVATION = -0.05;

// 太阳那一份直射能量的权重。
float skyDirectSunAmount(float sunElevation)
{
    return smoothstep(SKY_DIRECT_HANDOVER_ELEVATION, 0.06, sunElevation);
}

// 月亮那一份直射能量的权重（含月出/月落的淡入淡出）。
float skyDirectMoonAmount(float sunElevation)
{
    return 1.0 - smoothstep(-0.11, SKY_DIRECT_HANDOVER_ELEVATION, sunElevation);
}

// 当前该用太阳还是月亮作直射方向。**只**在两侧权重都为 0 的那一点翻转。
bool skyDirectIsSun(float sunElevation)
{
    return sunElevation >= SKY_DIRECT_HANDOVER_ELEVATION;
}

// 阳光色核心（**白天路径本体**：不含天气缩放、不含夜蓝早退）。
// goldenStrength：日照金山混入强度（1.0 全档 / 0.5 云半档）。
// goldenStrength=1.0 时 mix(x,y,1.0) 逐位等于 y（0·x+y，IEEE 精确），与
// 旧直写 skySunGoldenTint(...) 语义一致。
vec3 skyModelSunColorDaylight(vec3 lightDirection, float goldenStrength)
{
#if SKY_SUN_TRANSMITTANCE_COLOR
    vec3 color = skySunPhysicalBaseColor(lightDirection.y);
#else
    float highSun = smoothstep(0.08, 0.68, max(lightDirection.y, 0.0));
    vec3 color = mix(vec3(1.0, 0.45, 0.18), vec3(1.0, 0.96, 0.90), highSun);
#endif
    float sunset = 1.0 - smoothstep(0.04, 0.22, lightDirection.y);
    vec3 twilightTint = twilightPalette(lightDirection.y);
    color = mix(color, twilightTint, sunset * 0.78);
#if SKY_DAY_PHASE
    // 2026-11-16 午后暖光：12.6°..43° 之间的色温偏移（正午 55° 不受影响）。
    // 必须在 goldenTint 之前 —— goldenTint 是贴地那一刻的强调，午后是它的
    // 前奏，顺序反了会让黄金时刻的饱和度被暖光乘子削弱。
    color = skySunAfternoonWarmth(color, lightDirection.y);
    color = mix(color, skySunGoldenTint(color, lightDirection.y), goldenStrength);
#endif
    return color;
}

// 2026-11-19：**连续版**阳光色（不判夜、不早退）。
//
// skyModelSunColor() 在 sunElevation < 0 处硬返回月蓝（2026-11-13 的修正）。
// 对"直射能量在地平线已经归零"的地形没问题；但云 / 体积光 / 雾的直射项在
// 地平线附近还有一段收尾斜坡（太阳仍在地平线下照亮云顶）。那一刻若颜色硬切，
// 画面上就是整片云/光晕突然换色 —— 用户报告的"夕阳到夜晚光照突然跳变"。
//
// 所以交接带里用本函数取"太阳侧"的连续色，交接点由调用方把能量压到 0。
vec3 skyModelSunColorContinuous(vec3 lightDirection, float goldenStrength)
{
    float rain = clamp(cam.weatherParams.x, 0.0, 1.0);
    float thunder = clamp(cam.weatherParams.y, 0.0, 1.0);
    vec3 weatherScale = mix(1.0, 0.35, rain) *
        mix(vec3(1.0), vec3(0.72, 0.78, 0.86), thunder);
    return skyModelSunColorDaylight(lightDirection, goldenStrength) * weatherScale;
}

vec3 skyModelSunColor(vec3 lightDirection, float goldenStrength)
{
    float rain = clamp(cam.weatherParams.x, 0.0, 1.0);
    float thunder = clamp(cam.weatherParams.y, 0.0, 1.0);
    vec3 weatherScale = mix(1.0, 0.35, rain) *
        mix(vec3(1.0), vec3(0.72, 0.78, 0.86), thunder);

    // =====================================================
    // 2026-11-13 夜间口径修正（用户：日落后到月出前，云的光照/颜色很怪）
    //
    // 病灶：本函数用 lightDirection.y 判暮色，但夜间的 lightDirection 已经被
    // getTimeDrivenSkyLightDirection() 翻成**月亮**（y 为正）。太阳刚落、月亮
    // 刚升起时 lightDirection.y ≈ 0..0.22 ⇒ sunset ≈ 1 ⇒ 月光被混进 78% 的
    // 黄昏橘红。而 rt_test.rgen 里那份副本有 elevation<0 的冷月蓝分支 ⇒
    // 地形冷蓝、云/体积光橘红，两边分裂。
    //
    // 修法：夜间直接给与 rgen 同值的冷月蓝（0.46,0.56,1.00）。
    // **白天路径逐位不变** —— 早退之后的所有表达式保持原样。
    // 暮色暖调只属于"太阳仍在地平线上"的直射光；日落之后的暖色应该来自
    // 天空散射（sky_atmosphere.comp 里的 twilightPalette），不是这一束平行光。
    // 回退：删掉下面这个 if 块即逐位回到改动前。
    // =====================================================
    // ⚠ 夜间判据必须与 getTimeDrivenSunElevation() 逐条对齐：那个函数在**末地**
    //   （weatherParams.z ≈ 3）恒返回 1.0（光源球固定天顶，注释原文：
    //   "昼夜翻转 / 暮色染色 / 月光档都不再参与末地"）。
    //   本函数 2026-11-13 的首版漏了这个特例 ⇒ 末地在 phase 下半段会被强制成
    //   冷月蓝。这里补上；非末地逐位不变（endDimension 恒 false）。
#if END_ORB_ENABLE
    const bool skyModelEndDimension =
        abs(cam.weatherParams.z - 3.0) < 0.25;
#else
    const bool skyModelEndDimension = false;
#endif

    float sunElevationNow =
        sin(fract(cam.historyParams.w) * 6.28318530718);

    if(!skyModelEndDimension && sunElevationNow < 0.0)
    {
        // 2026-11-13：与 rt_test.rgen 的 getTimeDrivenSkyLightColorRaw() 同口径
        //（云/体积光走这里，地形走那边，两份必须一致，否则夜里"云蓝地橙"）。
        //   ① 月亮越高越银白、越低越深蓝
        float moonHeightNow = clamp(-sunElevationNow, 0.0, 1.0);

        vec3 moonColor = skyModelMoonColor(sunElevationNow);

        // ⚠ 月相调光**只**在 rt_test.rgen 侧做。本头文件被 cloud_bevy_preview.comp
        // 包含，而它用的是**裁剪版 camera block**（没有 sssParams 字段），
        // 在这里引用 cam.sssParams 会让那个工具着色器编译失败
        //（实测："'sssParams' : no such field in structure"）。
        // 代价：云的月光不随相位变暗，地形的变暗 —— 已知的不一致，宁可如此。
        return moonColor * weatherScale;
    }

    return skyModelSunColorDaylight(lightDirection, goldenStrength) * weatherScale;
}

// ---------------------------------------------------------------------------
// 2026-10-21 雷暴频闪（整场脉冲）
//
// weatherParams.y（原版 thunder，[0,1]）门控：每个 ~0.6 s 间隔按哈希决定要不要
// 打一次闪，主闪 ~0.08 s 指数衰减，0.14 s 后一次较弱的回声。返回 [0,1]，乘上
// 各消费点自己的颜色/强度就是一次天光闪。
//
// 时钟用 cam.frameCount：没有新增 uniform，且同一帧里天空 LUT / 云 / 视线天空
// 读的是同一个值 ⇒ 三处同步同一次闪。按 60 Hz 折算（帧率翻了闪频跟着翻，可接受）。
// 非雷暴（thunder <= 0）逐位返回 0 ⇒ 晴天/普通雨天画面的每一步都不受影响。
// ---------------------------------------------------------------------------
float lightningFlashEnvelope()
{
    float thunder = clamp(cam.weatherParams.y, 0.0, 1.0);
    if(thunder <= 0.001)
    {
        return 0.0;
    }
    float t = float(cam.frameCount) * (1.0 / 60.0);
    float scaled = t * 1.6;
    float cell = floor(scaled);
    float f = fract(scaled);
    float h = fract(sin(cell * 12.9898 + 4.1414) * 43758.5453);
    float gate = step(0.62, h);
    float a = exp(-f * 42.0);
    float b = 0.55 * exp(-max(f - 0.14, 0.0) * 55.0);
    return gate * (a + b) * thunder;
}

#endif
