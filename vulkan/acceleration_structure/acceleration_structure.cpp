module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vulkan/vulkan.h>

module deren.vulkan.acceleration_structure;

import deren.utility;

namespace deren::vulkan::acceleration_structure {
    namespace rhi = deren::promise::rhi;

    namespace {
        /// The contract's view of the device, and the reason EVERY factory and ability call in this file
        /// goes through one of these helpers. `core` implements `api_core`, so a call written on the
        /// CONCRETE `core&` compiles to a direct call and emits an undefined reference to
        /// `core::create_buffer` / `core::query_extension` in the engine half - which JOINS the
        /// backend-boundary worklist this migration is measured by. Through the contract's interface the
        /// call is virtual and emits no symbol at all.
        rhi::api_core& contract_of(core& gpu) {
            return static_cast<rhi::api_core&>(gpu);
        }

        /// The escape, obtained from the contract once and then used through ITS pointer.
        rhi::vulkan_escape* escape_of(core& gpu) {
            return static_cast<rhi::vulkan_escape*>(contract_of(gpu).query_extension(rhi::extension_kind::vulkan_escape));
        }

        /// ... and the address ability the same way (`device_address` is its own tier-2 ability).
        rhi::device_address* address_of(core& gpu) {
            return static_cast<rhi::device_address*>(contract_of(gpu).query_extension(rhi::extension_kind::device_address));
        }

        /// The borrowed VkBuffer behind a contract buffer; null when the buffer carries none.
        VkBuffer native_buffer_of(core& gpu, rhi::buffer const& buffer) {
            auto* const escape = escape_of(gpu);
            return escape == nullptr ? VK_NULL_HANDLE : reinterpret_cast<VkBuffer>(escape->native_buffer(buffer));
        }

        /// The device address of a contract buffer created with `rhi::buffer_flag::device_address`; 0 when
        /// the address could not be answered (the flag was not set, or the ability is not announced).
        VkDeviceAddress buffer_address_of(core& gpu, rhi::buffer const& buffer) {
            auto* const addresses = address_of(gpu);
            return addresses == nullptr ? 0 : static_cast<VkDeviceAddress>(addresses->buffer_address(buffer, 0));
        }

        /// The memory intent + capability flags a device address is asked through: what the renderer's
        /// own `build_input_usage` carries, as the contract's two names for it.
        constexpr rhi::buffer_flags device_address_flag = rhi::to_bits(rhi::buffer_flag::device_address);
    } // namespace
    namespace {
        /// round @p value up to the next multiple of @p alignment (a power of two, as Vulkan requires)
        constexpr VkDeviceSize align_up(VkDeviceSize const value, VkDeviceSize const alignment) noexcept {
            return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
        }

        /**
         * @brief the four acceleration-structure entry points, resolved per device
         *
         * They are NOT in the SDK's vulkan-1 import library - checked, not assumed: that lib exports
         * `vkGetBufferDeviceAddress` (core 1.2) and no `vk*AccelerationStructure*` symbol at all, so a
         * direct call is an undefined symbol at LINK time on this toolchain. Resolving through
         * vkGetDeviceProcAddr is the documented way to reach an extension entry point and the only one
         * that works here.
         *
         * A missing pointer is not fatal: the builder reports it as an error and the caller keeps its
         * raster path, which is the bargain the whole ray-tracing feature makes.
         */
    } // namespace

    // The nested type declared in the interface, defined here: a function-pointer table is an
    // implementation detail (the header only needs to know it exists, so it holds a unique_ptr).
    struct bottom_level_structures::entry_points {
        PFN_vkCreateAccelerationStructureKHR create = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR get_device_address = nullptr;

        [[nodiscard]] bool loaded() const noexcept {
            return this->create != nullptr && this->destroy != nullptr && this->get_build_sizes != nullptr && this->cmd_build != nullptr && this->get_device_address != nullptr;
        }

        [[nodiscard]] bool load(VkDevice const device) noexcept {
            this->create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
            this->destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
            this->get_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
            this->cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
            this->get_device_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
            return this->loaded();
        }
    };

