
        
// =====================================================================
// DH 远场几何（stage B, 2026-09-23）
// =====================================================================
// 2026-11-19 rasterLod（mode=14）新增：本项目 DH LOD 投影 compute pass
// （dh_lod_raster.comp）的输出图，写入方 = Frame.cpp 在 RT 主派发前录制的
// recordDhLodRasterPass（每 DH 节一次 dispatch）。仅 mode=14 动态访问；
// 其余 mode 不引用（与 binding 94 同一约定：共享 layout 先声明，描述符由
// createDhLodRasterResources 按需写入，旗标关闭时不写、着色器不访问）。
//   binding 96 = RGBA8 color（顶点色，display 编码，与 DH 图同一色彩约定）
//   binding 97 = R32F depth（clip.z/w，Vulkan [0,1]；0.0 = 无几何）
// 布局约定与 DH 图同一套：像素 y 按 DH 翻转写入 ⇒ 采样沿用 dhFarTextureUv
// 的翻转 + 缩放映射，mode=13 的方向采样也天然一致，dhFarTextureUv 无需改动。
// =====================================================================
layout(set = 0, binding = 96, rgba8) readonly uniform image2D dhLodRasterColorImage;
layout(set = 0, binding = 97, r32f) readonly uniform image2D dhLodRasterDepthImage;
layout(set = 0, binding = 101, rgba8) readonly uniform image2D orionLodNormalMatImage;

// rasterLod 模式判据（mode=14 只会在 Java 侧 rasterLod=true 时被推送，
// 所以它本身就是旗标的运行时载体，无需额外 UBO 位）。
bool dhLodRasterModeActive()
{
    // 2026-11-19 mode=15 四联诊断：诊断档也吃本项目光栅深度（象限 1/2/5/6
    // 只消费几何链不吃颜色），否则 rasterLod 下诊断档只能看到 DH 贴图深度。
    // 安全不变量与 mode=14 相同：Java 只在 RASTER_LOD=true 时推 15
    //（DhFarFieldGeometry 推送门 + legacy 路径 lane[16] 钳零），
    // ⇒ mode∈{14,15} ⇒ 描述符已写，不存在未绑定读取。
    const int m = int(cam.dhFarFieldLight.x + 0.5);
    return m == 14 || m == 15;
}

// =====================================================================
// Orion 内置自研远景 LOD（mode=16，不依赖外部 DH/Voxy）
// 硬件光栅 RenderPass 已直接以 Vulkan 屏上坐标（像素 y=0 在顶）写入
// binding 96 / 97 / 101，因此这里直接按 pixelCoord 做 1:1 imageLoad，无需翻转 Y。
// =====================================================================
bool orionBuiltinLodModeActive()
{
    return int(cam.dhFarFieldLight.x + 0.5) == 16;
}

ivec2 orionLodClampCoord(ivec2 pixelCoord, ivec2 imgSize)
{
    ivec2 outSize = max(imageSize(image), ivec2(1));
    if (imgSize == outSize) {
        return clamp(pixelCoord, ivec2(0), max(imgSize - ivec2(1), ivec2(0)));
    }
    vec2 uv = (vec2(pixelCoord) + 0.5) / vec2(outSize);
    return clamp(ivec2(uv * vec2(max(imgSize, ivec2(1)))),
                 ivec2(0), max(imgSize - ivec2(1), ivec2(0)));
}

float orionLodLinearDistAt(ivec2 pixelCoord)
{
    ivec2 sz = imageSize(dhLodRasterDepthImage);
    if (sz.x <= 1 || sz.y <= 1) return 0.0;
    return imageLoad(dhLodRasterDepthImage, orionLodClampCoord(pixelCoord, sz)).r;
}

vec4 orionLodColorAoAt(ivec2 pixelCoord)
{
    ivec2 sz = imageSize(dhLodRasterColorImage);
    if (sz.x <= 1 || sz.y <= 1) return vec4(0.0, 0.0, 0.0, 1.0);
    return imageLoad(dhLodRasterColorImage, orionLodClampCoord(pixelCoord, sz));
}

