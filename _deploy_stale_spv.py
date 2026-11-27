# -*- coding: utf-8 -*-
"""编译/部署 RT 着色器 SPV（内容 + 编译口径指纹判据，替代旧的 mtime 判据）。

为什么不能再用 mtime（旧脚本踩过的坑，2026-11-17 重写）
------------------------------------------------------------------
旧版判据是"源码（含其 include 闭包）最新 mtime > spv mtime 才算脏"。mtime
不可信，因为它只记录"最后一次写盘时间"，不记录"写进去的是什么内容"，下面
两类漏网它都抓不到：

  1. **内容来自别的源状态**：源码被回滚 / 手工改回 / 从备份拷回时，mtime 会被
     刷新成"比 spv 新"或"比 spv 旧"，但 spv 里跑的可能根本不是当前源码 ——
     历史上 rt_test.rgen.spv 就出现过"mtime 比源码新、却和当前源码编出来的
     产物不一致"的情况，一整轮诊断跑在旧代码上。
  2. **编译口径变了源码没变**：宏清单（BASE_FLAGS / RT_COUNTERS_FLAGS /
     RMISS_SLIM_FLAGS / 各变体 EXTRA_FLAGS）改了，但源码一个字没动 ⇒ mtime
     两边都不变，旧判据判"不脏"，于是带着旧宏的 spv 继续部署。2026-11 还
     出现过 rt_test_translucent.rchit.spv 被 mtime 判据跳过、部署出无
     Binding 93 的关闭档、evGlass 计数器假零的事故。

新判据（指纹清单 `cpp/shaders/.deploy_fingerprint.json`）
------------------------------------------------------------------
对每个 spv 目标记录一份指纹：
  · source   : 产物对应的源文件；
  · flags    : 本次编译的**完整宏列表**（BASE_FLAGS + RT_COUNTERS_FLAGS +
               RMISS_SLIM_FLAGS + 该变体 EXTRA_FLAGS，顺序固定）；
  · sources  : 源文件 + 递归 include 闭包内每个文件的内容 MD5；
  · spv      : cpp/shaders 下该 spv 产物本身的 MD5。

清单与当前计算结果**逐项不一致**才重编；一致则跳过。宏值改动、源码改动、
include 改动、产物被回滚 / 拷错，任一变化都会体现在指纹里，不再依赖 mtime。

部署后反汇编校验（spirv-dis，位置参数，不支持 -f）
------------------------------------------------------------------
校验**永远不空跑**（2026-11-17 修过一次假阳性：旧版关档把整段校验
`if COUNTERS_ENABLED:` 包住静默跳过，汇总却照样打"校验错误 0"，读起来像
校验通过，实际一项没查）。现在按本次模式双向断言：

  · 计数档（ORION_RT_STAGE_COUNTERS=1）：凡源码闭包引用 rt_stage_counters.glsl
    的 spv，反汇编必须出现 Binding 93 装饰，且计数器缓冲上的原子写数量达到
    该 TU 刷新函数的下限；缺失即报错退出（非 0）。
  · 关闭档（未设计数器宏）：同一批 TU 反汇编必须**不出现** Binding 93；
    出现即说明计数器宏漏裁 / 串档，非 0 退出。（sharc 诊断计数、perf 统计等
    其它 SSBO 的原子写与 Binding 93 无关，不计入本判据。）
  · 若本次带 ORION_RMISS_SLIM=1：rt_test.rmiss.spv 的哈希必须不同于 P4 关档
    基线；相同即报错（说明瘦身宏没进编译口径）。
  · 三处部署目录（run/shaders、src/main/resources/shaders、
    build/resources/main/shaders）逐文件哈希必须一致；不一致报错退出。

汇总行写明实际校验了多少项（计数档 N 项 / 关档 M 项 / 失败 E），不再出现
"什么都没查却打印 0 错误"的歧义。

用法：
  python _deploy_stale_spv.py            # 按指纹增量重编 + 部署 + 校验
  python _deploy_stale_spv.py --force    # 无视指纹，全部目标强制重编
"""

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = 'cpp/shaders'
RUN = 'run/shaders'
GLSLANG = r'C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe'
SPIRV_DIS = r'C:\VulkanSDK\1.4.309.0\Bin\spirv-dis.exe'
# 指纹清单固定位置（仓库内，随部署更新；不要手改）。
FINGERPRINT_FILE = os.path.join(ROOT, '.deploy_fingerprint.json')

