module;

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

export module deren.vulkan.core:init_utils;
export import deren.vstd;
import deren.utility;
import deren.vulkan.constant_init;

/**
 * @file init_utils.cppm
 * @defgroup vulkan_init_utils Vulkan Init Utils
 * @brief pure utility functions and data types for device/queue/swap chain selection and creation
 * @note
 *      - most functions are noexcept, failures either return a sentinel or panic
 *      - used by deren.vulkan.core during initialization
 */
/**
 * @ingroup vulkan_init_utils
 * @brief a created logical device together with its queue handles
 */
export struct logical_device {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family_index = 0;
    uint32_t present_family_index = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
};

/**
 * @ingroup vulkan_init_utils
 * @brief capabilities, formats and present modes supported by a swap chain
 */
export struct swap_chain_support_details {
    VkSurfaceCapabilitiesKHR capabilities = {};
    std::vector<VkSurfaceFormatKHR> formats = {};
    std::vector<VkPresentModeKHR> present_modes = {};
};

/**
 * @ingroup vulkan_init_utils
 * @brief queue family indices found on a physical device
 * @note is_complete() is true when both graphics and present families are found
 */
export struct queue_family_indices {
    std::optional<uint32_t> graphics_family = {};
    std::optional<uint32_t> present_family = {};
    std::optional<uint32_t> compute_family = {};
    std::optional<uint32_t> transfer_family = {};

    [[nodiscard]] bool is_complete() const noexcept {
        return graphics_family.has_value() && present_family.has_value();
    }
};

/**
 * @ingroup vulkan_init_utils
 * @brief collected physical device capabilities: core 1.x features and properties
 * @note
 *      - query() builds the pNext chains from the requested api_version and issues
 *        vkGetPhysicalDeviceFeatures2 + vkGetPhysicalDeviceProperties2 in one pass
 *      - after query(), features stay enabled where the driver supports them (except the
 *        deliberately disabled ones), so the struct is directly usable for vkCreateDevice
 *      - device_pnext() returns the feature chain head to pass via device_creation_info::pNext
 */
export struct device_capabilities {
    // ---- Feature chain (shared by query and device creation): the sType of every member is
    //      fixed at construction (designated initializer), so query() only re-wires the pNext
    //      chain and issues the vkGetPhysicalDevice*2 calls each time. The remaining members of
    //      the Vulkan chain structs are deliberately left to zero-initialization (correct for
    //      query/creation), which -Wmissing-designated-field-initializers would otherwise flag.
    //      THE PRAGMA IS CLANG-ONLY BY NAME, so it is guarded: MSVC does not know `#pragma clang
    //      diagnostic` and answers it with C4068 (unknown pragma), which /WX turns into an error. The
    //      warning it suppresses is a clang warning group with no MSVC spelling, and MSVC does not warn
    //      about partially-initialized designated initializers at /W4, so there is nothing to suppress
    //      there. This is the only first-party use of a clang-specific pragma/attribute/builtin in the
    //      tree (everything else lives under third_party/, which is compiled with /external:W0).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
    VkPhysicalDeviceFeatures2 features_2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features features_1_1 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features_1_2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features_1_3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features features_1_4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};

    // ---- Ray tracing (VK_KHR_acceleration_structure + VK_KHR_ray_query). These are EXTENSION
    //      features, so they cannot live in the core 1.x structs above: they are chained after them,
    //      and ONLY when the device advertises every extension they need. A struct whose extension is
    //      not enabled must not appear in the vkCreateDevice chain at all, so "is it linked" IS the
    //      availability flag - see ray_query_available below, which query() sets.
    //
    //      VK_KHR_deferred_host_operations is in the list because the acceleration-structure extension
    //      requires it (the build commands are specified in terms of it), not because this engine
    //      builds asynchronously - it builds on the frame thread and waits. ----
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    // ---- Ray tracing, the rest of it: the RT PIPELINE (with its maintenance1 features) and the OPACITY
    //      MICROMAP. Both are chained AFTER ray query and only when their own extensions are advertised,
    //      so "is it linked" is again the availability flag (see the note above). The pipeline is what
    //      turns the traced shadow from a per-pixel ray query into a traceRaysEXT dispatch that can run an
    //      any-hit shader; the micromap is what makes an alphaMode MASK surface opaque/transparent per
    //      MICROtriangle instead of per triangle - the hardware answer to the limitation rt_mask_bake's
    //      triangle-collapse rule measured as worse than doing nothing. ----
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR ray_tracing_maintenance1_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT opacity_micromap_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
    /// the subdivision levels the device allows, queried with the feature that gates them
    VkPhysicalDeviceOpacityMicromapPropertiesEXT opacity_micromap_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT};
    /// The three numbers a SHADER BINDING TABLE cannot be built without, and they are the reason this is
    /// queried before any pipeline exists: shaderGroupHandleSize is how many bytes one group's handle is,
    /// shaderGroupBaseAlignment is what an SBT REGION's device address must be a multiple of, and
    /// shaderGroupHandleAlignment is what a handle's address inside a region must be. A region whose
    /// stride is the handle size alone is invalid on a device whose handle alignment is larger - the
    /// classic first-attempt VUID - so the stride is this struct's business, not the pass's.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    // The two limits the builder needs are in here, next to the features that gate them:
    // minAccelerationStructureScratchOffsetAlignment (a scratch buffer's device address must be a
    // multiple of it) and maxInstanceCount/maxGeometryCount (what fits in one level).
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    /**
     * @brief whether the device has the acceleration-structure and ray-query extensions AND both
     *        features, i.e. whether the two structs above are in the query/device chains
     */
    bool ray_query_available = false;
    /// @brief whether the device has VK_KHR_ray_tracing_pipeline + maintenance1 AND the rayTracingPipeline
    ///        feature: a traceRaysEXT dispatch with a shader binding table needs both
    bool ray_tracing_pipeline_available = false;
    /// @brief whether the device has VK_EXT_opacity_micromap AND its feature (it needs the RT pipeline too)
    bool opacity_micromap_available = false;
    /**
     * @brief VK_EXT_descriptor_heap: descriptors in a buffer the APPLICATION manages, instead of descriptor
     *        sets, layouts and pools (see vulkan/core/core.cpp for what the renderer does with it)
     *
     * @note independent of everything above: the heap replaces the SET model rather than extending the
     *       ray-tracing one, so its two structs are the first extension links in the chains below.
     * @note the PROPERTIES are the numbers a heap cannot be laid out without: bufferDescriptorSize /
     *       imageDescriptorSize / samplerDescriptorSize are the strides a descriptor of each kind occupies
     *       (with their own alignment fields), resourceHeapAlignment and samplerHeapAlignment are what a heap
     *       binding's offset must respect, and minResourceHeapReservedRange plus
     *       minSamplerHeapReservedRangeWithEmbedded are how much of each heap an implementation wants
     *       reserved before descriptors may be placed after the reserved area - the WithEmbedded one being
     *       for combined image samplers whose sampler part lives in the resource heap.
     */
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    /**
     * @brief the feature the heap's SHADERS need, and the extension name that provides it
     *
     * @note A `descriptor_heap` declaration compiles to an UNTYPED POINTER (SPIR-V UntypedPointersKHR), so a
     *       module declaring one is refused unless VK_KHR_shader_untyped_pointers is enabled and this feature is
     *       on: "SPIR-V Capability UntypedPointersKHR was declared, but ... shaderUntypedPointers" - measured, by
     *       the heap-native probe (shaders/heap_probe_comp.slang), which was the first shader in this renderer to
     *       declare one. The name is kept beside the heap's own dependency for the same reason that one is: the
     *       device-creation list must not re-derive it.
     */
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    bool untyped_pointers_available = false;
    /// the extension NAME to enable, or nullptr when the feature is not available (see descriptor_heap_extension_name)
    char const* untyped_pointers_dependency = nullptr;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptor_heap_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    /**
     * @brief which extension satisfies the heap's own dependency, or nullptr when neither is present
     *
     * @note VK_EXT_descriptor_heap REQUIRES one of VK_KHR_extended_flags or VK_KHR_maintenance5 to be enabled
     *       alongside it, which validation states as VUID-vkCreateDevice-ppEnabledExtensionNames-01387 the
     *       moment the heap is enabled without one. It is held as a NAME rather than as a second bool because
     *       the device-creation list needs the string, and both are static string literals.
     */
    // called descriptor_heap_extension_name, not descriptor_heap_dependency: the local of that name in query()
    // below would hide a member of that name and MSVC /W4 reports C4458, an error under /WX
    char const* descriptor_heap_extension_name = nullptr;
    /// @brief whether the device has VK_EXT_descriptor_heap, its descriptorHeap feature, AND that dependency
    bool descriptor_heap_available = false;

    /**
     * @brief VK_EXT_mesh_shader: a MESH (and, when the device has it, a TASK) shader stage that REPLACES the
     *        vertex stage rather than extending it, so it is the second independent link in the chains below
     *
     * @note like the heap, this is an extension of how work is SUBMITTED rather than of ray tracing, so it
     *       joins the chains on its own. It has no extension dependency to resolve: VK_EXT_mesh_shader
     *       depends on VK_KHR_spirv_1_4 and VK_VERSION_1_2, and the engine's 1.3 device already satisfies
     *       the promoted one.
     *
     * @note the PROPERTIES are the limits a meshlet split cannot be sized without, and they are NOT readable
     *       from the physical device once the extension is not enabled: maxMeshOutputVertices /
     *       maxMeshOutputPrimitives / maxMeshOutputComponents / maxMeshOutputMemorySize are what one
     *       workgroup may EMIT, maxMeshWorkGroupSize is how many invocations it may have, and
     *       maxTaskWorkGroupSize is the same for the task stage. A device that reports the extension but
     *       not the feature reports these as zero, which is exactly what a mesh pipeline created on such a
     *       device fails on: "output vertices count exceeds the maxMeshOutputVertices of 0" (VUID
     *       07115/07116) - measured, on the heap-native probe before this feature was enabled.
     */
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    /// @brief whether the device has VK_EXT_mesh_shader AND its meshShader feature (taskShader is reported
    ///        separately by mesh_shader_features.taskShader, which is what a task shader must be gated on)
    bool mesh_shader_available = false;

    // ---- VK_KHR_unified_image_layouts: the feature that promises VK_IMAGE_LAYOUT_GENERAL is as efficient as
    //      the purpose-built layouts, which is what lets this renderer stop moving images between layouts
    //      altogether. It has no extension dependency left to resolve (VK_KHR_get_physical_device_properties2
    //      and VK_VERSION_1_1 are both satisfied by the 1.3 device this engine creates) and it adds no
    //      commands, so there is nothing else to enable alongside it.
    //      unifiedImageLayoutsVideo is NOT needed and is forced off below: video layouts are a separate feature
    //      bit that only matters when a video extension is enabled, and none is.
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_image_layouts_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    /// @brief whether the device has VK_KHR_unified_image_layouts AND its unifiedImageLayouts feature.
    ///        Unlike every other extension here this one is REQUIRED rather than optional-fall-back: a device
    ///        without it cannot run this renderer, so core.constructor refuses to create the device instead of
    ///        silently falling back to per-layout transitions.
    bool unified_image_layouts_available = false;

    // ---- VK_EXT_host_image_copy: a copy between image memory and host memory that the IMPLEMENTATION performs.
    //      It is REQUIRED by this renderer now (device creation refuses a device without it): the images the
    //      renderer creates for itself are uploaded with vkCopyMemoryToImageEXT and read back with
    //      vkCopyImageToMemoryEXT, so there is no second mechanism to fall back to. The feature bit is not the
    //      whole capability, though: a host copy may only read an image whose layout is one of the device's
    //      VkPhysicalDeviceHostImageCopyPropertiesEXT::pCopySrcLayouts, and only write one whose layout is in its
    //      pCopyDstLayouts, while this renderer keeps every image in GENERAL - so availability means "GENERAL is
    //      in the SOURCE list" (read-back) AND "GENERAL is in the DESTINATION list" (upload), checked separately
    //      below because a device may list one and not the other.
    VkPhysicalDeviceHostImageCopyFeaturesEXT host_image_copy_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT};
    VkPhysicalDeviceHostImageCopyPropertiesEXT host_image_copy_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT};
    /// @brief the layouts a host copy may read FROM (host_image_copy_properties.pCopySrcLayouts). A fixed array
    ///        because the only question asked of it is "is GENERAL listed": if a device listed more layouts than
    ///        fit here the tail would be invisible, whose worst case is a named startup failure (GENERAL not
    ///        found), never a copy issued in a layout the device does not accept.
    static constexpr uint32_t host_image_copy_max_src_layouts = 16;
    std::array<VkImageLayout, host_image_copy_max_src_layouts> host_image_copy_src_layouts = {};
    /// @brief the layouts a host copy may write TO (host_image_copy_properties.pCopyDstLayouts) - the upload
    ///        direction's list, kept separate from the source list for the same reason the properties are.
    std::array<VkImageLayout, host_image_copy_max_src_layouts> host_image_copy_dst_layouts = {};
    /// @brief whether the device has VK_EXT_host_image_copy, its hostImageCopy feature AND GENERAL among its
    ///        copy-SOURCE layouts - what a host copy of an image OUT of this renderer needs (the read-back).
    bool host_image_copy_available = false;
    /// @brief the same for the other direction: GENERAL among the device's copy-DESTINATION layouts, which is
    ///        what vkCopyMemoryToImageEXT (the image upload) needs. Separate, because the two lists differ.
    bool host_image_copy_upload_available = false;

    // ---- Property chain (query only, for renderer decisions/diagnostics) ----
    VkPhysicalDeviceProperties2 properties_2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceDriverProperties driver_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties subgroup_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceMaintenance4Properties maintenance4_properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    /**
     * @brief query all features and properties of the physical device
     * @param physical_device the device to query
     * @param api_version the API version to target, decides which 1.x structs are chained
     */
    void query(VkPhysicalDevice physical_device, uint32_t api_version = VK_API_VERSION_1_3) noexcept;

    // 选卡和创建设备共用同一份强制能力检查，避免选中不能启动渲染器的 GPU。
    [[nodiscard]] std::vector<char const*> renderer_missing_requirements() const {
        std::vector<char const*> missing;
        if (features_1_3.dynamicRendering != VK_TRUE) {
            missing.push_back("dynamicRendering (Vulkan 1.3)");
        }
        if (!descriptor_heap_available) {
            missing.push_back("VK_EXT_descriptor_heap and its dependency");
        }
        if (!untyped_pointers_available) {
            missing.push_back("VK_KHR_shader_untyped_pointers: shaderUntypedPointers");
        }
        if (!unified_image_layouts_available) {
            missing.push_back("VK_KHR_unified_image_layouts: unifiedImageLayouts");
        }
        if (!host_image_copy_available) {
            missing.push_back("VK_EXT_host_image_copy: hostImageCopy and GENERAL source layout");
        }
        if (!host_image_copy_upload_available) {
            missing.push_back("VK_EXT_host_image_copy: GENERAL destination layout");
        }
        return missing;
    }

    /**
     * @brief pNext chain head for vkCreateDevice (enables the queried features)
     */
    [[nodiscard]] void const* device_pnext() const noexcept;
};