vec4 orionLodNormalMatAt(ivec2 pixelCoord)
{
    ivec2 sz = imageSize(orionLodNormalMatImage);
    if (sz.x <= 1 || sz.y <= 1) return vec4(0.5, 0.5, 0.0, 0.0);
    return imageLoad(orionLodNormalMatImage, orionLodClampCoord(pixelCoord, sz));
}

vec3 orionDecodeOctNormal(vec2 e01)
{
    vec2 f = e01 * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    float lenSq = dot(n, n);
    return (lenSq > 1.0e-8) ? n * inversesqrt(lenSq) : vec3(0.0, 1.0, 0.0);
}

vec3 orionLodWaterWaveNormal(vec2 worldXZ, float dist, float timeSec)
{
    float fade = (1.0 - smoothstep(100.0, 850.0, dist)) * 0.065;
    if (fade <= 1.0e-4) {
        return vec3(0.0, 1.0, 0.0);
    }
    float p1 = dot(worldXZ, vec2(0.37, 0.23)) + timeSec * 1.45;
    float p2 = dot(worldXZ, vec2(-0.19, 0.41)) - timeSec * 1.15;
    float p3 = dot(worldXZ, vec2(0.61, -0.53)) + timeSec * 1.95;
    float dx = (cos(p1) * 0.37 - cos(p2) * 0.19 + cos(p3) * 0.30) * fade;
    float dz = (cos(p1) * 0.23 + cos(p2) * 0.41 - cos(p3) * 0.26) * fade;
    return normalize(vec3(-dx, 1.0, -dz));
}

// 前向声明：本文件的助手函数在 dhFarTextureUv 定义之前就要用它
//（同文件里 dhFarPrimaryRay 的前向声明同一先例）。
vec2 dhFarTextureUv(ivec2 pixelCoord);

// 本项目光栅 depth：与 dhFarDepthAt 同一套 texel 映射（dhFarTextureUv 翻转 +
// 缩放到本项目图尺寸），但读 storage image（imageLoad，永不混合）。几何判据
// 与 DH 版同构：depth > 0（未覆盖像素由宿主每帧清零）。
bool dhLodRasterDepthIsGeometry(float depthValue)
{
    return depthValue > 0.0 && depthValue <= 1.0;
}

float dhLodRasterDepthAt(ivec2 pixelCoord)
{
    ivec2 outputSize = imageSize(image);
    if (any(lessThan(pixelCoord, ivec2(0)))
        || any(greaterThanEqual(pixelCoord, outputSize))) return 0.0;
    vec2 uv = dhFarTextureUv(pixelCoord);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return 0.0;
    ivec2 rasterSize = imageSize(dhLodRasterDepthImage);
    ivec2 rasterPixel = clamp(ivec2(uv * vec2(rasterSize)),
                              ivec2(0), rasterSize - ivec2(1));
    return imageLoad(dhLodRasterDepthImage, rasterPixel).r;
}

vec3 dhLodRasterColorAt(ivec2 pixelCoord)
{
    ivec2 outputSize = imageSize(image);
    if (any(lessThan(pixelCoord, ivec2(0)))
        || any(greaterThanEqual(pixelCoord, outputSize))) return vec3(0.0);
    vec2 uv = dhFarTextureUv(pixelCoord);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return vec3(0.0);
    ivec2 rasterSize = imageSize(dhLodRasterColorImage);
    ivec2 rasterPixel = clamp(ivec2(uv * vec2(rasterSize)),
                              ivec2(0), rasterSize - ivec2(1));
    return imageLoad(dhLodRasterColorImage, rasterPixel).rgb;
}