# 三处部署目录：运行时唯一读取位 / 随包资源 / 开发态 mod root。
# 只写 run/ 的话，NativeVulkan.extractBundledShaders() 下次启动会用
# Files.mismatch 把 mod root 旧档刷回去（2026-10-26 的教训）。
DEPLOY_DIRS = [RUN, 'src/main/resources/shaders', 'build/resources/main/shaders']

# 规范编译口径（2026-10-21 起）：生产档（材质在命中着色器里求值）
# + 地形 32B 顶点。两者都是**编译期**决定，漏一个就会静默换档 / 换顶点 ABI。
# 与 tools/audit_spv_freshness.py 的 BASE_FLAGS 必须逐项一致。
BASE_FLAGS = ['-DRT_BUILD_PRODUCTION_MODE', '-DSECTION_VERTEX_32B=1']

# 2026-11-16：RT 阶段三计数器编译期总闸。与 C++ 侧 getenv 同名；缺省（未设 / 0 / f）
# 不追加宏 ⇒ 着色器整段裁掉，编出的 spv 与基线逐位可比。采集时置
# ORION_RT_STAGE_COUNTERS=1 再跑本脚本，才会把 -DORION_RT_STAGE_COUNTERS=1 带进去。
_RT_COUNTERS_ENV = os.environ.get('ORION_RT_STAGE_COUNTERS')
RT_COUNTERS_FLAGS = (
    ['-DORION_RT_STAGE_COUNTERS=1']
    if _RT_COUNTERS_ENV and _RT_COUNTERS_ENV not in ('0', 'f', 'F', '')
    else []
)
COUNTERS_ENABLED = bool(RT_COUNTERS_FLAGS)

# 2026-11-24：默认启用 P4 miss 路径瘦身（跳过未命中光线对 payload.emission 的死写回）；
# 如需回到旧基线口径，可显式置 ORION_RMISS_SLIM=0 再跑本脚本。
_RMISS_SLIM_ENV = os.environ.get('ORION_RMISS_SLIM')
RMISS_SLIM_FLAGS = (
    []
    if _RMISS_SLIM_ENV in ('0', 'f', 'F')
    else ['-DORION_RMISS_SLIM=1']
)
RMISS_SLIM_ENABLED = bool(RMISS_SLIM_FLAGS)

# P4 关档基线 rt_test.rmiss.spv 的 MD5（ORION_RMISS_SLIM 未设时的现役产物）。
# 开瘦身宏后产物必须与它不同；相同说明宏没进编译口径，属事故档。
BASELINE_RMISS_OFF_MD5 = 'bca51df9e6af4202b13db153663055ab'