/**
 * @ingroup vulkan_init_utils
 * @brief print a startup summary of the collected device capabilities
 * @param capabilities the queried capabilities
 * @note prints driver/api version/device name and all enabled 1.1/1.2/1.3 features
 */
export void print_device_capabilities(device_capabilities const& capabilities);

/**
 * @ingroup vulkan_init_utils
 * @brief input data for creating a logical device
 */
export struct device_creation_info {
    queue_family_indices queue_families = {};
    std::vector<char const*> extensions = {};
    std::vector<char const*> validation_layers = {};
    // Feature chain head for vkCreateDevice: must be a VkPhysicalDeviceFeatures2 struct (e.g.
    // device_capabilities::device_pnext()) whose .features carries the Vulkan 1.0 core features
    // and whose pNext links the 1.1/1.2/... feature structs. pEnabledFeatures stays NULL.
    void const* pNext = nullptr;
};

/**
 * @ingroup vulkan_init_utils
 * @brief Vulkan debug messenger callback, prints validation layer messages
 */
export [[maybe_unused]] VKAPI_ATTR VkBool32 VKAPI_CALL
debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT const message_severity,
    [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT message_type,
    VkDebugUtilsMessengerCallbackDataEXT const* callback_data,
    [[maybe_unused]] void* user_data) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief check whether the given validation layers are supported
 * @param validation_layers layer names to check
 * @return true if all layers are supported
 */
export bool check_validation_layer_support(std::vector<char const*> const& validation_layers) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief check whether the physical device supports the required device extensions
 * @param physical_device the device to check
 * @param required_extensions extension names that must be supported
 * @return true if all extensions are supported
 */
export bool check_device_extension_support(
    VkPhysicalDevice physical_device,
    std::vector<char const*> const& required_extensions) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief create a logical device and fetch its queues
 * @param physical_device the physical device to create from
 * @param create_info device features, extensions and validation layers
 * @return logical_device holding the device and queue handles on success; panics on failure
 */
export logical_device create_logical_device(
    VkPhysicalDevice physical_device,
    device_creation_info const& create_info) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief find graphics/present/compute/transfer queue family indices of a physical device
 * @param device the physical device to query
 * @param surface the presentation surface, may be VK_NULL_HANDLE
 * @return queue family indices, missing families are std::nullopt
 */
export queue_family_indices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief pick the most suitable physical device for the given surface
 * @param instance the vulkan instance
 * @param surface the presentation surface
 * @return the chosen physical device; panics if none is suitable
 */
export VkPhysicalDevice pick_suitable_device(VkInstance instance, VkSurfaceKHR surface) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief query swap chain support details of a physical device
 * @param device the physical device to query
 * @param surface the presentation surface
 * @return swap chain capabilities, formats and present modes
 */
export swap_chain_support_details query_swap_chain_support(VkPhysicalDevice device, VkSurfaceKHR surface) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief choose a present mode from the available ones
 * @param available_present_modes modes supported by the swap chain
 * @param vsync true prefers FIFO_KHR (vsync), false prefers MAILBOX_KHR (low latency)
 * @return the chosen present mode
 */
