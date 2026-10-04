// 宿主的 Vulkan pass 适配器：所有资源由契约工厂创建/释放，原生句柄仅用于录制。
module;
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.engine_gpu;
export import deren.promise.rhi;
export import deren.utility.backend_context;
import deren.utility;
export import deren.vulkan.constant_init;

export namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    struct gpu_context {
        deren::utility::backend_token token;
        rhi::vulkan_escape* escape = nullptr;
        explicit gpu_context(deren::utility::backend_token value)
            : token(std::move(value)) {
            escape = static_cast<rhi::vulkan_escape*>(token->core().query_extension(rhi::extension_kind::vulkan_escape));
            if (!escape)
                deren::utility::panic("Vulkan recording extension unavailable");
        }
        gpu_context() = default;
        rhi::api_core& api() const noexcept {
            return token->core();
        }
    };

    // 格式在此显式转换，不能把 VkFormat 数字解释成通用契约枚举。
    inline rhi::image_format contract_format(VkFormat value) noexcept {
        switch (value) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return rhi::image_format::rgba8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return rhi::image_format::rgba8_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return rhi::image_format::bgra8_unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return rhi::image_format::bgra8_srgb;
        case VK_FORMAT_R8_UNORM:
            return rhi::image_format::r8_unorm;
        case VK_FORMAT_R16_SFLOAT:
            return rhi::image_format::r16_sfloat;
        case VK_FORMAT_R16G16_SFLOAT:
            return rhi::image_format::rg16_sfloat;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return rhi::image_format::rgba16_sfloat;
        case VK_FORMAT_R32_SFLOAT:
            return rhi::image_format::r32_sfloat;
        case VK_FORMAT_R32G32_SFLOAT:
            return rhi::image_format::rg32_sfloat;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return rhi::image_format::rgba32_sfloat;
        case VK_FORMAT_R32_UINT:
            return rhi::image_format::r32_uint;
        case VK_FORMAT_D16_UNORM:
            return rhi::image_format::d16_unorm;
        case VK_FORMAT_D32_SFLOAT:
            return rhi::image_format::d32_sfloat;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return rhi::image_format::d24_unorm_s8_uint;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return rhi::image_format::d32_sfloat_s8_uint;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return rhi::image_format::bc1_rgba_unorm;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            return rhi::image_format::bc1_rgba_srgb;
        case VK_FORMAT_BC3_UNORM_BLOCK:
            return rhi::image_format::bc3_unorm;
        case VK_FORMAT_BC3_SRGB_BLOCK:
            return rhi::image_format::bc3_srgb;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return rhi::image_format::bc5_unorm;
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return rhi::image_format::bc7_unorm;
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return rhi::image_format::bc7_srgb;
        default:
            return rhi::image_format::unknown;
        }
    }
    inline rhi::shader_stage contract_stage(VkShaderStageFlagBits value) noexcept {
        switch (value) {
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return rhi::shader_stage::fragment;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return rhi::shader_stage::compute;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return rhi::shader_stage::task;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return rhi::shader_stage::mesh;
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return rhi::shader_stage::ray_generation;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return rhi::shader_stage::ray_miss;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return rhi::shader_stage::ray_closest_hit;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return rhi::shader_stage::ray_any_hit;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return rhi::shader_stage::ray_intersection;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            return rhi::shader_stage::ray_callable;
        default:
            return rhi::shader_stage::vertex;
        }
    }
    template <class Object, class Native>
    class resource_owner {
    protected:
        rhi::object_manager<Object> owned;
        Native native = VK_NULL_HANDLE;

    public:
        resource_owner() = default;
        resource_owner(Object* resource, gpu_context const& gpu, Native handle)
            : owned(resource, gpu.token)
            , native(handle) {
        }
        resource_owner(resource_owner&& other) noexcept
            : owned(std::move(other.owned))
            , native(std::exchange(other.native, VK_NULL_HANDLE)) {
        }
        resource_owner& operator=(resource_owner&& other) noexcept {
            if (this != &other) {
                owned = std::move(other.owned);
                native = std::exchange(other.native, VK_NULL_HANDLE);
            }
            return *this;
        }
        bool valid() const noexcept {
            return owned.get() != nullptr;
        }
        Native const& operator*() const noexcept {
            return native;
        }
        Native const& get() const noexcept {
            return native;
        }
        Object* resource() const noexcept {
            return owned.get();
        }
        void release() noexcept {
            owned = {};
            native = VK_NULL_HANDLE;
        }
    };
    using vk_image_view = resource_owner<rhi::image_view, VkImageView>;
    using vk_sampler = resource_owner<rhi::sampler, VkSampler>;
    using vk_shader_module = resource_owner<rhi::shader, VkShaderModule>;
    class vk_image : public resource_owner<rhi::image, VkImage> {
    public:
        using resource_owner::resource_owner;
        std::uint64_t handle() const noexcept {
            return reinterpret_cast<std::uintptr_t>(resource());
        }
    };
    class vk_buffer : public resource_owner<rhi::buffer, VkBuffer> {
    public:
        using resource_owner::resource_owner;
        std::uint64_t handle() const noexcept {
            return reinterpret_cast<std::uintptr_t>(resource());
        }
    };
    struct vk_pipeline : resource_owner<rhi::pipeline, VkPipeline> {
        VkViewport viewport{};
        VkRect2D scissor{};
        vk_pipeline() = default;
        vk_pipeline(rhi::pipeline* object, gpu_context const& gpu)
            : resource_owner(object, gpu, object ? reinterpret_cast<VkPipeline>(gpu.escape->native_pipeline(*object)) : VK_NULL_HANDLE) {
        }
        VkPipeline get_pipeline() const noexcept {
            return native;
        }
        void begin_pipeline(VkCommandBuffer cmd) const {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, native);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }
    };

    // 命令缓冲通过后端分配/释放。frame primary 为借用，无释放权。
    class vk_command_buffer {
        gpu_context gpu;
        VkCommandBuffer native = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;

    public:
        vk_command_buffer() = default;
        vk_command_buffer(VkCommandBuffer value, gpu_context context, VkCommandPool owning_pool = VK_NULL_HANDLE)
            : gpu(std::move(context))
            , native(value)
            , pool(owning_pool) {
        }
        vk_command_buffer(vk_command_buffer const&) = delete;
        vk_command_buffer& operator=(vk_command_buffer const&) = delete;
        vk_command_buffer(vk_command_buffer&& other) noexcept
            : gpu(std::move(other.gpu))
            , native(std::exchange(other.native, VK_NULL_HANDLE))
            , pool(std::exchange(other.pool, VK_NULL_HANDLE)) {
        }
        vk_command_buffer& operator=(vk_command_buffer&& other) noexcept {
            if (this != &other) {
                release();
                gpu = std::move(other.gpu);
                native = std::exchange(other.native, VK_NULL_HANDLE);
                pool = std::exchange(other.pool, VK_NULL_HANDLE);
            }
            return *this;
        }
        ~vk_command_buffer() {
            release();
        }
        void release() noexcept {
            if (native && pool && gpu.escape) {
                rhi::vulkan_command command{};
                command.pool = reinterpret_cast<uint64_t>(pool);
                command.command_buffer = native;
                static_cast<void>(gpu.escape->service(rhi::vulkan_service::free_command, &command, sizeof(command)));
            }
            native = VK_NULL_HANDLE;
            pool = VK_NULL_HANDLE;
        }
        VkCommandBuffer const& get() const noexcept {
            return native;
        }
        VkCommandBuffer const& operator*() const noexcept {
            return native;
        }
    };
    class heap_access {
        gpu_context gpu;

    public:
        explicit heap_access(gpu_context context)
            : gpu(std::move(context)) {
        }
        bool ready() const noexcept {
            return true;
        }
        bool write_buffer(VkDeviceSize offset, VkDeviceAddress address, VkDeviceSize size, VkDescriptorType type) const {
            rhi::vulkan_heap_buffer call{offset, address, size, static_cast<uint32_t>(type), 0};
            return gpu.escape->service(rhi::vulkan_service::heap_write_buffer, &call, sizeof(call)) == rhi::error::ok && call.accepted;
        }
        bool write_image(VkDeviceSize offset, VkImageViewCreateInfo const& info, VkImageLayout layout, VkDescriptorType type) const {
            rhi::vulkan_heap_image call{};
            call.offset = offset;
            call.view = {const_cast<VkImageViewCreateInfo*>(&info), sizeof(info)};
            call.layout = layout;
            call.type = type;
            return gpu.escape->service(rhi::vulkan_service::heap_write_image, &call, sizeof(call)) == rhi::error::ok && call.accepted;
        }
        bool write_samplers(VkDeviceSize offset, std::span<VkSamplerCreateInfo const> infos) const {
            rhi::vulkan_heap_samplers call{};
            call.offset = offset;
            call.infos = {const_cast<VkSamplerCreateInfo*>(infos.data()), static_cast<uint32_t>(infos.size_bytes())};
            call.count = static_cast<uint32_t>(infos.size());
            return gpu.escape->service(rhi::vulkan_service::heap_write_samplers, &call, sizeof(call)) == rhi::error::ok && call.accepted;
        }
        void record_bind(VkCommandBuffer command) const {
            rhi::vulkan_heap_commands call{};
            call.command_buffer = command;
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::heap_bind, &call, sizeof(call)));
        }
        bool push_data(VkCommandBuffer command, uint32_t offset, std::span<std::byte const> bytes) const {
            rhi::vulkan_heap_commands call{};
            call.command_buffer = command;
            call.offset = offset;
            call.data = bytes.data();
            call.data_size = static_cast<uint32_t>(bytes.size());
            return gpu.escape->service(rhi::vulkan_service::heap_push, &call, sizeof(call)) == rhi::error::ok && call.accepted;
        }
        void bind_infos(VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler) const {
            rhi::vulkan_heap_commands call{};
            call.resource_bind = {&resource, sizeof(resource)};
            call.sampler_bind = {&sampler, sizeof(sampler)};
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::heap_bind_infos, &call, sizeof(call)));
        }
    };

    inline std::expected<vk_pipeline, std::string_view> make_pipeline(
        gpu_context const& gpu, std::span<VkFormat const> formats, VkFormat depth,
        std::span<uint8_t const> first_code, std::span<uint8_t const> fragment_code,
        VkSampleCountFlagBits samples, bool depth_test = true, float bias_constant = 0,
        float bias_slope = 0, float bias_clamp = 0,
        std::span<VkPipelineColorBlendAttachmentState const> blends = {},
        VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT,
        VkCompareOp comparison = VK_COMPARE_OP_LESS_OR_EQUAL) {
        std::vector<rhi::image_format> colors;
        for (auto value : formats)
            colors.push_back(contract_format(value));
        std::vector<rhi::blend_attachment> states;
        for (auto const& value : blends)
            states.push_back({value.blendEnable,
                              static_cast<rhi::blend_factor>(value.srcColorBlendFactor), static_cast<rhi::blend_factor>(value.dstColorBlendFactor),
                              static_cast<rhi::blend_op>(value.colorBlendOp), static_cast<rhi::blend_factor>(value.srcAlphaBlendFactor),
                              static_cast<rhi::blend_factor>(value.dstAlphaBlendFactor), static_cast<rhi::blend_op>(value.alphaBlendOp), value.colorWriteMask});
        std::array<rhi::pipeline_stage, 2> stages{{{contract_stage(first_stage), nullptr, first_code, "main"},
                                                   {rhi::shader_stage::fragment, nullptr, fragment_code, "main"}}};
        rhi::pipeline_desc desc{};
        desc.stages = stages;
        desc.color_formats = colors;
        desc.depth_format = contract_format(depth);
        desc.blends = states;
        desc.sample_count = samples;
        desc.depth_test_enabled = depth_test;
        desc.depth_comparison = static_cast<rhi::compare_op>(comparison);
        desc.depth_bias_constant = bias_constant;
        desc.depth_bias_slope = bias_slope;
        desc.depth_bias_clamp = bias_clamp;
        auto* object = gpu.api().create_pipeline(desc);
        if (!object)
            return std::unexpected("graphics pipeline factory refused descriptor");
        return vk_pipeline(object, gpu);
    }
    inline std::expected<vk_pipeline, std::string_view> make_pipeline(
        gpu_context const& gpu, VkFormat color, VkFormat depth, std::span<uint8_t const> first,
        std::span<uint8_t const> fragment, VkSampleCountFlagBits samples, bool depth_test = true,
        bool has_color = true, float bias_constant = 0, float bias_slope = 0, float bias_clamp = 0,
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT) {
        return make_pipeline(gpu, std::span(&color, has_color ? 1u : 0u), depth, first, fragment,
                             samples, depth_test, bias_constant, bias_slope, bias_clamp, {}, stage);
    }
    inline std::optional<vk_pipeline> make_compute_pipeline(gpu_context const& gpu, std::span<uint8_t const> code) {
        rhi::pipeline_stage stage{rhi::shader_stage::compute, nullptr, code, "main"};
        rhi::pipeline_desc desc{};
        desc.kind = rhi::pipeline_kind::compute;
        desc.stages = std::span(&stage, 1);
        auto* object = gpu.api().create_pipeline(desc);
        if (!object)
            return {};
        return vk_pipeline(object, gpu);
    }
    inline std::optional<vk_shader_module> make_shader_module(std::span<uint8_t const> code, gpu_context const& gpu) {
        rhi::shader_desc desc{};
        desc.code = code;
        auto* object = gpu.api().create_shader(desc);
        if (!object)
            return {};
        return vk_shader_module(object, gpu, reinterpret_cast<VkShaderModule>(gpu.escape->native_shader(*object)));
    }

    enum class buffer_type { vertex,
                             index,
                             uniform_gpu_only,
                             uniform_coherent,
                             uniform_cached,
                             storage_coherent,
                             readback_coherent,
                             acceleration_structure_storage,
                             acceleration_structure_scratch,
                             storage_gpu_only };
    enum class image_type { texture_2d,
                            texture_2d_color,
                            texture_2d_depth,
                            texture_2d_staging,
                            texture_cubemap,
                            render_target };
    struct image_create_info {
        uint32_t width = 0, height = 0, mip_levels = 1, array_layers = 1;
        VkFormat format{};
        VkImageUsageFlags extra_usage = 0;
    };
    struct image_detail {
        VkImage image{};
        image_create_info create_info{};
    };
    struct buffer_detail {
        VkBuffer buffer{};
        struct {
            void* pMappedData = nullptr;
        } allocation_info;
    };

    // 宿主资源目录仅缓存自己的引用和原生录制信息，没有后端 VMA 指针或分配器。
    class resource_catalog {
        gpu_context gpu;
        std::map<uint64_t, image_detail> images;
        std::map<uint64_t, buffer_detail> buffers;

    public:
        explicit resource_catalog(gpu_context context)
            : gpu(std::move(context)) {
        }
        vk_image create_image(uint8_t const* data, uint64_t bytes, image_create_info const& info, image_type type) {
            rhi::image_desc desc{};
            desc.extent = {info.width, info.height, 1};
            desc.format = contract_format(info.format);
            desc.mip_levels = info.mip_levels;
            desc.array_layers = info.array_layers;
            desc.dimension = type == image_type::texture_cubemap ? rhi::image_dimension::cube : rhi::image_dimension::texture_2d;
            desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_destination);
            if (type == image_type::texture_2d_depth)
                desc.flags |= rhi::to_bits(rhi::image_flag::depth_attachment);
            if (info.extra_usage & VK_IMAGE_USAGE_STORAGE_BIT)
                desc.flags |= rhi::to_bits(rhi::image_flag::storage);
            if (info.extra_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                desc.flags |= rhi::to_bits(rhi::image_flag::color_attachment);
            if (info.extra_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                desc.flags |= rhi::to_bits(rhi::image_flag::transfer_source);
            if (data)
                desc.initial_bytes = std::span(reinterpret_cast<std::byte const*>(data), static_cast<size_t>(bytes));
            auto* object = gpu.api().create_image(desc);
            if (!object)
                return {};
            vk_image answer(object, gpu, reinterpret_cast<VkImage>(gpu.escape->native_image(*object)));
            images[answer.handle()] = {*answer, info};
            return answer;
        }
        template <class T, size_t N>
        vk_image create_image(std::span<T, N> data, image_create_info const& info, image_type type) {
            return create_image(reinterpret_cast<uint8_t const*>(data.data()), data.size_bytes(), info, type);
        }
        image_detail const* get_image_detail(uint64_t handle) const {
            auto found = images.find(handle);
            return found == images.end() ? nullptr : &found->second;
        }
        rhi::image* image_resource(VkImage image) const {
            for (auto const& [key, detail] : images)
                if (detail.image == image)
                    return reinterpret_cast<rhi::image*>(key);
            return nullptr;
        }
        vk_buffer create_buffer(uint8_t const* data, uint64_t bytes, buffer_type type, VkBufferUsageFlags flags = 0) {
            rhi::buffer_desc desc{};
            desc.size = bytes;
            desc.usage = static_cast<rhi::buffer_usage>(type);
            if (flags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                desc.flags |= rhi::to_bits(rhi::buffer_flag::device_address);
            if (flags & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
                desc.flags |= rhi::to_bits(rhi::buffer_flag::storage);
            if (data)
                desc.initial_bytes = std::span(reinterpret_cast<std::byte const*>(data), static_cast<size_t>(bytes));
            auto* object = gpu.api().create_buffer(desc);
            if (!object)
                return {};
            vk_buffer answer(object, gpu, reinterpret_cast<VkBuffer>(gpu.escape->native_buffer(*object)));
            buffers[answer.handle()] = {*answer, {object->mapped().data()}};
            return answer;
        }
        buffer_detail const* get_buffer_detail(uint64_t handle) const {
            auto found = buffers.find(handle);
            return found == buffers.end() ? nullptr : &found->second;
        }
        void log_statistics() const noexcept {
        }
    };
} // namespace deren::vulkan