// =====================================================
// stage-1 只把 DH 的 color 贴过来，远场的几何与光照全是 DH 的。stage B 要的是
// 规划 §7.3 的「图像定色 + 几何定光」：color 仍然由 DH 给（它是唯一知道远景长什么
// 样的东西），但**光**换成我们的 —— 这需要先把 DH 的 depth 变回世界几何。
//
// 反投影：world = dhInverseMvmProjectionMatrix × vec4(ndc, depth, 1)，再除以 w。
// DH 的 depth 是 reverse-Z / ZERO_TO_POS_ONE，所以 z 直接就是 clip 空间的 z。
//
// ⚠ 两个方向上的约定**不在这里假设**：
//   1. 矩阵的 16 个 float 是行序还是列序；
//   2. NDC 的 y 指向（DH 的图相对本 pass 是上下翻转的，见合成块注释）。
// 组合起来四个变体，由调用点用一条判据现场自选：**重建出来的点必须落在本像素的
// 主射线上**（误差 = 1 - cos(重建方向, 主射线方向)）。这一条同时验证了上面两点、
// 翻转方向，以及"DH 用的就是这台相机"。自检结果在 mode=1 下直接可视化。
//
// 纹理坐标与屏幕坐标的对应关系只有一处不同：DH 的图上下翻转，所以"取纹素"要走
// dhFarTextureUv，"反投影"要走未翻转的屏幕归一化坐标（DH 的图就是同一台相机的
// 同一幅画面）。color 与 depth 必须共用同一个 uv，否则判据与取色会错位一整屏。
// =====================================================

vec2 dhFarScreenUv(ivec2 pixelCoord)
{
    return (vec2(pixelCoord) + 0.5) / vec2(imageSize(image));
}

// 前向声明：dhFarSampleUv 要用它，而它的定义在下面（那里有完整推导）。
void dhFarPrimaryRay(ivec2 pixelCoord, out vec3 cameraPos, out vec3 rayDir);