/**
 * @brief VK_PRESENT_MODE_FIFO_LATEST_READY (VK_EXT_present_mode_fifo_latest_ready, core in Vulkan 1.4)
 *
 * Spelled out numerically so the build does not depend on how new the Vulkan headers are: the value
 * is fixed by the extension. Like FIFO it is vsync-locked and never tears, but the presentation
 * engine shows the newest ready image at each vblank instead of draining a queue, so an application
 * that renders faster than the display does not pay FIFO's extra frame of latency.
 */
export constexpr VkPresentModeKHR present_mode_fifo_latest_ready = static_cast<VkPresentModeKHR>(1000361000);

export VkPresentModeKHR choose_swap_present_mode(std::vector<VkPresentModeKHR> const& available_present_modes, bool vsync = false) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief choose a surface format from the available ones
 * @param available_formats formats supported by the swap chain
 * @return the chosen surface format
 */
export VkSurfaceFormatKHR choose_swap_surface_format(std::vector<VkSurfaceFormatKHR> const& available_formats) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief choose the swap chain extent: the surface's own answer when it has one, else the window's
 *        framebuffer size, else the size the window was REQUESTED at
 * @param capabilities surface capabilities
 * @param window the GLFW window, used to query the framebuffer size
 * @param requested_width the width `glfwCreateWindow` was asked for, as the fallback
 * @param requested_height the height `glfwCreateWindow` was asked for, as the fallback
 * @return the chosen extent
 */
export VkExtent2D choose_swap_extent(VkSurfaceCapabilitiesKHR capabilities, GLFWwindow* window, int32_t requested_width, int32_t requested_height) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief find a memory type matching the filter and the required properties
 * @param type_filter bit mask of allowed memory types
 * @param properties required memory property flags
 * @param physical_device the physical device to query
 * @return the matching memory type index; panics if none matches
 */
export uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties, VkPhysicalDevice physical_device) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief find a supported depth format by preference order
 * @param physical_device the physical device to query
 * @return the chosen depth format; panics if none is supported
 */
export VkFormat find_depth_format(VkPhysicalDevice physical_device) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief create an image view for the given image
 * @param image the source image
 * @param format the image format
 * @param aspect_flags the image aspect mask
 * @param device the logical device
 * @return the created image view
 */
export VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief create an image view of a chosen dimensionality for the given image
 * @param image the source image
 * @param format the image format
 * @param aspect_flags the image aspect mask
 * @param device the logical device
 * @param view_type the view's dimensionality (VK_IMAGE_VIEW_TYPE_2D for everything the engine drew
 *        before this, VK_IMAGE_VIEW_TYPE_3D for a 3D image
 * @return the created image view
 * @note a view's type has to agree with the image's: a 2D view of a 3D image is a validation error the
 *       moment it is used, and a sampler3D binding needs the 3D one. One mip and one layer, like the
 *       2D overload - neither the grid nor any target here is mipped or layered.
 */
export VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device, VkImageViewType view_type) noexcept;

/**
 * @ingroup vulkan_init_utils
 * @brief create an image view of a chosen dimensionality AND layer count
 * @param image the source image
 * @param format the image format
 * @param aspect_flags the image aspect mask
 * @param device the logical device
 * @param view_type the view''s dimensionality (VK_IMAGE_VIEW_TYPE_CUBE for the constant environment below)
 * @param layer_count how many array layers the view covers (six for a cube, one for everything else)
 * @return the created image view
 * @note a layer count is what the other two overloads cannot express: they fix it at one, which is right for
 *       every target this engine drew before the verification mode needed a cube. A CUBE view of a
 *       six-layer image is the only user, and it needs all six layers in one view or the sampler sees one
 *       face.
 */
export VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device, VkImageViewType view_type, uint32_t layer_count) noexcept;

[[maybe_unused]] VKAPI_ATTR VkBool32 VKAPI_CALL
debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT const message_severity,
    [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT message_type,
    VkDebugUtilsMessengerCallbackDataEXT const* callback_data,
    [[maybe_unused]] void* user_data) noexcept {
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        // Errors go through error(): Debug prints red to stderr, Release writes to the log file
        deren::utility::error(callback_data->pMessage);
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        deren::utility::log("[WARNING] {}", callback_data->pMessage);
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        deren::utility::log("[INFO] {}", callback_data->pMessage);
    } else {
        deren::utility::log("[VERBOSE] {}", callback_data->pMessage);
    }

    return VK_FALSE; // VK_FALSE means not terminate this function call
}

bool check_validation_layer_support(std::vector<char const*> const& validation_layers) noexcept {
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    std::unordered_set<std::string> available_names;
    for (auto const& layer : available_layers) {
        available_names.insert(layer.layerName);
    }

    return std::ranges::all_of(validation_layers,
                               [&](char const* name) { return available_names.contains(name); });
}

bool check_device_extension_support(
    VkPhysicalDevice physical_device,
    std::vector<char const*> const& required_extensions) noexcept {
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                         available_extensions.data());

    std::set<std::string> required_set(required_extensions.begin(), required_extensions.end());
    for (auto const& [extension_name, spec_version] : available_extensions) {
        required_set.erase(extension_name);
    }

    return required_set.empty();
}

