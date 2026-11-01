// Orion backend — Platform 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



std::string GetRTXBackendDirectory()
{
    static const std::string cachedDirectory = []() -> std::string
    {
        std::vector<char> buffer(
            32768,
            '\0'
        );


        const DWORD length =
            GetModuleFileNameA(
                reinterpret_cast<HMODULE>(
                    &__ImageBase
                ),
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()
                )
            );


        if(
            length == 0 ||
            length >= buffer.size()
        )
        {
            fprintf(
                stderr,
                "[RUNTIME PATH] GetModuleFileNameA failed error=%lu\n",
                GetLastError()
            );

            fflush(stderr);

            return {};
        }


        std::string modulePath(
            buffer.data(),
            length
        );


        const std::string::size_type slash =
            modulePath.find_last_of(
                "\\/"
            );


        if(slash == std::string::npos)
        {
            fprintf(
                stderr,
                "[RUNTIME PATH] invalid backend module path=%s\n",
                modulePath.c_str()
            );

            fflush(stderr);

            return {};
        }


        return modulePath.substr(
            0,
            slash
        );
    }();


    return cachedDirectory;
}


bool IsAbsoluteWindowsPath(
    const char* path
)
{
    if(
        !path ||
        path[0] == '\0'
    )
    {
        return false;
    }


    if(
        path[0] != '\0' &&
        path[1] == ':' &&
        (
            path[2] == '\\' ||
            path[2] == '/'
        )
    )
    {
        return true;
    }


    return
        path[0] == '\\' ||
        path[0] == '/';
}


std::string ResolveRTXRuntimePath(
    const char* path
)
{
    if(
        !path ||
        path[0] == '\0'
    )
    {
        return {};
    }


    if(IsAbsoluteWindowsPath(path))
    {
        return std::string(path);
    }


    const std::string baseDirectory =
        GetRTXBackendDirectory();


    if(baseDirectory.empty())
    {
        return std::string(path);
    }


    return
        baseDirectory +
        "\\" +
        path;
}


// =====================================================
// Optional MFG Ada unlock module (RTXMFG.dll)
// =====================================================
//
// Third-party module that makes 3x/4x/6x frame generation legal on Ada by
// detouring Streamline (slInit, slDLSSGSetOptions, slDLSSGGetState,
// slGetFeatureFunction) and the NGX DLSS-G provider entry points inside this
// process, and by correcting the provider's temporal program in mapped memory.
// It must be inside the process before slInit runs and before the DLSS-G
// pipeline is created, which is why the backend loads it rather than leaning on
// the game: the executable in a Fabric run is java.exe inside the JDK
// directory, so the mod's documented "rename it to a proxy DLL next to the exe"
// route would mean writing into the JDK.
//
// Entirely optional.  A missing file is not an error, nothing in this backend
// depends on it, and deleting RTXMFG.dll turns the unlock off again: the
// multiplier setting stays clamped to whatever the driver reports, and this
// module is what raises that ceiling.
void LoadMfgUnlockModuleIfPresent()
{
    static bool attempted = false;


    if(attempted)
    {
        return;
    }

    attempted = true;


    const std::string modulePath =
        ResolveRTXRuntimePath(
            "RTXMFG.dll"
        );


    if(
        modulePath.empty() ||
        GetFileAttributesA(
            modulePath.c_str()
        ) == INVALID_FILE_ATTRIBUTES
    )
    {
        fprintf(
            stderr,
            "[MFG UNLOCK] RTXMFG.dll not present, unlock inactive\n"
        );

        fflush(stderr);

        return;
    }


    HMODULE unlockModule =
        LoadLibraryExA(
            modulePath.c_str(),
            nullptr,
            0
        );


    fprintf(
        stderr,
        "[MFG UNLOCK] path=%s handle=%p error=%lu\n",
        modulePath.c_str(),
        reinterpret_cast<void*>(
            unlockModule
        ),
        unlockModule
            ? 0ul
            : GetLastError()
    );

    fflush(stderr);
}