# 2026-11-02：玻璃"解析半侧"的拆分是跨 TU 的 —— rgen 把确定性反射从 RR 输入里
# 减掉，显示变换再把旁路图加回来。三个 TU 必须同一个宏值，否则那部分能量直接消失
# （玻璃反射全黑）或重复计入。rgen 侧的定义在 rt_test.rgen；这两个 comp 靠这里传。
EXTRA_FLAGS = {
    'rr_sdr_post.comp.spv': [],
    'rr_hdr_encode.comp.spv': [],
    # 2026-11-08：关掉 edge-aware，回到 2026-11-02 的纯 rate 掩码。
    # 打开时（11-04 21:35 起）掩码会把高对比像素抬回全速率
    # （sparse_active_mask.comp:56  bestDepth>0.15 || bestLuma>0.35），
    # 实际活跃率远高于请求值 ⇒ 30% 档的收益从 -46% 掉到约 -30%。
    # 回退：把下面这行改回 ['-DSPARSE_EDGE_AWARE=1'] 并重编该 comp。
    'sparse_active_mask.comp.spv': ['-DSPARSE_ORDERED_BAYER=0'],
    # 2026-11-05 B：K1/K2 变体的编译期开关。stale 路径（EXTRA_FLAGS.get）与
    # 下方变体补录共用同一份 ⇒ 漏了会把变体重编成主变体。
    'rt_test_k1.rgen.spv': ['-DORION_K1_PRIMARY', '-DORION_K1_DEFER_GLASS'],
    'rt_test_k1_ser.rgen.spv': ['-DORION_K1_PRIMARY', '-DORION_K1_DEFER_GLASS', '-DRT_ENABLE_SER'],
    'rt_test_k1_glass.rgen.spv': ['-DORION_K1_PRIMARY', '-DORION_K1_GLASS_ONLY'],
    'rt_test_k1_glass_ser.rgen.spv': ['-DORION_K1_PRIMARY', '-DORION_K1_GLASS_ONLY', '-DRT_ENABLE_SER'],
    'rt_test_k2.rgen.spv': ['-DORION_K2_TRANSPORT'],
    'rt_test_k2_ser.rgen.spv': ['-DORION_K2_TRANSPORT', '-DRT_ENABLE_SER'],
    'rt_test_k1.rchit.spv': ['-DORION_K1_PRIMARY'],
    # 2026-11-05 D1/B①：K2 内联材质版（ORION_K2_VARIANT=2 加载）
    'rt_test_k2_inline.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE'],
    # 2026-11-05 ③：K2-D（直射级）/ K2-I（间接级）拆分变体（ORION_K2_SPLIT=1 加载）
    'rt_test_k2d.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_STAGE'],
    'rt_test_k2d_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_STAGE', '-DRT_ENABLE_SER'],
    'rt_test_k2i.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_INDIRECT_STAGE', '-DORION_K2_INDIRECT_TRACE_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY'],
    'rt_test_k2i_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_INDIRECT_STAGE', '-DORION_K2_INDIRECT_TRACE_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY', '-DRT_ENABLE_SER'],
    'rt_test_k2i_compose.comp.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_INDIRECT_STAGE', '-DORION_K2_INDIRECT_COMPOSE_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY', '-DORION_K2I_COMPOSE_COMPUTE'],
    # 2026-11-12 ④：K2-D 再拆分三腿变体（ORION_K2_SPLIT2=0 回退 ③ 两腿）。
    # SUN = 水+太阳直射/GGX/SSS/RTAO（写 binding 89）；LOC = 局部光/水面反射
    # 太阳/GGX 镜面（load-add 复用 binding 89）。K2-I 不变。
    'rt_test_k2sun.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE'],
    'rt_test_k2sun_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE', '-DRT_ENABLE_SER'],
    'rt_test_k2loc.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_CONSUMER_MAT_REUSE_ONLY'],
    'rt_test_k2loc_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_CONSUMER_MAT_REUSE_ONLY',
         '-DRT_ENABLE_SER'],
    'rt_test_k2loc_nee.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_LOC_NEE_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY'],
    'rt_test_k2loc_nee_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_LOC_NEE_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY', '-DRT_ENABLE_SER'],
    'rt_test_k2loc_glossy.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_LOC_GLOSSY_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY'],
    'rt_test_k2loc_glossy_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_LOCAL_STAGE', '-DORION_K2_LOC_GLOSSY_ONLY',
         '-DORION_K2_CONSUMER_MAT_REUSE_ONLY', '-DRT_ENABLE_SER'],
    # 2026-11-22 ⑤：K2-SUN 再拆四腿变体（ORION_K2_SPLIT3=0 回退 ④ 三腿）。
    # SUNA = 水 + RTAO（写 binding 89，rgb 占位 0 / a=rtao）；SUNB = 阴影簇+
    # GGX+植被SSS（load-add 复用 binding 89，直射叠加）。切分依据：RTAO 的
    # rtaoFactor 不进阴影簇（只进出口 alpha）⇒ 依赖干净；k2sun 单腿 20GB
    # commit 冷编四连 OOM（2026-11-21/22），劈半把峰值拉到墙下。
    'rt_test_k2sunA.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE_A'],
    'rt_test_k2sunA_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE_A', '-DRT_ENABLE_SER'],
    'rt_test_k2sunB.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE_B', '-DORION_K2_CONSUMER_MAT_REUSE_ONLY'],
    'rt_test_k2sunB_ser.rgen.spv':
        ['-DORION_K2_TRANSPORT', '-DORION_K2_MATERIAL_INLINE',
         '-DORION_K2_DIRECT_SUN_STAGE_B', '-DORION_K2_CONSUMER_MAT_REUSE_ONLY',
         '-DRT_ENABLE_SER'],
}