void device_capabilities::query(VkPhysicalDevice const physical_device, uint32_t const api_version) noexcept {
    // ---- Ray tracing first, because it decides whether two structs join the chains below: the
    //      feature structs of an extension that is not enabled must not be in the vkCreateDevice
    //      chain, and the device may support the extensions without the features (or vice versa). ----
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, available_extensions.data());
    auto const has_extension = [&available_extensions](char const* name) {
        return std::any_of(available_extensions.begin(), available_extensions.end(), [name](VkExtensionProperties const& entry) {
            return std::string_view(entry.extensionName) == name;
        });
    };
    bool const ray_tracing_extensions = has_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                                        has_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    // The RT pipeline needs the acceleration-structure set as well (it traces the same structures), and
    // the micromap needs the pipeline (its state is consumed by a traced ray, not by a query).
    bool const ray_tracing_pipeline_extensions = ray_tracing_extensions && has_extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                                                 has_extension(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
    bool const opacity_micromap_extension = ray_tracing_pipeline_extensions && has_extension(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
    // DESCRIPTOR HEAP, and it is an extension of the BINDING MODEL rather than of ray tracing: it replaces
    // descriptor sets with descriptors the application writes into a buffer, so it shares nothing with the two
    // conditions above and joins the chains as their FIRST extension link rather than after them.
    //
    // It also REQUIRES one of two other extensions to be enabled with it (VK_KHR_extended_flags or
    // VK_KHR_maintenance5), which validation reported as VUID-vkCreateDevice-ppEnabledExtensionNames-01387 the
    // first time the heap was enabled without one - so the dependency is resolved here and the NAME is kept
    // for the device-creation list rather than being re-derived there.
    char const* const descriptor_heap_dependency = has_extension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)    ? VK_KHR_MAINTENANCE_5_EXTENSION_NAME
                                                   : has_extension(VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME) ? VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME
                                                                                                         : nullptr;
    bool const descriptor_heap_extension = has_extension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) && descriptor_heap_dependency != nullptr;
    // ... and the heap's SHADERS need one more extension and feature, which nothing declared until the
    // heap-native probe did: GL_EXT_descriptor_heap compiles every `descriptor_heap` declaration to an UNTYPED
    // POINTER (SPIR-V UntypedPointersKHR), and validation refuses such a module unless
    // VK_KHR_shader_untyped_pointers is enabled AND its feature is on ("SPIR-V Capability UntypedPointersKHR was
    // declared, but ... shaderUntypedPointers" - measured, on the probe's first run). It hangs off the heap's own
    // link because a device without the heap has no heap shaders to compile.
    bool const untyped_pointers_extension = descriptor_heap_extension && has_extension(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME);
    // MESH SHADERS, the other independent extension: like the heap it changes how work is submitted rather
    // than how rays are traced, and unlike the heap it has no extension dependency left to resolve -
    // VK_EXT_mesh_shader depends on VK_KHR_spirv_1_4 and VK_VERSION_1_2, and the second one is satisfied by
    // the 1.3 device this engine creates.
    bool const mesh_shader_extension = has_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    // VK_KHR_unified_image_layouts, the third independent extension: it is a FEATURE and adds no commands, so
    // unlike the heap and mesh shaders there is no shader-side consumer to gate - the whole renderer is written
    // against GENERAL, and a device without the feature is rejected at device creation (core.constructor).
    bool const unified_image_layouts_extension = has_extension(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    // Host image copy is independent of the other extensions and mandatory for renderer-owned uploads/read-back.
    bool const host_image_copy_extension = has_extension(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);

    // ---- Feature pNext chain: features_2 -> 1_1 -> 1_2 -> 1_3 -> 1_4 (truncated by api_version),
    //      then the extension features when the device has them. The TAIL is tracked rather than
    //      assumed, and that is not defensive style: the engine queries at API 1.3, so features_1_4 is
    //      not in the chain at all - a struct hung off it is never reached, and an unreached feature
    //      struct reads back as zeros. That is exactly how "this device has ray queries" turned into
    //      "not available" the first time this was written. ----
    features_2.pNext = api_version >= VK_API_VERSION_1_1 ? &features_1_1 : nullptr;
    features_1_1.pNext = api_version >= VK_API_VERSION_1_2 ? &features_1_2 : nullptr;
    features_1_2.pNext = api_version >= VK_API_VERSION_1_3 ? &features_1_3 : nullptr;
    features_1_3.pNext = api_version >= VK_API_VERSION_1_4 ? &features_1_4 : nullptr;
    features_1_4.pNext = nullptr;
    void* feature_tail = &features_2;
    if (api_version >= VK_API_VERSION_1_1) {
        feature_tail = &features_1_1;
    }
    if (api_version >= VK_API_VERSION_1_2) {
        feature_tail = &features_1_2;
    }
    if (api_version >= VK_API_VERSION_1_3) {
        feature_tail = &features_1_3;
    }
    if (api_version >= VK_API_VERSION_1_4) {
        feature_tail = &features_1_4;
    }
    // (the cast is what the newer headers need: a concrete feature struct's pNext is void*, while
    // VkBaseOutStructure's own pNext is typed - and the tail is reached through the base type)
    //
    // Chain order: core 1.x, then the INDEPENDENT extension links (descriptor heap, mesh shaders), then the
    // ray-tracing chain. The two independent ones are ahead because neither depends on ray tracing: a device
    // can have a heap and mesh shaders without RT and the other way round, so the RT chain hangs off whichever
    // of them is LAST rather than off a fixed one - hanging it off the heap alone is what dropped mesh shaders
    // on a device without the heap, whose feature struct then read back as zeros and looked unsupported.
    // NOTE this pre-query chain is built from extension PRESENCE, because the feature bits are unknown until
    // vkGetPhysicalDeviceFeatures2 has run; it is rebuilt from AVAILABILITY right after, and that rebuild is
    // what the vkCreateDevice chain actually contains.
    VkBaseOutStructure* independent_tail = static_cast<VkBaseOutStructure*>(feature_tail);
    if (descriptor_heap_extension) {
        independent_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&descriptor_heap_features);
        independent_tail = reinterpret_cast<VkBaseOutStructure*>(&descriptor_heap_features);
    }
    // 堆着色器的依赖独立追加；不能挂在可选光追/micromap 节点后面。
    if (untyped_pointers_extension) {
        independent_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&untyped_pointers_features);
        independent_tail = reinterpret_cast<VkBaseOutStructure*>(&untyped_pointers_features);
    }
    if (mesh_shader_extension) {
        independent_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&mesh_shader_features);
        independent_tail = reinterpret_cast<VkBaseOutStructure*>(&mesh_shader_features);
    }
    if (unified_image_layouts_extension) {
        independent_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&unified_image_layouts_features);
        independent_tail = reinterpret_cast<VkBaseOutStructure*>(&unified_image_layouts_features);
    }
    if (host_image_copy_extension) {
        independent_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&host_image_copy_features);
        independent_tail = reinterpret_cast<VkBaseOutStructure*>(&host_image_copy_features);
    }
    independent_tail->pNext = ray_tracing_extensions ? reinterpret_cast<VkBaseOutStructure*>(&acceleration_structure_features) : nullptr;
    acceleration_structure_features.pNext = ray_tracing_extensions ? &ray_query_features : nullptr;
    // ... and the rest of the ray-tracing chain hangs off ray query, each link present only when its own
    // extensions are: an extension feature struct whose extension is NOT enabled must not appear in the
    // vkCreateDevice chain at all.
    ray_query_features.pNext = ray_tracing_pipeline_extensions ? reinterpret_cast<VkBaseOutStructure*>(&ray_tracing_pipeline_features) : nullptr;
    ray_tracing_pipeline_features.pNext = ray_tracing_pipeline_extensions ? &ray_tracing_maintenance1_features : nullptr;
    ray_tracing_maintenance1_features.pNext = opacity_micromap_extension ? reinterpret_cast<VkBaseOutStructure*>(&opacity_micromap_features) : nullptr;
    opacity_micromap_features.pNext = nullptr;
    vkGetPhysicalDeviceFeatures2(physical_device, &features_2);

    // Both feature bits have to be true for the two structs to be worth keeping in the chain: the
    // extensions can be advertised by a device that does not actually support ray queries. Unlinking
    // here is what keeps "enabled at device creation" and "available to the renderer" the same thing.
    ray_query_available = ray_tracing_extensions && acceleration_structure_features.accelerationStructure == VK_TRUE && ray_query_features.rayQuery == VK_TRUE;
    ray_tracing_pipeline_available = ray_tracing_pipeline_extensions && ray_query_available && ray_tracing_pipeline_features.rayTracingPipeline == VK_TRUE;
    opacity_micromap_available = opacity_micromap_extension && ray_tracing_pipeline_available && opacity_micromap_features.micromap == VK_TRUE;
    descriptor_heap_available = descriptor_heap_extension && descriptor_heap_features.descriptorHeap == VK_TRUE;
    untyped_pointers_available = untyped_pointers_extension && untyped_pointers_features.shaderUntypedPointers == VK_TRUE;
    // The mesh shader is available when the extension is there and its meshShader feature is on. taskShader is
    // NOT part of this flag: a device may have mesh shaders without task shaders, and a task stage must be
    // gated on mesh_shader_features.taskShader instead (see the mesh shader pass).
    mesh_shader_available = mesh_shader_extension && mesh_shader_features.meshShader == VK_TRUE;
    // Both the extension and its feature bit have to be there: the extension can be advertised by a device
    // that reports unifiedImageLayouts == VK_FALSE, and that device would create fine while still needing the
    // per-layout transitions the renderer no longer performs - so it must NOT count as available.
    unified_image_layouts_available = unified_image_layouts_extension && unified_image_layouts_features.unifiedImageLayouts == VK_TRUE;
    // The host image copy flag needs two answers and only the first is known here: the feature bit (queried just
    // above) and whether GENERAL is one of the layouts a host copy may read from (a PROPERTY, queried below with
    // the rest of the property chain) - so this line is deliberately amended after that query.
    host_image_copy_available = host_image_copy_extension && host_image_copy_features.hostImageCopy == VK_TRUE;
    this->untyped_pointers_dependency = untyped_pointers_available ? VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME : nullptr;
    this->descriptor_heap_extension_name = descriptor_heap_available ? descriptor_heap_dependency : nullptr;
    // Rebuild the extension chain from the core tail with ONLY the available links: the extension may be
    // advertised by a device that does not actually support it, and an enabled-but-unsupported struct is a
    // device-creation error. Rebuilding rather than unlinking one link at a time is what makes the result
    // independent of link ORDER: every cut below has to hand the tail to the next link still standing, and
    // the heap link's cut used to have to know what it was holding - a link inserted or dropped after it
    // would silently take the rest of the chain with it.
    {
        VkBaseOutStructure* tail = static_cast<VkBaseOutStructure*>(feature_tail);
        auto const link = [&tail](bool const present, void* node) {
            tail->pNext = present ? static_cast<VkBaseOutStructure*>(node) : nullptr;
            if (present) {
                tail = static_cast<VkBaseOutStructure*>(node);
            }
        };
        link(descriptor_heap_available, &descriptor_heap_features);
        link(mesh_shader_available, &mesh_shader_features);
        link(unified_image_layouts_available, &unified_image_layouts_features);
        link(host_image_copy_available, &host_image_copy_features);
        link(ray_query_available, &acceleration_structure_features);
        link(ray_query_available, &ray_query_features);
        link(ray_tracing_pipeline_available, &ray_tracing_pipeline_features);
        link(ray_tracing_pipeline_available, &ray_tracing_maintenance1_features);
        link(opacity_micromap_available, &opacity_micromap_features);
        link(untyped_pointers_available, &untyped_pointers_features);
        link(false, nullptr); // terminate the chain at the last available link
    }

    // ---- Property pNext chain: properties_2 -> driver -> subgroup -> descriptor indexing -> maintenance4
    //      -> descriptor heap -> mesh shader -> acceleration structure (only when available) ----
    properties_2.pNext = &driver_properties;
    driver_properties.pNext = &subgroup_properties;
    subgroup_properties.pNext = &descriptor_indexing_properties;
    descriptor_indexing_properties.pNext = &maintenance4_properties;
    // the host image copy list is inserted BEFORE the descriptor heap link and hangs the rest of the chain off
    // itself, so adding it cannot make any later property struct unreachable. It is a "count in, count out"
    // query: copySrcLayoutCount comes back as the device's total and at most the number passed in is written.
    host_image_copy_properties.pCopySrcLayouts = host_image_copy_src_layouts.data();
    host_image_copy_properties.copySrcLayoutCount = static_cast<uint32_t>(host_image_copy_src_layouts.size());
    host_image_copy_properties.pCopyDstLayouts = host_image_copy_dst_layouts.data();
    host_image_copy_properties.copyDstLayoutCount = static_cast<uint32_t>(host_image_copy_dst_layouts.size());
    // 每项属性按自身条件接到尾部，缺少前一个可选扩展不能截断后续查询。
    {
        auto* tail = reinterpret_cast<VkBaseOutStructure*>(&maintenance4_properties);
        auto const link = [&tail](bool const present, void* node) {
            if (present) {
                tail->pNext = static_cast<VkBaseOutStructure*>(node);
                tail = static_cast<VkBaseOutStructure*>(node);
            }
        };
        link(host_image_copy_extension, &host_image_copy_properties);
        link(descriptor_heap_extension, &descriptor_heap_properties);
        link(mesh_shader_extension, &mesh_shader_properties);
        link(ray_query_available, &acceleration_structure_properties);
        link(opacity_micromap_available, &opacity_micromap_properties);
        link(ray_tracing_pipeline_available, &ray_tracing_pipeline_properties);
        tail->pNext = nullptr;
    }
    vkGetPhysicalDeviceProperties2(physical_device, &properties_2);

    // ---- The rest of the host-image-copy capability, in BOTH directions. The feature bit promises the entry
    //      points work; it says nothing about whether THIS renderer's images may be copied that way. A host copy
    //      needs the image's actual layout to be one of the device's copy-source layouts (the read-back) or one
    //      of its copy-destination layouts (the upload), and every image here is in GENERAL
    //      (VK_KHR_unified_image_layouts) - so each direction is checked against its OWN list, separately,
    //      because a device may list GENERAL in one and not the other. A miss is a named startup failure at
    //      device creation; it is not a downgrade to another mechanism. Counts are clamped to the arrays for the
    //      same reason as above.
    if (host_image_copy_available) {
        uint32_t const listed = host_image_copy_properties.copySrcLayoutCount < host_image_copy_max_src_layouts
                                    ? host_image_copy_properties.copySrcLayoutCount
                                    : host_image_copy_max_src_layouts;
        bool general_listed = false;
        for (uint32_t index = 0; index < listed; ++index) {
            general_listed = general_listed || host_image_copy_src_layouts[index] == VK_IMAGE_LAYOUT_GENERAL;
        }
        host_image_copy_available = general_listed;

        uint32_t const upload_listed = host_image_copy_properties.copyDstLayoutCount < host_image_copy_max_src_layouts
                                           ? host_image_copy_properties.copyDstLayoutCount
                                           : host_image_copy_max_src_layouts;
        bool general_dst_listed = false;
        for (uint32_t index = 0; index < upload_listed; ++index) {
            general_dst_listed = general_dst_listed || host_image_copy_dst_layouts[index] == VK_IMAGE_LAYOUT_GENERAL;
        }
        host_image_copy_upload_available = general_dst_listed;
    }

    // ---- Feature policy: pass through driver support except explicitly disabled ones (take most features except ray tracing) ----
    features_1_1.protectedMemory = VK_FALSE; // protected memory not needed for now
    // The mesh-shader feature struct is passed through like the core ones, with ONE exception:
    // primitiveFragmentShadingRateMeshShader is only legal when
    // VkPhysicalDeviceFragmentShadingRateFeaturesKHR::primitiveFragmentShadingRate is enabled as well, and this
    // renderer never enables that extension at all - so the bit is forced off
    // (VUID-VkPhysicalDeviceMeshShaderFeaturesEXT-primitiveFragmentShadingRateMeshShader-07033, which validation
    // reported as a vkCreateDevice error the first time the mesh feature was enabled). multiviewMeshShader needs
    // no such handling: its dependency is the 1.1 multiview feature above, which this device has on.
    mesh_shader_features.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    // unifiedImageLayoutsVideo is passed through by the driver only where video layouts are in play, and no
    // video extension is enabled here, so it is pinned off rather than forwarded: enabling it would promise
    // something about layouts (VK_IMAGE_LAYOUT_VIDEO_*) this renderer never creates.
    unified_image_layouts_features.unifiedImageLayoutsVideo = VK_FALSE;
}