// Writes one timestamped line.  Flushing is opt-in: the buffered path exists so
// filesystem latency never lands on the render thread.
void RTLogWrite(
    const std::string& msg,
    bool flush
)
{
    auto now =
        std::chrono::system_clock::now();


    auto t =
        std::chrono::system_clock::to_time_t(now);


    std::lock_guard<std::mutex> lock(g_rtLogMutex);

    rtLogFile
        << "[CPP] "
        << std::put_time(
            std::localtime(&t),
            "%H:%M:%S"
        )
        << " "
        << msg
        << "\n";

    if(flush)
    {
        rtLogFile.flush();
    }
}


void RTLog(
    const std::string& msg
)
{
    RTLogWrite(msg, false);
}


// Flushed variant for the rare diagnostics whose loss would invalidate a whole
// capture: the host can die before the buffered stream ever reaches disk, and a
// report that was never written is indistinguishable from a stall that never
// happened.
void RTLogFlushed(
    const std::string& msg
)
{
    RTLogWrite(msg, true);
}
void RTXTerminateHandler()
{
    if(auto e = std::current_exception())
    {
        try
        {
            std::rethrow_exception(e);
        }
        catch(const std::exception& ex)
        {
            RTLogFlushed(std::string("[FATAL] uncaught C++ exception: ") + ex.what());
        }
        catch(...)
        {
            RTLogFlushed("[FATAL] uncaught non-std C++ exception");
        }
    }
    else
    {
        RTLogFlushed("[FATAL] std::terminate with no active exception");
    }
    std::abort();
}
void RTXMarkPhase(const char* name)
{
    const uint64_t seq =
        g_rtxPhaseSeq.fetch_add(1, std::memory_order_relaxed) + 1u;
    const int slot = static_cast<int>(seq % RTX_PHASE_RING);

    g_rtxPhaseStamp[slot].store(seq, std::memory_order_relaxed);
    g_rtxPhaseName[slot].store(name, std::memory_order_relaxed);
    g_rtxPhaseCurrent.store(name, std::memory_order_relaxed);
}
std::string RTXHex(uint64_t value)
{
    static const char* digits = "0123456789ABCDEF";

    char buffer[20] = { '0', 'x' };
    int pos = 2;
    bool started = false;

    for(int shift = 60; shift >= 0; shift -= 4)
    {
        const int nibble = static_cast<int>((value >> shift) & 0xFull);

        if(nibble != 0)
        {
            started = true;
        }

        if(started || shift == 0)
        {
            buffer[pos] = digits[nibble];
            pos += 1;
        }
    }

    buffer[pos] = '\0';

    return std::string(buffer);
}

// 故障地址 → "模块名+偏移"（与 hs_err 的 Problematic frame 同口径）。
std::string RTXModuleOfPc(uintptr_t pc)
{
    HMODULE module = nullptr;

    if(GetModuleHandleExA(
           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
           reinterpret_cast<LPCSTR>(pc),
           &module) &&
       module != nullptr)
    {
        char path[MAX_PATH] = {};

        if(GetModuleFileNameA(module, path, MAX_PATH) > 0)
        {
            const char* leaf = std::strrchr(path, '\\');
            leaf = (leaf != nullptr) ? (leaf + 1) : path;

            return std::string(leaf) +
                   "+" + RTXHex(pc - reinterpret_cast<uintptr_t>(module));
        }

        return std::string("<module>+") +
               RTXHex(pc - reinterpret_cast<uintptr_t>(module));
    }

    return std::string("<unknown module>@") + RTXHex(pc);
}