STAGE = {'.rgen': 'rgen', '.rchit': 'rchit', '.rahit': 'rahit', '.rmiss': 'rmiss',
         '.comp': 'comp',
         # 2026-11-05 D1：材质 callable TU
         '.rcall': 'rcall'}

# 计数器 TU 判定：源码闭包内出现 rt_stage_counters.glsl 的 include。
COUNTERS_INCLUDE = 'rt_stage_counters.glsl'


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as handle:
            return handle.read()
    except OSError:
        return ''


def includes_of(path):
    """递归 include 闭包用：取一个源文件直接 #include 的同目录文件名。"""
    out = []
    for line in read(path).split('\n'):
        if '#include' in line:
            match = re.search(r'"([^"]+)"', line)
            if match:
                candidate = os.path.join(ROOT, os.path.basename(match.group(1)))
                if os.path.exists(candidate):
                    out.append(candidate)
    return out


def include_closure(path, seen=None):
    """返回源文件 + 递归 include 闭包的去重绝对/规范路径列表。"""
    if seen is None:
        seen = set()
    path = os.path.normpath(path)
    if path in seen:
        return sorted(seen)
    seen.add(path)
    for include in includes_of(path):
        include_closure(include, seen)
    return sorted(seen)


def digest(path):
    try:
        with open(path, 'rb') as handle:
            return hashlib.md5(handle.read()).hexdigest()
    except OSError:
        return None


def content_md5(path):
    return digest(path)


def full_flags(extra):
    """该目标的完整编译宏列表（顺序固定，进指纹）。"""
    return BASE_FLAGS + RT_COUNTERS_FLAGS + RMISS_SLIM_FLAGS + extra


def build(source, spv, extra):
    stage = (
        'comp'
        if ('-DORION_K2I_COMPOSE_COMPUTE' in extra or spv.endswith('.comp.spv'))
        else STAGE[os.path.splitext(source)[1]]
    )

    def command_for(target_env):
        command = [GLSLANG, '-V', '--target-env', target_env, '-S', stage, '-e', 'main']
        command += full_flags(extra)
        command += ['-o', spv, source]
        return command

    # 先 1.2；1.2 不命中再试 1.4（同为有效产物，只是 SPIR-V 风味不同，见 audit 脚本说明）。
    last_output = ''
    for target_env in ('vulkan1.2', 'vulkan1.4'):
        result = subprocess.run(command_for(target_env), capture_output=True, text=True)
        if result.returncode == 0:
            return True, (result.stdout + result.stderr).strip(), target_env
        last_output = (result.stdout + result.stderr).strip()
    return False, last_output, 'vulkan1.4'