void const* device_capabilities::device_pnext() const noexcept {
    // Chain head is the VkPhysicalDeviceFeatures2 struct: its .features member carries the
    // Vulkan 1.0 core features (fullDrawIndexUint32 etc.) that must be enabled via the pNext
    // chain when pEnabledFeatures is NULL. The 1.1/1.2/... structs hang off its pNext.
    return &this->features_2;
}

namespace {
    // Append names of enabled (VK_TRUE) members to the output string, return the enabled count
    template <typename T>
    size_t append_enabled_features(std::string& out, T const& features,
                                   std::initializer_list<std::pair<char const*, VkBool32 T::*>> const& entries) {
        size_t count = 0;
        for (auto const& [name, member] : entries) {
            if (features.*member == VK_TRUE) {
                if (!out.empty()) {
                    out += ", ";
                }
                out += name;
                ++count;
            }
        }
        return count;
    }

    // Wrap output at the given width, indenting continuation lines
    void print_wrapped(std::string const& text, int32_t const width, std::string_view const indent) {
        std::string current(indent);
        size_t start = 0;
        while (start < text.size()) {
            size_t const comma = text.find(", ", start);
            size_t const token_end = comma == std::string::npos ? text.size() : comma;
            std::string_view const token(text.data() + start, token_end - start);
            if (current.size() > indent.size() && current.size() + token.size() + 2 > static_cast<size_t>(width)) {
                deren::utility::log("{}", current);
                current = std::string(indent);
            }
            if (current.size() > indent.size()) {
                current += ", ";
            }
            current += token;
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 2;
        }
        if (!text.empty()) {
            deren::utility::log("{}", current);
        }
    }
} // namespace