// 阶段面包屑倒序打印（最新在前）。崩溃时也写一份到独立文件，
// 避免 rt_debug.log 的流在崩溃现场不可用。
void RTXDumpCrashContext(const std::string& header)
{
    const uint64_t seq = g_rtxPhaseSeq.load(std::memory_order_relaxed);
    const uint64_t frame = g_rtxLastFrameSerial.load(std::memory_order_relaxed);
    const char* current = g_rtxPhaseCurrent.load(std::memory_order_relaxed);

    RTLogFlushed(header);

    const uint64_t detailKey =
        g_rtxDetailSectionKey.load(std::memory_order_relaxed);
    const uint64_t detailVerts =
        g_rtxDetailSectionVerts.load(std::memory_order_relaxed);
    const uint64_t detailInstances =
        g_rtxDetailInstances.load(std::memory_order_relaxed);

    RTLogFlushed(
        std::string("[CRASH] frame=") + std::to_string(frame) +
        " phase=" + (current != nullptr ? current : "?") +
        " sectionKey=" + std::to_string(detailKey) +
        " sectionVerts=" + std::to_string(detailVerts) +
        " tlasInstances=" + std::to_string(detailInstances)
    );

    for(uint64_t back = 1; back <= 8u && back <= seq; ++back)
    {
        const uint64_t stamp = seq - back + 1u;
        const int slot = static_cast<int>(stamp % RTX_PHASE_RING);

        if(g_rtxPhaseStamp[slot].load(std::memory_order_relaxed) != stamp)
        {
            continue;
        }

        const char* name = g_rtxPhaseName[slot].load(std::memory_order_relaxed);

        RTLogFlushed(
            std::string("[CRASH PATH] #") + std::to_string(stamp) +
            " " + (name != nullptr ? name : "?")
        );
    }

    std::ofstream crashFile("rt_crash.log", std::ios::app);

    if(crashFile.is_open())
    {
        crashFile << header << "\n";
        crashFile << "[CRASH] frame=" << frame
                  << " phase=" << (current != nullptr ? current : "?")
                  << " sectionKey=" << detailKey
                  << " sectionVerts=" << detailVerts
                  << " tlasInstances=" << detailInstances << "\n";

        for(uint64_t back = 1; back <= 8u && back <= seq; ++back)
        {
            const uint64_t stamp = seq - back + 1u;
            const int slot = static_cast<int>(stamp % RTX_PHASE_RING);

            if(g_rtxPhaseStamp[slot].load(std::memory_order_relaxed) != stamp)
            {
                continue;
            }

            const char* name = g_rtxPhaseName[slot].load(std::memory_order_relaxed);

            crashFile << "[CRASH PATH] #" << stamp << " "
                      << (name != nullptr ? name : "?") << "\n";
        }

        crashFile.flush();
    }
}
LONG WINAPI RTXUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    DWORD exceptionCode = 0u;
    uintptr_t exceptionAddress = 0u;
    uintptr_t instructionPointer = 0u;
    uintptr_t stackPointer = 0u;

    if(info != nullptr)
    {
        if(info->ExceptionRecord != nullptr)
        {
            exceptionCode = info->ExceptionRecord->ExceptionCode;
            exceptionAddress =
                reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        }

        if(info->ContextRecord != nullptr)
        {
#if defined(_M_X64)
            instructionPointer =
                static_cast<uintptr_t>(info->ContextRecord->Rip);
            stackPointer =
                static_cast<uintptr_t>(info->ContextRecord->Rsp);
#elif defined(_M_ARM64)
            instructionPointer =
                static_cast<uintptr_t>(info->ContextRecord->Pc);
            stackPointer =
                static_cast<uintptr_t>(info->ContextRecord->Sp);
#else
            instructionPointer = exceptionAddress;
#endif
        }
    }

    const uintptr_t faultPc =
        (exceptionAddress != 0u) ? exceptionAddress : instructionPointer;

    RTXDumpCrashContext(
        std::string("[CRASH] unhandled native exception code=") +
        RTXHex(exceptionCode) +
        " at=" + RTXModuleOfPc(faultPc) +
        " rip=" + RTXHex(instructionPointer) +
        " rsp=" + RTXHex(stackPointer) +
        " thread=" + std::to_string(GetCurrentThreadId())
    );

    if(g_rtxPreviousExceptionFilter != nullptr)
    {
        return g_rtxPreviousExceptionFilter(info);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}