def collect_targets():
    """枚举全部编译目标：(源路径, spv 路径, 该变体额外宏)。

    保留旧脚本里所有"同一份源码的 permutation 变体"补录；脏不脏不再在这里判，
    统一交给指纹清单。
    """
    targets = []

    # 1) 每个有 .spv 产物的源文件，源名与产物名一一对应。
    for path in sorted(glob.glob(ROOT + '/*')):
        extension = os.path.splitext(path)[1]
        if extension not in STAGE:
            continue
        spv = path + '.spv'
        if os.path.exists(spv):
            targets.append((path, spv, EXTRA_FLAGS.get(os.path.basename(spv), [])))

    def add(source_rel, spv_name, extra):
        source = ROOT + '/' + source_rel
        spv = ROOT + '/' + spv_name
        if spv not in [t[1] for t in targets]:
            targets.append((source, spv, extra))

    # 2) SER 变体：同一份源码 + -DRT_ENABLE_SER。
    add('rt_test.rgen', 'rt_test_ser.rgen.spv', ['-DRT_ENABLE_SER'])
    add('rt_test.rmiss', 'rt_test_ser.rmiss.spv', ['-DRT_ENABLE_SER'])
    add('rt_test.rmiss', 'rt_test_nocloud.rmiss.spv', ['-DRMISS_SKIP_CLOUDS=1'])

    # 3) SHaRC update 变体：同一份源码 + -DSHARC_UPDATE=1（走 K2-I 轻量化管线 + MatReuse 消费瘦身）。
    sharc_update_flags = [
        '-DSHARC_UPDATE=1',
        '-DORION_K2_TRANSPORT',
        '-DORION_K2_MATERIAL_INLINE',
        '-DORION_K2_INDIRECT_STAGE',
        '-DORION_K2_CONSUMER_MAT_REUSE_ONLY',
    ]
    add('rt_test.rgen', 'rt_test_sharc_update.rgen.spv', sharc_update_flags)
    add('rt_test.rgen', 'rt_test_sharc_update_ser.rgen.spv', sharc_update_flags + ['-DRT_ENABLE_SER'])

    # 3.5) 批4b 植物真几何摇摆：foliage split 顶点逐帧位移 compute（无描述符，
    #      全 push constant；位移后走 foliage BLAS refit）。
    add('foliage_sway.comp', 'foliage_sway.comp.spv', [])
    add('vpt_light_grid_build.comp', 'vpt_light_grid_build.comp.spv', [])
    add('vpt_restir_spatial.comp', 'vpt_restir_spatial.comp.spv', [])
    add('vpt_sparse_compact.comp', 'vpt_sparse_compact.comp.spv', [])
    add('vpt_k2loc_compact.comp', 'vpt_k2loc_compact.comp.spv', [])
    add('vpt_k1_glass_compact.comp', 'vpt_k1_glass_compact.comp.spv', [])
    add('vpt_sharc_compact.comp', 'vpt_sharc_compact.comp.spv', [])
    add('vpt_k2_mega_compact.comp', 'vpt_k2_mega_compact.comp.spv', [])
    add('aces2_bake_lut.comp', 'aces2_bake_lut.comp.spv', [])

    # 4) 半分辨率 GI 变体：同一份源码 + -DGI_HALFRES=1。
    add('rt_test.rgen', 'rt_test_gi_half.rgen.spv', ['-DGI_HALFRES=1'])

    # 5) POM 用途分级变体：同一份源码 + -DPOM_DEFERRED_ONLY=1。
    add('rt_test.rchit', 'rt_test_pomdeferred.rchit.spv', ['-DPOM_DEFERRED_ONLY=1'])

    # 6) K1/K2 两趟拆分变体。
    for spv_name in ('rt_test_k1.rgen.spv',
                     'rt_test_k1_ser.rgen.spv',
                     'rt_test_k1_glass.rgen.spv',
                     'rt_test_k1_glass_ser.rgen.spv',
                     'rt_test_k2.rgen.spv',
                     'rt_test_k2_ser.rgen.spv'):
        add('rt_test.rgen', spv_name, EXTRA_FLAGS[spv_name])

    # 7) K2 内联 / D / I / SUN / LOC / LOC_NEE / LOC_GLOSSY / SUNA / SUNB 变体。
    for spv_name in ('rt_test_k2_inline.rgen.spv',
                     'rt_test_k2d.rgen.spv',
                     'rt_test_k2d_ser.rgen.spv',
                     'rt_test_k2i.rgen.spv',
                     'rt_test_k2i_ser.rgen.spv',
                     'rt_test_k2i_compose.comp.spv',
                     'rt_test_k2sun.rgen.spv',
                     'rt_test_k2sun_ser.rgen.spv',
                     'rt_test_k2loc.rgen.spv',
                     'rt_test_k2loc_ser.rgen.spv',
                     'rt_test_k2loc_nee.rgen.spv',
                     'rt_test_k2loc_nee_ser.rgen.spv',
                     'rt_test_k2loc_glossy.rgen.spv',
                     'rt_test_k2loc_glossy_ser.rgen.spv',
                     'rt_test_k2sunA.rgen.spv',
                     'rt_test_k2sunA_ser.rgen.spv',
                     'rt_test_k2sunB.rgen.spv',
                     'rt_test_k2sunB_ser.rgen.spv'):
        add('rt_test.rgen', spv_name, EXTRA_FLAGS[spv_name])

    # 8) K1 主命中组精简 rchit + 极简主光线专用 rchit（~8 KB）。
    add('rt_test.rchit', 'rt_test_k1.rchit.spv', ['-DORION_K1_PRIMARY'])
    add('rt_test_k1_primary.rchit', 'rt_test_k1_primary.rchit.spv', [])

    # 8.5) 水面 FFT 共享内存融合计算管线（3/5 趟取代 21/41 趟）。
    add('water_fft_fused.comp', 'water_fft_fused.comp.spv', [])

    # 9) 材质 callable TU（生成文件，单一事实源 rt_test.rchit）。
    add('rt_test_material.rcall', 'rt_test_material.spv', [])

    return targets


