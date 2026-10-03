#include "texture_regions_fixture.h"
#include "texture_upload_layout.h"
#include "vk_test.h"

#include <array>
#include <limits>
#include <vector>

int main() {
    using deren::vulkan::detail::packed_image_size;
    struct test_case {
        VkFormat format;
        uint32_t width, height, layers, mips;
        VkDeviceSize bytes;
    };
    // Expected byte counts are hand-derived from the Vulkan format block sizes.
    test_case const cases[] = {
        {VK_FORMAT_R8_UNORM, 5, 7, 1, 1, 35},
        {VK_FORMAT_R8G8B8A8_UNORM, 5, 7, 2, 3, 336}, // (35 + 6 + 1) * 4 * 2
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 1, 1, 8},
        {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 5, 7, 2, 3, 96}, // (4 + 1 + 1) blocks * 8 * 2
        {VK_FORMAT_BC4_UNORM_BLOCK, 4, 4, 1, 1, 8},
        {VK_FORMAT_BC4_SNORM_BLOCK, 1, 1, 1, 1, 8},
        {VK_FORMAT_BC2_UNORM_BLOCK, 5, 5, 1, 1, 64},
        {VK_FORMAT_BC3_SRGB_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_BC7_SRGB_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 4, 4, 1, 1, 8},
        {VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, 4, 4, 1, 1, 8},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_EAC_R11_UNORM_BLOCK, 4, 4, 1, 1, 8},
        {VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 4, 4, 1, 1, 16},
        {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 5, 5, 1, 1, 64},
        {VK_FORMAT_ASTC_5x4_SRGB_BLOCK, 6, 5, 1, 1, 64},
        {VK_FORMAT_ASTC_5x5_UNORM_BLOCK, 5, 5, 1, 1, 16},
        {VK_FORMAT_ASTC_6x5_UNORM_BLOCK, 6, 5, 1, 1, 16},
        {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 6, 6, 1, 1, 16},
        {VK_FORMAT_ASTC_8x5_UNORM_BLOCK, 8, 5, 1, 1, 16},
        {VK_FORMAT_ASTC_8x6_UNORM_BLOCK, 8, 6, 1, 1, 16},
        {VK_FORMAT_ASTC_8x8_UNORM_BLOCK, 8, 8, 1, 1, 16},
        {VK_FORMAT_ASTC_10x5_UNORM_BLOCK, 10, 5, 1, 1, 16},
        {VK_FORMAT_ASTC_10x6_UNORM_BLOCK, 10, 6, 1, 1, 16},
        {VK_FORMAT_ASTC_10x8_UNORM_BLOCK, 10, 8, 1, 1, 16},
        {VK_FORMAT_ASTC_10x10_UNORM_BLOCK, 10, 10, 1, 1, 16},
        {VK_FORMAT_ASTC_12x10_UNORM_BLOCK, 12, 10, 1, 1, 16},
        {VK_FORMAT_ASTC_12x12_SRGB_BLOCK, 13, 13, 1, 1, 64},
    };
    for (auto const& c : cases) {
        auto const size = packed_image_size(c.format, c.width, c.height, c.layers, c.mips);
        CHECK(size.has_value());
        CHECK_MSG(size == c.bytes, "tightly packed mip/layer byte count");
    }
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, 0, 1, 1, 1));
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, 1, 0, 1, 1));
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, 1, 1, 0, 1));
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, 1, 1, 1, 0));
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, 1, 1, 1, 2));
    CHECK(!packed_image_size(VK_FORMAT_R64G64_UINT, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1));
    CHECK(!packed_image_size(VK_FORMAT_UNDEFINED, 4, 4, 1, 1));
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, UINT32_MAX, 1, 1, 33));
    CHECK(packed_image_size(VK_FORMAT_R8_UNORM, UINT32_MAX, 1, 1, 32) == 8589934558ull);
    // Each mip fits in uint64, but adding the second mip would overflow it.
    CHECK(!packed_image_size(VK_FORMAT_R8_UNORM, UINT32_MAX, UINT32_MAX, 1, 2));

    std::array<uint8_t, 96> payload{};
    auto const regions = deren::vulkan::host_copy_regions(payload.data(), {5, 7, 2, 3, VK_FORMAT_BC1_RGBA_UNORM_BLOCK});
    CHECK(regions.size() == 3);
    CHECK(regions[0].pHostPointer == payload.data());
    CHECK(regions[1].pHostPointer == payload.data() + 64);
    CHECK(regions[2].pHostPointer == payload.data() + 80);
    CHECK(regions[1].imageExtent.width == 2);
    CHECK(regions[1].imageExtent.height == 3);
    CHECK(regions[2].imageSubresource.mipLevel == 2);
    CHECK(regions[2].imageSubresource.layerCount == 2);
    return deren::vk_test::finish("test_texture_upload_layout");
}
