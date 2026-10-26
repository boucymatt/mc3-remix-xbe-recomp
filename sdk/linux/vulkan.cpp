// Native Vulkan command recording for the console GPU state (no OpenGL
// translation layer).
#include "app/app.h"
#include "core/log.h"
#include "draw_constants.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "platform.h"
#include "vulkan_shader.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace gpu {
namespace {
using namespace linuxgl;
struct PerfScope {
    uint64_t &total;
    std::chrono::steady_clock::time_point started;
    static bool Enabled() {
        static const bool enabled = std::getenv("MC3_PERF") != nullptr;
        return enabled;
    }
    explicit PerfScope(uint64_t &value) : total(value) {
        if (Enabled())
            started = std::chrono::steady_clock::now();
    }
    ~PerfScope() {
        if (Enabled())
            total += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                         .count();
    }
};
void Check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " (VkResult " + std::to_string(r) + ")");
}
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{}, sample_view{};
    VkFormat format{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 1, h = 1, z = 1, levels = 1, layers = 1;
    bool depth = false;
    TextureDesc desc{};
    bool written = false;
    uint64_t epoch = 0, signature = 0, checked_frame = ~0ull;
};
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    uint8_t *data{};
    VkDeviceSize size{}, used{};
};
uint64_t TargetKey(const TextureDesc &d) {
    return uint64_t(d.data) | (uint64_t(d.width) << 32) | (uint64_t(d.height) << 46) |
           (uint64_t(Format(d.format).depth) << 63);
}
VkCompareOp Compare(uint32_t x) { return VkCompareOp(CompareIndex(x)); }
VkBlendFactor Blend(uint32_t x) {
    switch (x) {
    case 0:
        return VK_BLEND_FACTOR_ZERO;
    case 1:
        return VK_BLEND_FACTOR_ONE;
    case 0x300:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:
        return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003:
        return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:
        throw std::runtime_error("Unsupported Vulkan blend factor " + std::to_string(x));
    }
}
VkBlendOp BlendOp(uint32_t x) {
    switch (x) {
    case 0x800A:
        return VK_BLEND_OP_SUBTRACT;
    case 0x800B:
    case 0xF005:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007:
        return VK_BLEND_OP_MIN;
    case 0x8008:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}
VkStencilOp Stencil(uint32_t x) {
    switch (x) {
    case 0:
        return VK_STENCIL_OP_ZERO;
    case 0x1E01:
        return VK_STENCIL_OP_REPLACE;
    case 0x1E02:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A:
        return VK_STENCIL_OP_INVERT;
    case 0x8507:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:
        return VK_STENCIL_OP_KEEP;
    }
}
VkSamplerAddressMode Address(uint32_t x) {
    switch (x) {
    case 1:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
class Vulkan final : public Backend {
  public:
    bool Init(uint32_t, uint32_t) override { return true; }
    void Clear(const State &, uint32_t, const uint8_t *, uint32_t, uint32_t, float, uint32_t) override;
    void Draw(const State &, uint32_t, uint32_t, uint32_t, const uint8_t *) override;
    void Copy(const Surface &, const Surface &, uint32_t, uint32_t, uint32_t) override;
    void Present(const Surface &) override;
    void Sync(uint32_t, uint32_t) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override {
        Guard([&] { Submit(); });
    }
    void SetGamma(const uint8_t *ramp) override {
        Guard([&] { std::memcpy(gamma_, ramp, 768); });
    }
    void ClearReport() override {
        Guard([&] {
            ResolveQueries();
            samples_ = 0;
        });
    }
    void GetReport(uint32_t report) override {
        Guard([&] {
            ResolveQueries();
            kernel::W32(report, uint32_t(frame_));
            kernel::W32(report + 4, 0);
            kernel::W32(report + 8, uint32_t(std::min<double>(std::round(samples_), 0xffffffff)));
            std::atomic_thread_fence(std::memory_order_release);
            kernel::W32(report + 12, 0);
        });
    }

  private:
    template <class F> void Guard(F &&f) {
        if (failed_)
            return;
        try {
            if (!ready_)
                Ready();
            f();
        } catch (const std::exception &e) {
            LOGE("Native Vulkan: %s", e.what());
            failed_ = true;
            linuxapp::exit_code = 1;
            linuxapp::stop = true;
        }
    }
    void Ready();
    void Swapchain();
    void Begin();
    void EndRender() {
        if (rendering_) {
            vkCmdEndRendering(cmd_);
            rendering_ = false;
        }
    }
    void Submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    Buffer MakeBuffer(VkDeviceSize bytes);
    VkDeviceSize Upload(const void *data, size_t size, size_t align = 16);
    Image MakeImage(uint32_t w, uint32_t h, VkFormat format, uint32_t levels = 1, uint32_t layers = 1, uint32_t z = 1,
                    bool cube = false);
    void Barrier(Image &image, VkImageLayout layout, bool force = false);
    void UploadImage(Image &, const void *, size_t, uint32_t level = 0, uint32_t layer = 0,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    void Blit(Image &, Image &, const int32_t *, const int32_t *);
    Image *Target(const Surface &);
    void UploadTarget(Image &);
    void Targets(const State &);
    void Render(Image *color, Image *depth);
    Image *Texture(const State &, int, TexInfo &, DrawConstants &);
    VkSampler Sampler(const State &, int, const TexInfo &, uint32_t levels);
    VkShaderModule Shader(const std::string &, bool);
    VkPipeline Pipeline(const State &, const Assembled &, VkShaderModule, VkShaderModule, bool negative = false,
                        VkFormat presentation = VK_FORMAT_UNDEFINED);
    VkDescriptorSet Descriptors(const State &, const DrawConstants &, Image *const *, const VkSampler *);
    void Readback(Image &);
    void ResolveQueries();
    uint32_t LightEffect(const TextureDesc &);
    bool Skip(const State &, const Assembled &, uint32_t, uint32_t);
    int Scale(int x) const { return int(std::lround(x * (app::Get().render_height / 480.0))); }
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkQueue queue_{};
    uint32_t family_{};
    VkSurfaceKHR surface_{};
    VkSwapchainKHR swapchain_{};
    VkExtent2D extent_{};
    VkFormat swap_format_{};
    std::vector<Image> swap_images_;
    std::vector<VkSemaphore> present_semaphores_;
    VkSemaphore acquired_{};
    VkFence fence_{};
    VkCommandPool commands_{};
    VkCommandBuffer cmd_{};
    VkDescriptorSetLayout set_layout_{};
    VkPipelineLayout pipeline_layout_{};
    VkDescriptorPool descriptors_{};
    VkPipelineCache pipeline_cache_{};
    VkQueryPool queries_{};
    VkPhysicalDeviceProperties properties_{};
    Buffer arena_{};
    bool ready_ = false, failed_ = false, recording_ = false, rendering_ = false;
    Image *rt_ = nullptr, *zs_ = nullptr, *render_color_ = nullptr, *render_depth_ = nullptr;
    Image white_, gamma_image_;
    uint8_t gamma_[768]{};
    Assembled assembled_;
    uint32_t width_ = 0, height_ = 0, query_count_ = 0;
    std::vector<double> query_scales_;
    double samples_ = 0;
    uint64_t frame_ = 0, draws_ = 0, draw_ns_ = 0, gpu_wait_ns_ = 0;
    std::unordered_map<uint64_t, Image> targets_, textures_, feedback_;
    std::unordered_map<uint64_t, VkShaderModule> shaders_, console_shaders_;
    std::unordered_map<std::string, VkPipeline> pipelines_;
    std::unordered_map<std::string, VkSampler> samplers_;
    std::chrono::steady_clock::time_point report_time_;
};
uint32_t Vulkan::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(physical_, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("No compatible Vulkan memory type");
}
Buffer Vulkan::MakeBuffer(VkDeviceSize size) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateBuffer(device_, &ci, nullptr, &b.buffer), "create upload buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device_, &alloc, nullptr, &b.memory), "allocate upload memory");
    Check(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "bind upload memory");
    Check(vkMapMemory(device_, b.memory, 0, size, 0, reinterpret_cast<void **>(&b.data)), "map upload memory");
    return b;
}
void Vulkan::Ready() {
    uint32_t extension_count = 0;
    auto extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!extensions)
        throw std::runtime_error(SDL_GetError());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MC3 Remix";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    ic.enabledExtensionCount = extension_count;
    ic.ppEnabledExtensionNames = extensions;
    // Opt-in validation uses the installed Khronos layer, never required for
    // users.
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("MC3_VK_VALIDATION")) {
        ic.enabledLayerCount = 1;
        ic.ppEnabledLayerNames = &validation;
    }
    Check(vkCreateInstance(&ic, nullptr, &instance_), "create instance (Vulkan 1.3 required)");
    if (!SDL_Vulkan_CreateSurface(linuxapp::Window(), instance_, nullptr, &surface_))
        throw std::runtime_error(SDL_GetError());
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "enumerate GPUs");
    int best = -1;
    for (auto d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
        std::vector<VkQueueFamilyProperties> q(n);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n, q.data());
        for (uint32_t i = 0; i < n; ++i) {
            VkBool32 present = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && score > best) {
                physical_ = d;
                family_ = i;
                best = score;
            }
        }
    }
    if (!physical_)
        throw std::runtime_error("No Vulkan 1.3 graphics/present device");
    vkGetPhysicalDeviceProperties(physical_, &properties_);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(physical_, &supported);
    if (!f13.dynamicRendering || !supported.features.textureCompressionBC || !supported.features.depthClamp)
        throw std::runtime_error("Device lacks dynamic rendering, BC textures or depth clamp");
    f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = true;
    VkPhysicalDeviceFeatures features{};
    features.textureCompressionBC = true;
    features.depthClamp = true;
    features.fillModeNonSolid = supported.features.fillModeNonSolid;
    features.occlusionQueryPrecise = supported.features.occlusionQueryPrecise;
    features.shaderClipDistance = supported.features.shaderClipDistance;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *swap_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &f13;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qi;
    dc.pEnabledFeatures = &features;
    dc.enabledExtensionCount = 1;
    dc.ppEnabledExtensionNames = &swap_extension;
    Check(vkCreateDevice(physical_, &dc, nullptr, &device_), "create device");
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family_;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    Check(vkCreateCommandPool(device_, &pc, nullptr, &commands_), "create command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = commands_;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device_, &ca, &cmd_), "allocate command buffer");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(device_, &fc, nullptr, &fence_), "create fence");
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vkCreateSemaphore(device_, &sc, nullptr, &acquired_), "create acquire semaphore");
    VkDescriptorSetLayoutBinding bindings[10]{};
    for (uint32_t i = 0; i < 10; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = i < 2   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                     : i < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                             : VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 10;
    sl.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &set_layout_), "create descriptor layout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    Check(vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_), "create pipeline layout");
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 65536}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 16384;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptors_), "create descriptor pool");
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    Check(vkCreatePipelineCache(device_, &cache, nullptr, &pipeline_cache_), "create pipeline cache");
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_OCCLUSION;
    query.queryCount = 8192;
    Check(vkCreateQueryPool(device_, &query, nullptr, &queries_), "create query pool");
    arena_ = MakeBuffer(128ull << 20);
    Swapchain();
    Begin();
    white_ = MakeImage(1, 1, VK_FORMAT_B8G8R8A8_UNORM);
    uint32_t white = 0xffffffff;
    UploadImage(white_, &white, 4);
    gamma_image_ = MakeImage(256, 1, VK_FORMAT_B8G8R8A8_UNORM);
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned i = 0; i < 256; ++i)
            gamma_[c * 256 + i] = i;
    ready_ = true;
    report_time_ = std::chrono::steady_clock::now();
    LOGR("Native Vulkan 1.3 — %s", properties_.deviceName);
}
void Vulkan::Swapchain() {
    Check(vkDeviceWaitIdle(device_), "wait for swapchain");
    for (auto &i : swap_images_)
        vkDestroyImageView(device_, i.view, nullptr);
    swap_images_.clear();
    for (auto semaphore : present_semaphores_)
        vkDestroySemaphore(device_, semaphore, nullptr);
    present_semaphores_.clear();
    VkSurfaceCapabilitiesKHR caps;
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    auto format = formats.front();
    for (auto f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            format = f;
            break;
        }
    swap_format_ = format.format;
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(linuxapp::Window(), &w, &h);
        extent_ = {std::clamp(uint32_t(std::max(1, w)), caps.minImageExtent.width, caps.maxImageExtent.width),
                   std::clamp(uint32_t(std::max(1, h)), caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!app::Get().vsync) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                mode = m;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
                mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount)
        ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        ci.compositeAlpha = VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = true;
    ci.oldSwapchain = swapchain_;
    Check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "create swapchain");
    if (ci.oldSwapchain)
        vkDestroySwapchainKHR(device_, ci.oldSwapchain, nullptr);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images.data());
    for (auto handle : images) {
        Image i;
        i.image = handle;
        i.format = format.format;
        i.w = extent_.width;
        i.h = extent_.height;
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = handle;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = format.format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device_, &v, nullptr, &i.view), "create swapchain view");
        i.sample_view = i.view;
        swap_images_.push_back(i);
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore;
        Check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore), "create image presentation semaphore");
        present_semaphores_.push_back(semaphore);
    }
}
void Vulkan::Begin() {
    Check(vkResetCommandPool(device_, commands_, 0), "reset command pool");
    Check(vkResetDescriptorPool(device_, descriptors_, 0), "reset descriptors");
    arena_.used = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(cmd_, &bi), "begin commands");
    recording_ = true;
    vkCmdResetQueryPool(cmd_, queries_, 0, 8192);
}
void Vulkan::Submit(VkSemaphore wait, VkSemaphore signal) {
    if (!recording_)
        return;
    EndRender();
    Check(vkEndCommandBuffer(cmd_), "end commands");
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd_;
    if (wait) {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.pWaitDstStageMask = &stage;
    }
    if (signal) {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &signal;
    }
    Check(vkQueueSubmit(queue_, 1, &info, fence_), "submit commands");
    {
        PerfScope wait(gpu_wait_ns_);
        Check(vkWaitForFences(device_, 1, &fence_, true, UINT64_MAX), "wait for GPU");
    }
    Check(vkResetFences(device_, 1, &fence_), "reset fence");
    if (query_count_) {
        std::vector<uint64_t> results(query_count_);
        Check(vkGetQueryPoolResults(device_, queries_, 0, query_count_, results.size() * 8, results.data(), 8,
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "read visibility queries");
        for (size_t i = 0; i < results.size(); ++i)
            samples_ += results[i] / query_scales_[i];
        query_count_ = 0;
        query_scales_.clear();
    }
    recording_ = false;
    Begin();
}
VkDeviceSize Vulkan::Upload(const void *data, size_t size, size_t align) {
    const auto offset = (arena_.used + align - 1) / align * align;
    // Do not reset the arena mid-draw: descriptors already refer to earlier
    // allocations.
    if (offset + size > arena_.size)
        throw std::runtime_error("Vulkan frame upload arena exhausted");
    if (data)
        std::memcpy(arena_.data + offset, data, size);
    arena_.used = offset + size;
    return offset;
}
Image Vulkan::MakeImage(uint32_t w, uint32_t h, VkFormat format, uint32_t levels, uint32_t layers, uint32_t z,
                        bool cube) {
    Image i;
    i.w = w;
    i.h = h;
    i.z = z;
    i.levels = levels;
    i.layers = layers;
    i.format = format;
    i.depth = format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = z > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, z};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (i.depth)
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format == VK_FORMAT_B8G8R8A8_UNORM)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    Check(vkCreateImage(device_, &ci, nullptr, &i.image), "create image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, i.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &alloc, nullptr, &i.memory), "allocate image");
    Check(vkBindImageMemory(device_, i.image, i.memory, 0), "bind image");
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = i.image;
    v.format = format;
    v.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : z > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    v.subresourceRange = {i.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    Check(vkCreateImageView(device_, &v, nullptr, &i.sample_view), "create sampled image view");
    i.view = i.sample_view;
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        v.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        Check(vkCreateImageView(device_, &v, nullptr, &i.view), "create depth stencil view");
    }
    return i;
}
void Vulkan::Barrier(Image &i, VkImageLayout layout, bool force) {
    if (i.layout == layout && !force)
        return;
    EndRender();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = i.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.image;
    b.subresourceRange = {i.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, i.levels, 0, i.layers};
    if (i.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        b.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    b.srcAccessMask =
        i.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    i.layout = layout;
}
void Vulkan::UploadImage(Image &i, const void *pixels, size_t bytes, uint32_t level, uint32_t layer,
                         VkImageAspectFlags aspect) {
    Barrier(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = Upload(pixels, bytes, 16);
    c.imageSubresource = {aspect, level, layer, 1};
    c.imageExtent = {std::max(1u, i.w >> level), std::max(1u, i.h >> level), std::max(1u, i.z >> level)};
    vkCmdCopyBufferToImage(cmd_, arena_.buffer, i.image, i.layout, 1, &c);
}
void Vulkan::Blit(Image &from, Image &to, const int32_t *r, const int32_t *p) {
    Barrier(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = {from.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[0] = {r[0], r[1], 0};
    b.srcOffsets[1] = {r[2], r[3], 1};
    b.dstOffsets[0] = {p[0], p[1], 0};
    b.dstOffsets[1] = {p[2], p[3], 1};
    vkCmdBlitImage(cmd_, from.image, from.layout, to.image, to.layout, 1, &b, VK_FILTER_NEAREST);
    if (from.format == VK_FORMAT_D32_SFLOAT_S8_UINT && to.format == from.format) {
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdBlitImage(cmd_, from.image, from.layout, to.image, to.layout, 1, &b, VK_FILTER_NEAREST);
    }
}
Image *Vulkan::Target(const Surface &surface) {
    TextureDesc d;
    if (!DescribeSurface(surface, d) || !d.width || !d.height || d.width > 4096 || d.height > 4096)
        return nullptr;
    auto [it, added] = targets_.try_emplace(TargetKey(d));
    auto &t = it->second;
    if (added) {
        t = MakeImage(std::max(1, Scale(d.width)), std::max(1, Scale(d.height)),
                      Format(d.format).depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_B8G8R8A8_UNORM);
        t.desc = d;
        UploadTarget(t);
    } else if (DirtyEpoch(d.data) != t.epoch) {
        t.desc = d;
        UploadTarget(t);
    }
    return &t;
}
void Vulkan::UploadTarget(Image &t) {
    std::vector<uint8_t> pixels;
    uint32_t pitch;
    DecodeLevel(t.desc, 0, 0, static_cast<const uint8_t *>(kernel::HostPtr(t.desc.data)), nullptr, pixels, pitch);
    // Nearest expansion keeps guest readback and higher resolution attachments
    // consistent.
    std::vector<uint32_t> scaled(size_t(t.w) * t.h);
    for (uint32_t y = 0; y < t.h; ++y)
        for (uint32_t x = 0; x < t.w; ++x)
            std::memcpy(&scaled[y * t.w + x],
                        pixels.data() +
                            4 * (size_t(y) * t.desc.height / t.h * t.desc.width + size_t(x) * t.desc.width / t.w),
                        4);
    UploadImage(t, scaled.data(), scaled.size() * 4, 0, 0,
                t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    if (t.depth) {
        std::vector<uint8_t> stencil(size_t(t.w) * t.h);
        UploadImage(t, stencil.data(), stencil.size(), 0, 0, VK_IMAGE_ASPECT_STENCIL_BIT);
    }
    t.epoch = DirtyEpoch(t.desc.data);
    t.written = false;
}
void Vulkan::Targets(const State &s) {
    rt_ = Target(s.rt);
    zs_ = Target(s.zs);
    auto *extent = rt_ ? rt_ : zs_;
    width_ = extent ? extent->desc.width : 0;
    height_ = extent ? extent->desc.height : 0;
    if (zs_ && (zs_->desc.width != width_ || zs_->desc.height != height_))
        zs_ = nullptr;
}
void Vulkan::Render(Image *color, Image *depth) {
    if (rendering_ && render_color_ == color && render_depth_ == depth)
        return;
    EndRender();
    if (color)
        Barrier(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    if (depth)
        Barrier(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true);
    auto *size = color ? color : depth;
    if (!size)
        throw std::runtime_error("Draw without render target");
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
        z{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = color ? color->view : VK_NULL_HANDLE;
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    z.imageView = depth ? depth->view : VK_NULL_HANDLE;
    z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    z.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    z.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea.extent = {size->w, size->h};
    info.layerCount = 1;
    info.colorAttachmentCount = color ? 1 : 0;
    info.pColorAttachments = &c;
    if (depth) {
        info.pDepthAttachment = &z;
        info.pStencilAttachment = &z;
    }
    vkCmdBeginRendering(cmd_, &info);
    rendering_ = true;
    render_color_ = color;
    render_depth_ = depth;
}
VkShaderModule Vulkan::Shader(const std::string &source, bool vertex) {
    const auto key = Hash(source.data(), source.size(), vertex ? 1 : 2);
    auto [it, added] = shaders_.try_emplace(key);
    if (added) {
        auto words = linuxvk::CompileSpirv(source, vertex);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words.size() * 4;
        ci.pCode = words.data();
        Check(vkCreateShaderModule(device_, &ci, nullptr, &it->second), "create shader module");
    }
    return it->second;
}
VkPipeline Vulkan::Pipeline(const State &s, const Assembled &a, VkShaderModule vs, VkShaderModule ps, bool negative,
                            VkFormat presentation) {
    auto m = [&](uint32_t k) { return s.simple[k / 4]; };
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    VkVertexInputBindingDescription binding{0, a.buffers[0].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t i = 0; i < 16; ++i)
        if (a.regs & (1u << i))
            attributes.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, a.elements[i].offset});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (a.regs) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = attributes.size();
        vi.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = a.topology == Topology::Points  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : a.topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = true;
    raster.lineWidth = 1;
    raster.polygonMode = s.rs[kRsFillMode] == 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.frontFace = s.rs[kRsFrontFace] == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    const auto cull = s.rs[kRsCullMode];
    if (a.topology == Topology::Triangles && (cull == 0x900 || cull == 0x901))
        raster.cullMode = cull == s.rs[kRsFrontFace] ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    raster.depthBiasEnable = m(kMSolidOffsetEnable) != 0;
    raster.depthBiasConstantFactor = Bits(m(kMDepthOffset));
    raster.depthBiasSlopeFactor = Bits(m(kMSlopeScale));
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = zs_ && s.rs[kRsZEnable];
    ds.depthWriteEnable = m(kMZWriteEnable) != 0;
    ds.depthCompareOp = Compare(m(kMZFunc));
    ds.stencilTestEnable = zs_ && s.rs[kRsStencilEnable];
    ds.front = {Stencil(s.rs[kRsStencilFail]),
                Stencil(m(kMStencilPass)),
                Stencil(m(kMStencilZFail)),
                Compare(m(kMStencilFunc)),
                m(kMStencilMask),
                m(kMStencilWriteMask),
                m(kMStencilRef)};
    ds.back = ds.front;
    VkPipelineColorBlendAttachmentState blend{};
    const auto cw = m(kMColorWriteEnable);
    blend.colorWriteMask = ((cw >> 16) & 1) | (((cw >> 8) & 1) << 1) | ((cw & 1) << 2) | (((cw >> 24) & 1) << 3);
    blend.blendEnable = m(kMBlendEnable) != 0;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = Blend(m(kMSrcBlend));
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = Blend(m(kMDestBlend));
    blend.colorBlendOp = blend.alphaBlendOp = BlendOp(m(kMBlendOp));
    if (negative)
        blend.colorBlendOp = blend.alphaBlendOp =
            m(kMBlendOp) == 0xF006 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = rt_ ? 1 : 0;
    cb.pAttachments = &blend;
    Color(m(kMBlendColor), cb.blendConstants);
    VkFormat color = rt_ ? rt_->format : VK_FORMAT_UNDEFINED, depth = zs_ ? zs_->format : VK_FORMAT_UNDEFINED;
    if (presentation != VK_FORMAT_UNDEFINED) {
        color = presentation;
        depth = VK_FORMAT_UNDEFINED;
        cb.attachmentCount = 1;
        ds.depthTestEnable = ds.depthWriteEnable = ds.stencilTestEnable = false;
        raster.cullMode = 0;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.depthBiasEnable = false;
        blend.blendEnable = false;
        blend.colorWriteMask = 15;
    }
    // Serialize only value fields: never hash pointer addresses or uninitialized
    // padding.
    std::string key;
    auto append = [&](const auto &value) { key.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
    append(vs);
    append(ps);
    append(a.regs);
    append(binding.stride);
    for (auto &x : attributes) {
        append(x.location);
        append(x.offset);
    }
    append(ia.topology);
    append(raster.polygonMode);
    append(raster.cullMode);
    append(raster.frontFace);
    append(raster.depthBiasEnable);
    append(raster.depthBiasConstantFactor);
    append(raster.depthBiasSlopeFactor);
    append(ds.depthTestEnable);
    append(ds.depthWriteEnable);
    append(ds.depthCompareOp);
    append(ds.stencilTestEnable);
    append(ds.front);
    append(blend);
    append(cb.blendConstants);
    append(color);
    append(depth);
    auto existing = pipelines_.find(key);
    if (existing != pipelines_.end())
        return existing->second;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = cb.attachmentCount;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = pipeline_layout_;
    VkPipeline result;
    Check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &ci, nullptr, &result), "create graphics pipeline");
    pipelines_.emplace(std::move(key), result);
    return result;
}
VkDescriptorSet Vulkan::Descriptors(const State &s, const DrawConstants &dc, Image *const *images,
                                    const VkSampler *samplers) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptors_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    VkDescriptorSet set;
    Check(vkAllocateDescriptorSets(device_, &alloc, &set), "allocate draw descriptors");
    auto alignment = std::max<VkDeviceSize>(16, properties_.limits.minUniformBufferOffsetAlignment);
    VkDescriptorBufferInfo buffers[] = {{arena_.buffer, Upload(s.vsc.data(), sizeof(s.vsc), alignment), sizeof(s.vsc)},
                                        {arena_.buffer, Upload(&dc, sizeof(dc), alignment), sizeof(dc)}};
    VkDescriptorImageInfo views[4]{}, samples[4]{};
    VkWriteDescriptorSet writes[10]{};
    for (unsigned i = 0; i < 10; ++i) {
        auto &w = writes[i];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = i;
        w.descriptorCount = 1;
        if (i < 2) {
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &buffers[i];
        } else if (i < 6) {
            auto &v = views[i - 2];
            v.imageView = images[i - 2]->sample_view;
            v.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &v;
        } else {
            samples[i - 6].sampler = samplers[i - 6];
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w.pImageInfo = &samples[i - 6];
        }
    }
    vkUpdateDescriptorSets(device_, 10, writes, 0, nullptr);
    return set;
}
VkSampler Vulkan::Sampler(const State &s, int stage, const TexInfo &info, uint32_t levels) {
    auto &t = s.tss[stage];
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = t[kTssMagFilter] == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = t[kTssMinFilter] == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = t[kTssMipFilter] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = Address(t[kTssAddressU]);
    ci.addressModeV = Address(t[kTssAddressV]);
    ci.addressModeW = Address(t[kTssAddressW]);
    ci.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -properties_.limits.maxSamplerLodBias,
                               properties_.limits.maxSamplerLodBias);
    ci.maxLod = t[kTssMipFilter] ? float(levels - 1) : 0;
    ci.compareEnable = info.shadow != 0;
    ci.compareOp = Compare(s.rs[kRsShadowFunc]);
    ci.borderColor = t[kTssBorderColor] == 0xffffffff    ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                     : (t[kTssBorderColor] & 0xff000000) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    std::string key(reinterpret_cast<const char *>(&ci.magFilter),
                    sizeof(ci) - offsetof(VkSamplerCreateInfo, magFilter));
    auto [it, added] = samplers_.try_emplace(key);
    if (added)
        Check(vkCreateSampler(device_, &ci, nullptr, &it->second), "create sampler");
    return it->second;
}
Image *Vulkan::Texture(const State &s, int stage, TexInfo &info, DrawConstants &dc) {
    TextureDesc d;
    Image *image = &white_;
    if (s.textures[stage] && Describe(s.textures[stage], d)) {
        const auto &f = Format(d.format);
        info.kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        info.rect = d.linear;
        info.shadow = f.depth ? (f.bits == 16 ? 2 : 1) : 0;
        dc.texsize[stage][0] = d.width;
        dc.texsize[stage][1] = d.height;
        auto found = targets_.find(TargetKey(d));
        if (found != targets_.end() && !d.cube && d.depth == 1) {
            auto &target = found->second;
            if (DirtyEpoch(d.data) != target.epoch)
                UploadTarget(target);
            image = &target;
            dc.texsize[stage][2] = -.5f / d.width;
            dc.texsize[stage][3] = -.5f / d.height;
            if (image == rt_ || image == zs_) {
                auto &copy = feedback_[TargetKey(d)];
                if (!copy.image)
                    copy = MakeImage(target.w, target.h, target.format);
                int32_t r[] = {0, 0, int32_t(target.w), int32_t(target.h)};
                Blit(target, copy, r, r);
                image = &copy;
            }
        } else {
            const uint32_t layout[] = {d.data,   d.format, d.width,          d.height,          d.depth,
                                       d.levels, d.pitch,  uint32_t(d.cube), uint32_t(d.linear)};
            auto key = Hash(layout, sizeof(layout));
            uint32_t palette[256]{};
            const uint32_t *pal = nullptr;
            if (d.format == 0x0b && s.palettes[stage]) {
                uint32_t va = mem::VirtualFromPhysical(kernel::R32(s.palettes[stage] + 4));
                std::memcpy(palette, kernel::HostPtr(va), sizeof(palette) >> (kernel::R32(s.palettes[stage]) >> 30));
                pal = palette;
                key = Hash(&va, sizeof(va), key);
            }
            auto &t = textures_[key];
            const auto epoch = DirtyEpoch(d.data);
            if (!t.image || t.checked_frame != frame_ || t.epoch != epoch) {
                auto bytes = TotalSize(d);
                if (bytes > (64u << 20))
                    throw std::runtime_error("Oversized guest texture");
                auto *src = static_cast<const uint8_t *>(kernel::HostPtr(d.data));
                auto signature = Hash(src, bytes);
                if (pal)
                    signature = Hash(palette, sizeof(palette), signature);
                if (!t.image || signature != t.signature) {
                    if (!t.image) {
                        auto format = VK_FORMAT_B8G8R8A8_UNORM;
                        if (f.host == Host::BC1)
                            format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                        if (f.host == Host::BC2)
                            format = VK_FORMAT_BC2_UNORM_BLOCK;
                        if (f.host == Host::BC3)
                            format = VK_FORMAT_BC3_UNORM_BLOCK;
                        if (f.depth)
                            format = VK_FORMAT_D32_SFLOAT;
                        t = MakeImage(d.width, d.height, format, d.levels, d.cube ? 6 : 1, d.depth, d.cube);
                        t.desc = d;
                    }
                    std::vector<uint8_t> pixels;
                    uint32_t pitch;
                    for (uint32_t face = 0; face < t.layers; ++face)
                        for (uint32_t level = 0; level < d.levels; ++level) {
                            DecodeLevel(d, level, face, src, pal, pixels, pitch);
                            UploadImage(t, pixels.data(), pixels.size(), level, face,
                                        f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
                        }
                    t.signature = signature;
                }
                t.checked_frame = frame_;
                t.epoch = epoch;
            }
            image = &t;
        }
    }
    Barrier(*image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto &t = s.tss[stage];
    dc.bump[stage][0] = Bits(t[kTssBumpEnvMat00]);
    dc.bump[stage][1] = Bits(t[kTssBumpEnvMat01]);
    dc.bump[stage][2] = Bits(t[kTssBumpEnvMat10]);
    dc.bump[stage][3] = Bits(t[kTssBumpEnvMat11]);
    dc.bumpl[stage][0] = Bits(t[kTssBumpEnvLScale]);
    dc.bumpl[stage][1] = Bits(t[kTssBumpEnvLOffset]);
    return image;
}
uint32_t Vulkan::LightEffect(const TextureDesc &d) {
    struct Signature {
        uint32_t w, h, format, bytes;
        uint64_t hash, effect;
    };
    static_assert(sizeof(Signature) == 32);
    static const auto signatures = [] {
        std::vector<Signature> result;
        std::ifstream file(app::ExeDir() / "mc3_light_glare.bin", std::ios::binary);
        char magic[5]{};
        uint32_t count = 0;
        if (file.read(magic, 5) && std::memcmp(magic, "MC3L2", 5) == 0 &&
            file.read(reinterpret_cast<char *>(&count), 4) && count > 0 && count <= 128) {
            result.resize(count);
            if (!file.read(reinterpret_cast<char *>(result.data()), count * sizeof(Signature)))
                result.clear();
        }
        if (result.empty())
            LOGW("Linux: light-glare signatures missing; run build.sh to enable the "
                 "glare toggle");
        return result;
    }();
    if (!d.data || (d.format != 0x0C && d.format != 0x0F))
        return false;
    const uint32_t bytes = ((d.width + 3) / 4) * ((d.height + 3) / 4) * (d.format == 0x0C ? 8 : 16);
    uint64_t hash = 0;
    bool hashed = false;
    for (const auto &signature : signatures) {
        if (signature.w != d.width || signature.h != d.height || signature.format != d.format ||
            signature.bytes != bytes)
            continue;
        if (!hashed) {
            hash = Hash(kernel::HostPtr(d.data), bytes);
            hashed = true;
        }
        if (hash == signature.hash)
            return uint32_t(signature.effect);
    }
    return false;
}

bool Vulkan::Skip(const State &s, const Assembled &a, uint32_t prim, uint32_t count) {
    // The actual streetlamp shafts are procedural 32-segment cone strips,
    // separate from the archive-backed glow sprites below. Match their
    // generated texture as well as their draw state to retain road lights.
    if (!app::Get().light_streaks && !a.pretransformed && !s.ps && prim == 6 && count == 64 && s.vs_handle == 0x142 &&
        s.rs[kRsZEnable] && !s.simple[kMZWriteEnable / 4] && s.simple[kMBlendEnable / 4] &&
        s.simple[kMSrcBlend / 4] == 0x302 && s.simple[kMDestBlend / 4] == 1 && !s.textures[1] && !s.textures[2] &&
        !s.textures[3]) {
        TextureDesc cone;
        if (Describe(s.textures[0], cone) && cone.data && cone.format == 0x06 && cone.width == 64 &&
            cone.height == 64 && Hash(kernel::HostPtr(cone.data), 64 * 64 * 4) == 0x2905F9F1A0D6C474ull) {
            return true;
        }
    }
    if (!app::Get().motion_blur && a.pretransformed && prim == 6 && count == 4 && s.vs_handle == 0x1C4 && !s.ps &&
        rt_ && zs_ && width_ >= 640 && height_ == 480 && s.viewport.x == 0 && s.viewport.y == 0 &&
        s.viewport.w == width_ && s.viewport.h == height_ && s.textures[0] && !s.textures[1] && !s.textures[2] &&
        !s.textures[3] && s.simple[kMBlendEnable / 4] && s.simple[kMSrcBlend / 4] == 0x302 &&
        s.simple[kMDestBlend / 4] == 0x303) {
        TextureDesc source;
        if (Describe(s.textures[0], source) && source.width * 2 == width_ && source.height * 2 == height_) {
            return true;
        }
    }
    // Glare cards/streaks and translucent lamp cones are separate effects.
    // Identify their complete texture data from the owned archive. Cone
    // meshes also use programmable vertex shaders and alpha blending;
    // restricting them to XYZRHW/additive draws leaves the beams visible.
    // Streetlamp streaks also use Xbox QUADLIST (8), not only triangles.
    const bool light_geometry =
        (a.pretransformed && s.vs_handle == 0x144) || (!a.pretransformed && s.vs_handle == 0x142);
    const uint32_t destination_blend = s.simple[kMDestBlend / 4];
    if (!app::Get().light_streaks && (prim == 5 || prim == 6 || prim == 7 || prim == 8) && s.rs[kRsZEnable] &&
        !s.simple[kMZWriteEnable / 4] && s.simple[kMBlendEnable / 4] && s.simple[kMSrcBlend / 4] == 0x302 &&
        (destination_blend == 1 || destination_blend == 0x303)) {
        TextureDesc light;
        const uint32_t effect = Describe(s.textures[0], light) ? LightEffect(light) : 0;
        if (effect == 2) {
            return true;
        }
        if (effect == 1 && light_geometry && destination_blend == 1) {

            return true;
        }
    }
    return false;
}
void Vulkan::Draw(const State &s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t *indices) {
    Guard([&] {
        PerfScope draw_time(draw_ns_);
        if (!Assemble(s, prim, start, count, indices, assembled_))
            return;
        auto &a = assembled_;
        Targets(s);
        if (!width_ || !height_ || Skip(s, a, prim, count))
            return;
        DrawConstants dc{};
        TexInfo info[4]{};
        Image *images[4];
        VkSampler samplers[4];
        for (int i = 0; i < 4; ++i) {
            images[i] = Texture(s, i, info[i], dc);
            samplers[i] = Sampler(s, i, info[i], images[i]->levels);
        }
        auto pk = MakePixelKey(s, info);
        const auto equation = s.simple[kMBlendOp / 4];
        const bool signed_blend = s.simple[kMBlendEnable / 4] && (equation == 0xF005 || equation == 0xF006) &&
                                  s.simple[kMSrcBlend / 4] == 1 && s.simple[kMDestBlend / 4] == 1;
        if (signed_blend)
            pk.signed_half = 1;
        const bool program = s.vs && !s.vs->program.empty();
        auto fk = MakeFixedKey(s);
        auto vertex_key = Hash(&a.regs, sizeof(a.regs), 1);
        vertex_key = Hash(&a.pretransformed, sizeof(a.pretransformed), vertex_key);
        vertex_key = Hash(&program, sizeof(program), vertex_key);
        vertex_key = program ? Hash(s.vs->program.data(), s.vs->program.size() * 4, vertex_key)
                             : Hash(&fk, sizeof(fk), vertex_key);
        auto [vi, new_vertex] = console_shaders_.try_emplace(vertex_key);
        if (new_vertex)
            vi->second = Shader(program ? VertexProgramHlsl(s.vs->program, a.regs)
                                        : FixedVertexHlsl(a.regs, a.pretransformed, 0, fk),
                                true);
        auto pixel_shader = [&](const PixelKey &key) {
            auto [pi, added] = console_shaders_.try_emplace(Hash(&key, sizeof(key), 2));
            if (added)
                pi->second = Shader(PixelHlsl(key), false);
            return pi->second;
        };
        auto vs = vi->second, ps = pixel_shader(pk);
        FillSurfaceConstants(s, {width_, height_}, dc);
        FillFogConstants(s, program, dc);
        FillCombinerConstants(s, dc);
        if (a.topology == Topology::Points)
            dc.surf[3] = 1;
        if (!program && !a.pretransformed) {
            FillTransformConstants(s, {width_, height_}, dc);
            FillLightConstants(s, fk, dc);
        }
        const auto vb = Upload(a.vertices.data(), a.vertices.size() * 4);
        const auto ib = Upload(a.indices.data(), a.indices.size() * 4);
        auto set = Descriptors(s, dc, images, samplers);
        auto pipeline = Pipeline(s, a, vs, ps);
        Render(rt_, zs_);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &arena_.buffer, &vb);
        vkCmdBindIndexBuffer(cmd_, arena_.buffer, ib, VK_INDEX_TYPE_UINT32);
        VkViewport viewport{0, float(Scale(height_)), float(Scale(width_)), -float(Scale(height_)), 0, 1};
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        int l = s.viewport.x, t = s.viewport.y, r = l + s.viewport.w, b = t + s.viewport.h;
        if (s.scissor_count) {
            l = std::max(l, int(s.scissors[0]));
            t = std::max(t, int(s.scissors[1]));
            r = std::min(r, int(s.scissors[2]));
            b = std::min(b, int(s.scissors[3]));
        }
        l = std::clamp(Scale(l), 0, Scale(width_));
        t = std::clamp(Scale(t), 0, Scale(height_));
        r = std::clamp(Scale(r), l, Scale(width_));
        b = std::clamp(Scale(b), t, Scale(height_));
        VkRect2D scissor{{l, t}, {uint32_t(r - l), uint32_t(b - t)}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        const bool query = s.simple[kMCountEnable / 4] != 0;
        if (query) {
            if (query_count_ >= 8192)
                throw std::runtime_error("Too many visibility queries in one submission");
            vkCmdBeginQuery(cmd_, queries_, query_count_, 0);
        }
        vkCmdDrawIndexed(cmd_, a.indices.size(), 1, 0, 0, 0);
        if (signed_blend) {
            pk.signed_half = 2;
            auto neg = pixel_shader(pk);
            auto p = Pipeline(s, a, vs, neg, true);
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            vkCmdDrawIndexed(cmd_, a.indices.size(), 1, 0, 0, 0);
        }
        if (query) {
            vkCmdEndQuery(cmd_, queries_, query_count_++);
            auto *target = rt_ ? rt_ : zs_;
            query_scales_.push_back(double(target->w) * target->h / (double(width_) * height_));
        }
        if (rt_ && s.simple[kMColorWriteEnable / 4])
            rt_->written = true;
        if (zs_ && (s.simple[kMZWriteEnable / 4] || s.rs[kRsStencilEnable]))
            zs_->written = true;
        ++draws_;
    });
}
void Vulkan::Clear(const State &s, uint32_t count, const uint8_t *rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil) {
    Guard([&] {
        Targets(s);
        if (!width_ || !height_)
            return;
        VkClearAttachment attachments[2]{};
        uint32_t n = 0;
        const bool partial = (flags & kClearTarget) && (flags & kClearTarget) != kClearTarget;
        if (rt_ && (flags & kClearTarget) && !partial) {
            attachments[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            Color(color, attachments[n].clearValue.color.float32);
            ++n;
        }
        if (zs_ && (flags & (kClearZ | kClearStencil))) {
            attachments[n].aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                        ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            attachments[n].clearValue.depthStencil = {z, stencil};
            ++n;
        }
        Render(rt_, zs_);
        for (uint32_t i = 0; i < std::max(1u, count); ++i) {
            int32_t r[] = {0, 0, int32_t(width_), int32_t(height_)};
            if (count && rects)
                std::memcpy(r, rects + i * 16, 16);
            const int l = std::clamp(Scale(r[0]), 0, Scale(width_)), t = std::clamp(Scale(r[1]), 0, Scale(height_));
            VkClearRect cr{{{l, t},
                            {uint32_t(std::max(0, std::min(Scale(r[2]), Scale(width_)) - l)),
                             uint32_t(std::max(0, std::min(Scale(r[3]), Scale(height_)) - t))}},
                           0,
                           1};
            if (n)
                vkCmdClearAttachments(cmd_, n, attachments, 1, &cr);
            if (partial && rt_) {
                State clear{};
                clear.simple[kMColorWriteEnable / 4] = ((flags & 0x40) ? 0x10000 : 0) | ((flags & 0x20) ? 0x100 : 0) |
                                                       ((flags & 0x10) ? 1 : 0) | ((flags & 0x80) ? 0x1000000 : 0);
                Assembled a;
                auto vs = Shader("float4 main(uint id:SV_VertexID):SV_Position {return "
                                 "float4(float2((id<<1)&2,id&2)*2-1,0,1);}",
                                 true);
                float c[4];
                Color(color, c);
                std::string source = "float4 main():SV_Target {return float4(";
                for (int k = 0; k < 4; ++k)
                    source += (k ? "," : "") + std::to_string(c[k]);
                source += ");}";
                auto ps = Shader(source, false);
                auto p = Pipeline(clear, a, vs, ps);
                vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                VkViewport vp{0, 0, float(Scale(width_)), float(Scale(height_)), 0, 1};
                vkCmdSetViewport(cmd_, 0, 1, &vp);
                vkCmdSetScissor(cmd_, 0, 1, &cr.rect);
                vkCmdDraw(cmd_, 3, 1, 0, 0);
            }
        }
        if (rt_ && (flags & kClearTarget))
            rt_->written = true;
        if (zs_ && (flags & (kClearZ | kClearStencil)))
            zs_->written = true;
    });
}
void Vulkan::Copy(const Surface &src, const Surface &dst, uint32_t count, uint32_t rects, uint32_t points) {
    Guard([&] {
        auto *from = Target(src);
        auto *to = Target(dst);
        if (!from || !to || from->depth != to->depth) {
            Sync(src.data, 0);
            CopySurfaceMemory(src, dst, count, rects, points);
            return;
        }
        if (from == to) {
            Sync(src.data, 0);
            CopySurfaceMemory(src, dst, count, rects, points);
            UploadTarget(*to);
            return;
        }
        for (uint32_t i = 0; i < std::max(1u, count); ++i) {
            int32_t r[] = {0, 0, int32_t(from->desc.width), int32_t(from->desc.height)}, p[2]{};
            if (count && rects)
                std::memcpy(r, kernel::HostPtr(rects + i * 16), 16);
            if (count && points)
                std::memcpy(p, kernel::HostPtr(points + i * 8), 8);
            int32_t sr[] = {Scale(r[0]), Scale(r[1]), Scale(r[2]), Scale(r[3])},
                    dr[] = {Scale(p[0]), Scale(p[1]), Scale(p[0] + r[2] - r[0]), Scale(p[1] + r[3] - r[1])};
            Blit(*from, *to, sr, dr);
        }
        to->written = true;
    });
}
void Vulkan::ResolveQueries() {
    if (query_count_)
        Submit();
}
void Vulkan::Sync(uint32_t data, uint32_t size) {
    Guard([&] {
        for (auto &[key, t] : targets_)
            if (t.written && (size ? uint64_t(data) < uint64_t(t.desc.data) + TotalSize(t.desc) &&
                                         uint64_t(t.desc.data) < uint64_t(data) + size
                                   : data == t.desc.data))
                Readback(t);
    });
}
void Vulkan::Readback(Image &t) {
    Barrier(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const size_t bytes = size_t(t.w) * t.h * 4;
    auto offset = Upload(nullptr, bytes);
    VkBufferImageCopy c{};
    c.bufferOffset = offset;
    c.imageSubresource = {t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.w, t.h, 1};
    vkCmdCopyImageToBuffer(cmd_, t.image, t.layout, arena_.buffer, 1, &c);
    size_t stencil_offset = 0;
    if (t.depth) {
        stencil_offset = Upload(nullptr, size_t(t.w) * t.h);
        c.bufferOffset = stencil_offset;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdCopyImageToBuffer(cmd_, t.image, t.layout, arena_.buffer, 1, &c);
    }
    Submit(); // Fence completion makes coherent memory available to the CPU.
    const auto &d = t.desc;
    auto *dst = static_cast<uint8_t *>(kernel::HostPtr(d.data));
    const auto bits = Format(d.format).bits;
    for (uint32_t y = 0; y < d.height; ++y)
        for (uint32_t x = 0; x < d.width; ++x) {
            const auto source = (size_t(y) * t.h / d.height * t.w + size_t(x) * t.w / d.width);
            uint32_t v;
            std::memcpy(&v, arena_.data + offset + source * 4, 4);
            if (t.depth) {
                float f;
                std::memcpy(&f, &v, 4);
                v = (uint32_t(std::clamp(f, 0.f, 1.f) * 16777215.f) << 8) | arena_.data[stencil_offset + source];
            }
            uint32_t address = 0, out = 0;
            if (d.linear)
                address = y * d.pitch + x * (bits / 8);
            else {
                for (uint32_t b = 0; (1u << b) < std::max(d.width, d.height); ++b) {
                    if ((1u << b) < d.width)
                        address |= ((x >> b) & 1u) << out++;
                    if ((1u << b) < d.height)
                        address |= ((y >> b) & 1u) << out++;
                }
                address *= bits / 8;
            }
            if (bits == 16) {
                uint16_t c = t.depth ? uint16_t(v >> 16)
                                     : uint16_t(((v >> 19) & 31) << 11 | ((v >> 10) & 63) << 5 | ((v >> 3) & 31));
                std::memcpy(dst + address, &c, 2);
            } else
                std::memcpy(dst + address, &v, 4);
        }
    t.written = false;
}
void Vulkan::Present(const Surface &front) {
    Guard([&] {
        auto *target = Target(front);
        if (!target || target->depth)
            return;
        uint32_t index;
        auto result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            Submit();
            Swapchain();
            result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
        }
        if (result != VK_SUBOPTIMAL_KHR)
            Check(result, "acquire swapchain image");
        auto &output = swap_images_[index];
        uint32_t ramp[256];
        for (unsigned i = 0; i < 256; ++i)
            ramp[i] = 0xff000000 | (uint32_t(gamma_[i]) << 16) | (uint32_t(gamma_[256 + i]) << 8) | gamma_[512 + i];
        UploadImage(gamma_image_, ramp, sizeof(ramp));
        Barrier(gamma_image_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Barrier(*target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Barrier(white_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        State s{};
        for (auto &t : s.tss) {
            t[kTssAddressU] = t[kTssAddressV] = 3;
            t[kTssMinFilter] = t[kTssMagFilter] = 2;
        }
        DrawConstants dc{};
        Image *images[] = {target, &gamma_image_, &white_, &white_};
        VkSampler samplers[4];
        for (int i = 0; i < 4; ++i)
            samplers[i] = Sampler(s, i, {}, 1);
        auto set = Descriptors(s, dc, images, samplers);
        auto vs = Shader("struct O{float4 pos:SV_Position;float2 tc0:TEXCOORD0;}; O main(uint "
                         "id:SV_VertexID){O "
                         "o;o.tc0=float2((id<<1)&2,id&2);o.pos=float4(o.tc0*2-1,0,1);return o;}",
                         true);
        auto ps = Shader("Texture2D<float4> scene:register(t0);Texture2D<float4> "
                         "ramp:register(t1);SamplerState smp:register(s0);float4 main(float2 "
                         "tc0:TEXCOORD0):SV_Target {float3 c=scene.Sample(smp,tc0).rgb;int3 "
                         "i=int3(saturate(c)*255+.5);return "
                         "float4(ramp.Load(int3(i.r,0,0)).r,ramp.Load(int3(i.g,0,0)).g,ramp."
                         "Load(int3(i.b,0,0)).b,1);}",
                         false);
        Assembled a;
        auto pipeline = Pipeline(s, a, vs, ps, false, swap_format_);
        Render(&output, nullptr);
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color.float32[3] = 1;
        VkClearRect cr{{{0, 0}, extent_}, 0, 1};
        vkCmdClearAttachments(cmd_, 1, &clear, 1, &cr);
        double aspect = app::Widescreen() ? 16.0 / 9.0 : 4.0 / 3.0;
        const auto dw = std::min(extent_.width, uint32_t(extent_.height * aspect)),
                   dh = std::min(extent_.height, uint32_t(extent_.width / aspect));
        VkViewport vp{float((extent_.width - dw) / 2), float((extent_.height - dh) / 2), float(dw), float(dh), 0, 1};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &cr.rect);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        const char *every = std::getenv("MC3_CAPTURE_EVERY");
        const bool capture =
            g_capture.exchange(false) || (every && std::atoi(every) > 0 && (frame_ + 1) % std::atoi(every) == 0);
        size_t capture_offset = 0;
        if (capture) {
            Barrier(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            capture_offset = Upload(nullptr, size_t(output.w) * output.h * 4);
            VkBufferImageCopy c{};
            c.bufferOffset = capture_offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {output.w, output.h, 1};
            vkCmdCopyImageToBuffer(cmd_, output.image, output.layout, arena_.buffer, 1, &c);
        }
        Barrier(output, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        Submit(acquired_, present_semaphores_[index]);
        if (capture) {
            auto dir = app::ExeDir() / "captures";
            std::filesystem::create_directories(dir);
            auto path = dir / ("vulkan_" + std::to_string(frame_ + 1) + ".tga");
            std::ofstream file(path, std::ios::binary);
            uint8_t header[18] = {0, 0, 2};
            header[12] = output.w;
            header[13] = output.w >> 8;
            header[14] = output.h;
            header[15] = output.h >> 8;
            header[16] = 32;
            header[17] = 0x28;
            file.write(reinterpret_cast<char *>(header), 18);
            file.write(reinterpret_cast<char *>(arena_.data + capture_offset), size_t(output.w) * output.h * 4);
            LOGR("Linux screenshot: %s", path.string().c_str());
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &present_semaphores_[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        result = vkQueuePresentKHR(queue_, &present);
        // A semaphore belongs to each swapchain image. Reacquiring that image
        // guarantees the previous presentation has consumed its semaphore.
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            Swapchain();
        else
            Check(result, "present");
        ++frame_;
        linuxapp::frames = frame_;
        if (frame_ % 120 == 0) {
            // Submission has completed; no descriptor still references retired textures.
            for (auto it = textures_.begin(); it != textures_.end();) {
                auto &texture = it->second;
                if (frame_ > texture.checked_frame + 300) {
                    if (texture.view != texture.sample_view)
                        vkDestroyImageView(device_, texture.view, nullptr);
                    vkDestroyImageView(device_, texture.sample_view, nullptr);
                    vkDestroyImage(device_, texture.image, nullptr);
                    vkFreeMemory(device_, texture.memory, nullptr);
                    it = textures_.erase(it);
                } else
                    ++it;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - report_time_).count();
            LOGR("Vulkan: frame %llu, %.1f FPS, %llu draws, %zu pipelines, %zu "
                 "textures",
                 (unsigned long long)frame_, 120 / elapsed, (unsigned long long)draws_, pipelines_.size(),
                 textures_.size());
            if (PerfScope::Enabled()) {
                LOGR("Vulkan perf: frame=%llu draw_ms=%.3f gpu_wait_ms=%.3f guest_wait_ms=%.3f",
                     (unsigned long long)frame_, draw_ns_ / 120e6, gpu_wait_ns_ / 120e6,
                     g_perf_wait_ns.exchange(0) / 120e6);
                draw_ns_ = gpu_wait_ns_ = 0;
            }
            report_time_ = now;
            draws_ = 0;
        }
    });
}
} // namespace
std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<Vulkan>(); }
} // namespace gpu