def load_manifest():
    if os.path.exists(FINGERPRINT_FILE):
        try:
            with open(FINGERPRINT_FILE, encoding='utf-8') as handle:
                return json.load(handle)
        except (OSError, json.JSONDecodeError):
            # 清单损坏 ⇒ 视同空清单，本轮全量重编后重建。
            return {'schema': 1, 'entries': {}}
    return {'schema': 1, 'entries': {}}


def entry_fingerprint(source, spv, extra):
    """计算某目标当前指纹：宏清单 + include 闭包内容哈希 + 产物哈希。"""
    closure = include_closure(source)
    sources = {os.path.relpath(item, ROOT).replace('\\', '/'): content_md5(item)
               for item in closure}
    return {
        'source': os.path.relpath(source, ROOT).replace('\\', '/'),
        'flags': full_flags(extra),
        'sources': sources,
        'spv': digest(spv),
    }


def manifest_matches(entry, current):
    """清单条目与当前指纹是否逐项一致。"""
    if entry.get('source') != current['source']:
        return False
    if entry.get('flags') != current['flags']:
        return False
    if entry.get('sources') != current['sources']:
        return False
    # 产物哈希参与比对：spv 被回滚 / 拷错 / 手工替换时也能抓出来。
    if entry.get('spv') != current['spv']:
        return False
    return True