    bottom_level_structures::bottom_level_structures(core& device)
        : gpu(&device)
        , functions(std::make_unique<entry_points>()) {
        if (!this->functions->load(device.logical_device)) {
            deren::utility::log("acceleration structures: the loader does not expose the vk*AccelerationStructure* entry points "
                                "(vkGetDeviceProcAddr returned null) - ray-traced shadows stay off");
        }
    }

    bottom_level_structures::~bottom_level_structures() {
        // The structures are destroyed before their storage buffers are released (the contract owners
        // below drop their reference when the entries die): vkDestroyAccelerationStructureKHR only drops
        // the handle, but a structure whose memory is gone is not something to leave to member-destruction order.
        for (entry const& item : this->entries) {
            if (item.handle != VK_NULL_HANDLE && this->gpu != nullptr && this->functions != nullptr && this->functions->loaded()) {
                this->functions->destroy(this->gpu->logical_device, item.handle, nullptr);
            }
        }
    }

    std::expected<uint32_t, std::string> bottom_level_structures::add(geometry_source const& source, bool const refittable) {
        core& vk = *this->gpu;
        // Every geometry gets an entry, even one with nothing to build: the caller's index into this
        // list is the caller's index into its own geometry array, and skipping one silently would
        // shift every later index by one.
        entry item = {};
        uint32_t const triangle_count = source.index_count / 3u;
        this->stats.triangle_count += triangle_count;
        if (triangle_count == 0) {
            this->entries.push_back(std::move(item));
            return static_cast<uint32_t>(this->entries.size() - 1);
        }

        VkAccelerationStructureGeometryTrianglesDataKHR triangles = {};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // position at offset 0 of the interleaved vertex
        triangles.vertexData.deviceAddress = source.vertex_address;
        triangles.vertexStride = source.vertex_stride;
        triangles.maxVertex = source.vertex_count == 0 ? 0u : source.vertex_count - 1u;
        // A zero index address means the geometry is NOT indexed, and the type has to say so: the module's
        // own geometry_source documents it ("index_address may be 0 for a non-indexed geometry, which the
        // build then reads as a flat vertex list"), but leaving the caller's index type in place sent the
        // build to read indices from address zero instead - which the mask bake's expanded geometry would
        // have hit on its first run (see shaders/mask_bake.slang).
        bool const indexed = source.index_address != 0;
        triangles.indexType = indexed ? source.index_type : VK_INDEX_TYPE_NONE_KHR;
        triangles.indexData.deviceAddress = indexed ? source.index_address : 0;

        VkAccelerationStructureGeometryKHR geometry = {};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // NO OPAQUE FLAG, which is a deliberate reversal of what this module shipped with. OPAQUE on a geometry
        // DECLARES that this geometry has no any-hit work to do - "any-hit shaders must not be invoked here" -
        // so setting it on every geometry, which is what this line used to do, is a statement that an
        // alpha-tested surface is as solid as its bounding triangles. Whether a geometry is opaque is a property
        // of its MATERIAL and this module is handed vertex and index addresses rather than materials, so the
        // per-material decision belongs to the step that adds the alpha test.
        //
        // MEASURED, because the flag's effect is not what the declaration suggests: on an NVIDIA RTX 4060
        // (591.59.0.0) the ray-tracing shadow's any-hit stage is invoked even WITH this flag set (and even with
        // the raygen's `gl_RayFlagsOpaqueEXT` set as well), so lifting it does not change the image - three
        // capture arms with the closest-hit stage silenced all produced the correct frame, differing by 0.01
        // whole-frame mean, which is this flag changing the BUILT structure and with it the traversal order.
        // It is lifted anyway because the alpha test must not depend on a driver over-invoking a stage that two
        // declarations say must not run: a conforming driver would skip it and the alpha test would silently do
        // nothing. The cost is the opaque-traversal shortcut, and the per-material decision can restore it.
        geometry.flags = 0;
        geometry.geometry.triangles = triangles;
        if (source.opacity_micromap != VK_NULL_HANDLE && source.opacity_index_address != 0) {
            // THE OPACITY MICROMAP CHAINED INTO THIS GEOMETRY. Both structs live in a container whose elements
            // never move, because geometry.pNext points at them and the build reads them later, at RECORD time -
            // a container that reallocates (or an entry that gets copied) would leave that pointer dangling, and
            // the failure mode is a traversal that consults freed memory rather than a compile error.
            this->micromap_geometries.push_back(micromap_attachment{});
            micromap_attachment& slot = this->micromap_geometries.back();
            slot.usage = source.opacity_usage;
            slot.attachment.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
            slot.attachment.pNext = nullptr;
            slot.attachment.indexType = source.opacity_index_type;
            slot.attachment.indexBuffer.deviceAddress = source.opacity_index_address;
            slot.attachment.indexStride = source.opacity_index_stride;
            slot.attachment.baseTriangle = 0;
            slot.attachment.usageCountsCount = 1;
            slot.attachment.pUsageCounts = &slot.usage;
            slot.attachment.micromap = source.opacity_micromap;
            // IT CHAINS INTO THE TRIANGLES DATA, not into the geometry: VkAccelerationStructureGeometryKHR's own
            // pNext accepts only the micromap-DATA struct (the KHR way of BUILDING a micromap, which this does not
            // use - it builds through vkCmdBuildMicromapsEXT), and validation named exactly that when this was
            // first attached in the wrong place. The union member was copied from `triangles` above, so this edits
            // the copy the build will read.
            geometry.geometry.triangles.pNext = &slot.attachment;
        }
        item.geometries.push_back(geometry);

        VkAccelerationStructureBuildGeometryInfoKHR size_info = {};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        size_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // ALLOW_UPDATE changes the scratch the device asks for (an update needs no build scratch, a build
        // does), so the size query has to carry the same flags the build will - a mismatch is a scratch
        // buffer that is too small on the REFIT, which is a validation error rather than a wrong image.
        size_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          (refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u);
        size_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        size_info.geometryCount = static_cast<uint32_t>(item.geometries.size());
        size_info.pGeometries = item.geometries.data();

        VkAccelerationStructureBuildSizesInfoKHR sizes = {};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        this->functions->get_build_sizes(vk.logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &size_info, &triangle_count, &sizes);
        if (sizes.accelerationStructureSize == 0) {
            return std::unexpected(std::string("acceleration structure: the device reported a zero-sized bottom level structure"));
        }

        item.storage = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
            rhi::buffer_desc{.size = sizes.accelerationStructureSize, .usage = rhi::buffer_usage::acceleration_structure_storage})};
        if (!item.storage) {
            return std::unexpected(std::string("acceleration structure: the bottom level storage allocation failed"));
        }
        VkBuffer const storage_native = native_buffer_of(vk, *item.storage);
        if (storage_native == VK_NULL_HANDLE) {
            return std::unexpected(std::string("acceleration structure: the bottom level storage has no native buffer"));
        }

        VkAccelerationStructureCreateInfoKHR create = {};
        create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create.buffer = storage_native;
        create.offset = 0;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (this->functions->create(vk.logical_device, &create, nullptr, &item.handle) != VK_SUCCESS) {
            return std::unexpected(std::string("acceleration structure: vkCreateAccelerationStructureKHR failed"));
        }

        // The scratch range is aligned per geometry (see record_build), so the sizes accumulate here
        // and only the offsets are decided then - the buffer itself is allocated once, in record_build.
        item.scratch_size = sizes.buildScratchSize;
        item.range.primitiveCount = triangle_count;
        item.refittable = refittable;

        this->stats.geometry_count += 1;
        this->stats.structure_bytes += sizes.accelerationStructureSize;
        this->stats.scratch_bytes += sizes.buildScratchSize;
        this->entries.push_back(std::move(item));
        return static_cast<uint32_t>(this->entries.size() - 1);
    }

    std::expected<void, std::string> bottom_level_structures::record_build(VkCommandBuffer const command_buffer) {
        core& vk = *this->gpu;
        auto const start = std::chrono::steady_clock::now();

        if (this->entries.empty()) {
            return {}; // nothing was added: an empty command is not an error, it is an empty scene
        }

        // One scratch buffer for every build, each geometry's range aligned to what the device
        // requires of a SCRATCH ADDRESS (not of an offset - the requirement is on the address the
        // build is handed, which is why the base address is taken into account and why the buffer
        // carries one alignment worth of slack).
        VkDeviceSize const alignment = std::max<VkDeviceSize>(vk.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment, 1);
        VkDeviceSize total = 0;
        for (entry& item : this->entries) {
            total += item.scratch_size + alignment;
        }
        this->scratch = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
            rhi::buffer_desc{.size = total, .usage = rhi::buffer_usage::acceleration_structure_scratch, .flags = device_address_flag})};
        if (!this->scratch) {
            return std::unexpected(std::string("acceleration structure: the scratch allocation failed"));
        }
        VkDeviceAddress const scratch_base = buffer_address_of(vk, *this->scratch);
        if (scratch_base == 0) {
            return std::unexpected(std::string("acceleration structure: the scratch buffer has no device address"));
        }

        this->build_infos.clear();
        this->range_ptrs.clear();
        this->build_infos.reserve(this->entries.size());
        this->range_ptrs.reserve(this->entries.size());
        VkDeviceSize cursor = 0;
        for (entry& item : this->entries) {
            if (item.handle == VK_NULL_HANDLE) {
                continue; // a geometry with no triangles: no build, no scratch range
            }
            VkDeviceAddress const address = align_up(scratch_base + cursor, alignment);
            item.scratch_offset = address - scratch_base;
            cursor = item.scratch_offset + item.scratch_size;

            VkAccelerationStructureBuildGeometryInfoKHR info = {};
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                         (item.refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u);
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.dstAccelerationStructure = item.handle;
            info.geometryCount = static_cast<uint32_t>(item.geometries.size());
            info.pGeometries = item.geometries.data();
            info.scratchData.deviceAddress = address;
            this->build_infos.push_back(info);
            this->range_ptrs.push_back(&item.range);
        }

        if (!this->build_infos.empty()) {
            // ONE call for every structure: the pieces of a single vkCmdBuildAccelerationStructuresKHR
            // are executed in order, so a geometry's build is complete before the next one starts.
            this->functions->cmd_build(command_buffer, static_cast<uint32_t>(this->build_infos.size()), this->build_infos.data(), this->range_ptrs.data());
        }

        this->scratch_address = scratch_base;
        this->scratch_size = total;
        this->stats.scratch_bytes = total;
        this->stats.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return {};
    }

    std::expected<void, std::string> bottom_level_structures::record_update(VkCommandBuffer const command_buffer, std::span<uint32_t const> const indices) {
        if (this->scratch_address == 0) {
            return std::unexpected(std::string("acceleration structure: no build has been recorded, so there is no scratch to refit against"));
        }
        // The SAME geometries the build used, with the same addresses and counts: an update is legal
        // exactly when only the bytes behind them changed. That is what makes it cheap - no size query, no
        // allocation, no new structure - and it is also the whole reason a compute skinning pass can feed
        // one (see shaders/compute_skin.slang).
        this->update_infos.clear();
        this->update_range_ptrs.clear();
        this->update_infos.reserve(indices.size());
        this->update_range_ptrs.reserve(indices.size());
        for (uint32_t const index : indices) {
            if (index >= this->entries.size()) {
                continue;
            }
            entry const& item = this->entries[index];
            if (item.handle == VK_NULL_HANDLE || !item.refittable) {
                continue; // not built, or built without ALLOW_UPDATE: an update against it is illegal
            }
            VkAccelerationStructureBuildGeometryInfoKHR info = {};
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            info.srcAccelerationStructure = item.handle;
            info.dstAccelerationStructure = item.handle;
            info.geometryCount = static_cast<uint32_t>(item.geometries.size());
            info.pGeometries = item.geometries.data();
            info.scratchData.deviceAddress = this->scratch_address + item.scratch_offset;
            this->update_infos.push_back(info);
            this->update_range_ptrs.push_back(&item.range);
        }
        if (!this->update_infos.empty()) {
            this->functions->cmd_build(command_buffer, static_cast<uint32_t>(this->update_infos.size()), this->update_infos.data(), this->update_range_ptrs.data());
        }
        return {};
    }

    // ---- top level structure ----
    namespace {
        /// the world matrix the raster passes draw with, as the 3x4 ROW-major transform the instance
        /// wants. glm is column-major (m[column][row]) and VkTransformMatrixKHR is row-major
        /// (matrix[row][column]), so the indices swap - which is the one place a transposed instance
        /// would silently mirror the whole scene.
        VkTransformMatrixKHR to_instance_transform(glm::mat4 const& matrix) noexcept {
            VkTransformMatrixKHR out = {};
            for (uint32_t row = 0; row < 3; ++row) {
                for (uint32_t column = 0; column < 4; ++column) {
                    out.matrix[row][column] = matrix[column][row];
                }
            }
            return out;
        }
    } // namespace

    struct top_level_structure::entry_points {
        PFN_vkCreateAccelerationStructureKHR create = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR get_device_address = nullptr;

        [[nodiscard]] bool loaded() const noexcept {
            return this->create != nullptr && this->destroy != nullptr && this->get_build_sizes != nullptr && this->cmd_build != nullptr && this->get_device_address != nullptr;
        }

        [[nodiscard]] bool load(VkDevice const device) noexcept {
            this->create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
            this->destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
            this->get_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
            this->cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
            this->get_device_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
            return this->loaded();
        }
    };

    top_level_structure::top_level_structure(core& device, uint32_t const frame_slot_count)
        : gpu(&device)
        , functions(std::make_unique<entry_points>())
        , slots(frame_slot_count) {
        if (!this->functions->load(device.logical_device)) {
            deren::utility::log("acceleration structures: the loader does not expose the vk*AccelerationStructure* entry points "
                                "(vkGetDeviceProcAddr returned null) - ray-traced shadows stay off");
        }
    }

    top_level_structure::~top_level_structure() {
        for (slot const& item : this->slots) {
            if (item.handle != VK_NULL_HANDLE && this->gpu != nullptr && this->functions != nullptr && this->functions->loaded()) {
                this->functions->destroy(this->gpu->logical_device, item.handle, nullptr);
            }
        }
    }

    std::expected<void, std::string> top_level_structure::begin(uint32_t const frame_slot) {
        if (!this->functions->loaded()) {
            return std::unexpected(std::string("acceleration structures: the top level entry points were not resolved"));
        }
        if (frame_slot >= this->slots.size()) {
            return std::unexpected(std::string("acceleration structures: frame slot out of range"));
        }
        this->current_slot = frame_slot;
        this->slots[frame_slot].count = 0;
        return {};
    }

    std::expected<void, std::string> top_level_structure::add(bottom_level_structures const& levels, instance_source const& source) {
        core& vk = *this->gpu;
        if (this->current_slot >= this->slots.size()) {
            return std::unexpected(std::string("acceleration structures: no frame slot is being built"));
        }
        VkAccelerationStructureKHR const blas = levels.handle(source.blas_index);
        if (blas == VK_NULL_HANDLE) {
            return {}; // a geometry with no triangles: no instance, and the caller's indices stay put
        }
        slot& target = this->slots[this->current_slot];

        // Grow the per-slot arrays when this frame's list outgrew them. Doubling keeps the reallocation
        // rare (it destroys and recreates the structure, so it is not something to do every frame), and
        // the capacity - not the count - is what the structure is sized for, which is legal: the size
        // query's instance count is an upper bound for the build.
        if (target.count >= target.capacity) {
            uint32_t const wanted = std::max(target.capacity * 2u, 256u);
            if (wanted > vk.acceleration_structure_properties.maxInstanceCount) {
                return std::unexpected(std::string("acceleration structures: the scene has more instances than the device allows in one top level structure"));
            }
            // The two arrays are HOST-VISIBLE and coherent (the caller fills them through add()), and they
            // carry a device address + the acceleration-structure build-input capability - the pair the
            // renderer's own geometry buffers are uploaded with, so a build can read them directly.
            constexpr rhi::buffer_flags instance_flags = device_address_flag | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input);
            target.instances = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
                rhi::buffer_desc{.size = static_cast<uint64_t>(wanted) * sizeof(VkAccelerationStructureInstanceKHR),
                                 .usage = rhi::buffer_usage::storage_coherent,
                                 .flags = instance_flags})};
            target.records = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
                rhi::buffer_desc{.size = static_cast<uint64_t>(wanted) * sizeof(instance_record),
                                 .usage = rhi::buffer_usage::storage_coherent,
                                 .flags = instance_flags})};
            if (!target.instances || !target.records) {
                return std::unexpected(std::string("acceleration structures: the instance buffers could not be allocated"));
            }
            if (target.instances->mapped().data() == nullptr || target.records->mapped().data() == nullptr) {
                return std::unexpected(std::string("acceleration structures: the instance buffers are not mapped"));
            }

            // The structure the new capacity needs. The old handle goes first: a structure must not
            // outlive the memory it was created in, and that memory is about to be released.
            if (target.handle != VK_NULL_HANDLE) {
                this->functions->destroy(vk.logical_device, target.handle, nullptr);
                target.handle = VK_NULL_HANDLE;
            }
            VkAccelerationStructureBuildGeometryInfoKHR size_info = {};
            size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            size_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            size_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            size_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            size_info.geometryCount = 1;
            VkAccelerationStructureGeometryKHR geometry = {};
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geometry.geometry.instances.arrayOfPointers = VK_FALSE;
            geometry.geometry.instances.data.deviceAddress = 0; // the size query does not read the data
            size_info.pGeometries = &geometry;

            VkAccelerationStructureBuildSizesInfoKHR sizes = {};
            sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            this->functions->get_build_sizes(vk.logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &size_info, &wanted, &sizes);
            if (sizes.accelerationStructureSize == 0) {
                return std::unexpected(std::string("acceleration structures: the device reported a zero-sized top level structure"));
            }
            target.storage = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
                rhi::buffer_desc{.size = sizes.accelerationStructureSize, .usage = rhi::buffer_usage::acceleration_structure_storage})};
            if (!target.storage) {
                return std::unexpected(std::string("acceleration structures: the top level storage allocation failed"));
            }
            VkBuffer const top_storage_native = native_buffer_of(vk, *target.storage);
            if (top_storage_native == VK_NULL_HANDLE) {
                return std::unexpected(std::string("acceleration structures: the top level storage has no native buffer"));
            }
            VkAccelerationStructureCreateInfoKHR create = {};
            create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            create.buffer = top_storage_native;
            create.offset = 0;
            create.size = sizes.accelerationStructureSize;
            create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            if (this->functions->create(vk.logical_device, &create, nullptr, &target.handle) != VK_SUCCESS) {
                return std::unexpected(std::string("acceleration structures: the top level structure could not be created"));
            }
            // KEPT because a heap descriptor for it is an address RANGE that must carry a real size (see
            // top_level_structure::structure_size): the size query above is the only place that number exists.
            target.structure_size = create.size;
            target.capacity = wanted;
            target.scratch_size = sizes.buildScratchSize;
            this->stats.structure_bytes += sizes.accelerationStructureSize;
        }

        // The instance itself: a device address for the geometry, the world transform, and the record
        // the shader will see through instanceCustomIndex (which is why the index IS the table slot).
        VkAccelerationStructureDeviceAddressInfoKHR const address_info = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .pNext = nullptr, .accelerationStructure = blas};
        VkAccelerationStructureInstanceKHR instance = {};
        instance.transform = to_instance_transform(source.transform);
        instance.instanceCustomIndex = target.count;         // == the slot in the instance table
        instance.mask = 0xFF;                                // visible to every ray: nothing here is ray-type specific
        instance.instanceShaderBindingTableRecordOffset = 0; // unused by a ray query (no SBT exists)
        // FACING_CULL_DISABLE: a shadow ray must be blocked by a surface it approaches from behind, and
        // the raster shadow pass has the same property for the casters whose pipeline disables culling.
        // Leaving culling on would make every plane and every open mesh leak light.
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = this->functions->get_device_address(vk.logical_device, &address_info);

        std::memcpy(target.instances->mapped().data() + static_cast<std::size_t>(target.count) * sizeof(VkAccelerationStructureInstanceKHR),
                    &instance,
                    sizeof(instance));
        std::memcpy(target.records->mapped().data() + static_cast<std::size_t>(target.count) * sizeof(instance_record),
                    &source.record,
                    sizeof(source.record));
        target.count += 1;
        return {};
    }

    std::expected<void, std::string> top_level_structure::record_build(VkCommandBuffer const command_buffer) {
        core& vk = *this->gpu;
        auto const start = std::chrono::steady_clock::now();
        slot& target = this->slots[this->current_slot];
        if (target.count == 0 || target.handle == VK_NULL_HANDLE) {
            return {}; // an empty scene has an empty top level structure, and nothing to trace against
        }

        VkDeviceAddress const instances_address = buffer_address_of(vk, *target.instances);

        VkDeviceSize const alignment = std::max<VkDeviceSize>(vk.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment, 1);
        target.scratch_size = 0;
        VkAccelerationStructureGeometryKHR geometry = {};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = instances_address;

        VkAccelerationStructureBuildGeometryInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = target.handle;
        info.geometryCount = 1;
        info.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR sizes = {};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        this->functions->get_build_sizes(vk.logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &target.count, &sizes);

        // One scratch buffer per slot, sized for the count actually being built. It is allocated on the
        // FIRST build of a slot and kept: the count is culled per frame and drifts, but a buffer sized
        // for the largest count seen is what a build of any smaller count needs.
        if (!target.scratch || target.scratch_size < sizes.buildScratchSize) {
            target.scratch = rhi::object_manager<rhi::buffer>{contract_of(vk).create_buffer(
                rhi::buffer_desc{.size = sizes.buildScratchSize + alignment,
                                 .usage = rhi::buffer_usage::acceleration_structure_scratch,
                                 .flags = device_address_flag})};
            if (!target.scratch) {
                return std::unexpected(std::string("acceleration structures: the top level scratch allocation failed"));
            }
        }
        target.scratch_size = sizes.buildScratchSize + alignment;
        VkDeviceAddress const scratch_base = buffer_address_of(vk, *target.scratch);
        if (scratch_base == 0) {
            return std::unexpected(std::string("acceleration structures: the top level scratch has no device address"));
        }
        info.scratchData.deviceAddress = align_up(scratch_base, alignment);

        VkAccelerationStructureBuildRangeInfoKHR range = {};
        range.primitiveCount = target.count;
        VkAccelerationStructureBuildRangeInfoKHR const* ranges[1] = {&range};
        this->functions->cmd_build(command_buffer, 1, &info, ranges);

        this->stats.geometry_count = target.count;
        this->stats.scratch_bytes = target.scratch_size;
        this->stats.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return {};
    }

    VkBuffer top_level_structure::instance_table(uint32_t const frame_slot) const noexcept {
        // The slot holds a CONTRACT buffer, so the native handle is asked of the escape - borrowed, and
        // valid while the slot's owner holds its reference (the descriptor heap writes a range over this
        // buffer, so it must not outlive the slot).
        if (this->gpu == nullptr || frame_slot >= this->slots.size()) {
            return VK_NULL_HANDLE;
        }
        slot const& target = this->slots[frame_slot];
        return target.records ? native_buffer_of(*this->gpu, *target.records) : VK_NULL_HANDLE;
    }

    rhi::buffer const* top_level_structure::instance_table_buffer(uint32_t const frame_slot) const noexcept {
        // THE SAME SLOT, ANSWERED AS THE CONTRACT HELPER RATHER THAN AS A NARROWED HANDLE, so the caller that
        // needs the buffer's device address asks this backend's `device_address` ability for it instead of
        // calling vkGetBufferDeviceAddress on a handle it had to obtain from the escape itself.
        //
        // BORROWED, AND THE SLOT OWNS THE REFERENCE: the pointer is valid for as long as this slot holds the
        // buffer - `add()` replaces it only while GROWING the capacity, which happens before the build of the
        // frame that uses it, and never releases a slot's buffer while a frame could still read it.
        if (frame_slot >= this->slots.size()) {
            return nullptr;
        }
        slot const& target = this->slots[frame_slot];
        return target.records ? target.records.get() : nullptr;
    }
} // namespace deren::vulkan::acceleration_structure