// ============================================================
// 2026-10-29 OPT-EXT①：GPU 挂死诊断
//
// VK_EXT_device_fault + VK_NV_device_diagnostic_checkpoints。
//
// 背景：2026-10-21 那次"进世界必崩 / 5s semaphore 超时"排查了四轮才落到
// "rchit 的 #else 分支少写一条 ray payload ⇒ material_resolve 拿上一条射线的
// identity 索引 ⇒ buffer_reference 解引用任意地址 ⇒ GPU 挂死"。当时没有任何
// 设备侧读数可用，只能二分 + 读源码。本函数唯一目的：设备真的丢掉时，把驱动
// 给的描述/地址与队列最后一个 checkpoint 写进 rt_debug.log，下一次先看日志。
//
// 两者都是只读查询，不改变任何渲染行为。扩展未启用时只打一行 "not enabled"。
// ============================================================
void logDeviceFaultDiagnostics(const char* where)
{
    if(g_device == VK_NULL_HANDLE)
    {
        return;
    }

    std::string line = "[GPU FAULT] ";
    line += (where != nullptr) ? where : "?";

    auto getFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(g_device, "vkGetDeviceFaultInfoEXT"));

    if(getFaultInfo == nullptr)
    {
        RTLog(line + " : VK_EXT_device_fault not enabled");
    }
    else
    {
        VkDeviceFaultCountsEXT counts{};
        counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

        const VkResult countsResult = getFaultInfo(g_device, &counts, nullptr);

        if(countsResult != VK_SUCCESS)
        {
            RTLog(line + " : vkGetDeviceFaultInfoEXT(counts) returned " +
                std::to_string(static_cast<int>(countsResult)));
        }
        else
        {
            std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(counts.vendorInfoCount);
            std::vector<char> vendorBinary(static_cast<size_t>(counts.vendorBinarySize));

            VkDeviceFaultInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
            info.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
            info.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
            info.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

            const VkResult infoResult = getFaultInfo(g_device, nullptr, &info);

            if(infoResult != VK_SUCCESS)
            {
                RTLog(line + " : vkGetDeviceFaultInfoEXT(info) returned " +
                    std::to_string(static_cast<int>(infoResult)));
            }
            else
            {
                std::stringstream fault;
                fault << line
                      << " : description=\"" << info.description << "\""
                      << " addressInfos=" << counts.addressInfoCount
                      << " vendorInfos=" << counts.vendorInfoCount
                      << " vendorBinaryBytes=" << counts.vendorBinarySize;

                for(uint32_t i = 0u; i < counts.addressInfoCount; ++i)
                {
                    fault << " | addr[" << i << "]"
                          << " type=" << static_cast<int>(addressInfos[i].addressType)
                          << " reported=0x" << std::hex << addressInfos[i].reportedAddress
                          << " precision=" << addressInfos[i].addressPrecision
                          << std::dec;
                }

                for(uint32_t i = 0u; i < counts.vendorInfoCount; ++i)
                {
                    fault << " | vendor[" << i << "]=" << vendorInfos[i].description
                          << " code=0x" << std::hex << vendorInfos[i].vendorFaultCode
                          << std::dec;
                }

                RTLog(fault.str());
            }
        }
    }

    auto getCheckpointData = reinterpret_cast<PFN_vkGetQueueCheckpointData2NV>(
        vkGetDeviceProcAddr(g_device, "vkGetQueueCheckpointData2NV"));

    if(getCheckpointData == nullptr || g_queue == VK_NULL_HANDLE)
    {
        return;
    }

    uint32_t checkpointCount = 0u;
    getCheckpointData(g_queue, &checkpointCount, nullptr);

    if(checkpointCount == 0u)
    {
        RTLog(line + " : queue has no checkpoint (vkCmdSetCheckpointNV never hit)");
        return;
    }

    std::vector<VkCheckpointData2NV> checkpoints(checkpointCount);

    for(uint32_t i = 0u; i < checkpointCount; ++i)
    {
        checkpoints[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
        checkpoints[i].pNext = nullptr;
    }

    getCheckpointData(g_queue, &checkpointCount, checkpoints.data());

    std::stringstream cp;
    cp << line << " : checkpoints=" << checkpointCount;

    for(uint32_t i = 0u; i < checkpointCount; ++i)
    {
        cp << " | cp[" << i << "] stage=0x" << std::hex << checkpoints[i].stage
           << " marker=" << std::dec;

        if(checkpoints[i].pCheckpointMarker != nullptr)
        {
            cp << static_cast<const char*>(checkpoints[i].pCheckpointMarker);
        }
        else
        {
            cp << "(null)";
        }
    }

    RTLog(cp.str());
}