void print_device_capabilities(device_capabilities const& capabilities) {
    constexpr std::string_view box_line = "================================================";
    constexpr std::string_view sep_line = "------------------------------------------------";

    deren::utility::log("{}", box_line);
    deren::utility::log(" Vulkan device capabilities");
    deren::utility::log("{}", box_line);

    static constexpr std::array<char const*, 5> device_type_names = {
        "other",
        "integrated gpu",
        "discrete gpu",
        "virtual gpu",
        "cpu",
    };
    uint32_t const device_type = static_cast<uint32_t>(capabilities.properties_2.properties.deviceType);
    char const* type_name = device_type < device_type_names.size() ? device_type_names[device_type] : "unknown";

    deren::utility::log(" driver        : {} {}", capabilities.driver_properties.driverName, capabilities.driver_properties.driverInfo);
    deren::utility::log(" api version   : {}.{}.{}",
                        VK_API_VERSION_MAJOR(capabilities.properties_2.properties.apiVersion),
                        VK_API_VERSION_MINOR(capabilities.properties_2.properties.apiVersion),
                        VK_API_VERSION_PATCH(capabilities.properties_2.properties.apiVersion));
    deren::utility::log(" device        : {} ({})", capabilities.properties_2.properties.deviceName, type_name);
    deren::utility::log("{}", sep_line);

    using feature_1_1 = VkPhysicalDeviceVulkan11Features;
    using feature_1_2 = VkPhysicalDeviceVulkan12Features;
    using feature_1_3 = VkPhysicalDeviceVulkan13Features;

    std::string enabled_1_1;
    size_t const count_1_1 = append_enabled_features(enabled_1_1, capabilities.features_1_1, {
                                                                                                 {"storageBuffer16BitAccess", &feature_1_1::storageBuffer16BitAccess},
                                                                                                 {"uniformAndStorageBuffer16BitAccess", &feature_1_1::uniformAndStorageBuffer16BitAccess},
                                                                                                 {"storagePushConstant16", &feature_1_1::storagePushConstant16},
                                                                                                 {"storageInputOutput16", &feature_1_1::storageInputOutput16},
                                                                                                 {"multiview", &feature_1_1::multiview},
                                                                                                 {"multiviewGeometryShader", &feature_1_1::multiviewGeometryShader},
                                                                                                 {"multiviewTessellationShader", &feature_1_1::multiviewTessellationShader},
                                                                                                 {"variablePointersStorageBuffer", &feature_1_1::variablePointersStorageBuffer},
                                                                                                 {"variablePointers", &feature_1_1::variablePointers},
                                                                                                 {"protectedMemory", &feature_1_1::protectedMemory},
                                                                                                 {"samplerYcbcrConversion", &feature_1_1::samplerYcbcrConversion},
                                                                                                 {"shaderDrawParameters", &feature_1_1::shaderDrawParameters},
                                                                                             });
    deren::utility::log(" vulkan 1.1 features ({})", count_1_1);
    print_wrapped(enabled_1_1, 100, "   ");

    std::string enabled_1_2;
    size_t const count_1_2 = append_enabled_features(enabled_1_2, capabilities.features_1_2, {
                                                                                                 {"samplerMirrorClampToEdge", &feature_1_2::samplerMirrorClampToEdge},
                                                                                                 {"drawIndirectCount", &feature_1_2::drawIndirectCount},
                                                                                                 {"storageBuffer8BitAccess", &feature_1_2::storageBuffer8BitAccess},
                                                                                                 {"uniformAndStorageBuffer8BitAccess", &feature_1_2::uniformAndStorageBuffer8BitAccess},
                                                                                                 {"storagePushConstant8", &feature_1_2::storagePushConstant8},
                                                                                                 {"shaderBufferInt64Atomics", &feature_1_2::shaderBufferInt64Atomics},
                                                                                                 {"shaderSharedInt64Atomics", &feature_1_2::shaderSharedInt64Atomics},
                                                                                                 {"shaderFloat16", &feature_1_2::shaderFloat16},
                                                                                                 {"shaderInt8", &feature_1_2::shaderInt8},
                                                                                                 {"descriptorIndexing", &feature_1_2::descriptorIndexing},
                                                                                                 {"shaderInputAttachmentArrayDynamicIndexing", &feature_1_2::shaderInputAttachmentArrayDynamicIndexing},
                                                                                                 {"shaderUniformTexelBufferArrayDynamicIndexing", &feature_1_2::shaderUniformTexelBufferArrayDynamicIndexing},
                                                                                                 {"shaderStorageTexelBufferArrayDynamicIndexing", &feature_1_2::shaderStorageTexelBufferArrayDynamicIndexing},
                                                                                                 {"shaderUniformBufferArrayNonUniformIndexing", &feature_1_2::shaderUniformBufferArrayNonUniformIndexing},
                                                                                                 {"shaderSampledImageArrayNonUniformIndexing", &feature_1_2::shaderSampledImageArrayNonUniformIndexing},
                                                                                                 {"shaderStorageBufferArrayNonUniformIndexing", &feature_1_2::shaderStorageBufferArrayNonUniformIndexing},
                                                                                                 {"shaderStorageImageArrayNonUniformIndexing", &feature_1_2::shaderStorageImageArrayNonUniformIndexing},
                                                                                                 {"shaderInputAttachmentArrayNonUniformIndexing", &feature_1_2::shaderInputAttachmentArrayNonUniformIndexing},
                                                                                                 {"shaderUniformTexelBufferArrayNonUniformIndexing", &feature_1_2::shaderUniformTexelBufferArrayNonUniformIndexing},
                                                                                                 {"shaderStorageTexelBufferArrayNonUniformIndexing", &feature_1_2::shaderStorageTexelBufferArrayNonUniformIndexing},
                                                                                                 {"descriptorBindingUniformBufferUpdateAfterBind", &feature_1_2::descriptorBindingUniformBufferUpdateAfterBind},
                                                                                                 {"descriptorBindingSampledImageUpdateAfterBind", &feature_1_2::descriptorBindingSampledImageUpdateAfterBind},
                                                                                                 {"descriptorBindingStorageImageUpdateAfterBind", &feature_1_2::descriptorBindingStorageImageUpdateAfterBind},
                                                                                                 {"descriptorBindingStorageBufferUpdateAfterBind", &feature_1_2::descriptorBindingStorageBufferUpdateAfterBind},
                                                                                                 {"descriptorBindingUniformTexelBufferUpdateAfterBind", &feature_1_2::descriptorBindingUniformTexelBufferUpdateAfterBind},
                                                                                                 {"descriptorBindingStorageTexelBufferUpdateAfterBind", &feature_1_2::descriptorBindingStorageTexelBufferUpdateAfterBind},
                                                                                                 {"descriptorBindingUpdateUnusedWhilePending", &feature_1_2::descriptorBindingUpdateUnusedWhilePending},
                                                                                                 {"descriptorBindingPartiallyBound", &feature_1_2::descriptorBindingPartiallyBound},
                                                                                                 {"descriptorBindingVariableDescriptorCount", &feature_1_2::descriptorBindingVariableDescriptorCount},
                                                                                                 {"runtimeDescriptorArray", &feature_1_2::runtimeDescriptorArray},
                                                                                                 {"samplerFilterMinmax", &feature_1_2::samplerFilterMinmax},
                                                                                                 {"scalarBlockLayout", &feature_1_2::scalarBlockLayout},
                                                                                                 {"imagelessFramebuffer", &feature_1_2::imagelessFramebuffer},
                                                                                                 {"uniformBufferStandardLayout", &feature_1_2::uniformBufferStandardLayout},
                                                                                                 {"shaderSubgroupExtendedTypes", &feature_1_2::shaderSubgroupExtendedTypes},
                                                                                                 {"separateDepthStencilLayouts", &feature_1_2::separateDepthStencilLayouts},
                                                                                                 {"hostQueryReset", &feature_1_2::hostQueryReset},
                                                                                                 {"timelineSemaphore", &feature_1_2::timelineSemaphore},
                                                                                                 {"bufferDeviceAddress", &feature_1_2::bufferDeviceAddress},
                                                                                                 {"bufferDeviceAddressCaptureReplay", &feature_1_2::bufferDeviceAddressCaptureReplay},
                                                                                                 {"bufferDeviceAddressMultiDevice", &feature_1_2::bufferDeviceAddressMultiDevice},
                                                                                                 {"vulkanMemoryModel", &feature_1_2::vulkanMemoryModel},
                                                                                                 {"vulkanMemoryModelDeviceScope", &feature_1_2::vulkanMemoryModelDeviceScope},
                                                                                                 {"vulkanMemoryModelAvailabilityVisibilityChains", &feature_1_2::vulkanMemoryModelAvailabilityVisibilityChains},
                                                                                                 {"shaderOutputViewportIndex", &feature_1_2::shaderOutputViewportIndex},
                                                                                                 {"shaderOutputLayer", &feature_1_2::shaderOutputLayer},
                                                                                                 {"subgroupBroadcastDynamicId", &feature_1_2::subgroupBroadcastDynamicId},
                                                                                             });
    deren::utility::log(" vulkan 1.2 features ({})", count_1_2);
    print_wrapped(enabled_1_2, 100, "   ");

    std::string enabled_1_3;
    size_t const count_1_3 = append_enabled_features(enabled_1_3, capabilities.features_1_3, {
                                                                                                 {"robustImageAccess", &feature_1_3::robustImageAccess},
                                                                                                 {"inlineUniformBlock", &feature_1_3::inlineUniformBlock},
                                                                                                 {"descriptorBindingInlineUniformBlockUpdateAfterBind", &feature_1_3::descriptorBindingInlineUniformBlockUpdateAfterBind},
                                                                                                 {"pipelineCreationCacheControl", &feature_1_3::pipelineCreationCacheControl},
                                                                                                 {"privateData", &feature_1_3::privateData},
                                                                                                 {"shaderDemoteToHelperInvocation", &feature_1_3::shaderDemoteToHelperInvocation},
                                                                                                 {"shaderTerminateInvocation", &feature_1_3::shaderTerminateInvocation},
                                                                                                 {"subgroupSizeControl", &feature_1_3::subgroupSizeControl},
                                                                                                 {"computeFullSubgroups", &feature_1_3::computeFullSubgroups},
                                                                                                 {"synchronization2", &feature_1_3::synchronization2},
                                                                                                 {"textureCompressionASTC_HDR", &feature_1_3::textureCompressionASTC_HDR},
                                                                                                 {"shaderZeroInitializeWorkgroupMemory", &feature_1_3::shaderZeroInitializeWorkgroupMemory},
                                                                                                 {"dynamicRendering", &feature_1_3::dynamicRendering},
                                                                                                 {"shaderIntegerDotProduct", &feature_1_3::shaderIntegerDotProduct},
                                                                                                 {"maintenance4", &feature_1_3::maintenance4},
                                                                                             });
    deren::utility::log(" vulkan 1.3 features ({})", count_1_3);
    print_wrapped(enabled_1_3, 100, "   ");

    // ---- Ray tracing: whether the optional extension features joined the chain (see query) ----
    if (capabilities.ray_query_available) {
        deren::utility::log(" ray tracing   : ray query available (VK_KHR_acceleration_structure + VK_KHR_ray_query)");
        if (capabilities.ray_tracing_pipeline_available) {
            deren::utility::log("   sbt         : handle {} B, base alignment {}, handle alignment {}, max recursion {}",
                                capabilities.ray_tracing_pipeline_properties.shaderGroupHandleSize,
                                capabilities.ray_tracing_pipeline_properties.shaderGroupBaseAlignment,
                                capabilities.ray_tracing_pipeline_properties.shaderGroupHandleAlignment,
                                capabilities.ray_tracing_pipeline_properties.maxRayRecursionDepth);
        }
        deren::utility::log("                 {} / opacity micromap {}",
                            capabilities.ray_tracing_pipeline_available ? "ray pipeline available (VK_KHR_ray_tracing_pipeline + maintenance1)" : "ray pipeline NOT available",
                            capabilities.opacity_micromap_available ? std::format("available (VK_EXT_opacity_micromap, max subdivision level {} 2-state / {} 4-state)", capabilities.opacity_micromap_properties.maxOpacity2StateSubdivisionLevel, capabilities.opacity_micromap_properties.maxOpacity4StateSubdivisionLevel) : "not available");
        deren::utility::log("   limits      : {} instances, {} geometries, scratch alignment {}",
                            capabilities.acceleration_structure_properties.maxInstanceCount,
                            capabilities.acceleration_structure_properties.maxGeometryCount,
                            capabilities.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment);
    } else {
        deren::utility::log(" ray tracing   : not available (ray-traced shadows and GI stay off)");
    }

    // ---- Descriptor heap: independent of ray tracing, so it reports outside that block ----
    if (capabilities.descriptor_heap_available) {
        deren::utility::log(" descriptor heap: available (VK_EXT_descriptor_heap, revision 1)");
        deren::utility::log("   descriptors : buffer {} B (align {}), image {} B (align {}), sampler {} B (align {})",
                            capabilities.descriptor_heap_properties.bufferDescriptorSize,
                            capabilities.descriptor_heap_properties.bufferDescriptorAlignment,
                            capabilities.descriptor_heap_properties.imageDescriptorSize,
                            capabilities.descriptor_heap_properties.imageDescriptorAlignment,
                            capabilities.descriptor_heap_properties.samplerDescriptorSize,
                            capabilities.descriptor_heap_properties.samplerDescriptorAlignment);
        deren::utility::log("   heaps       : resource max {} MiB (alignment {}), sampler max {} KiB (alignment {})",
                            capabilities.descriptor_heap_properties.maxResourceHeapSize / (1024 * 1024),
                            capabilities.descriptor_heap_properties.resourceHeapAlignment,
                            capabilities.descriptor_heap_properties.maxSamplerHeapSize / 1024,
                            capabilities.descriptor_heap_properties.samplerHeapAlignment);
        deren::utility::log("   reserved    : {} KiB resource, {} KiB sampler with embedded samplers, {} embedded samplers max, push data {} B",
                            capabilities.descriptor_heap_properties.minResourceHeapReservedRange / 1024,
                            capabilities.descriptor_heap_properties.minSamplerHeapReservedRangeWithEmbedded / 1024,
                            capabilities.descriptor_heap_properties.maxDescriptorHeapEmbeddedSamplers,
                            capabilities.descriptor_heap_properties.maxPushDataSize);
    } else {
        deren::utility::log(" descriptor heap: not available (the heap is the only binding model this renderer has)");
    }

    // ---- Mesh shaders: independent of both of the above, so they report outside those blocks ----
    if (capabilities.mesh_shader_available) {
        deren::utility::log(" mesh shaders  : available (VK_EXT_mesh_shader, task shaders {})",
                            capabilities.mesh_shader_features.taskShader == VK_TRUE ? "available" : "NOT available");
        deren::utility::log("   output      : {} vertices / {} primitives / {} components / {} B per workgroup, {} invocations max",
                            capabilities.mesh_shader_properties.maxMeshOutputVertices,
                            capabilities.mesh_shader_properties.maxMeshOutputPrimitives,
                            capabilities.mesh_shader_properties.maxMeshOutputComponents,
                            capabilities.mesh_shader_properties.maxMeshOutputMemorySize,
                            capabilities.mesh_shader_properties.maxMeshWorkGroupInvocations);
        deren::utility::log("   workgroup   : mesh {}x{}x{}, task {}x{}x{}",
                            capabilities.mesh_shader_properties.maxMeshWorkGroupSize[0],
                            capabilities.mesh_shader_properties.maxMeshWorkGroupSize[1],
                            capabilities.mesh_shader_properties.maxMeshWorkGroupSize[2],
                            capabilities.mesh_shader_properties.maxTaskWorkGroupSize[0],
                            capabilities.mesh_shader_properties.maxTaskWorkGroupSize[1],
                            capabilities.mesh_shader_properties.maxTaskWorkGroupSize[2]);
    } else {
        deren::utility::log(" mesh shaders  : not available (geometry stays on the vertex stage)");
    }

    // ---- Unified image layouts: a REQUIRED feature, so this line is a receipt rather than a decision ----
    if (capabilities.unified_image_layouts_available) {
        deren::utility::log(" unified layout: available (VK_KHR_unified_image_layouts, every image stays in GENERAL)");
    } else {
        deren::utility::log(" unified layout: NOT available (this renderer requires it, device creation refuses)");
    }

    // ---- Host image copy: REQUIRED (device creation refuses a device without it), so this line reports the two
    //      direction-specific layout facts the checks above measured rather than a choice of path ---------------
    deren::utility::log(" host image copy: REQUIRED - GENERAL in the copy-source layouts: {}, in the copy-destination layouts: {} "
                        "(VK_EXT_host_image_copy; the swapchain read-back still uses the copy command, because a swapchain image's "
                        "usage is bounded by the surface's supportedUsageFlags)",
                        capabilities.host_image_copy_available,
                        capabilities.host_image_copy_upload_available);

    deren::utility::log("{}", box_line);
}

