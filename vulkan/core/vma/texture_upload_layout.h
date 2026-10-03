#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <vulkan/vulkan.h>

// Backend-private CPU layout calculation, shared by payload validation and mip offsets.
namespace deren::vulkan::detail {
    struct format_block {
        uint32_t width, height, bytes;
    };
    constexpr std::optional<format_block> vk_format_block(VkFormat const format) noexcept {
        switch (format) {
        // 8-bit single channel
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
            return format_block{1, 1, 1};

        // 16-bit single channel / 8-bit dual channel
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
            return format_block{1, 1, 2};

        // 24-bit
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8_SRGB:
            return format_block{1, 1, 3};

        // 32-bit
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_USCALED:
        case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
            return format_block{1, 1, 4};

        // 64-bit
        case VK_FORMAT_R64_UINT:
        case VK_FORMAT_R64_SINT:
        case VK_FORMAT_R64_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return format_block{1, 1, 8};

        // 96-bit
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return format_block{1, 1, 12};

        // 128-bit
        case VK_FORMAT_R64G64_UINT:
        case VK_FORMAT_R64G64_SINT:
        case VK_FORMAT_R64G64_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return format_block{1, 1, 16};

        // Depth/stencil
        case VK_FORMAT_D16_UNORM:
            return format_block{1, 1, 2};
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
            return format_block{1, 1, 4};
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return format_block{1, 1, 8};
        // Stencil-only: one byte per texel. It used to fall through into the BC1 group below and be
        // reported as 8.
        case VK_FORMAT_S8_UINT:
            return format_block{1, 1, 1};

        // BC / ETC2 / EAC: 4 x 4 texels per block.
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            return format_block{4, 4, 8};

        // BC / ETC2 / EAC: 4 x 4 texels per block.
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return format_block{4, 4, 16};

        // ASTC block extent varies by format; every block occupies 16 bytes.
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK:
            return format_block{4, 4, 16};
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK:
            return format_block{5, 4, 16};
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK:
            return format_block{5, 5, 16};
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK:
            return format_block{6, 5, 16};
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK:
            return format_block{6, 6, 16};
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK:
            return format_block{8, 5, 16};
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK:
            return format_block{8, 6, 16};
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK:
            return format_block{8, 8, 16};
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK:
            return format_block{10, 5, 16};
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK:
            return format_block{10, 6, 16};
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK:
            return format_block{10, 8, 16};
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK:
            return format_block{10, 10, 16};
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK:
            return format_block{12, 10, 16};
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK:
            return format_block{12, 12, 16};

        default:
            return std::nullopt;
        }
    }

    // 先向上取整为块数，再检查乘法溢出；小于一个块的 mip 仍占完整块。
    constexpr std::optional<VkDeviceSize> packed_mip_size(VkFormat format, uint32_t width, uint32_t height, uint32_t layers) noexcept {
        auto const block = vk_format_block(format);
        if (!block || width == 0 || height == 0 || layers == 0) {
            return std::nullopt;
        }
        VkDeviceSize size = width / block->width + (width % block->width != 0);
        VkDeviceSize const rows = height / block->height + (height % block->height != 0);
        for (VkDeviceSize const factor : {rows, VkDeviceSize{layers}, VkDeviceSize{block->bytes}}) {
            if (size > std::numeric_limits<VkDeviceSize>::max() / factor) {
                return std::nullopt;
            }
            size *= factor;
        }
        return size;
    }

    constexpr std::optional<VkDeviceSize> packed_image_size(VkFormat format, uint32_t width, uint32_t height, uint32_t layers, uint32_t mips) noexcept {
        if (width == 0 || height == 0 || layers == 0 || mips == 0 || mips > static_cast<uint32_t>(std::bit_width(std::max(width, height)))) {
            return std::nullopt;
        }
        VkDeviceSize total = 0;
        for (uint32_t mip = 0; mip < mips; ++mip) {
            auto const size = packed_mip_size(format, std::max(1u, width >> mip), std::max(1u, height >> mip), layers);
            if (!size || total > std::numeric_limits<VkDeviceSize>::max() - *size) {
                return std::nullopt;
            }
            total += *size;
        }
        return total;
    }
} // namespace deren::vulkan::detail