// =====================================================
// 2026-10-16 泄漏归因探针（只读时钟+只读计数器，零行为变更）
//
// 现场：Vulkan 后端下进程提交量以约 38MB/s 持续增长（105 秒 +4.0GB）；
// 已用闸门排除 Streamline/NGX 整块（`ORION_SL_FEATURES=none` 照涨 38.3MB/s），
// 并证明只要跳过我们的设备级初始化（`orion.native.deviceInit=false`）就归零
// （−0.3MB/s）。本探针回答"这 38MB/s 具体花在哪一段时间里"：
//   · inside  = 在 beginStreamlineFrame **进入→退出**之间涨的 ⇒ 落在我们自己的每帧路径；
//   · outside = 在**退出→下一次进入**之间涨的 ⇒ 不在我们的调用里（驱动/present 线程/其它模块）；
// 每 60 帧一行，同时给墙钟秒数 ⇒ 按帧 还是 按秒 一眼可判。
// 口径与 [RT MEM] 的 privateMB 完全一致（GetProcessMemoryInfo().PagefileUsage）。
// =====================================================
uint64_t leakProbeCommitBytes()
{
    PROCESS_MEMORY_COUNTERS counters{};

    if(!GetProcessMemoryInfo(
           GetCurrentProcess(),
           &counters,
           sizeof(counters)))
    {
        return 0u;
    }

    return static_cast<uint64_t>(
        counters.PagefileUsage);
}

// 嵌套安全：只有最外层的进入/退出才记账，这样把多个每帧 JNI 入口逐个包上也不会重复计数。
// 2026-10-17：每帧探针**默认关闭**（ORION_LEAK_PROBE=1 才开）。
// 之前部署版里它常开：3 个每帧 JNI 入口 × 进/出各一次 GetProcessMemoryInfo，
// 180fps 下约 1000 次/秒；内存吃紧时这个调用本身会变慢，是"一卡一卡"的可疑来源之一。
// 它只是测量工具，不该出现在给人玩的构建里。
bool leakProbeEnabled()
{
    static const bool enabled = []() -> bool
    {
        const char* raw =
            std::getenv("ORION_LEAK_PROBE");

        return raw != nullptr
               && (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "true") == 0);
    }();

    return enabled;
}
void leakProbeFrameBegin()
{
    if(!leakProbeEnabled())
    {
        return;
    }

    if(g_leakProbeDepth++ != 0)
    {
        return;
    }

    const uint64_t now = leakProbeCommitBytes();

    if(g_leakProbeLastExitBytes != 0u)
    {
        g_leakProbeOutsideBytes +=
            static_cast<int64_t>(now) -
            static_cast<int64_t>(g_leakProbeLastExitBytes);
    }

    if(g_leakProbeFrameCount == 0u)
    {
        g_leakProbeWindowStart =
            std::chrono::steady_clock::now();
    }

    g_leakProbeEnterBytes = now;
}
void leakProbeFrameEnd()
{
    if(!leakProbeEnabled())
    {
        return;
    }

    if(--g_leakProbeDepth != 0)
    {
        return;
    }

    const uint64_t now = leakProbeCommitBytes();

    g_leakProbeInsideBytes +=
        static_cast<int64_t>(now) -
        static_cast<int64_t>(g_leakProbeEnterBytes);

    g_leakProbeLastExitBytes = now;

    ++g_leakProbeFrameCount;

    if(g_leakProbeFrameCount < 60u)
    {
        return;
    }

    const double seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() -
                g_leakProbeWindowStart
        ).count();

    const double insideMb =
        static_cast<double>(g_leakProbeInsideBytes) / 1048576.0;

    const double outsideMb =
        static_cast<double>(g_leakProbeOutsideBytes) / 1048576.0;

    fprintf(
        stderr,
        "[LEAK PROBE] frames=%u sec=%.2f insideMB=%.1f outsideMB=%.1f "
        "insideMBps=%.2f outsideMBps=%.2f\n",
        g_leakProbeFrameCount,
        seconds,
        insideMb,
        outsideMb,
        seconds > 0.0 ? insideMb / seconds : 0.0,
        seconds > 0.0 ? outsideMb / seconds : 0.0
    );

    fflush(stderr);

    g_leakProbeInsideBytes = 0;
    g_leakProbeOutsideBytes = 0;
    g_leakProbeFrameCount = 0u;
}