logical_device create_logical_device(
    VkPhysicalDevice const physical_device, // NOLINT(*-misplaced-const)
    device_creation_info const& create_info) noexcept {
    if (!create_info.queue_families.is_complete()) {
        deren::utility::error("Queue families not complete");
        deren::utility::panic("Queue families not complete");
    }

    if (!create_info.queue_families.compute_family || !create_info.queue_families.graphics_family || !create_info.queue_families.present_family) {
        deren::utility::panic("queue family is empty");
    }

    // Use a set to collect unique queue family indices
    std::set<uint32_t> unique_queue_families = {
        create_info.queue_families.graphics_family.value(),
        create_info.queue_families.present_family.value(),
    };

    // Optional: add more queue families
    if (create_info.queue_families.compute_family) {
        unique_queue_families.insert(create_info.queue_families.compute_family.value());
    }
    if (create_info.queue_families.transfer_family) {
        unique_queue_families.insert(create_info.queue_families.transfer_family.value());
    }

    // Create queue infos
    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    constexpr float queue_priority = 1.0f;

    for (uint32_t queue_family : unique_queue_families) {
        queue_create_infos.push_back(deren::vulkan::make_device_queue_info(queue_family, &queue_priority));
    }

    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifo_latest_ready_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
        .pNext = nullptr,
        .presentModeFifoLatestReady = VK_FALSE,
    };
    VkPhysicalDeviceFeatures2 probe_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &fifo_latest_ready_features,
        .features = {},
    };
    vkGetPhysicalDeviceFeatures2(physical_device, &probe_features);

    // Create the device
    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    device_create_info.pQueueCreateInfos = queue_create_infos.data();
    // Features go through the pNext chain (headed by VkPhysicalDeviceFeatures2, provided by the
    // caller through create_info.pNext - see device_capabilities::device_pnext(), which carries
    // the 1.0 core features in features_2.features plus the 1.1/1.2/... structs behind it).
    // pEnabledFeatures must be NULL: with VkPhysicalDeviceFeatures2 / VkPhysicalDeviceVulkan11Features
    // in the chain, setting it violates VUID-VkDeviceCreateInfo-pNext-04748 / -02829.
    device_create_info.pEnabledFeatures = nullptr;

    // Extensions - must handle empty vectors correctly
    device_create_info.enabledExtensionCount = static_cast<uint32_t>(create_info.extensions.size());
    device_create_info.ppEnabledExtensionNames =
        create_info.extensions.empty() ? nullptr : create_info.extensions.data();

    // Validation layers - modern Vulkan usually doesn't enable them at device level
    device_create_info.enabledLayerCount = static_cast<uint32_t>(create_info.validation_layers.size());
    device_create_info.ppEnabledLayerNames =
        create_info.validation_layers.empty() ? nullptr : create_info.validation_layers.data();

    // Assemble the pNext chain: the caller's feature chain (VkPhysicalDeviceFeatures2 head, which
    // carries the queried 1.0 core + 1.1/1.2/... features) is the device chain. The optional
    // FifoLatestReady extension feature (not part of the caller's chain) is prepended when the
    // device exposes it, keeping the caller's Features2 head in the chain afterwards.
    void const* chain_head = create_info.pNext;
    if (fifo_latest_ready_features.presentModeFifoLatestReady == VK_TRUE) {
        fifo_latest_ready_features.pNext = const_cast<void*>(chain_head);
        chain_head = &fifo_latest_ready_features;
    }
    device_create_info.pNext = chain_head;

    VkDevice device = {};
    VkResult const result = vkCreateDevice(physical_device, &device_create_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        deren::utility::panic(std::source_location::current(), "Failed to create logical device: {}", std::to_string(result));
    }

    // Get queues
    logical_device logical_device;
    logical_device.device = device;
    logical_device.graphics_family_index = create_info.queue_families.graphics_family.value();
    logical_device.present_family_index = create_info.queue_families.present_family.value();

    vkGetDeviceQueue(device, logical_device.graphics_family_index, 0,
                     &logical_device.graphics_queue);
    vkGetDeviceQueue(device, logical_device.present_family_index, 0,
                     &logical_device.present_queue);

    return logical_device;
}

