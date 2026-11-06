// Orion backend — PipelineCache 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

std::string ComputeRTPipelineCacheKey()
{
    const std::string directory =
        GetRTXBackendDirectory();

    if(directory.empty())
    {
        return {};
    }

    uint64_t hash = 1469598103934665603ull;

    const auto fold = [&hash](const void* data, size_t size)
    {
        const unsigned char* bytes =
            static_cast<const unsigned char*>(data);

        for(size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };

    // 2026-11-02：A/B 用盐。
    //
    // 现状数据（[PERF PIPE] kind=rt）：同一份 SPV 命中缓存 182.8 ms；改 SPV 后
    // 全量编译 519 772 / 874 477 / 1 064 060 ms（8m40s ~ 17m44s）。做一次对照实验
    // 若必须改 shader 来换 key，实验成本就是 10~20 分钟起步，无法迭代。
    // 这里允许用环境变量强制一个独立缓存键，从而**不改任何 SPV**复测冷编：
    //   ORION_PC_KEY_SALT=noopt   → 新 key，必然走全量 RT 编译
    //   ORION_PC_KEY_SALT=plain   → 又一新 key
    // 与 ORION_RT_PIPE_NO_OPT / ORION_RT_PIPE_NO_CACHE 组合即可做矩阵。
    // 不设该变量时行为逐位不变（空串也视为未设）。
    {
        const char* keySalt = std::getenv("ORION_PC_KEY_SALT");

        if(keySalt != nullptr && keySalt[0] != '\0')
        {
            static const char saltTag[] = "ORION_PC_KEY_SALT:";

            fold(saltTag, sizeof(saltTag) - 1u);
            fold(keySalt, std::strlen(keySalt));
        }
    }

    for(const char* relative : RT_PIPELINE_SPV_FILES)
    {
        fold(relative, std::strlen(relative));

        const std::string fullPath =
            directory + "\\" + relative;

        std::ifstream f(
            fullPath,
            std::ios::binary |
            std::ios::ate
        );

        if(!f)
        {
            continue;
        }

        const std::streamsize size = f.tellg();

        f.seekg(0, std::ios::beg);

        std::vector<char> bytes(
            size > 0
                ? static_cast<size_t>(size)
                : 0u
        );

        if(!bytes.empty())
        {
            f.read(
                bytes.data(),
                static_cast<std::streamsize>(bytes.size())
            );

            fold(bytes.data(), bytes.size());
        }
    }

    char text[32] = {};

    std::snprintf(
        text,
        sizeof(text),
        "%016llx",
        static_cast<unsigned long long>(hash)
    );

    return std::string(text);
}
std::string GetPipelineCacheFilePath()
{
    static const std::string cachedPath = []() -> std::string
    {
        const std::string directory =
            GetRTXBackendDirectory();

        if(directory.empty())
        {
            return {};
        }

        const std::string key =
            ComputeRTPipelineCacheKey();

        if(key.empty())
        {
            return directory + "\\rtx_pipeline_cache.bin";
        }

        return
            directory +
            "\\rtx_pipeline_cache_" +
            key +
            ".bin";
    }();

    return cachedPath;
}

// 目录里除 keepPath 之外、最近的 maxCount 份 rtx_pipeline_cache*.bin（mtime 降序）。
std::vector<std::string> FindNewestOtherPipelineCaches(
    const std::string& keepPath,
    uint32_t maxCount
)
{
    std::vector<std::string> out;

    const std::string directory = GetRTXBackendDirectory();

    if(directory.empty() || maxCount == 0u)
    {
        return out;
    }

    const std::string pattern =
        directory + "\\rtx_pipeline_cache*.bin";

    WIN32_FIND_DATAA entry = {};
    HANDLE search =
        FindFirstFileA(pattern.c_str(), &entry);

    if(search == INVALID_HANDLE_VALUE)
    {
        return out;
    }

    struct Candidate
    {
        uint64_t writeTime = 0u;
        size_t size = 0u;
        std::string path;
    };

    std::vector<Candidate> candidates;

    do
    {
        if((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        const std::string candidatePath =
            directory + "\\" + entry.cFileName;

        if(_stricmp(candidatePath.c_str(), keepPath.c_str()) == 0)
        {
            continue;
        }

        const size_t size =
            (static_cast<size_t>(entry.nFileSizeHigh) << 32) |
            static_cast<size_t>(entry.nFileSizeLow);

        if(size == 0u)
        {
            continue;
        }

        Candidate candidate;
        candidate.writeTime =
            (static_cast<uint64_t>(entry.ftLastWriteTime.dwHighDateTime) << 32) |
            static_cast<uint64_t>(entry.ftLastWriteTime.dwLowDateTime);
        candidate.size = size;
        candidate.path = candidatePath;

        candidates.push_back(std::move(candidate));
    }
    while(FindNextFileA(search, &entry));

    FindClose(search);

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            return a.writeTime > b.writeTime;
        }
    );

    size_t budget = PIPELINE_CACHE_SEED_BUDGET_BYTES;

    for(const Candidate& candidate : candidates)
    {
        if(out.size() >= maxCount)
        {
            break;
        }

        if(candidate.size > budget)
        {
            continue;
        }

        budget -= candidate.size;
        out.push_back(candidate.path);
    }

    return out;
}

// 读一个缓存文件到 outData（0 字节或读失败返回 false）。与 getSharedPipelineCache
// 里读当前键文件用的是同一套逻辑，抽出来共用。
bool LoadPipelineCacheFile(
    const std::string& path,
    std::vector<uint8_t>& outData)
{
    FILE* file = fopen(path.c_str(), "rb");

    if(file == nullptr)
    {
        return false;
    }

    if(fseek(file, 0, SEEK_END) == 0)
    {
        const long size = ftell(file);

        if(size > 0 &&
           fseek(file, 0, SEEK_SET) == 0)
        {
            outData.resize(static_cast<size_t>(size));

            const size_t read = fread(
                outData.data(),
                1,
                outData.size(),
                file
            );

            outData.resize(read);
        }
    }

    fclose(file);

    return !outData.empty();
}

// Deletes every rtx_pipeline_cache*.bin except the one this session uses.  That
// includes the unsuffixed name Perf-PC.1/PC.2 wrote, which is how the 78.8 MB
// blob from the keyed build's first launch gets cleared out.
//
// 2026-10-07（Perf-PC.4）：**保留最新的那个旧键文件**，它是下一版缓存的增量种子。
void DeleteStalePipelineCaches()
{
    const std::string keepPath =
        GetPipelineCacheFilePath();

    const std::string directory =
        GetRTXBackendDirectory();

    if(keepPath.empty() || directory.empty())
    {
        return;
    }

    const std::string pattern =
        directory + "\\rtx_pipeline_cache*.bin";

    WIN32_FIND_DATAA entry = {};

    HANDLE search =
        FindFirstFileA(pattern.c_str(), &entry);

    if(search == INVALID_HANDLE_VALUE)
    {
        return;
    }

    size_t removedCount = 0;
    size_t removedBytes = 0;

    const std::vector<std::string> seedKeepPaths =
        FindNewestOtherPipelineCaches(keepPath, PIPELINE_CACHE_KEEP_SEEDS);

    do
    {
        if((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        const std::string candidate =
            directory + "\\" + entry.cFileName;

        if(_stricmp(candidate.c_str(), keepPath.c_str()) == 0)
        {
            continue;
        }

        bool keepAsSeed = false;

        for(const std::string& seedPath : seedKeepPaths)
        {
            if(_stricmp(candidate.c_str(), seedPath.c_str()) == 0)
            {
                keepAsSeed = true;
                break;
            }
        }

        if(keepAsSeed)
        {
            continue;
        }

        const size_t size =
            (static_cast<size_t>(entry.nFileSizeHigh) << 32) |
            static_cast<size_t>(entry.nFileSizeLow);

        if(DeleteFileA(candidate.c_str()))
        {
            ++removedCount;
            removedBytes += size;
        }
    }
    while(FindNextFileA(search, &entry));

    FindClose(search);

    if(removedCount > 0)
    {
        fprintf(
            stderr,
            "[PERF PC.3] stale pipeline caches removed count=%zu bytes=%zu keep=%s\n",
            removedCount,
            removedBytes,
            keepPath.c_str()
        );

        fflush(stderr);

        RTLog(
            "[PERF PC.3] stale pipeline caches removed count=" +
            std::to_string(removedCount) +
            " bytes=" +
            std::to_string(removedBytes)
        );
    }
}
VkPipelineCache getSharedPipelineCache()
{
    if(g_pipelineCache != VK_NULL_HANDLE)
    {
        return g_pipelineCache;
    }

    if(g_device == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    std::vector<uint8_t> initialData;

    const std::string path =
        GetPipelineCacheFilePath();

    DeleteStalePipelineCaches();

    if(!path.empty())
    {
        const auto cacheLoadStart =
            std::chrono::steady_clock::now();

        LoadPipelineCacheFile(path, initialData);

        // 2026-11-02 P0（冷编分解）：加载可能是"0 字节新建"，也可能是
        // "96 MiB 读盘 + 驱动校验"，不单独计时就分不清启动时间是花在这里
        // 还是花在 vkCreate*Pipelines 的编译上。
        OrionPipelineLogCachePhase(
            "load",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cacheLoadStart
            ).count(),
            initialData.size(),
            path.c_str()
        );
    }

    std::vector<std::string> mergeSeedPaths;

    if(initialData.empty() && !path.empty())
    {
        const std::vector<std::string> seedPaths =
            FindNewestOtherPipelineCaches(path, PIPELINE_CACHE_KEEP_SEEDS);

        if(!seedPaths.empty())
        {
            std::vector<uint8_t> seedData;

            if(LoadPipelineCacheFile(seedPaths[0], seedData))
            {
                initialData.swap(seedData);

                fprintf(
                    stderr,
                    "[PERF PC.4] incremental seed from %s bytes=%zu\n",
                    seedPaths[0].c_str(),
                    initialData.size()
                );

                fflush(stderr);

                RTLog(
                    "[PERF PC.4] incremental seed bytes=" +
                    std::to_string(initialData.size())
                );

                for(size_t i = 1u; i < seedPaths.size(); ++i)
                {
                    mergeSeedPaths.push_back(seedPaths[i]);
                }
            }
        }
    }

    if(initialData.size() > PIPELINE_CACHE_MAX_RESTORE_BYTES)
    {
        fprintf(
            stderr,
            "[PERF PC.3] pipeline cache oversized, starting empty bytes=%zu path=%s\n",
            initialData.size(),
            path.c_str()
        );

        fflush(stderr);

        RTLog(
            "[PERF PC.3] pipeline cache oversized, starting empty bytes=" +
            std::to_string(initialData.size())
        );

        initialData.clear();

        DeleteFileA(path.c_str());
    }

    VkPipelineCacheCreateInfo createInfo{
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
    };

    createInfo.initialDataSize = initialData.size();
    createInfo.pInitialData =
        initialData.empty()
            ? nullptr
            : initialData.data();

    VkResult result = vkCreatePipelineCache(
        g_device,
        &createInfo,
        nullptr,
        &g_pipelineCache
    );

    if(result != VK_SUCCESS && !initialData.empty())
    {
        g_pipelineCache = VK_NULL_HANDLE;
        initialData.clear();

        VkPipelineCacheCreateInfo emptyInfo{
            VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
        };

        result = vkCreatePipelineCache(
            g_device,
            &emptyInfo,
            nullptr,
            &g_pipelineCache
        );
    }

    if(result != VK_SUCCESS)
    {
        g_pipelineCache = VK_NULL_HANDLE;

        fprintf(
            stderr,
            "[PERF PC.1] vkCreatePipelineCache failed result=%d\n",
            static_cast<int>(result)
        );

        fflush(stderr);

        return VK_NULL_HANDLE;
    }

    if(!initialData.empty())
    {
        fprintf(
            stderr,
            "[PERF PC.1] pipeline cache restored bytes=%zu path=%s\n",
            initialData.size(),
            path.c_str()
        );

        RTLog(
            "[PERF PC.1] pipeline cache restored bytes=" +
            std::to_string(initialData.size())
        );

        // 2026-11-06：把"已落盘字节数"记成刚恢复的那一份。
        //
        // 否则 g_pipelineCachePersistedBytes 一直是 0，于是启动后第一次
        // maybePersistSharedPipelineCache() 必然把**与磁盘上完全相同**的 blob
        // 再原样写一遍。实测（18:10:46 [PERF PC.1] saved bytes=184033829 ms=120）
        // 就是这种空写：大小与 restored 完全一致，120ms 纯浪费。
        // 判据与 maybePersist 里已有的去重口径相同（只比大小，见本文件 :751）。
        // 恢复之后若又创建了新管线（K1/K2、GI-half），blob 大小会变，仍会正常落盘。
        g_pipelineCachePersistedBytes = initialData.size();
    }
    else
    {
        fprintf(
            stderr,
            "[PERF PC.1] pipeline cache created empty path=%s\n",
            path.empty() ? "<none>" : path.c_str()
        );

        RTLog(
            "[PERF PC.1] pipeline cache created empty path=" +
            (path.empty() ? std::string("<none>") : path)
        );
    }

    if(!mergeSeedPaths.empty() && g_pipelineCache != VK_NULL_HANDLE)
    {
        const auto cacheMergeStart =
            std::chrono::steady_clock::now();

        size_t mergedCount = 0u;
        size_t mergedBytes = 0u;

        for(const std::string& seedPath : mergeSeedPaths)
        {
            std::vector<uint8_t> seedData;

            if(!LoadPipelineCacheFile(seedPath, seedData) || seedData.empty())
            {
                continue;
            }

            VkPipelineCacheCreateInfo seedInfo{
                VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
            };

            seedInfo.initialDataSize = seedData.size();
            seedInfo.pInitialData = seedData.data();

            VkPipelineCache seedCache = VK_NULL_HANDLE;

            if(vkCreatePipelineCache(
                   g_device,
                   &seedInfo,
                   nullptr,
                   &seedCache) != VK_SUCCESS ||
               seedCache == VK_NULL_HANDLE)
            {
                continue;
            }

            if(vkMergePipelineCaches(
                   g_device,
                   g_pipelineCache,
                   1u,
                   &seedCache) == VK_SUCCESS)
            {
                ++mergedCount;
                mergedBytes += seedData.size();
            }

            vkDestroyPipelineCache(g_device, seedCache, nullptr);
        }

        if(mergedCount > 0u)
        {
            fprintf(
                stderr,
                "[PERF PC.5] merged extra seeds count=%zu bytes=%zu\n",
                mergedCount,
                mergedBytes
            );

            fflush(stderr);

            RTLog(
                "[PERF PC.5] merged extra seeds count=" +
                std::to_string(mergedCount) +
                " bytes=" +
                std::to_string(mergedBytes)
            );
        }

        // 2026-11-02 P0：merge 是"把几份大 blob 合进当前 cache"，驱动侧可能很贵；
        // count=0 也要打，否则会误以为没走这条路径。
        OrionPipelineLogCachePhase(
            "merge",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cacheMergeStart
            ).count(),
            mergedBytes,
            (std::to_string(mergedCount) + " seeds").c_str()
        );
    }

    fflush(stderr);

    return g_pipelineCache;
}
void persistSharedPipelineCache()
{
    if(g_pipelineCache == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
    {
        return;
    }

    size_t blobSize = 0;

    if(vkGetPipelineCacheData(
           g_device,
           g_pipelineCache,
           &blobSize,
           nullptr) != VK_SUCCESS ||
       blobSize == 0)
    {
        return;
    }

    std::vector<uint8_t> blob(blobSize);

    if(vkGetPipelineCacheData(
           g_device,
           g_pipelineCache,
           &blobSize,
           blob.data()) != VK_SUCCESS)
    {
        return;
    }

    const std::string path =
        GetPipelineCacheFilePath();

    if(path.empty())
    {
        return;
    }

    // 2026-11-02 P0（冷编分解）：96 MiB 的 blob 每次落盘都要计一次 ——
    // 这条路径在 RT 管线建完（Device.cpp）与每 1800 帧都会走，写盘时间不能算进"编译"。
    const auto cacheSaveStart =
        std::chrono::steady_clock::now();

    FILE* file = fopen(path.c_str(), "wb");

    if(file)
    {
        const size_t written = fwrite(
            blob.data(),
            1,
            blobSize,
            file
        );

        fclose(file);

        const double cacheSaveMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cacheSaveStart
            ).count();

        fprintf(
            stderr,
            "[PERF PC.1] pipeline cache saved bytes=%zu ms=%.1f\n",
            written,
            cacheSaveMilliseconds
        );

        RTLog(
            "[PERF PC.1] pipeline cache saved bytes=" +
            std::to_string(written) +
            " ms=" +
            std::to_string(static_cast<long long>(cacheSaveMilliseconds))
        );
    }
    else
    {
        fprintf(
            stderr,
            "[PERF PC.1] pipeline cache save failed path=%s\n",
            path.c_str()
        );

        RTLog(
            "[PERF PC.1] pipeline cache save failed path=" +
            path
        );
    }

    fflush(stderr);
}
void maybePersistSharedPipelineCache()
{
    if(g_pipelineCachePersistCountdown > 0u)
    {
        --g_pipelineCachePersistCountdown;

        return;
    }

    g_pipelineCachePersistCountdown =
        PIPELINE_CACHE_PERSIST_INTERVAL;

    if(g_pipelineCache == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
    {
        return;
    }

    size_t blobSize = 0;

    if(vkGetPipelineCacheData(
           g_device,
           g_pipelineCache,
           &blobSize,
           nullptr) != VK_SUCCESS ||
       blobSize == 0 ||
       blobSize == g_pipelineCachePersistedBytes)
    {
        return;
    }

    persistSharedPipelineCache();

    g_pipelineCachePersistedBytes = blobSize;
}
void releaseSharedPipelineCache()
{
    if(g_pipelineCache != VK_NULL_HANDLE)
    {
        persistSharedPipelineCache();

        if(g_device != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(
                g_device,
                g_pipelineCache,
                nullptr
            );
        }

        g_pipelineCache = VK_NULL_HANDLE;
        g_pipelineCachePersistedBytes = 0;
        g_pipelineCachePersistCountdown = 60;
    }
}
