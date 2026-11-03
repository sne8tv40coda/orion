// Orion backend — Shaders 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

VkDeviceAddress getBufferDeviceAddress(VkDevice d,VkBuffer b){
    auto fn=reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(d,"vkGetBufferDeviceAddress"));
    if(!fn)throw std::runtime_error("vkGetBufferDeviceAddress unavailable");

    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer=b;

    VkDeviceAddress a=fn(d,&i);


    if(!a)throw std::runtime_error("Invalid buffer device address");

    return a;
}

// 2026-10-27：读取**非 SPIR-V** 的二进制资源（目前只有 128x128x64 的蓝噪声表）。
// 不能复用 loadSpv：那里有 SPIR-V magic 校验，会把 1 MB 的 R8 数据当坏着色器拒掉
// （实测日志："Invalid SPIR-V magic: ...blue_noise_128x128x64.r8"）。
std::vector<uint8_t> loadBinaryFile(const char* path)
{
    const std::string resolvedPath = ResolveRTXRuntimePath(path);

    std::ifstream file(resolvedPath, std::ios::binary | std::ios::ate);

    if(!file)
    {
        throw std::runtime_error(std::string("Failed to open: ") + resolvedPath);
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size > 0 ? size : 0));

    if(size > 0 &&
       !file.read(reinterpret_cast<char*>(data.data()), size))
    {
        throw std::runtime_error(std::string("Failed to read: ") + resolvedPath);
    }

    return data;
}
std::vector<uint8_t> loadSpv(const char* path)
{
    const std::string resolvedPath =
        ResolveRTXRuntimePath(
            path
        );


    std::ifstream f(
        resolvedPath,
        std::ios::binary |
        std::ios::ate
    );


    if(!f)
    {
        std::cerr
            << "[SPV] FAILED OPEN requested="
            << (path ? path : "<null>")
            << " resolved="
            << resolvedPath
            << "\n";


        throw std::runtime_error(
            std::string("Failed shader: ") +
            resolvedPath
        );
    }


    const std::streamsize size =
        f.tellg();


    if(size <= 0)
    {
        throw std::runtime_error(
            std::string("Invalid/empty shader: ") +
            resolvedPath
        );
    }


    std::cout
        << "[SPV] LOAD requested="
        << (path ? path : "<null>")
        << " resolved="
        << resolvedPath
        << " size="
        << size
        << "\n";


    f.seekg(
        0,
        std::ios::beg
    );


    std::vector<uint8_t> data(
        static_cast<size_t>(
            size
        )
    );


    if(
        !f.read(
            reinterpret_cast<char*>(
                data.data()
            ),
            size
        )
    )
    {
        throw std::runtime_error(
            std::string("Failed reading shader: ") +
            resolvedPath
        );
    }


    if(data.size() < sizeof(uint32_t))
    {
        throw std::runtime_error(
            std::string("Shader too small: ") +
            resolvedPath
        );
    }


    const uint32_t magic =
        *reinterpret_cast<const uint32_t*>(
            data.data()
        );


    if(magic != 0x07230203u)
    {
        throw std::runtime_error(
            std::string("Invalid SPIR-V magic: ") +
            resolvedPath
        );
    }


    // 2026-11-02 P0（冷编分解）：登记 SPV 内容哈希 -> 请求路径。之后管线创建日志
    // 经 module -> 哈希 -> 路径 反查，就能把每条 ms 对到具体 .spv 文件。
    // 只做观测，不改变加载/校验行为。
    OrionPipelineRegisterSpvName(
        OrionPipelineSpvHash(data.data(), data.size()),
        (path != nullptr) ? path : "<null>"
    );


    return data;
}
VkShaderModule createShaderModule(VkDevice d,const std::vector<uint8_t>& code){
    if(code.empty()||(code.size()%4)!=0)throw std::runtime_error("Invalid SPIR-V bytecode");
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=code.size();ci.pCode=reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m=VK_NULL_HANDLE;check(vkCreateShaderModule(d,&ci,nullptr,&m),"Failed to create shader module");return m;
}
