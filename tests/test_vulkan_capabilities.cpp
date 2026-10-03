#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

// Only driver entry points are doubled. The generated header contains the actual
// production bodies, not a model of how their pNext chains ought to work.
namespace deren::utility {
    template <class... Args>
    void error(Args&&...) {
    }
    template <class... Args>
    void log(Args&&...) {
    }
    [[noreturn]] inline void panic(char const*) {
        std::abort();
    }
} // namespace deren::utility

namespace {
    struct driver_fixture {
        std::vector<char const*> extensions;
        bool micromap = true;
        bool untyped = true;
        bool dynamic_rendering = true;
        bool general_src = true;
        bool general_dst = true;
        bool heap = true;
        bool unified_layouts = true;
        bool host_copy = true;
    };
    std::array<driver_fixture, 2> fixtures;
    std::vector<VkStructureType> queried_features, queried_properties;

    driver_fixture& fixture(VkPhysicalDevice device) {
        auto const id = reinterpret_cast<uintptr_t>(device);
        return fixtures[id == 2 ? 1 : 0];
    }
    VkPhysicalDevice device(uintptr_t id) {
        return reinterpret_cast<VkPhysicalDevice>(id);
    }
    std::vector<char const*> full_extensions() {
        return {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
                VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
                VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
                VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
                VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME};
    }
    void reset() {
        fixtures = {driver_fixture{full_extensions()}, driver_fixture{full_extensions()}};
    }
    void remove_extension(driver_fixture& f, char const* name) {
        std::erase_if(f.extensions, [name](char const* s) { return std::string_view(s) == name; });
    }
} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, char const*, uint32_t* count, VkExtensionProperties* out) {
    auto const& list = fixture(d).extensions;
    if (!out) {
        *count = static_cast<uint32_t>(list.size());
        return VK_SUCCESS;
    }
    *count = std::min(*count, static_cast<uint32_t>(list.size()));
    for (uint32_t i = 0; i < *count; ++i) {
        out[i] = {};
        std::strcpy(out[i].extensionName, list[i]);
    }
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t* count, VkPhysicalDevice* out) {
    if (out) {
        out[0] = device(1);
        out[1] = device(2);
    }
    *count = 2;
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_3;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* out) {
    *out = {};
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* out) {
    if (out) {
        *out = {};
        out->queueCount = 1;
        out->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    }
    *count = 1;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* out) {
    *out = VK_TRUE;
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* head) {
    queried_features.clear();
    auto const& f = fixture(d);
    for (auto* n = reinterpret_cast<VkBaseOutStructure*>(head); n; n = n->pNext) {
        queried_features.push_back(n->sType);
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(n)->dynamicRendering = f.dynamic_rendering;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(n)->descriptorHeap = f.heap;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceShaderUntypedPointersFeaturesKHR*>(n)->shaderUntypedPointers = f.untyped;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesEXT*>(n)->meshShader = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR*>(n)->unifiedImageLayouts = f.unified_layouts;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceHostImageCopyFeaturesEXT*>(n)->hostImageCopy = f.host_copy;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceAccelerationStructureFeaturesKHR*>(n)->accelerationStructure = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceRayQueryFeaturesKHR*>(n)->rayQuery = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceRayTracingPipelineFeaturesKHR*>(n)->rayTracingPipeline = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceOpacityMicromapFeaturesEXT*>(n)->micromap = f.micromap;
            break;
        default:
            break;
        }
    }
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* head) {
    queried_properties.clear();
    for (auto* n = reinterpret_cast<VkBaseOutStructure*>(head); n; n = n->pNext) {
        queried_properties.push_back(n->sType);
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT: {
            auto* p = reinterpret_cast<VkPhysicalDeviceHostImageCopyPropertiesEXT*>(n);
            p->pCopySrcLayouts[0] = fixture(d).general_src ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            p->copySrcLayoutCount = 1;
            p->pCopyDstLayouts[0] = fixture(d).general_dst ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            p->copyDstLayoutCount = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR: {
            auto* p = reinterpret_cast<VkPhysicalDeviceRayTracingPipelinePropertiesKHR*>(n);
            p->shaderGroupHandleSize = 32;
            p->shaderGroupHandleAlignment = 32;
            p->shaderGroupBaseAlignment = 64;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR:
            reinterpret_cast<VkPhysicalDeviceAccelerationStructurePropertiesKHR*>(n)->minAccelerationStructureScratchOffsetAlignment = 256;
            break;
        default:
            break;
        }
    }
}

#include "vulkan_query_fixture.h"

int main() {
    // Unrelated optional extensions must not hide heap shaders or RT limits.
    for (auto const missing : {VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
                               VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
                               VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME}) {
        reset();
        remove_extension(fixtures[0], missing);
        device_capabilities caps;
        caps.query(device(1));
        bool const heap = std::string_view(missing) != VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
        CHECK_MSG(caps.untyped_pointers_available == heap, missing);
        bool const rt = std::string_view(missing) != VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        CHECK_MSG(caps.ray_tracing_pipeline_available == rt, missing);
        if (rt) {
            CHECK_MSG(caps.ray_tracing_pipeline_properties.shaderGroupHandleSize == 32, missing);
            CHECK_MSG(caps.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment == 256, missing);
        }
    }
    reset();
    fixtures[0].micromap = false;
    device_capabilities caps;
    caps.query(device(1));
    CHECK(!caps.opacity_micromap_available);
    CHECK(caps.ray_tracing_pipeline_properties.shaderGroupHandleSize == 32);

    // The first GPU has usable queues/swapchain but lacks a mandatory renderer capability.
    // Selecting it instead of the second GPU is the bug; each fixture changes one requirement.
    for (int scenario = 0; scenario < 12; ++scenario) {
        reset();
        switch (scenario) {
        case 0:
            remove_extension(fixtures[0], VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
            break;
        case 1:
            remove_extension(fixtures[0], VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);
            break;
        case 2:
            fixtures[0].untyped = false;
            break;
        case 3:
            remove_extension(fixtures[0], VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
            break;
        case 4:
            remove_extension(fixtures[0], VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
            break;
        case 5:
            fixtures[0].general_src = false;
            break;
        case 6:
            fixtures[0].general_dst = false;
            break;
        case 7:
            fixtures[0].dynamic_rendering = false;
            break;
        case 8:
            fixtures[0].heap = false;
            break;
        case 9:
            fixtures[0].unified_layouts = false;
            break;
        case 10:
            fixtures[0].host_copy = false;
            break;
        case 11:
            remove_extension(fixtures[0], VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            break;
        }
        CHECK(pick_suitable_device(VK_NULL_HANDLE, reinterpret_cast<VkSurfaceKHR>(uintptr_t(1))) == device(2));
    }
    reset();
    CHECK(pick_suitable_device(VK_NULL_HANDLE, reinterpret_cast<VkSurfaceKHR>(uintptr_t(1))) == device(1));
    for (char const* optional : {VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
                                 VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME}) {
        reset();
        remove_extension(fixtures[0], optional);
        CHECK(pick_suitable_device(VK_NULL_HANDLE, reinterpret_cast<VkSurfaceKHR>(uintptr_t(1))) == device(1));
    }
    reset();
    fixtures[0].extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    fixtures[0].dynamic_rendering = false;
    device_capabilities incomplete;
    incomplete.query(device(1));
    CHECK(incomplete.renderer_missing_requirements().size() == 6);
    return deren::vk_test::finish("test_vulkan_capabilities");
}