def disassemble(spv):
    """spirv-dis 位置参数输出到 stdout；失败返回 None。"""
    result = subprocess.run([SPIRV_DIS, spv], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout + result.stderr


def counters_expect_atomics(source):
    """按源码调用的刷新函数，给出计数器缓冲原子写的下限数量。

    · rtCntFlushRgen() 落盘 raysIssued / materialEvals / evPrimary 三车道；
    · rtCntFlushHit()  落盘 materialEvals / evPrimary|evSecondary / evGlass 四车道；
    · rtCntHitAccepted() 直写 hitsAccepted 一车道。
    低于该下限说明该进计数器的原子写丢了（历史事故：缺 Binding 93 的关闭档）。
    """
    text = read(source)
    expected = 0
    if 'rtCntFlushRgen()' in text:
        expected += 3
    if 'rtCntFlushHit()' in text:
        expected += 4
    if 'rtCntHitAccepted()' in text:
        expected += 1
    return expected


def validate_counters(spv, source):
    """计数器档反汇编校验：Binding 93 必须在；原子写数量达到下限。

    返回 (ok, 说明)；ok 为 None 表示反汇编工具失败（SER 1.4 风味在本机
    spirv-dis 1.4.309.0 上报 capability 5388），该项跳过静态判定，不计失败。
    """
    disasm = disassemble(spv)
    if disasm is None:
        return None, 'spirv-dis 反汇编失败（SER 1.4 风味？），跳过静态判定'
    has_binding_93 = re.search(r'OpDecorate\s+%\w+\s+Binding\s+93\b', disasm) is not None
    atomic_adds = len(re.findall(r'OpAtomicIAdd', disasm))
    expected = counters_expect_atomics(source)
    if not has_binding_93:
        return False, '缺少 Binding 93 装饰（计数器缓冲未进产物）'
    if expected > 0 and atomic_adds < expected:
        return False, '计数器原子写不足：%d < 下限 %d' % (atomic_adds, expected)
    return True, 'Binding 93 在，OpAtomicIAdd=%d（下限 %d）' % (atomic_adds, expected)


def validate_closed(spv, source):
    """关闭档反汇编校验：不得出现 Binding 93（计数器缓冲已整段裁掉）。

    返回 (ok, 说明)；ok 为 None 表示反汇编工具失败、该项无法静态判定（不计入
    通过 / 失败，单独记跳过）。sharc 诊断计数、perf 统计等其它 SSBO 的原子写
    与 Binding 93 无关，本判据只盯计数器缓冲是否串档。
    """
    disasm = disassemble(spv)
    if disasm is None:
        return None, 'spirv-dis 反汇编失败（1.4 风味？），跳过静态判定'
    has_binding_93 = re.search(r'OpDecorate\s+%\w+\s+Binding\s+93\b', disasm) is not None
    atomic_adds = len(re.findall(r'OpAtomicIAdd', disasm))
    if has_binding_93:
        return False, '关闭档却出现 Binding 93（计数器宏漏裁 / 串档）'
    return True, 'Binding 93=0（其它 SSBO OpAtomicIAdd=%d，与计数器无关）' % atomic_adds


def source_references_counters(source):
    """源码闭包是否引用 rt_stage_counters.glsl。"""
    for item in include_closure(source):
        if os.path.basename(item) == COUNTERS_INCLUDE:
            return True
    return False


def validate_deploy_consistency():
    """三处部署目录逐文件哈希必须一致。返回不一致列表。"""
    mismatches = []
    cpp_dir = ROOT
    for entry_name in sorted(os.listdir(cpp_dir)):
        if not entry_name.endswith('.spv'):
            continue
        master = digest(os.path.join(cpp_dir, entry_name))
        for target_dir in DEPLOY_DIRS:
            destination = os.path.join(target_dir, entry_name)
            if not os.path.isdir(target_dir):
                continue
            if not os.path.exists(destination):
                mismatches.append((entry_name, target_dir, '缺失'))
                continue
            if digest(destination) != master:
                mismatches.append((entry_name, target_dir,
                                   'cpp=%s 部署=%s' % ((master or '-')[:8],
                                                      (digest(destination) or '-')[:8])))
    return mismatches


def main():
    parser = argparse.ArgumentParser(description='编译/部署 RT 着色器 SPV（指纹判据）')
    parser.add_argument('--force', action='store_true',
                        help='无视指纹清单，全部目标强制重编')
    args = parser.parse_args()

    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

    targets = collect_targets()
    manifest = load_manifest()
    entries = manifest.setdefault('entries', {})

    # 顶层记录本次部署模式（计数档 / 瘦身档），避免两种模式在清单里看起来一样。
    current_mode = {'counters': COUNTERS_ENABLED, 'rmiss_slim': RMISS_SLIM_ENABLED}
    old_mode = manifest.get('mode')
    mode_changed = old_mode is not None and old_mode != current_mode

    stale = []
    for source, spv, extra in targets:
        current = entry_fingerprint(source, spv, extra)
        name = os.path.basename(spv)
        if args.force:
            stale.append((source, spv, extra, current))
            continue
        # 模式切换 ⇒ 编译口径整体变了，全部目标按新模式重编（不靠单条指纹推断）。
        old = entries.get(name)
        if mode_changed or old is None or not manifest_matches(old, current):
            stale.append((source, spv, extra, current))

    print('== 本次部署模式：COUNTERS=%s, RMISS_SLIM=%s =='
          % ('ON' if COUNTERS_ENABLED else 'OFF',
             'ON' if RMISS_SLIM_ENABLED else 'OFF'))
    if not COUNTERS_ENABLED:
        print('   警告：关闭档产物已整段裁掉计数器；运行期再设 '
              'ORION_RT_STAGE_COUNTERS=1 也只会得到全 0 计数，不要这样起局。')
    if mode_changed:
        print('   注意：部署模式与上次清单（%s）不同，本轮全部目标按新模式重编。'
              % old_mode)
    print('== 指纹比对：共 %d 个目标，待重编 %d 个 ==' % (len(targets), len(stale)))

    changed = []
    same = []
    failed = []

    for source, spv, extra, current in stale:
        before = digest(spv)
        ok, output, flavor = build(source, spv, extra)
        name = os.path.basename(spv)
        if not ok:
            failed.append((name, output))
            print('   FAIL %-34s (%s)' % (name, flavor))
            continue
        after = digest(spv)
        if after == before:
            same.append(name)
            print('   same %-34s （逐字节未变 ⇒ 该着色器其实不受影响）' % name)
        else:
            changed.append(name)
            print('   DIFF %-34s  %s -> %s' % (name, (before or '-')[:8], (after or '-')[:8]))

        # 无论变没变，都用当前指纹刷新清单条目（产物哈希以编完后的为准）。
        entries[name] = entry_fingerprint(source, spv, extra)

    # 部署：只把真正变了的 spv 写到三处目录。
    deployed = 0
    print()
    print('== 部署到 %d 处：%s ==' % (len(DEPLOY_DIRS), ', '.join(DEPLOY_DIRS)))
    for source, spv, extra, _ in stale:
        name = os.path.basename(spv)
        if name not in changed:
            continue
        mine = digest(spv)
        wrote = []
        for target_dir in DEPLOY_DIRS:
            if not os.path.isdir(target_dir):
                continue
            destination = os.path.join(target_dir, name)
            if digest(destination) == mine:
                continue
            with open(spv, 'rb') as src_handle, open(destination, 'wb') as dst_handle:
                dst_handle.write(src_handle.read())
            wrote.append(target_dir)
        if wrote:
            deployed += 1
            print('   %-34s 已部署 %s -> %s' % (name, mine[:8], ' + '.join(wrote)))
        else:
            print('   %-34s 三处已是同一份' % name)

    # ---- 部署后反汇编 / 一致性校验；任一失败 ⇒ 非 0 退出，不静默放过。 ----
    # 校验永远不空跑：计数档断言 Binding 93 在 + 原子写达标；关闭档反向断言
    # Binding 93 不在。两项分别计数，汇总行写明实际校验了多少项。
    errors = []
    counters_checked = 0
    closed_checked = 0
    skipped_disasm = 0

    print()
    if COUNTERS_ENABLED:
        print('== 计数档反汇编校验（Binding 93 ≥ 1 + 原子写下限）==')
    else:
        print('== 关闭档反汇编校验（反向断言 Binding 93 == 0）==')
    for source, spv, extra in targets:
        if not source_references_counters(source):
            continue
        name = os.path.basename(spv)
        if COUNTERS_ENABLED:
            ok, note = validate_counters(spv, source)
            if ok is None:
                skipped_disasm += 1
                print('   SKIP %-34s %s' % (name, note))
                continue
            counters_checked += 1
        else:
            ok, note = validate_closed(spv, source)
            if ok is None:
                skipped_disasm += 1
                print('   SKIP %-34s %s' % (name, note))
                continue
            closed_checked += 1
        if ok:
            print('   OK   %-34s %s' % (name, note))
        else:
            errors.append('%s：%s' % (name, note))
            print('   FAIL %-34s %s' % (name, note))

    if RMISS_SLIM_ENABLED:
        print()
        print('== miss 瘦身档基线差异校验 ==')
        rmiss = os.path.join(ROOT, 'rt_test.rmiss.spv')
        rmiss_hash = digest(rmiss)
        if rmiss_hash == BASELINE_RMISS_OFF_MD5:
            errors.append('rt_test.rmiss.spv 与 P4 关档基线相同（瘦身宏未进编译口径）')
            print('   FAIL rt_test.rmiss.spv == 关档基线 %s' % BASELINE_RMISS_OFF_MD5[:8])
        else:
            print('   OK   rt_test.rmiss.spv = %s ≠ 关档基线 %s'
                  % ((rmiss_hash or '-')[:8], BASELINE_RMISS_OFF_MD5[:8]))

    print()
    print('== 三处部署目录一致性校验 ==')
    mismatches = validate_deploy_consistency()
    for entry_name, target_dir, reason in mismatches:
        errors.append('%s @ %s：%s' % (entry_name, target_dir, reason))
        print('   MISMATCH %-30s %s (%s)' % (entry_name, target_dir, reason))
    if not mismatches:
        print('   三处目录逐文件哈希一致。')

    # 清单只在全部成功后落盘；中途失败先不落，避免把脏指纹写进去。
    if not errors and not failed:
        manifest['mode'] = current_mode
        with open(FINGERPRINT_FILE, 'w', encoding='utf-8') as handle:
            json.dump(manifest, handle, ensure_ascii=False, indent=2, sort_keys=True)
        print()
        print('指纹清单已更新：%s' % FINGERPRINT_FILE)
    elif errors or failed:
        print()
        print('!! 存在失败项，指纹清单未落盘（避免把脏指纹写进清单）。')

    print()
    print('汇总：目标 %d / 重编 %d / 有变化 %d / 已部署 %d / 未变 %d / 失败 %d'
          % (len(targets), len(stale), len(changed), deployed, len(same),
             len(failed)))
    print('校验：计数档 %d 项 / 关档 %d 项 / 反汇编跳过 %d 项 / 校验错误 %d'
          % (counters_checked, closed_checked, skipped_disasm, len(errors)))
    for name, output in failed:
        print('---- %s ----' % name)
        print(output[:1500])

    return 1 if (failed or errors) else 0


if __name__ == '__main__':
    sys.exit(main())