// =====================================================
// 2026-10-16 诊断闸门 ORION_DEVICE_INIT_STOP=N（默认 0 = 不停 = 现状）
//
// 用途：设备级初始化（initDeviceHandlesNative）里逐里程碑二分，找出哪一步打开
// 了"宿主提交量以 ~36MB/s 按秒增长 + 一个 nvoglv64.dll 线程占满一个核"。
// 现状证据：跳过整个设备级初始化 ⇒ −0.3MB/s；零 Streamline feature 也照涨；
// 流失不在任何每帧 JNI 里（[LEAK PROBE] inside≈0）；按秒不按帧。
// 每个里程碑打一行 [DEV STEP]，达到 N 就 return（后面的初始化不做，只用于测量）。
// =====================================================
uint32_t deviceInitStopStep()
{
    static const uint32_t stopStep = []() -> uint32_t
    {
        const char* raw =
            std::getenv("ORION_DEVICE_INIT_STOP");

        return raw == nullptr
                   ? 0u
                   : static_cast<uint32_t>(std::strtoul(raw, nullptr, 10));
    }();

    return stopStep;
}

// 2026-10-31：渲染线程提优先级，默认关闭。
// 理由：C2ME 在把 worker 压到 THREAD_PRIORITY_LOWEST 的同时，把渲染主线程提到
// maximum-performance。这里默认只做"worker 降级"这一半（无副作用），主线程提级
// 会影响 Reflex / Streamline 自己的线程观感，需要实测 ⇒ 留在
// ORION_RENDER_THREAD_PRIORITY=1 后面。
void applyRenderThreadPriorityOnce()
{
    static bool applied = false;

    if(applied)
    {
        return;
    }

    applied = true;

    const char* raw = std::getenv("ORION_RENDER_THREAD_PRIORITY");

    if(raw == nullptr || raw[0] == '\0' || raw[0] == '0')
    {
        return;
    }

    const BOOL ok =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    RTLog(
        std::string("[RT THREAD] render thread priority=HIGHEST ok=") +
        (ok ? "1" : "0"));
}