// =====================================================
// dhFarSampleUv：DH 图的采样 uv（mode 13 = 方向采样 / 其余 = 屏幕直通）
// =====================================================
// 这是**唯一**的采样入口 —— 颜色、深度掩码、法线邻居全都经 dhFarTextureUv
// 转发到这里，所以在 mode 13 下改走方向，整张 DH 图的重投影就是一致的
// （深度与颜色不可能各自错位）。
//
// mode 13（-Dorion.dh.farLight.mode=13）：把本像素的主射线**方向**穿 DH
// 自己的投影矩阵：
//     clip = dhFarFieldMvp * vec4(rayDir, 0.0)      // w = 0 ⇒ 平移列不起作用
//     uv   = clip.xy / clip.w * 0.5 + 0.5
// 它不再假设"DH 用的相机就是本 pass 这台相机"，因此对 FOV / 朝向 / 世界重定基
// 免疫。上下翻转保留（DH 的图相对本 pass 是翻转的，与直通路径同一约定）。
//
// 有效性自证：真实透视投影的 m00 恒非零（= 1/(tan(fovY/2)·aspect)），所以宿主
// 还没推矩阵时这里是全零 ⇒ 自动退回屏幕直通，不需要额外车道。
//
// uv 越界 ⇒ 该方向在 DH 图里没有内容，由 dhFarDepthAt 判成"无几何"。方向保守：
// 宁可少接管，也不要在视锥边缘凭空补几何出来。
vec2 dhFarSampleUv(ivec2 pixelCoord)
{
    int modeRequested = int(cam.dhFarFieldLight.x + 0.5);

    if (modeRequested != 13 || cam.dhFarFieldMvp[0][0] == 0.0) {
        // 屏幕直通（历史路径）：1.0 - uv.y 给出的就是镜像纹素中心
        // (H-1-y+0.5)/H ≡ 1 - (y+0.5)/H，没有半个纹素的偏差。
        vec2 uv = dhFarScreenUv(pixelCoord);
        uv.y = 1.0 - uv.y;
        return uv;
    }

    vec3 cameraPos = vec3(0.0);
    vec3 rayDir = vec3(0.0, 0.0, -1.0);
    dhFarPrimaryRay(pixelCoord, cameraPos, rayDir);

    vec4 clip = cam.dhFarFieldMvp * vec4(rayDir, 0.0);
    if (abs(clip.w) < 1.0e-9) {
        return vec2(-1.0);            // 退化 ⇒ 判越界 ⇒ 无几何
    }
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

vec2 dhFarTextureUv(ivec2 pixelCoord)
{
    return dhFarSampleUv(pixelCoord);
}

// =====================================================
// dhFarDepthAt：深度必须按【纹素整数坐标】取，绝不能用 texture() 线性过滤取
// =====================================================
// depth 在这里是“该纹素有没有 DH 几何”的**布尔掩码**，不是可以按比例混合的量。
// 用 texture() 取（DH 的 sampler 是 LINEAR）会把“有几何”的深度和清屏值 0 插值成
// 一片中间值，而中间值照样落在 dhFarDepthIsGeometry 的通过区间 ⇒ 几何轮廓外沿会
// 多出一圈“半几何”像素：一侧取到地形色、一侧取到 DH 天空色，混在同一条细带上，
// 屏幕上看就是**随相机移动的异色细线**。texelFetch 精确命中纹素，永不混合。
// 越界（pixelCoord 在屏幕外）一律返回 0.0 =“无几何”，方向保守：宁可少接管，
// 也不要在屏幕边缘凭空补一圈几何出来。
float dhFarDepthAt(ivec2 pixelCoord)
{
    // 2026-11-19 rasterLod（mode=14）：深度源换成本项目光栅 pass 的 R32F 图
    // （binding 97）。texel 映射与 DH 版同一套（dhFarTextureUv 翻转 + 缩放），
    // 上游模式自选/邻居/法线机器全部复用。rasterLod 关闭时 mode 永远不会是
    // 14，本分支不执行，与改前逐位一致。
    if (dhLodRasterModeActive()) {
        return dhLodRasterDepthAt(pixelCoord);
    }

    ivec2 outputSize = imageSize(image);
    if (any(lessThan(pixelCoord, ivec2(0)))
        || any(greaterThanEqual(pixelCoord, outputSize))) return 0.0;
    vec2 uv = dhFarTextureUv(pixelCoord);
    // mode 13（方向采样）下 uv 可能落到图外（DH 的视锥比我们的窄，或方向在
    // 背面）⇒ 必须判成"无几何"。少了这一条，下面的 clamp 会把图外方向全部
    // 拉到边缘纹素上，在视锥边缘糊出一圈假的 DH 地形。
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return 0.0;
    ivec2 depthSize = textureSize(dhFarDepth, 0);
    ivec2 depthPixel = clamp(ivec2(uv * vec2(depthSize)),
                              ivec2(0), depthSize - ivec2(1));
    return texelFetch(dhFarDepth, depthPixel, 0).r;
}

// DH 的 depth 是 reverse-Z / ZERO_TO_POS_ONE ⇒ 清屏值 0.0（远）、被几何遮住落在 (0,1]。
// 与 DH 自己的 apply shader 判据同构（它写的是 drawnTo = (fragmentDepth != 0.0)）。
//
// 09-23 第五次定案（gpt6 提交）：**上下界都删掉**，直接对齐 DH 自己的判据。
// 前几轮加 0.998 / 0.9999 的动机是“排掉 DH 近裁剪空档”，但那个空档在 reverse-Z
// 下落在**下界**一侧（清屏值 0.0），上界根本不承担这个职责：reverse-Z 的 0.9999
// 换算回去只有 z ≈ near（本机 DH near ≈ 10.7 格 ⇒ 只有 z<10.70 格才会被判“没画”），
// 等于把“贴着近平面的合法几何”判成“没画”，在屏幕上剪出一道边界；下界 0.0001 同理。
// 于是只剩 DH 自己那一条：fragmentDepth != 0（NaN/Inf 因严格比较恒假而自然排除）。
bool dhFarDepthIsGeometry(float depthValue)
{
    return depthValue > 0.0 && depthValue <= 1.0;
}

// 非有限值（NaN/Inf）统一判据。
//
// C++ 侧已经把非有限矩阵挡在 UBO 之外（updateDhFarField 的 std::isfinite），这里再
// 挡一次，因为**距离是标量**：一旦是 NaN，位置、法线、RR guide 会一起变 NaN，而 NaN
// 参与的比较恒假 ⇒ 后面每一级回落都不成立。所以必须显式判断，不能靠"大于/小于"。
bool dhFarFinite(float value)
{
    return !(isnan(value) || isinf(value));
}

// 像素 → (相机位置, 世界主射线方向)。与 main 开头逐字一致：
//   inUV = (pixel + 0.5) / size ；d = inUV * 2 - 1 ；d.y = -d.y
//   target = projInverse * (d, 1, 1) ；rayDir = normalize(viewInverse * (normalize(target), 0))
// 所以这里给出的就是这个像素真正的主射线（差别只有 DLSS 的亚像素 jitter，量级 1e-7）。
void dhFarPrimaryRay(ivec2 pixelCoord, out vec3 cameraPos, out vec3 rayDir)
{
    cameraPos = (cam.viewInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    vec2 uv = dhFarScreenUv(pixelCoord);
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    vec4 target = cam.projInverse * vec4(ndc, 1.0, 1.0);
    vec3 viewTarget = abs(target.w) > 1.0e-6 ? target.xyz / target.w : target.xyz;
    float viewLength = length(viewTarget);

    rayDir = viewLength > 1.0e-6
        ? normalize((cam.viewInverse * vec4(viewTarget / viewLength, 0.0)).xyz)
        : vec3(0.0, 0.0, -1.0);
}

// variant 位 0 = NDC 的 y 取反；位 1 = 上传的矩阵需要转置。
vec3 dhFarNdc(ivec2 pixelCoord, float depthValue, int variant)
{
    vec2 uv = dhFarScreenUv(pixelCoord);
    float ndcY = (((variant & 1) == 0) ? 1.0 : -1.0) * (1.0 - 2.0 * uv.y);
    return vec3(uv.x * 2.0 - 1.0, ndcY, depthValue);
}

// 反投影一次。注意 **w 取绝对值再除**：某些约定下（矩阵本身是对的、但 z 的
// 正/反与矩阵不符）w 会变号，除以负 w 会把点镜像到相机背后 —— 那只影响"落在射线
// 哪一侧"，由调用方（距离只取沿射线分量）和变体自检一起处理，这里不做判断。
vec3 dhFarWorldPoint(ivec2 pixelCoord, float depthValue, int variant)
{
    mat4 inverseMvp = ((variant & 2) == 0)
        ? cam.dhFarFieldInvMvp
        : transpose(cam.dhFarFieldInvMvp);
    vec4 homogeneous = inverseMvp * vec4(dhFarNdc(pixelCoord, depthValue, variant), 1.0);
    return abs(homogeneous.w) > 1.0e-6
        ? homogeneous.xyz / homogeneous.w
        : homogeneous.xyz;
}

// =====================================================
// 深度 → 沿主射线的距离（09-23 晚定案：解析式升为第一优先）
// =====================================================
// ⭐⭐ "微缩模型"定案（09-23 20:4x，用户截图 + DH jar 反汇编）：
//   Java 侧曾把 DH 的 nearClipPlane（**视距占比** 0..1，实测 0.891323）
//   当方块值下发 ⇒ lanesUsable 误判可用 ⇒ 解析式
//   z = 0.891×9216/(0.891 + d×9215) 把真实 200~3000 格的地形全部压到
//   相机周围 6~107 格 ⇒ 整张 DH 全图渲染塌缩成一个"随玩家移动的微缩模型"
//   （用户原话：每个地图都有一个微缩模型、永远无法到达、复刻地图的一切）。
//   Java 侧现已改为下发 RenderUtil.getNearClipPlaneInBlocks() 的方块值
//   （与 DH 自己的 SSAO 对同一张深度缓冲用的值逐字相同）。
//
// 优先级（lane 正确时解析式必对 —— 它没有任何约定可猜）：
//   0 = 解析 reverse-Z：z = n·f / (n + d·(f−n))；d=0 ⇒ far、d=1 ⇒ near
//       （DH API 自报 ZERO_TO_POS_ONE + REVERSE_Z；与 DH 的深度语义一致）
//   1 = 逆 MVP 直接给的距离
//   2 = z 取镜像 1-z（矩阵没问题，但采样到的 z 语义与矩阵不符）
//   3 = 点到相机距离（最后兜底，仍好过刷色块）
//
// 只取"沿射线分量" dot(P - camPos, rayDir)：坐标系若真存在平移，它的垂直分量在
// 这一步被彻底投影掉，而沿射线分量正是深度要说的事。
float dhFarDistanceFor(
    ivec2 pixelCoord,
    float depthValue,
    vec3 cameraPos,
    vec3 rayDir,
    int variant,
    float nearClip,
    float farClip,
    out int source
)
{
    // near/far 这两个 lane 由 Java 每帧从投影矩阵反解成方块后写进来。
    // ⭐ 单位护栏 nearClip > 0.0（不再是 >= 1.0）：DH 渲染深度图的真实 near 实测
    //    只有 0.891 格（`setDhProjectionMatrix` 里 near 被压到 min(near,7.5) 后
    //    再经 FOV/aspect 修正，最终 < 1）。旧护栏 `>= 1.0` 会把反解出的正确 near
    //    判废、回落到反射的 28.5 格（错误值），解析式比例错 32 倍 = 微缩模型。
    //    反解值直接来自投影矩阵 m22/m23，就是真实 near/far 编码，无"占比"歧义。
    bool lanesUsable = nearClip > 0.0 && farClip > nearClip * 4.0 + 1.0;
    float lowLimit = lanesUsable ? nearClip * 0.5 : 0.01;
    float highLimit = lanesUsable ? farClip * 1.5 : 1.0e12;

    if (lanesUsable) {
        // 解析 reverse-Z：DH 的深度就是用它自己的 near/far 渲出来的，
        // 这一步是精确逆变换，不依赖任何矩阵约定。
        float analyticDistance = nearClip * farClip
            / max(nearClip + depthValue * (farClip - nearClip), 1.0e-3);

        if (dhFarFinite(analyticDistance)
            && analyticDistance > lowLimit
            && analyticDistance < highLimit) {
            source = 0;
            return analyticDistance;
        }
    }

    vec3 matrixPoint = dhFarWorldPoint(pixelCoord, depthValue, variant);
    float matrixDistance = dot(matrixPoint - cameraPos, rayDir);

    if (dhFarFinite(matrixDistance)
        && matrixDistance > lowLimit
        && matrixDistance < highLimit) {
        source = 1;
        return matrixDistance;
    }

    vec3 mirrorPoint = dhFarWorldPoint(pixelCoord, 1.0 - depthValue, variant);
    float mirrorDistance = dot(mirrorPoint - cameraPos, rayDir);

    if (dhFarFinite(mirrorDistance)
        && mirrorDistance > lowLimit
        && mirrorDistance < highLimit) {
        source = 2;
        return mirrorDistance;
    }

    // 最后一级（lane 无效 + 矩阵也失灵）：退到"点到相机的距离"。
    source = 3;

    float fallbackLength = length(matrixPoint - cameraPos);

    if (!dhFarFinite(fallbackLength) || fallbackLength < 0.01) {
        fallbackLength = length(mirrorPoint - cameraPos);
    }

    if (!dhFarFinite(fallbackLength) || fallbackLength < 0.01) {
        fallbackLength = 1.0;
    }

    return fallbackLength;
}

// =====================================================
// 邻居世界点（法线用）：带"与中心同面"的有效性判据
// =====================================================
// DH 的 depth 图在**轮廓处**会有一个纹素级的巨大跳变（前景山脊压在远山上）。老写法
// 只检查"邻居是不是几何"，于是跨轮廓时把两个相差上千格的点叉在一起，法线在那一条
// 像素宽的山脊上乱转 —— 表现为远山轮廓上一圈黑边/亮边，也让重打光在轮廓上出现
// 一像素宽的亮缝。
//
// 判据：邻居必须与中心**深度相近**（相对差 ≤ 12%）。reverse-Z 下"相近"就是"同一个
// 面"的稳健代理。邻居不可用 ⇒ 返回 false，调用方换反方向再试（-x / -y），两个方向
// 都不可用才退回 up（天顶法线）。
bool dhFarNeighborPoint(
    ivec2 pixelCoord,
    ivec2 offset,
    float centerDepth,
    int variant,
    float nearClip,
    float farClip,
    out vec3 point
)
{
    ivec2 neighborCoord = pixelCoord + offset;
    float neighborDepth = dhFarDepthAt(neighborCoord);

    if (!dhFarDepthIsGeometry(neighborDepth)) {
        return false;
    }

    float depthScale = max(max(abs(centerDepth), abs(neighborDepth)), 1.0e-6);

    if (abs(neighborDepth - centerDepth) / depthScale > 0.12) {
        return false;
    }

    // 邻居必须走**邻居自己的射线**：像素差 1 格，射线方向差是屏幕级的小量，
    // 但深度跨度可能上千格，用中心射线会把这个小量放大成可见的坡度误差。
    vec3 neighborCameraPos;
    vec3 neighborRayDir;
    dhFarPrimaryRay(neighborCoord, neighborCameraPos, neighborRayDir);

    int neighborSource = 2;
    float neighborDistance = dhFarDistanceFor(
        neighborCoord,
        neighborDepth,
        neighborCameraPos,
        neighborRayDir,
        variant,
        nearClip,
        farClip,
        neighborSource
    );

    point = neighborCameraPos + neighborRayDir * neighborDistance;
    return true;
}

// ============================================================================
// 2026-10-24 半分辨率 GI 的编译期口径（只有 _deploy_stale_spv.py 里的
// rt_test_gi_half 目标定义 GI_HALFRES=1；其余变体走下面的 0）。
// ============================================================================
#ifndef GI_HALFRES
#define GI_HALFRES 0
#endif

// =====================================================
// 2026-10-26 稀疏渲染活跃判定（设置项「稀疏渲染采样率%」）。
//
// 语义直接照 RTX Remix 的 per_pixel_rate.slangh：
//   isActivePixelFromSamplingRate = whiteNoise(pixel, frameIdx, 0) <= rate
// 抽签只决定 **radiance 段是否着色**；depth / motion vector / RR guides 仍然
// 每像素每帧无条件写（与 10 月的棋盘格子半分辨率不同：那条跳过整像素，
// 导致 guides/MV 陈旧，这正是它被证伪的原因）。
//
// rate 以百分数编码在 cam.sparseParams.y（0 = 关，100 = 全采样）。抽签用
// Remix 的 uintHash 白噪声，避免棋盘/蓝噪声平铺在动态画面上形成固定纹理。
// SHARC_UPDATE 与 GI_HALFRES 变体不门控（前者由 SHaRC 侧负责，后者的
// 半分辨率上采样依赖自己的一套子像素调度）。
// =====================================================
// 噪声源与 Remix 默认一致：BlueNoise128x128x64x8。
// 判定式照 per_pixel_rate.slangh: threshold <= samplingRate。
// 2026-10-27：默认改读 sparse_active_mask.comp 产出的掩码（Remix 的
// isActivePixelFromMask），保证本 pass 与下游消费者用同一份判定。
// A/B 回退：-DSPARSE_USE_MASK=0 回到本 pass 内直算蓝噪声阈值。
#ifndef SPARSE_USE_MASK
#define SPARSE_USE_MASK 1
#endif

// =====================================================
// 2026-11-02 块内分层选点（与 sparse_active_mask.comp 的同名实现逐行一致）。
// 详见那个 comp 顶部的长注释：8×8 块 = 64 槽位，块内每帧恰好 k 个、
// 一 cycle 内每像素恰好 k 次，bit-reverse 把连续弧打散。
// 这里只服务 SPARSE_USE_MASK=0 的回退路径（掩码 pass 不可用时）。
// =====================================================
// 2026-11-02 实测回退：默认 0（走蓝噪声）。bit-reverse 版本出竖条纹，原因见
// sparse_active_mask.comp 同名宏处的注释。
#ifndef SPARSE_LAYERED_PATTERN
#define SPARSE_LAYERED_PATTERN 0
#endif

const uint kVptSparseTileSize = 8u;
