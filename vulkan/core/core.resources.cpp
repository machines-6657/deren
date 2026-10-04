// Product resource factories. All allocations, containers and destruction stay in the backend.
module;

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.core;

import deren.promise.rhi;
import deren.vulkan.core.pipeline;
import deren.vulkan.constant_init;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace {
        static_assert(std::is_standard_layout_v<rhi::image_desc>);
        static_assert(std::is_standard_layout_v<rhi::image_view_desc>);
        static_assert(std::is_standard_layout_v<rhi::sampler_desc>);
        static_assert(std::is_standard_layout_v<rhi::shader_desc>);
        static_assert(std::is_standard_layout_v<rhi::pipeline_desc>);

        [[nodiscard]] VkFormat native_format(rhi::image_format const format) noexcept {
            switch (format) {
            case rhi::image_format::unknown:
                return VK_FORMAT_UNDEFINED;
            case rhi::image_format::rgba8_unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case rhi::image_format::rgba8_srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case rhi::image_format::bgra8_unorm:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case rhi::image_format::bgra8_srgb:
                return VK_FORMAT_B8G8R8A8_SRGB;
            case rhi::image_format::r8_unorm:
                return VK_FORMAT_R8_UNORM;
            case rhi::image_format::r16_sfloat:
                return VK_FORMAT_R16_SFLOAT;
            case rhi::image_format::rg16_sfloat:
                return VK_FORMAT_R16G16_SFLOAT;
            case rhi::image_format::rgba16_sfloat:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case rhi::image_format::r32_sfloat:
                return VK_FORMAT_R32_SFLOAT;
            case rhi::image_format::rg32_sfloat:
                return VK_FORMAT_R32G32_SFLOAT;
            case rhi::image_format::rgba32_sfloat:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case rhi::image_format::r32_uint:
                return VK_FORMAT_R32_UINT;
            case rhi::image_format::d16_unorm:
                return VK_FORMAT_D16_UNORM;
            case rhi::image_format::d32_sfloat:
                return VK_FORMAT_D32_SFLOAT;
            case rhi::image_format::d24_unorm_s8_uint:
                return VK_FORMAT_D24_UNORM_S8_UINT;
            case rhi::image_format::d32_sfloat_s8_uint:
                return VK_FORMAT_D32_SFLOAT_S8_UINT;
            case rhi::image_format::bc1_rgba_unorm:
                return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case rhi::image_format::bc1_rgba_srgb:
                return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case rhi::image_format::bc3_unorm:
                return VK_FORMAT_BC3_UNORM_BLOCK;
            case rhi::image_format::bc3_srgb:
                return VK_FORMAT_BC3_SRGB_BLOCK;
            case rhi::image_format::bc5_unorm:
                return VK_FORMAT_BC5_UNORM_BLOCK;
            case rhi::image_format::bc7_unorm:
                return VK_FORMAT_BC7_UNORM_BLOCK;
            case rhi::image_format::bc7_srgb:
                return VK_FORMAT_BC7_SRGB_BLOCK;
            case rhi::image_format::rg8_unorm:
                return VK_FORMAT_R8G8_UNORM;
            case rhi::image_format::rgb8_unorm:
                return VK_FORMAT_R8G8B8_UNORM;
            case rhi::image_format::rgb8_srgb:
                return VK_FORMAT_R8G8B8_SRGB;
            case rhi::image_format::r8_srgb:
                return VK_FORMAT_R8_SRGB;
            case rhi::image_format::bc1_rgb_unorm:
                return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
            case rhi::image_format::bc1_rgb_srgb:
                return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
            case rhi::image_format::bc2_unorm:
                return VK_FORMAT_BC2_UNORM_BLOCK;
            case rhi::image_format::bc2_srgb:
                return VK_FORMAT_BC2_SRGB_BLOCK;
            case rhi::image_format::bc4_unorm:
                return VK_FORMAT_BC4_UNORM_BLOCK;
            case rhi::image_format::bc4_snorm:
                return VK_FORMAT_BC4_SNORM_BLOCK;
            case rhi::image_format::bc5_snorm:
                return VK_FORMAT_BC5_SNORM_BLOCK;
            case rhi::image_format::bc6h_ufloat:
                return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case rhi::image_format::bc6h_sfloat:
                return VK_FORMAT_BC6H_SFLOAT_BLOCK;
            case rhi::image_format::etc2_rgb8_unorm:
                return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
            case rhi::image_format::etc2_rgb8_srgb:
                return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
            case rhi::image_format::etc2_rgb8a1_unorm:
                return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
            case rhi::image_format::etc2_rgb8a1_srgb:
                return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
            case rhi::image_format::etc2_rgba8_unorm:
                return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
            case rhi::image_format::etc2_rgba8_srgb:
                return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
            case rhi::image_format::eac_r11_unorm:
                return VK_FORMAT_EAC_R11_UNORM_BLOCK;
            case rhi::image_format::eac_r11_snorm:
                return VK_FORMAT_EAC_R11_SNORM_BLOCK;
            case rhi::image_format::eac_rg11_unorm:
                return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
            case rhi::image_format::eac_rg11_snorm:
                return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
            case rhi::image_format::astc_4x4_unorm:
                return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
            case rhi::image_format::astc_4x4_srgb:
                return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
            case rhi::image_format::astc_5x4_unorm:
                return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
            case rhi::image_format::astc_5x4_srgb:
                return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
            case rhi::image_format::astc_5x5_unorm:
                return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
            case rhi::image_format::astc_5x5_srgb:
                return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
            case rhi::image_format::astc_6x5_unorm:
                return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
            case rhi::image_format::astc_6x5_srgb:
                return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
            case rhi::image_format::astc_6x6_unorm:
                return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
            case rhi::image_format::astc_6x6_srgb:
                return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
            case rhi::image_format::astc_8x5_unorm:
                return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
            case rhi::image_format::astc_8x5_srgb:
                return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
            case rhi::image_format::astc_8x6_unorm:
                return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
            case rhi::image_format::astc_8x6_srgb:
                return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
            case rhi::image_format::astc_8x8_unorm:
                return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
            case rhi::image_format::astc_8x8_srgb:
                return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
            case rhi::image_format::astc_10x5_unorm:
                return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
            case rhi::image_format::astc_10x5_srgb:
                return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
            case rhi::image_format::astc_10x6_unorm:
                return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
            case rhi::image_format::astc_10x6_srgb:
                return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
            case rhi::image_format::astc_10x8_unorm:
                return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
            case rhi::image_format::astc_10x8_srgb:
                return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
            case rhi::image_format::astc_10x10_unorm:
                return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
            case rhi::image_format::astc_10x10_srgb:
                return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
            case rhi::image_format::astc_12x10_unorm:
                return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
            case rhi::image_format::astc_12x10_srgb:
                return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
            case rhi::image_format::astc_12x12_unorm:
                return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
            case rhi::image_format::astc_12x12_srgb:
                return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
            }
            return VK_FORMAT_UNDEFINED;
        }

        [[nodiscard]] VkShaderStageFlagBits native_stage(rhi::shader_stage const stage) noexcept {
            switch (stage) {
            case rhi::shader_stage::vertex:
                return VK_SHADER_STAGE_VERTEX_BIT;
            case rhi::shader_stage::fragment:
                return VK_SHADER_STAGE_FRAGMENT_BIT;
            case rhi::shader_stage::compute:
                return VK_SHADER_STAGE_COMPUTE_BIT;
            case rhi::shader_stage::task:
                return VK_SHADER_STAGE_TASK_BIT_EXT;
            case rhi::shader_stage::mesh:
                return VK_SHADER_STAGE_MESH_BIT_EXT;
            case rhi::shader_stage::ray_generation:
                return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case rhi::shader_stage::ray_miss:
                return VK_SHADER_STAGE_MISS_BIT_KHR;
            case rhi::shader_stage::ray_closest_hit:
                return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case rhi::shader_stage::ray_any_hit:
                return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case rhi::shader_stage::ray_intersection:
                return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case rhi::shader_stage::ray_callable:
                return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            }
            return static_cast<VkShaderStageFlagBits>(0);
        }

        template <typename descriptor>
        [[nodiscard]] bool complete(descriptor const& desc) noexcept {
            // ABI 7 is exact-version negotiated. Refuse truncated descriptors before reading a tail.
            return desc.struct_size >= sizeof(descriptor);
        }

        struct image_resource final : rhi::image {
            core* owner = nullptr;
            vk_image allocation = {};
            VkImage native = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            rhi::image_extent image_size = {};
            rhi::image_format image_format = rhi::image_format::unknown;
            rhi::image_dimension dimension = rhi::image_dimension::texture_2d;
            std::uint32_t mip_levels = 1;
            std::uint32_t array_layers = 1;
            std::atomic<std::uint32_t> references = 1;

            ~image_resource() noexcept override {
                this->owner->unregister_resource(this);
                if (this->allocation.valid()) {
                    this->allocation.reset();
                } else if (this->native != VK_NULL_HANDLE) {
                    vkDestroyImage(this->owner->logical_device, this->native, nullptr);
                }
                if (this->memory != VK_NULL_HANDLE)
                    vkFreeMemory(this->owner->logical_device, this->memory, nullptr);
            }
            void retain() noexcept {
                this->references.fetch_add(1, std::memory_order_relaxed);
            }
            void release() noexcept override {
                if (this->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete this;
            }
            [[nodiscard]] rhi::image_extent extent() const noexcept override {
                return this->image_size;
            }
            [[nodiscard]] rhi::image_format format() const noexcept override {
                return this->image_format;
            }
        };

        struct view_resource final : rhi::image_view {
            core* owner = nullptr;
            image_resource* image = nullptr;
            VkImageView native = VK_NULL_HANDLE;
            ~view_resource() noexcept override {
                this->owner->unregister_resource(this);
                // 视图先销毁，再放开图片引用，避免留下引用已释放图片的 Vulkan 对象。
                if (this->native != VK_NULL_HANDLE)
                    vkDestroyImageView(this->owner->logical_device, this->native, nullptr);
                if (this->image != nullptr)
                    this->image->release();
            }
            void release() noexcept override {
                delete this;
            }
        };
        struct sampler_resource final : rhi::sampler {
            core* owner = nullptr;
            VkSampler native = VK_NULL_HANDLE;
            ~sampler_resource() noexcept override {
                this->owner->unregister_resource(this);
                if (this->native != VK_NULL_HANDLE)
                    vkDestroySampler(this->owner->logical_device, this->native, nullptr);
            }
            void release() noexcept override {
                delete this;
            }
        };
        struct shader_resource final : rhi::shader {
            core* owner = nullptr;
            VkShaderModule native = VK_NULL_HANDLE;
            rhi::shader_stage stage = rhi::shader_stage::vertex;
            std::vector<std::uint8_t> code = {};
            ~shader_resource() noexcept override {
                this->owner->unregister_resource(this);
                if (this->native != VK_NULL_HANDLE)
                    vkDestroyShaderModule(this->owner->logical_device, this->native, nullptr);
            }
            void release() noexcept override {
                delete this;
            }
        };
        struct pipeline_resource final : rhi::pipeline {
            core* owner = nullptr;
            VkPipeline native = VK_NULL_HANDLE;
            ~pipeline_resource() noexcept override {
                this->owner->unregister_resource(this);
                if (this->native != VK_NULL_HANDLE)
                    vkDestroyPipeline(this->owner->logical_device, this->native, nullptr);
            }
            void release() noexcept override {
                delete this;
            }
        };
        struct query_resource final : rhi::query {
            core* owner = nullptr;
            VkQueryPool native = VK_NULL_HANDLE;
            ~query_resource() noexcept override {
                this->owner->unregister_resource(this);
                if (this->native != VK_NULL_HANDLE)
                    vkDestroyQueryPool(this->owner->logical_device, this->native, nullptr);
            }
            void release() noexcept override {
                delete this;
            }
        };
        struct swapchain_resource final : rhi::swapchain {
            core* owner = nullptr;
            ~swapchain_resource() noexcept override {
                this->owner->unregister_resource(this);
            }
            void release() noexcept override {
                delete this;
            }
            [[nodiscard]] rhi::image_extent extent() const noexcept override {
                return {this->owner->swap_chain_extent.width, this->owner->swap_chain_extent.height, 1};
            }
            [[nodiscard]] rhi::image_format format() const noexcept override {
                switch (this->owner->swap_chain_image_format) {
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return rhi::image_format::rgba8_unorm;
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return rhi::image_format::rgba8_srgb;
                case VK_FORMAT_B8G8R8A8_UNORM:
                    return rhi::image_format::bgra8_unorm;
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return rhi::image_format::bgra8_srgb;
                default:
                    return rhi::image_format::unknown;
                }
            }
            [[nodiscard]] std::uint32_t image_count() const noexcept override {
                return static_cast<std::uint32_t>(this->owner->swap_chain_images.size());
            }
        };
    } // namespace

    void core::register_resource(void const* resource, resource_kind const kind, void* native, void* implementation) {
        std::lock_guard lock(this->resource_mutex);
        this->resource_handles.emplace(resource, resource_record{kind, native, implementation});
    }
    void core::unregister_resource(void const* resource) noexcept {
        std::lock_guard lock(this->resource_mutex);
        this->resource_handles.erase(resource);
    }
    core::resource_record core::find_resource(void const* resource, resource_kind const kind) const noexcept {
        std::lock_guard lock(this->resource_mutex);
        auto const found = this->resource_handles.find(resource);
        return found != this->resource_handles.end() && found->second.kind == kind ? found->second : resource_record{kind};
    }

    rhi::image* core::create_image(rhi::image_desc const& desc) {
        if (!complete(desc))
            return nullptr;
        VkFormat const format = native_format(desc.format);
        if (format == VK_FORMAT_UNDEFINED) {
            deren::utility::log("rhi: create_image rejects unsupported contract format {}", static_cast<std::uint32_t>(desc.format));
            return nullptr;
        }
        if (desc.sample_count == 0 || desc.sample_count > 64 || (desc.sample_count & (desc.sample_count - 1)) != 0 ||
            (desc.sample_count != 1 && (!desc.initial_bytes.empty() || desc.mip_levels != 1 || desc.dimension != rhi::image_dimension::texture_2d)))
            return nullptr;
        if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0 ||
            desc.mip_levels == 0 || desc.array_layers == 0 || desc.flags == 0 || (desc.flags & ~63u) != 0)
            return nullptr;
        if (desc.dimension == rhi::image_dimension::texture_3d) {
            if (desc.array_layers != 1 || !desc.initial_bytes.empty())
                return nullptr;
        } else if (desc.dimension == rhi::image_dimension::texture_2d || desc.dimension == rhi::image_dimension::cube) {
            if (desc.extent.depth != 1)
                return nullptr;
            if (desc.dimension == rhi::image_dimension::cube && (desc.extent.width != desc.extent.height || desc.array_layers % 6 != 0))
                return nullptr;
        } else
            return nullptr;
        std::uint32_t max_mips = 1;
        for (auto edge = std::max({desc.extent.width, desc.extent.height, desc.extent.depth}); edge > 1; edge >>= 1)
            ++max_mips;
        if (desc.mip_levels > max_mips)
            return nullptr;
        VkImageUsageFlags usage = 0;
        if (rhi::has_flag(desc.flags, rhi::image_flag::sampled))
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (rhi::has_flag(desc.flags, rhi::image_flag::storage))
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (rhi::has_flag(desc.flags, rhi::image_flag::color_attachment))
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (rhi::has_flag(desc.flags, rhi::image_flag::depth_attachment))
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (rhi::has_flag(desc.flags, rhi::image_flag::transfer_source))
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (rhi::has_flag(desc.flags, rhi::image_flag::transfer_destination))
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VkImageType const image_type = desc.dimension == rhi::image_dimension::texture_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        VkImageCreateFlags const image_flags = desc.dimension == rhi::image_dimension::cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        VkImageUsageFlags const effective_usage = !desc.initial_bytes.empty() ? usage | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT
                                                                              : usage;
        VkImageFormatProperties support = {};
        VkResult const supported = vkGetPhysicalDeviceImageFormatProperties(this->physical_device, format, image_type, VK_IMAGE_TILING_OPTIMAL,
                                                                            effective_usage, image_flags, &support);
        if (supported != VK_SUCCESS || desc.extent.width > support.maxExtent.width || desc.extent.height > support.maxExtent.height ||
            desc.extent.depth > support.maxExtent.depth || desc.array_layers > support.maxArrayLayers || desc.mip_levels > support.maxMipLevels ||
            (support.sampleCounts & desc.sample_count) == 0) {
            deren::utility::log("rhi: create_image unsupported on selected GPU: format {} usage {:#x} extent {}x{}x{} mips {} layers {} samples {} VkResult {}",
                                static_cast<int>(format), effective_usage, desc.extent.width, desc.extent.height, desc.extent.depth,
                                desc.mip_levels, desc.array_layers, desc.sample_count, static_cast<int>(supported));
            return nullptr;
        }

        auto answer = std::make_unique<image_resource>();
        answer->owner = this;
        answer->image_size = desc.extent;
        answer->image_format = desc.format;
        answer->dimension = desc.dimension;
        answer->mip_levels = desc.mip_levels;
        answer->array_layers = desc.array_layers;
        if (!desc.initial_bytes.empty()) {
            if (rhi::has_flag(desc.flags, rhi::image_flag::depth_attachment))
                return nullptr;
            image_create_info const info{desc.extent.width, desc.extent.height, desc.mip_levels, desc.array_layers, format, usage};
            auto const type = desc.dimension == rhi::image_dimension::cube ? deren::vulkan::image_type::texture_cubemap : deren::vulkan::image_type::texture_2d;
            answer->allocation = this->vma.create_image(reinterpret_cast<std::uint8_t const*>(desc.initial_bytes.data()), desc.initial_bytes.size(), info, type);
            if (!answer->allocation.valid())
                return nullptr;
            auto const* detail = this->vma.get_image_detail(answer->allocation.handle());
            if (detail == nullptr)
                return nullptr;
            answer->native = detail->image;
        } else {
            VkImageCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.flags = image_flags;
            info.imageType = image_type;
            info.format = format;
            info.extent = {desc.extent.width, desc.extent.height, desc.extent.depth};
            info.mipLevels = desc.mip_levels;
            info.arrayLayers = desc.array_layers;
            info.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkResult result = vkCreateImage(this->logical_device, &info, nullptr, &answer->native);
            if (result != VK_SUCCESS) {
                deren::utility::log("rhi: create_image failed: VkResult {} format {} usage {:#x}", static_cast<int>(result), static_cast<int>(format), usage);
                return nullptr;
            }
            VkMemoryRequirements required = {};
            vkGetImageMemoryRequirements(this->logical_device, answer->native, &required);
            VkPhysicalDeviceMemoryProperties properties = {};
            vkGetPhysicalDeviceMemoryProperties(this->physical_device, &properties);
            std::uint32_t memory_type = properties.memoryTypeCount;
            for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
                if ((required.memoryTypeBits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                    memory_type = i;
                    break;
                }
            }
            if (memory_type == properties.memoryTypeCount)
                return nullptr;
            VkMemoryAllocateInfo allocation = {};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = required.size;
            allocation.memoryTypeIndex = memory_type;
            result = vkAllocateMemory(this->logical_device, &allocation, nullptr, &answer->memory);
            if (result == VK_SUCCESS)
                result = vkBindImageMemory(this->logical_device, answer->native, answer->memory, 0);
            if (result != VK_SUCCESS) {
                deren::utility::log("rhi: image memory allocation/bind failed: VkResult {}", static_cast<int>(result));
                return nullptr;
            }
        }
        this->register_resource(answer.get(), resource_kind::image, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::image_view* core::create_image_view(rhi::image_view_desc const& desc) {
        if (!complete(desc) || desc.resource == nullptr || desc.aspects == 0 || (desc.aspects & ~7u) != 0)
            return nullptr;
        auto* const image = static_cast<image_resource*>(this->find_resource(desc.resource, resource_kind::image).implementation);
        if (image == nullptr || desc.base_mip_level >= image->mip_levels || desc.base_array_layer >= image->array_layers)
            return nullptr;
        auto const mips = desc.mip_level_count == 0 ? image->mip_levels - desc.base_mip_level : desc.mip_level_count;
        auto const layers = desc.array_layer_count == 0 ? image->array_layers - desc.base_array_layer : desc.array_layer_count;
        if (mips > image->mip_levels - desc.base_mip_level || layers > image->array_layers - desc.base_array_layer)
            return nullptr;
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
        switch (desc.dimension) {
        case rhi::image_view_dimension::texture_2d:
            if (layers != 1 || image->dimension == rhi::image_dimension::texture_3d)
                return nullptr;
            break;
        case rhi::image_view_dimension::texture_2d_array:
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            if (image->dimension == rhi::image_dimension::texture_3d)
                return nullptr;
            break;
        case rhi::image_view_dimension::texture_3d:
            type = VK_IMAGE_VIEW_TYPE_3D;
            if (image->dimension != rhi::image_dimension::texture_3d)
                return nullptr;
            break;
        case rhi::image_view_dimension::cube:
            type = VK_IMAGE_VIEW_TYPE_CUBE;
            if (image->dimension != rhi::image_dimension::cube || layers != 6 || desc.base_array_layer % 6 != 0)
                return nullptr;
            break;
        case rhi::image_view_dimension::cube_array:
            type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            if (image->dimension != rhi::image_dimension::cube || layers % 6 != 0 || desc.base_array_layer % 6 != 0)
                return nullptr;
            break;
        default:
            return nullptr;
        }
        auto const format = desc.format == rhi::image_format::unknown ? image->image_format : desc.format;
        // Images are not created mutable-format: a view cannot silently reinterpret their storage.
        if (format != image->image_format)
            return nullptr;
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image->native;
        info.viewType = type;
        info.format = native_format(format);
        info.subresourceRange = {desc.aspects, desc.base_mip_level, mips, desc.base_array_layer, layers};
        auto answer = std::make_unique<view_resource>();
        answer->owner = this;
        VkResult const result = vkCreateImageView(this->logical_device, &info, nullptr, &answer->native);
        if (result != VK_SUCCESS) {
            deren::utility::log("rhi: create_image_view failed: VkResult {}", static_cast<int>(result));
            return nullptr;
        }
        image->retain();
        answer->image = image;
        this->register_resource(answer.get(), resource_kind::image_view, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::sampler* core::create_sampler(rhi::sampler_desc const& desc) {
        if (!complete(desc) || !std::isfinite(desc.min_lod) || !std::isfinite(desc.max_lod) || !std::isfinite(desc.lod_bias) ||
            desc.min_lod > desc.max_lod || desc.max_anisotropy < 1.0f || !std::isfinite(desc.max_anisotropy) ||
            static_cast<std::uint32_t>(desc.min_filter) > 1 || static_cast<std::uint32_t>(desc.mag_filter) > 1 ||
            static_cast<std::uint32_t>(desc.mip_filter) > 1 || static_cast<std::uint32_t>(desc.address_u) > 3 ||
            static_cast<std::uint32_t>(desc.address_v) > 3 || static_cast<std::uint32_t>(desc.address_w) > 3 ||
            static_cast<std::uint32_t>(desc.comparison) > 7)
            return nullptr;
        VkPhysicalDeviceFeatures features = {};
        vkGetPhysicalDeviceFeatures(this->physical_device, &features);
        if ((desc.anisotropy_enabled != 0 && (features.samplerAnisotropy == VK_FALSE || desc.max_anisotropy > this->device_properties.limits.maxSamplerAnisotropy)) ||
            std::abs(desc.lod_bias) > this->device_properties.limits.maxSamplerLodBias) {
            deren::utility::log("rhi: create_sampler requests unsupported anisotropy or LOD bias");
            return nullptr;
        }
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.minFilter = static_cast<VkFilter>(desc.min_filter);
        info.magFilter = static_cast<VkFilter>(desc.mag_filter);
        info.mipmapMode = static_cast<VkSamplerMipmapMode>(desc.mip_filter);
        info.addressModeU = static_cast<VkSamplerAddressMode>(desc.address_u);
        info.addressModeV = static_cast<VkSamplerAddressMode>(desc.address_v);
        info.addressModeW = static_cast<VkSamplerAddressMode>(desc.address_w);
        info.minLod = desc.min_lod;
        info.maxLod = desc.max_lod;
        info.mipLodBias = desc.lod_bias;
        info.anisotropyEnable = desc.anisotropy_enabled != 0;
        info.maxAnisotropy = desc.max_anisotropy;
        info.compareEnable = desc.compare_enabled != 0;
        info.compareOp = static_cast<VkCompareOp>(desc.comparison);
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        auto answer = std::make_unique<sampler_resource>();
        answer->owner = this;
        VkResult const result = vkCreateSampler(this->logical_device, &info, nullptr, &answer->native);
        if (result != VK_SUCCESS) {
            deren::utility::log("rhi: create_sampler failed: VkResult {}", static_cast<int>(result));
            return nullptr;
        }
        this->register_resource(answer.get(), resource_kind::sampler, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::shader* core::create_shader(rhi::shader_desc const& desc) {
        if (!complete(desc) || desc.code.size() < 20 || desc.code.size() % 4 != 0 || native_stage(desc.stage) == 0)
            return nullptr;
        std::uint32_t magic = 0;
        std::memcpy(&magic, desc.code.data(), sizeof(magic));
        if (magic != 0x07230203u)
            return nullptr;
        // VkShaderModule requires uint32 alignment; borrowed byte spans need not be aligned.
        std::vector<std::uint32_t> words(desc.code.size() / 4);
        std::memcpy(words.data(), desc.code.data(), desc.code.size());
        VkShaderModuleCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = desc.code.size();
        info.pCode = words.data();
        auto answer = std::make_unique<shader_resource>();
        answer->owner = this;
        answer->stage = desc.stage;
        answer->code.assign(desc.code.begin(), desc.code.end());
        VkResult const result = vkCreateShaderModule(this->logical_device, &info, nullptr, &answer->native);
        if (result != VK_SUCCESS) {
            deren::utility::log("rhi: create_shader failed: VkResult {}", static_cast<int>(result));
            return nullptr;
        }
        this->register_resource(answer.get(), resource_kind::shader, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::pipeline* core::create_pipeline(rhi::pipeline_desc const& desc) {
        if (!complete(desc) || desc.stages.empty() || desc.stages.size() > std::numeric_limits<std::uint32_t>::max())
            return nullptr;
        std::vector<rhi::object_manager<rhi::shader>> temporary;
        std::vector<shader_resource*> shaders;
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (auto const& stage : desc.stages) {
            if (stage.entry_point == nullptr || stage.entry_point[0] == '\0' || (stage.module != nullptr && !stage.code.empty()))
                return nullptr;
            rhi::shader* module = stage.module;
            if (module == nullptr) {
                temporary.emplace_back(this->create_shader(rhi::shader_desc{.stage = stage.stage, .code = stage.code}));
                module = temporary.back().get();
            }
            auto* const shader = static_cast<shader_resource*>(this->find_resource(module, resource_kind::shader).implementation);
            if (shader == nullptr || shader->stage != stage.stage)
                return nullptr;
            shaders.push_back(shader);
            VkPipelineShaderStageCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage = native_stage(stage.stage);
            info.module = shader->native;
            info.pName = stage.entry_point;
            stages.push_back(info);
        }
        auto answer = std::make_unique<pipeline_resource>();
        answer->owner = this;
        VkResult result = VK_SUCCESS;
        if (desc.kind == rhi::pipeline_kind::graphics) {
            if (stages.size() != 2 || (stages[0].stage != VK_SHADER_STAGE_VERTEX_BIT && stages[0].stage != VK_SHADER_STAGE_MESH_BIT_EXT) ||
                stages[1].stage != VK_SHADER_STAGE_FRAGMENT_BIT || std::string_view(stages[0].pName) != "main" ||
                std::string_view(stages[1].pName) != "main" || desc.sample_count == 0 || desc.sample_count > 64 ||
                (desc.sample_count & (desc.sample_count - 1)) != 0 || static_cast<std::uint32_t>(desc.depth_comparison) > 7 ||
                (!desc.blends.empty() && desc.blends.size() != desc.color_formats.size()))
                return nullptr;
            if (stages[0].stage == VK_SHADER_STAGE_MESH_BIT_EXT && !this->mesh_shader_available) {
                deren::utility::log("rhi: create_pipeline requests meshShader, which was not enabled");
                return nullptr;
            }
            std::vector<VkFormat> formats;
            for (auto const format : desc.color_formats) {
                VkFormat const native = native_format(format);
                if (native == VK_FORMAT_UNDEFINED)
                    return nullptr;
                formats.push_back(native);
            }
            std::vector<VkPipelineColorBlendAttachmentState> blends;
            for (auto const& blend : desc.blends) {
                if (static_cast<std::uint32_t>(blend.source_color) > 9 || static_cast<std::uint32_t>(blend.destination_color) > 9 ||
                    static_cast<std::uint32_t>(blend.source_alpha) > 9 || static_cast<std::uint32_t>(blend.destination_alpha) > 9 ||
                    static_cast<std::uint32_t>(blend.color_operation) > 4 || static_cast<std::uint32_t>(blend.alpha_operation) > 4 || (blend.write_mask & ~15u) != 0)
                    return nullptr;
                blends.push_back({blend.enabled != 0, static_cast<VkBlendFactor>(blend.source_color), static_cast<VkBlendFactor>(blend.destination_color),
                                  static_cast<VkBlendOp>(blend.color_operation), static_cast<VkBlendFactor>(blend.source_alpha), static_cast<VkBlendFactor>(blend.destination_alpha),
                                  static_cast<VkBlendOp>(blend.alpha_operation), blend.write_mask});
            }
            auto built = deren::vulkan::make_pipeline(this->logical_device, std::span<VkFormat const>(formats), native_format(desc.depth_format),
                                                      std::span<std::uint8_t const>(shaders[0]->code), std::span<std::uint8_t const>(shaders[1]->code), static_cast<VkSampleCountFlagBits>(desc.sample_count),
                                                      desc.depth_test_enabled != 0, desc.depth_bias_constant, desc.depth_bias_slope, desc.depth_bias_clamp,
                                                      std::span<VkPipelineColorBlendAttachmentState const>(blends), stages[0].stage, static_cast<VkCompareOp>(desc.depth_comparison));
            if (!built) {
                deren::utility::log("rhi: create_pipeline graphics failed: {}", built.error());
                return nullptr;
            }
            // vk_pipeline::release() destroys the object; transfer by clearing its ownership fields.
            answer->native = built->pipeline;
            built->pipeline = VK_NULL_HANDLE;
            built->device = VK_NULL_HANDLE;
        } else if (desc.kind == rhi::pipeline_kind::compute) {
            if (stages.size() != 1 || stages[0].stage != VK_SHADER_STAGE_COMPUTE_BIT)
                return nullptr;
            VkPipelineCreateFlags2CreateInfo flags = {};
            flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
            flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
            VkComputePipelineCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.pNext = &flags;
            info.stage = stages[0];
            result = vkCreateComputePipelines(this->logical_device, VK_NULL_HANDLE, 1, &info, nullptr, &answer->native);
        } else if (desc.kind == rhi::pipeline_kind::ray_tracing) {
            auto const create = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(this->logical_device, "vkCreateRayTracingPipelinesKHR"));
            if (!this->ray_tracing_pipeline_available || create == nullptr || desc.ray_groups.empty() || desc.max_ray_recursion_depth == 0 ||
                desc.max_ray_recursion_depth > this->ray_tracing_pipeline_properties.maxRayRecursionDepth || desc.ray_groups.size() > UINT32_MAX) {
                deren::utility::log("rhi: create_pipeline ray tracing is unavailable or its group/recursion descriptor is empty");
                return nullptr;
            }
            std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
            for (auto const& group : desc.ray_groups) {
                auto const valid = [&stages](std::uint32_t index) { return index == rhi::unused_shader || index < stages.size(); };
                if (!valid(group.general_shader) || !valid(group.closest_hit_shader) || !valid(group.any_hit_shader) || !valid(group.intersection_shader) ||
                    static_cast<std::uint32_t>(group.kind) > 2)
                    return nullptr;
                auto const stage_matches = [&stages](std::uint32_t index, VkShaderStageFlagBits required) {
                    return index == rhi::unused_shader || stages[index].stage == required;
                };
                if (!stage_matches(group.closest_hit_shader, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR) ||
                    !stage_matches(group.any_hit_shader, VK_SHADER_STAGE_ANY_HIT_BIT_KHR) ||
                    !stage_matches(group.intersection_shader, VK_SHADER_STAGE_INTERSECTION_BIT_KHR))
                    return nullptr;
                if (group.kind == rhi::ray_group_kind::general) {
                    if (group.general_shader == rhi::unused_shader || group.closest_hit_shader != rhi::unused_shader ||
                        group.any_hit_shader != rhi::unused_shader || group.intersection_shader != rhi::unused_shader)
                        return nullptr;
                    auto const stage = stages[group.general_shader].stage;
                    if (stage != VK_SHADER_STAGE_RAYGEN_BIT_KHR && stage != VK_SHADER_STAGE_MISS_BIT_KHR && stage != VK_SHADER_STAGE_CALLABLE_BIT_KHR)
                        return nullptr;
                } else if (group.general_shader != rhi::unused_shader ||
                           (group.kind == rhi::ray_group_kind::triangles && group.intersection_shader != rhi::unused_shader) ||
                           (group.kind == rhi::ray_group_kind::procedural && group.intersection_shader == rhi::unused_shader))
                    return nullptr;
                VkRayTracingShaderGroupCreateInfoKHR info = {};
                info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
                info.type = static_cast<VkRayTracingShaderGroupTypeKHR>(group.kind);
                info.generalShader = group.general_shader;
                info.closestHitShader = group.closest_hit_shader;
                info.anyHitShader = group.any_hit_shader;
                info.intersectionShader = group.intersection_shader;
                groups.push_back(info);
            }
            VkPipelineCreateFlags2CreateInfo flags = {};
            flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
            flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
            VkRayTracingPipelineCreateInfoKHR info = {};
            info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
            info.pNext = &flags;
            info.stageCount = static_cast<std::uint32_t>(stages.size());
            info.pStages = stages.data();
            info.groupCount = static_cast<std::uint32_t>(groups.size());
            info.pGroups = groups.data();
            info.maxPipelineRayRecursionDepth = desc.max_ray_recursion_depth;
            result = create(this->logical_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &answer->native);
        } else
            return nullptr;
        if (result != VK_SUCCESS || answer->native == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_pipeline failed: VkResult {} kind {}", static_cast<int>(result), static_cast<std::uint32_t>(desc.kind));
            return nullptr;
        }
        this->register_resource(answer.get(), resource_kind::pipeline, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::query* core::create_query(rhi::query_desc const& desc) {
        if (!complete(desc) || desc.count == 0 || static_cast<std::uint32_t>(desc.kind) > 2 ||
            (desc.kind != rhi::query_kind::pipeline_statistics && desc.pipeline_statistics != 0))
            return nullptr;
        if (desc.kind == rhi::query_kind::pipeline_statistics) {
            VkPhysicalDeviceFeatures features = {};
            vkGetPhysicalDeviceFeatures(this->physical_device, &features);
            if (features.pipelineStatisticsQuery == VK_FALSE || desc.pipeline_statistics == 0) {
                deren::utility::log("rhi: create_query pipeline statistics are unavailable or no counters were selected");
                return nullptr;
            }
        }
        VkQueryPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = desc.kind == rhi::query_kind::timestamp ? VK_QUERY_TYPE_TIMESTAMP : (desc.kind == rhi::query_kind::occlusion ? VK_QUERY_TYPE_OCCLUSION : VK_QUERY_TYPE_PIPELINE_STATISTICS);
        info.queryCount = desc.count;
        info.pipelineStatistics = desc.pipeline_statistics;
        auto answer = std::make_unique<query_resource>();
        answer->owner = this;
        VkResult const result = vkCreateQueryPool(this->logical_device, &info, nullptr, &answer->native);
        if (result != VK_SUCCESS) {
            deren::utility::log("rhi: create_query failed: VkResult {}", static_cast<int>(result));
            return nullptr;
        }
        this->register_resource(answer.get(), resource_kind::query, reinterpret_cast<void*>(answer->native), answer.get());
        return answer.release();
    }

    rhi::swapchain* core::create_swapchain(rhi::swapchain_desc const& desc) {
        if (!complete(desc) || this->swap_chain == VK_NULL_HANDLE)
            return nullptr;
        auto answer = std::make_unique<swapchain_resource>();
        answer->owner = this;
        this->register_resource(answer.get(), resource_kind::swapchain, reinterpret_cast<void*>(this->swap_chain), answer.get());
        return answer.release();
    }

    void* core::frame_escape::native_image(rhi::image const& resource) const noexcept {
        if (&resource == &this->owner->frame_image_view)
            return reinterpret_cast<void*>(this->owner->frame_image_view.handle());
        return this->owner->find_resource(&resource, resource_kind::image).native;
    }
    void* core::frame_escape::native_image_view(rhi::image_view const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::image_view).native;
    }
    void* core::frame_escape::native_sampler(rhi::sampler const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::sampler).native;
    }
    void* core::frame_escape::native_shader(rhi::shader const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::shader).native;
    }
    void* core::frame_escape::native_pipeline(rhi::pipeline const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::pipeline).native;
    }
    void* core::frame_escape::native_query(rhi::query const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::query).native;
    }
    void* core::frame_escape::native_swapchain(rhi::swapchain const& resource) const noexcept {
        return this->owner->find_resource(&resource, resource_kind::swapchain).implementation != nullptr ? reinterpret_cast<void*>(this->owner->swap_chain) : nullptr;
    }
} // namespace deren::vulkan