// =====================================================
// 2026-10-09：[RT MEM] 内存记账（诊断行）。
//
// 为什么需要它：用户报"玩久了会直接占满内存"。此前本文件**没有任何**内存口径
// （`[ALLOC FAIL]` 从未出现过 ⇒ 显存分配没失败过 ⇒ 不是显存爆），所以既不能说
// 是哪个结构在涨，也不能说是涨在原生侧还是 Java 堆 / 驱动里。
//
// 判读（这一行给的就是这三种切分）：
//   · rssMB 随时间单调上升，而下列各项之和基本不变
//       ⇒ 涨在**本行没记到的地方**（Java 堆、Vulkan 驱动、或本文件的漏记结构）；
//   · 某一项与 rssMB 同步单调上升（最可能是 meshMB / rawMB / queued）
//       ⇒ 就是它，按持有者去查删除路径；
//   · 全部平稳、只有 sysAvailMB 在掉
//       ⇒ 是别的进程/别的消费者，不是本模组。
//
// 语义说明：
//   · meshMB = g_chunkMeshes 的顶点字节（CPU 侧权威镜像）
//   · rawMB  = g_chunkRawPayloads 的**原始**字节：每层再存一份未打包副本，用于
//              上传前 memcmp 去重 ⇒ 与 meshMB 同量级是**设计如此**，不是泄漏。
//   · 这些量都是世界规模的函数，探索得越远越大属正常；**要看的是它是否收敛**。
//   · 每 600 帧（≈10s）一行，与 [RT GEOMETRY] counters 同频，方便两条时间线对齐。
// =====================================================
void logNativeMemoryReport()
{
    PROCESS_MEMORY_COUNTERS memCounters{};
    const bool haveProcessMemory =
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            &memCounters,
            sizeof(memCounters)
        ) != FALSE;

    MEMORYSTATUSEX systemMemory{};
    systemMemory.dwLength = sizeof(systemMemory);
    const bool haveSystemMemory =
        GlobalMemoryStatusEx(&systemMemory) != FALSE;

    VramBudgetSample vramSample;
    queryVramBudget(vramSample);

    const double vramUsedMb = vramSample.usedMb;
    const double vramBudgetMb = vramSample.budgetMb;

    size_t meshSectionCount = 0u;
    size_t meshBytes = 0u;
    size_t rawBytes = 0u;
    size_t digestCount = 0u;
    size_t ommInputCount = 0u;
    size_t ommBytes = 0u;
    size_t dirtyKeyCount = 0u;

    {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);

        meshSectionCount = g_chunkMeshes.size();

        for(const auto& entry : g_chunkMeshes)
        {
            meshBytes += entry.second.size() * sizeof(Vertex);
        }

        for(const auto& entry : g_chunkRawPayloads)
        {
            rawBytes += entry.second.size();
        }

        digestCount = g_chunkUploadDigests.size();

        ommInputCount = g_cutoutOmmInputs.size();

        for(const auto& entry : g_cutoutOmmInputs)
        {
            ommBytes += entry.second.data.size();
        }

        dirtyKeyCount = g_dirtySectionKeys.size();
    }

    size_t lightSectionCount = 0u;
    size_t lightCount = 0u;
    size_t lightBytes = 0u;

    {
        std::lock_guard<std::mutex> lock(g_lightMutex);

        lightSectionCount = g_lightSections.size();

        for(const auto& entry : g_lightSections)
        {
            lightCount += entry.second.size();
            lightBytes += entry.second.size() * sizeof(LocalLightRecord);
        }
    }

    size_t capturedCount = 0u;

    {
        std::lock_guard<std::mutex> lock(g_capturedSectionsMutex);
        capturedCount =
            g_capturedSections.size() - g_capturedSectionsHead;
    }

    const size_t blasCount = g_sectionBLAS.size();
    const size_t voxySourceCount = g_voxyLodSources.size();

    const auto toMb =
        [](size_t bytes)
        {
            return static_cast<double>(bytes) / 1048576.0;
        };

    const RtxMemAccountSnapshot vkMem = rtxMemAccountSnapshot();

    VkDeviceSize asBlasBytes = 0u;
    VkDeviceSize asTrueBytes = 0u;
    VkDeviceSize asChunkBytes = 0u;

    for(const auto& asEntry : g_sectionBLAS)
    {
        const RTSectionAS& asSection = asEntry.second;

        asBlasBytes += asSection.blasBytes;
        asTrueBytes += asSection.vertexBytes + asSection.blasBytes;
        asChunkBytes += asSection.sectionMemoryChunkBytes;
    }

    size_t vtxStageCount = 0u;
    VkDeviceSize vtxStageBytes = 0u;
    size_t vtxStageInFlight = 0u;
    g_vertexStagingPool.stats(vtxStageCount, vtxStageBytes, vtxStageInFlight);

    size_t blasScratchCount = 0u;
    VkDeviceSize blasScratchBytes = 0u;
    size_t blasScratchInFlight = 0u;
    g_blasScratchPool.stats(
        blasScratchCount,
        blasScratchBytes,
        blasScratchInFlight
    );

    const std::string vkChurn = rtxBufferChurnTopReport(12u);

    std::ostringstream report;
    report << std::fixed << std::setprecision(1)
        << "[RT MEM]"
        << " rssMB="
        << (haveProcessMemory ? toMb(memCounters.WorkingSetSize) : -1.0)
        << " peakRssMB="
        << (haveProcessMemory ? toMb(memCounters.PeakWorkingSetSize) : -1.0)
        << " privateMB="
        << (haveProcessMemory ? toMb(memCounters.PagefileUsage) : -1.0)
        << " sysAvailMB="
        << (haveSystemMemory
                ? static_cast<double>(systemMemory.ullAvailPhys) / 1048576.0
                : -1.0)
        << " sysTotalMB="
        << (haveSystemMemory
                ? static_cast<double>(systemMemory.ullTotalPhys) / 1048576.0
                : -1.0)
        << " vramUsedMB=" << vramUsedMb
        << " vramBudgetMB=" << vramBudgetMb
        << " slDlssMB=" << toMb(static_cast<size_t>(g_slVramDlssBytes.load()))
        << " slRrdMB=" << toMb(static_cast<size_t>(g_slVramDlssdBytes.load()))
        << " slFgMB=" << toMb(static_cast<size_t>(g_slVramDlssgBytes.load()))
        << " slNgxMB=" << toMb(static_cast<size_t>(
               g_slVramDlssBytes.load() +
               g_slVramDlssdBytes.load() +
               g_slVramDlssgBytes.load()))
        << " | meshSec=" << meshSectionCount
        << " meshMB=" << toMb(meshBytes)
        << " rawMB=" << toMb(rawBytes)
        << " digests=" << digestCount
        << " omm=" << ommInputCount
        << " ommMB=" << toMb(ommBytes)
        << " dirty=" << dirtyKeyCount
        << " lightSec=" << lightSectionCount
        << " lights=" << lightCount
        << " lightMB=" << toMb(lightBytes)
        << " queued=" << capturedCount
        << " blas=" << blasCount
        << " asBlasMB=" << toMb(asBlasBytes)
        << " asTrueMB=" << toMb(asTrueBytes)
        << " asChunkMB=" << toMb(asChunkBytes)
        << " asWasteMB="
        << toMb(
               asChunkBytes > asTrueBytes
                   ? asChunkBytes - asTrueBytes
                   : 0u
           )
        << " voxy=" << voxySourceCount
        << " slabKeptMB=" << toMb(g_sectionMemoryPoolKeptBytes)
        << " slabAllocMB=" << toMb(g_sectionMemoryAllocateBytes)
        << " slabFreeMB=" << toMb(g_sectionMemoryFreeBytes)
        << " vtxStage=" << vtxStageCount << "/" << toMb(vtxStageBytes)
        << "MB(inflight=" << vtxStageInFlight << ")"
        << " blasScratch=" << blasScratchCount << "/" << toMb(blasScratchBytes)
        << "MB(inflight=" << blasScratchInFlight << ")"
        << " | vkAllocMB=" << toMb(vkMem.allocBytes)
        << " vkFreeMB=" << toMb(vkMem.freeBytes)
        << " vkLiveMB=" << toMb(vkMem.liveBytes)
        << " vkLiveDeviceMB=" << toMb(vkMem.liveDeviceLocalBytes)
        << " vkLiveHostMB=" << toMb(vkMem.liveHostBytes)
        << " vkLivePeakMB=" << toMb(vkMem.livePeakBytes)
        << " vkLiveN=" << vkMem.liveCount
        << " vkAllocN=" << vkMem.allocCount
        << " vkFreeN=" << vkMem.freeCount
        << " vkFreeUnknownN=" << vkMem.freeUnknownCount
        << " | obj: descPool="
        << g_rtxMemObj.descPool.load(std::memory_order_relaxed)
        << " descSet="
        << g_rtxMemObj.descSet.load(std::memory_order_relaxed)
        << " cmdPool="
        << g_rtxMemObj.cmdPool.load(std::memory_order_relaxed)
        << " cmdBuf="
        << g_rtxMemObj.cmdBuf.load(std::memory_order_relaxed)
        << " layout="
        << g_rtxMemObj.layout.load(std::memory_order_relaxed)
        << " shader="
        << g_rtxMemObj.shader.load(std::memory_order_relaxed)
        << " pipe="
        << g_rtxMemObj.pipe.load(std::memory_order_relaxed)
        << " view="
        << g_rtxMemObj.view.load(std::memory_order_relaxed)
        << " sampler="
        << g_rtxMemObj.sampler.load(std::memory_order_relaxed)
        << " cmdBufFree="
        << g_rtxMemObj.cmdBufFree.load(std::memory_order_relaxed)
        << " fence="
        << g_rtxMemObj.fence.load(std::memory_order_relaxed)
        << " semaphore="
        << g_rtxMemObj.semaphore.load(std::memory_order_relaxed)
        << " queryPool="
        << g_rtxMemObj.queryPool.load(std::memory_order_relaxed)
        << " asCreate="
        << g_rtxMemObj.asCreate.load(std::memory_order_relaxed)
        << " asDestroy="
        << g_rtxMemObj.asDestroy.load(std::memory_order_relaxed)
        << " asBuildCmd="
        << g_rtxMemObj.asBuildCmd.load(std::memory_order_relaxed)
        << " mmCreate="
        << g_rtxMemObj.mmCreate.load(std::memory_order_relaxed)
        << " mmDestroy="
        << g_rtxMemObj.mmDestroy.load(std::memory_order_relaxed)
        << " | churnTop:" << vkChurn;

    RTLog(report.str());

    rtLogSectionBucketStats();
}