queue_family_indices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) noexcept { // NOLINT(*-function-cognitive-complexity)
    queue_family_indices indices;

    // Get queue family properties
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    // Find a suitable queue family
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        auto const& queue_family = queue_families[i];

        // Check graphics support
        if ((queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) && !indices.graphics_family.has_value()) {
            indices.graphics_family = i;
        }

        // Check compute support (non-graphics queue)
        if ((queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !indices.compute_family.has_value() &&
            !(queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            indices.compute_family = i;
        }

        // Check transfer support (non-graphics/compute queue)
        if ((queue_family.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !indices.transfer_family.has_value() &&
            !(queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            indices.transfer_family = i;
        }

        // Check present support (requires a surface)
        if (surface != VK_NULL_HANDLE) {
            VkBool32 present_support = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
            if (present_support && !indices.present_family.has_value()) {
                indices.present_family = i;
            }
        }

        // Early-exit if only graphics support is required
        if (surface == VK_NULL_HANDLE && indices.graphics_family.has_value()) {
            break;
        }

        // Early-exit once all required queues are found
        if (surface == VK_NULL_HANDLE) {
            if (indices.graphics_family.has_value()) {
                break;
            }
        } else if (indices.is_complete()) {
            break;
        }
    }

    // Fallback: if no dedicated compute/transfer queue was found, use the graphics queue
    if (!indices.compute_family.has_value() && indices.graphics_family.has_value()) {
        indices.compute_family = indices.graphics_family;
    }

    if (!indices.transfer_family.has_value()) {
        // Prefer the graphics queue; if none exists, use the first available queue
        if (indices.graphics_family.has_value()) {
            indices.transfer_family = indices.graphics_family;
        } else {
            if (queue_family_count > 0) {
                indices.transfer_family = {0};
            } else {
                indices.transfer_family = std::nullopt;
            }
        }
    }

    return indices;
}

VkPhysicalDevice pick_suitable_device(VkInstance instance, VkSurfaceKHR surface) noexcept {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        deren::utility::panic("Failed to find GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    for (auto const& device : devices) {
        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);

        // The engine requires Vulkan 1.3: dynamic rendering (frame recording, the depth-only
        // shadow pass, the ImGui overlay) is core 1.3 - there is no classic render-pass fallback.
        if (device_properties.apiVersion < VK_API_VERSION_1_3) {
            deren::utility::error("GPU [{}] rejected: Vulkan 1.3 required", device_properties.deviceName);
            continue;
        }

        if (queue_family_indices indices = find_queue_families(device, surface); !indices.is_complete()) {
            deren::utility::error("GPU [{}] rejected: graphics/present queues required", device_properties.deviceName);
            continue;
        }

        // Check extension support
        std::vector<char const*> const required_extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        if (!check_device_extension_support(device, required_extensions)) {
            deren::utility::error("GPU [{}] rejected: VK_KHR_swapchain required", device_properties.deviceName);
            continue;
        }

        device_capabilities capabilities;
        capabilities.query(device);
        auto const missing = capabilities.renderer_missing_requirements();
        if (!missing.empty()) {
            for (char const* requirement : missing) {
                deren::utility::error("GPU [{}] rejected: {} required", device_properties.deviceName, requirement);
            }
            continue;
        }

        return device; // suitable device found
    }

    deren::utility::panic("Failed to find a GPU satisfying renderer requirements; see per-device missing capabilities");
}

swap_chain_support_details query_swap_chain_support(VkPhysicalDevice device, VkSurfaceKHR surface) noexcept {
    swap_chain_support_details details = {};

    // 1. Query surface capabilities
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    // 2. Query surface formats
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    if (format_count != 0) {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
    }

    // 3. Query present modes
    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);
    if (present_mode_count != 0) {
        details.present_modes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, details.present_modes.data());
    }

    deren::utility::log("present modes count: {}", details.present_modes.size());

    return details;
}

VkPresentModeKHR choose_swap_present_mode(std::vector<VkPresentModeKHR> const& available_present_modes, bool const vsync) noexcept {
    if (vsync) {
        // FIFO_LATEST_READY when the surface offers it (the core enables the extension only when the
        // device has it): vsync-locked like FIFO, but it presents the newest ready image at each
        // vblank instead of queueing, so a fast application does not pay FIFO's added latency. FIFO
        // is mandated by the spec, so it is always the fallback.
        if (std::ranges::find(available_present_modes, present_mode_fifo_latest_ready) != available_present_modes.end()) {
            return present_mode_fifo_latest_ready;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    // Prefer MAILBOX (low latency), else fall back to FIFO (mandated by the Vulkan spec)
    if (std::ranges::find(available_present_modes, VK_PRESENT_MODE_MAILBOX_KHR) != available_present_modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceFormatKHR choose_swap_surface_format(std::vector<VkSurfaceFormatKHR> const& available_formats) noexcept {
    for (auto const& available_format : available_formats) {
        if (available_format.format == VK_FORMAT_B8G8R8A8_SRGB && available_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return available_format;
        }
    }
    return available_formats[0];
}

VkExtent2D choose_swap_extent(VkSurfaceCapabilitiesKHR capabilities, GLFWwindow* window, int32_t requested_width, int32_t requested_height) noexcept {
    // ONE LINE, AND IT IS THE MEASUREMENT RATHER THAN A TIDY-UP: this function has two paths and only one
    // of them asks the window anything. VK_EXT_headless_surface and Wayland report `currentExtent` as
    // UINT32_MAX and leave the extent to the application; the Win32 surface this renderer runs on ANSWERS
    // the question, and the branch below this line is then never entered - measured, `surface currentExtent
    // 1080x960 (the surface answers, the window is not consulted)` for a HIDDEN 1080x960 window, i.e. a
    // window's visibility is not what this surface reads. That matters to the hidden-window path, because
    // this branch is the one place a zero extent could come from: on a surface that does not answer, a
    // window that is not shown (or is minimized, or has not been mapped yet) can report a 0x0 framebuffer,
    // and this renderer sizes its whole frame from that number. The log makes which path ran a fact of
    // every run rather than something deduced from the platform.
    deren::utility::log("swapchain extent: surface currentExtent {}x{} ({})",
                        capabilities.currentExtent.width,
                        capabilities.currentExtent.height,
                        capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max() ? "the surface answers, the window is not consulted" : "unspecified - the window is consulted");
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    int32_t width = 0;
    int32_t height = 0;
    glfwGetFramebufferSize(window, &width, &height);

    // A ZERO (OR ABSURD) FRAMEBUFFER SIZE IS A REAL ANSWER FROM A WINDOW NOBODY HAS SHOWN, and it must not
    // become the frame. `glfwGetFramebufferSize` reports what the window currently has: a hidden window, a
    // window that has not been mapped yet and a minimized one can all report 0x0, and this path is entered
    // exactly when the surface does not pin the extent. This file used to `std::max(width, 1)` - a
    // 1x1 swapchain, i.e. a silently different frame at every other resolution, or a validation failure
    // when the driver's minimum image extent is larger than 1. The fallback is instead the size the window
    // was REQUESTED at (`glfwCreateWindow`'s own arguments, which GLFW keeps whatever it can report for
    // the framebuffer): the caller already decided that extent, so it is the size this run asked for and
    // not a value invented here. `requested_*` is clamped up only so that the fallback cannot itself be a
    // zero-sized image on a config that asks for 0.
    if (width <= 0 || height <= 0) {
        deren::utility::log("swapchain extent: the window reports a {}x{} framebuffer (surface extent unspecified), so the {}x{} the window was REQUESTED at is used instead",
                            width,
                            height,
                            requested_width,
                            requested_height);
        width = requested_width;
        height = requested_height;
    }

    VkExtent2D actual_extent = {
        .width = static_cast<uint32_t>(std::max(width, 1)),
        .height = static_cast<uint32_t>(std::max(height, 1)),
    };

    actual_extent.width = std::clamp(actual_extent.width,
                                     capabilities.minImageExtent.width,
                                     capabilities.maxImageExtent.width);
    actual_extent.height = std::clamp(actual_extent.height,
                                      capabilities.minImageExtent.height,
                                      capabilities.maxImageExtent.height);

    return actual_extent;
}

uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties, VkPhysicalDevice physical_device) noexcept {
    // Get the physical device's memory properties
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

    // Iterate over all memory types
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        // Check whether the memory type satisfies the filter
        // type_filter is a bitmask; each bit corresponds to a memory type
        if ((type_filter & (1 << i)) &&
            // Check whether the memory properties meet the requirements
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i; // return the found memory type index
        }
    }
    deren::utility::panic("failed to find suitable memory type");
}

VkFormat find_depth_format(VkPhysicalDevice physical_device) noexcept {
    // Try to find a supported depth format, in order of preference
    std::vector<VkFormat> const candidates = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &props);

        // Check whether the format supports depth attachments
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    deren::utility::panic("failed to find supported depth format!");
}

VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device) noexcept {
    return create_image_view(image, format, aspect_flags, device, VK_IMAGE_VIEW_TYPE_2D);
}

VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device, VkImageViewType view_type) noexcept {
    // identity swizzle, one mip + one layer (see deren::vulkan::make_image_view_info); only the
    // dimensionality differs between callers
    VkImageViewCreateInfo const view_info = deren::vulkan::make_image_view_info(image, format, view_type, aspect_flags, 1, 1);

    VkImageView image_view;
    if (vkCreateImageView(device, &view_info, nullptr, &image_view) != VK_SUCCESS) {
        deren::utility::panic("failed to create image view!");
    }

    return image_view;
}
VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect_flags, VkDevice device, VkImageViewType view_type, uint32_t layer_count) noexcept {
    // identity swizzle, one mip, and the caller''s layer count: the sixth parameter is the whole reason this
    // overload exists (a cube view covers six layers, and the other two overloads fix it at one)
    VkImageViewCreateInfo const view_info = deren::vulkan::make_image_view_info(image, format, view_type, aspect_flags, 1, layer_count);

    VkImageView image_view;
    if (vkCreateImageView(device, &view_info, nullptr, &image_view) != VK_SUCCESS) {
        deren::utility::panic("failed to create image view!");
    }

    return image_view;
}
