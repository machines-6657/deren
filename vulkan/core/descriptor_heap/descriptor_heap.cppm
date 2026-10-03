module; // the macro-using Vulkan header must not be imported into a module purview

#include <vulkan/vulkan.h>
// LOAD-BEARING, for the reason chores.cpp documents at length: with -fno-exceptions and the vendored std
// module, a TU that instantiates std::vector sees TWO 'operator new(size_t, align_val_t)' declarations - module
// std's and the textual libc++ copy baked into utility:data_block.pcm - and resolves neither. This file
// allocates (the zero-filled heap contents, and deren::utility::log's formatting), and it died with an access
// violation inside the FIRST allocation it made until this include was added, with no log line, no validation
// message and no allocation error: exactly the shape of the ambiguous-operator-new failure, one step further
// along. Textually including glm merges the two copies. Do not remove this include to "clean up".
#include <fstream>
#include <glm/glm.hpp>
/**
 * @file vulkan/core/descriptor_heap/descriptor_heap.cppm
 * @brief VK_EXT_descriptor_heap as this renderer's binding model: descriptors in a buffer the application
 *        writes, instead of descriptor sets, layouts and pools.
 * @defgroup vulkan_core_descriptor_heap Descriptor Heap
 *
 * WHY IT IS A SUBMODULE OF core AND NOT A PASS OR A TOP-LEVEL MODULE: a heap is DEVICE-WIDE state. The
 * extension's own guidance is to bind one heap for the application's lifetime, because binding a new one costs
 * a pipeline flush; the descriptors in it are the same contract the shared scene block already describes; and
 * every pass reads them. So it lives beside the vma allocator as something `core` owns and hands down, and it
 * deliberately knows NOTHING about what a binding means - set 0 binding 7 being the light UBO is core's
 * business, not this file's.
 *
 * WHAT IT OWNS: the two heap buffers (resources and samplers, which are SEPARATE heaps in this API), the
 * layout arithmetic that the device's properties dictate, and the three calls that make the model work -
 * vkWriteResourceDescriptorsEXT to write a descriptor, vkCmdBindResourceHeapEXT/vkCmdBindSamplerHeapEXT to
 * bind the heaps, and VkDescriptorSetAndBindingMappingInfoEXT to let a shader keep naming `set N, binding M`.
 *
 * THE MAPPING IS WHAT MAKES THIS A MIGRATION RATHER THAN A REWRITE: it is chained into
 * VkPipelineShaderStageCreateInfo (not only into VkShaderCreateInfoEXT), so the shaders keep their existing
 * `layout(set = ..., binding = ...)` declarations and the heap is what those numbers resolve to. A combined
 * image sampler is expressed with an EMBEDDED SAMPLER in the resource heap
 * (VkDescriptorMappingSourceConstantOffsetEXT::pEmbeddedSampler, whose sampler part is taken from the reserved
 * sampler range) - which is why a heap binding must respect minSamplerHeapReservedRangeWithEmbedded.
 *
 * AND THE UNIT OF THAT MIGRATION IS THE FRAME, NOT THE PASS - measured, not assumed. Binding the heap is
 * command-buffer state that takes over EVERY stage recorded after it: with the bind recorded at the start of the
 * frame and every mapping switched off (so the heap held nothing any shader had asked for), all nine reference
 * scenarios came back as the SAME frame - hash DC5F6D66428C26D8, mean 0.00 against the unlit reference's 88.1 -
 * with validation SILENT, because a stage whose descriptors came from a set reads the heap instead once one is
 * bound. Two further rules cost one gate run each and are not optional: a mapping is silently IGNORED unless the
 * pipeline is created with VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT (a flags2 bit, so it arrives through
 * VkPipelineCreateFlags2CreateInfo), and that flag REQUIRES a null VkPipelineLayout - the layout is precisely what
 * the mapping replaces. So a renderer maps every stage of a frame or none of them: a half-migrated frame does not
 * render a half-right picture, it renders nothing.
 */

export module deren.vulkan.core:descriptor_heap;

import deren.vstd;
import :vma;         // the allocator the heaps are allocated from
import :vma_handles; // vk_buffer, which is what a heap buffer is
import deren.utility;
namespace deren::vulkan {
    /**
     * @ingroup vulkan_core_descriptor_heap
     * @brief the device's heap numbers, copied out of VkPhysicalDeviceDescriptorHeapPropertiesEXT at init
     * @note it is a struct of its own rather than a reference to the queried properties so that this module does
     *       not depend on the capability layer's header: the caller copies what a layout needs, and the heap
     *       never reaches back into device state it does not own.
     */
    export struct heap_limits {
        VkDeviceSize max_resource_size = 0;
        VkDeviceSize max_sampler_size = 0;
        /// what a heap binding's device address must be a multiple of
        VkDeviceSize resource_alignment = 0;
        VkDeviceSize sampler_alignment = 0;
        /// how much of each heap an implementation wants reserved before descriptors may follow
        VkDeviceSize resource_reserved = 0;
        VkDeviceSize sampler_reserved_with_embedded = 0;
        /// the strides a descriptor of each kind occupies in the resource heap
        uint32_t buffer_descriptor_size = 0;
        uint32_t image_descriptor_size = 0;
        uint32_t sampler_descriptor_size = 0;
        /// the push-data window (vkCmdPushDataEXT) and how many embedded samplers fit
        uint32_t max_push_data = 0;
        uint32_t max_embedded_samplers = 0;
    };

    /**
     * @ingroup vulkan_core_descriptor_heap
     * @brief one resource heap and one sampler heap, with the calls that write and bind them
     */
    export class descriptor_heap {
    public:
        descriptor_heap() = default;
        descriptor_heap(descriptor_heap const&) = delete;
        descriptor_heap& operator=(descriptor_heap const&) = delete;
        descriptor_heap(descriptor_heap&&) = delete;
        descriptor_heap& operator=(descriptor_heap&&) = delete;
        ~descriptor_heap();

        /**
         * @brief allocate both heaps and load the entry points
         * @param allocator the device's allocator, which the heaps are buffers from
         * @param device the logical device
         * @param limits the device's heap properties (see heap_limits)
         * @return whether both heaps exist and every entry point was published; a false leaves nothing behind,
         *         so a caller can keep running with descriptor sets exactly as before
         * @note the sizes asked for are the WORKING sizes this renderer needs rather than the device's maxima:
         *       the scene block's descriptors are counted in hundreds of bytes, and the sampler heap's working
         *       size is the reserved range the embedded-sampler path requires.
         */
        [[nodiscard]] bool init(vma_allocator& allocator, VkDevice device, heap_limits const& limits) noexcept;
        /// @brief release both heaps (the buffers free themselves; this drops the references)
        void destroy() noexcept;

        [[nodiscard]] bool ready() const noexcept {
            return this->write_resource_descriptors != nullptr && this->bind_resource_heap != nullptr && this->bind_sampler_heap != nullptr && this->resource_heap_address != 0;
        }
        /// @brief where the resource heap begins, i.e. what a mapping's heapOffset is relative to
        [[nodiscard]] VkDeviceAddress resource_address() const noexcept {
            return this->resource_heap_address;
        }
        [[nodiscard]] VkDeviceAddress sampler_address() const noexcept {
            return this->sampler_heap_address;
        }
        [[nodiscard]] VkDeviceSize resource_size() const noexcept {
            return this->resource_heap_size;
        }
        [[nodiscard]] VkDeviceSize sampler_size() const noexcept {
            return this->sampler_heap_size;
        }
        [[nodiscard]] heap_limits const& limits() const noexcept {
            return this->configured_limits;
        }

        /**
         * @brief write descriptors into the resource heap
         * @param descriptors_offset the byte offset into the resource heap the first descriptor lands at
         * @param infos one VkResourceDescriptorInfoEXT per descriptor (a buffer is written as an address range)
         * @return whether the write happened (false when the heap is not ready or the range would overflow it)
         * @note THE DESTINATION IS A HOST ADDRESS, not the heap's device address:
         *       vkWriteResourceDescriptorsEXT takes VkHostAddressRangeEXT, i.e. a pointer into memory the host has
         *       mapped, and this class keeps the heap's mapped pointer for exactly that. (Writing through the
         *       DEVICE address - which the first version of this did - crashes the process the moment a driver
         *       accepts the descriptor and dereferences it, and validation says nothing about it because the
         *       address is just a void* to it.)
         */
        [[nodiscard]] bool write_descriptors(VkDeviceSize descriptors_offset, std::span<VkResourceDescriptorInfoEXT const> infos) noexcept;

        /**
         * @brief write SAMPLER descriptors into the sampler heap
         * @param descriptors_offset the byte offset in the SAMPLER heap the first descriptor lands at
         * @param samplers the VkSamplerCreateInfo of each sampler, in the order they are written
         * @return whether the write happened (false when the heap is not ready or the range would overflow it)
         * @note SAMPLERS ARE THE OTHER HEAP, and this is not a detail: the API splits resources and samplers into
         *       two heaps, so a shader that names a heap sampler at all needs the sampler heap bound as well (see
         *       record_bind). The create info is what a heap descriptor carries - exactly as a heap IMAGE
         *       descriptor carries a view create info - because the driver creates the object inside the heap.
         */
        [[nodiscard]] bool write_samplers(VkDeviceSize descriptors_offset, std::span<VkSamplerCreateInfo const> samplers) noexcept;

        /**
         * @brief bind both heaps for the command buffer being recorded
         * @note the reserved range is what makes embedded samplers legal: a combined image sampler whose sampler
         *       lives in the resource heap needs the sampler heap's reserved window declared here.
         */
        void record_bind(VkCommandBuffer command_buffer) const noexcept;

        /**
         * @brief write THIS heap's two bind infos into storage the caller owns
         * @param resource filled with the resource heap's bind info (the same one record_bind binds)
         * @param sampler filled with the sampler heap's bind info
         *
         * WHY IT EXISTS SEPARATELY FROM record_bind: a SECONDARY COMMAND BUFFER IS VALIDATED ON ITS OWN, so the
         * bind the primary records never reaches it - validation refuses the draw with "The shader uses resource
         * descriptors, but VkCommandBufferInheritanceDescriptorHeapInfoEXT::pResourceHeapBindInfo is NULL"
         * (VUID-vkCmdDrawIndexed-None-11308), and a migrated frame whose leaves draw in secondaries therefore
         * comes out black. `VkCommandBufferInheritanceDescriptorHeapInfoEXT` POINTS at the bind infos rather than
         * copying them, which is why this fills caller-owned storage: it has to outlive vkBeginCommandBuffer, and
         * a stack local in the recording function does that.
         */
        void bind_infos(VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler) const noexcept;

        /**
         * @brief send PUSH CONSTANTS to a heap pipeline: the bytes a shader reads as `layout(push_constant)`
         * @param offset the byte offset into the push-data window (0 for a pass that pushes one block)
         * @param data the block itself, whose size must fit `maxPushDataSize - offset`
         * @return whether the push happened (false when the heap is not ready or no entry point was published)
         * @note THIS IS WHY A HEAP PIPELINE CAN STILL HAVE PARAMETERS. A pipeline created with
         *       VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT must have a NULL layout - the proposal says so in
         *       those words - so vkCmdPushConstants has nothing to push TO; the same document says the data is
         *       "accessed in the same way as before via the PushConstant storage class, it is now simply
         *       unnecessary to construct a pipeline layout to do that". The two commands invalidate each other, so
         *       a converted frame uses this one and nothing else.
         */
        [[nodiscard]] bool push_data(VkCommandBuffer command_buffer, uint32_t offset, std::span<std::byte const> data) const noexcept;

        /// @brief the heap offset of the descriptor KIND @p type occupies, or 0 when the heap cannot hold it
        [[nodiscard]] uint32_t descriptor_stride(VkDescriptorType type) const noexcept;

        /**
         * @brief write ONE image descriptor (a SAMPLED image) into the resource heap
         * @param offset_bytes the byte offset in the heap, e.g. `descriptor_offset(block, index, type)`
         * @param view the VIEW TO CREATE - and this is the model difference worth knowing: a heap image
         *        descriptor carries a VkImageViewCreateInfo, not an existing VkImageView, because the driver
         *        creates the view inside the descriptor. A descriptor-set path that already holds a view has to
         *        keep the create info it made that view from in order to write the same binding here.
         * @param layout the layout the image will be in when sampled
         * @param type VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE or VK_DESCRIPTOR_TYPE_STORAGE_IMAGE - and NOT
         *        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, which the resource heap does not accept at all
         *        (VUID-VkResourceDescriptorInfoEXT-type-11210 lists the kinds a heap descriptor may be, and a
         *        combined image sampler is not among them): the heap holds the IMAGE, and the sampler comes from
         *        the SAMPLER HEAP - the shaders combine the two at the point of use (`sampler2D(tex, samp)` in
         *        shaders/heap_slots.glsl), which is what removed the mapping step this comment used to describe.
         */
        [[nodiscard]] bool write_image(VkDeviceSize offset_bytes, VkImageViewCreateInfo const& view, VkImageLayout layout, VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) noexcept;
        /// @brief write ONE buffer descriptor (its device address range) into the resource heap
        [[nodiscard]] bool write_buffer(VkDeviceSize offset_bytes, VkDeviceAddress address, VkDeviceSize size, VkDescriptorType type) noexcept;

        /**
         * @brief reserve @p count descriptors of @p type in the resource heap and return their byte offset
         * @return the offset, or VK_WHOLE_SIZE when the heap is not ready or the reservation does not fit
         *
         * @note THIS IS WHAT KEEPS THE LAYOUT AND THE MAPPINGS FROM DISAGREEING. A caller that computes an offset
         *       by hand and a mapping built from another number is the silent failure this design has - the heap
         *       reads memory that was never written, validation says nothing, and the picture is simply wrong. So
         *       the offsets are RESERVED here, once, and both the write and the mapping use the reserved number.
         */
        [[nodiscard]] VkDeviceSize reserve(uint32_t count, VkDescriptorType type) noexcept;

        /**
         * @brief reserve @p bytes of the resource heap and return the offset, aligned to @p alignment
         * @return the offset, or VK_WHOLE_SIZE when the heap is not ready or the reservation does not fit
         * @note this is the form a MIXED block needs: a scene set holds descriptors of several kinds - buffers at
         *       one stride, images at another - so a block is laid out by hand and reserved as bytes, while the
         *       single-kind case above stays the convenient one.
         */
        [[nodiscard]] VkDeviceSize reserve_bytes(VkDeviceSize bytes, VkDeviceSize alignment) noexcept;

        /// @brief the byte offset of descriptor @p index of the block that starts at @p block_offset
        [[nodiscard]] VkDeviceSize descriptor_offset(VkDeviceSize block_offset, uint32_t index, VkDescriptorType type) const noexcept {
            return block_offset + static_cast<VkDeviceSize>(index) * this->descriptor_stride(type);
        }

        /**
         * @brief where a caller may START placing its own descriptors: past the implementation's reserved window
         *
         * @note THIS IS NOT OPTIONAL BOOKKEEPING. minResourceHeapReservedRange is memory the implementation reserves
         *       for itself (the embedded-sampler machinery lives in it), and descriptors written INSIDE it are not
         *       read back as written - which is not a validation error, it is a wrong picture. The first version of
         *       this renderer's texture array was written at offset 0 and every one of the nine reference frames
         *       changed, with validation silent; the mapping's heapOffset and the write offset both come from here
         *       for that reason, and they must stay the same number.
         */
        [[nodiscard]] VkDeviceSize usable_offset() const noexcept {
            VkDeviceSize const alignment = this->configured_limits.resource_alignment != 0 ? this->configured_limits.resource_alignment : 1u;
            return ((this->configured_limits.resource_reserved + alignment - 1u) / alignment) * alignment;
        }

    private:
        // called logical_device, not device: init()`s device parameter would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        VkDevice logical_device = VK_NULL_HANDLE;
        vk_buffer resource_heap = {};
        vk_buffer sampler_heap = {};
        /// the MAPPED pointer of the resource heap: vkWriteResourceDescriptorsEXT writes through a HOST address
        void* resource_mapped = nullptr;
        /// the same for the sampler heap, whose descriptors are written by vkWriteSamplerDescriptorsEXT
        void* sampler_mapped = nullptr;
        // called resource_heap_address, not resource_address: the resource_address() accessor of this class would collide with a member of that name
        VkDeviceAddress resource_heap_address = 0;
        // called sampler_heap_address, not sampler_address: the sampler_address() accessor of this class would collide with a member of that name
        VkDeviceAddress sampler_heap_address = 0;
        // called resource_heap_size, not resource_size: the resource_size() accessor of this class would collide with a member of that name
        VkDeviceSize resource_heap_size = 0;
        // called sampler_heap_size, not sampler_size: the sampler_size() accessor of this class would collide with a member of that name
        VkDeviceSize sampler_heap_size = 0;
        /// the bump pointer for reserve(), which starts past the implementation's reserved window
        VkDeviceSize next_free = 0;
        // called configured_limits, not limits: the limits parameter of init() would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX; the limits() accessor would collide with it as well
        heap_limits configured_limits = {};
        // called write_resource_descriptors, not write_descriptors: the write_descriptors() method of this class would collide with a member of that name
        PFN_vkWriteResourceDescriptorsEXT write_resource_descriptors = nullptr;
        // called write_sampler_descriptors, not write_samplers: the write_samplers() method of this class would collide with a member of that name
        PFN_vkWriteSamplerDescriptorsEXT write_sampler_descriptors = nullptr;
        /// the push-data entry point (see push_data): the push-constant path of a layout-less heap pipeline
        // called cmd_push_data, not push_data: the push_data() method of this class would collide with a member of that name
        PFN_vkCmdPushDataEXT cmd_push_data = nullptr;
        PFN_vkCmdBindResourceHeapEXT bind_resource_heap = nullptr;
        PFN_vkCmdBindSamplerHeapEXT bind_sampler_heap = nullptr;
    };
} // namespace deren::vulkan

namespace deren::vulkan {
    namespace {
        /// The working sizes. THIS RENDERER'S LAYOUT DECIDES THEM, not the device: the slot grid every
        /// heap-native shader addresses (see docs/descriptor_heap_migration.md) starts at a FIXED 1 MiB, so the
        /// resource heap has to be able to hold that base plus its slots - the first version asked for 256 KiB,
        /// which could not even fit the base, and the grid was refused with the heap left unused (measured). The
        /// sampler heap is capped at 128 KiB by the API, so its grid base is 64 KiB instead, which has to leave
        /// room for the samplers themselves - 64 KiB was entirely the reserved window the embedded-sampler path
        /// requires, i.e. no usable sampler space at all.
        constexpr VkDeviceSize resource_working_size = (1280u) * 1024u; // the 1 MiB grid base + its 64 KiB + the older blocks still in place
        constexpr VkDeviceSize sampler_working_size = 128u * 1024u;     // the API's maximum (64 reserved + 64 usable)

        /// the VkBuffer behind a vk_buffer (the wrapper holds a VMA handle, not the Vulkan one)
        VkBuffer buffer_of(vma_allocator& allocator, vk_buffer const& buffer) noexcept {
            auto const* const detail = buffer.valid() ? allocator.get_buffer_detail(buffer.handle()) : nullptr;
            return detail != nullptr ? detail->buffer : VK_NULL_HANDLE;
        }

        VkDeviceAddress address_of(vma_allocator& allocator, VkDevice device, vk_buffer const& buffer) noexcept {
            VkBuffer const handle = buffer_of(allocator, buffer);
            if (handle == VK_NULL_HANDLE) {
                return 0;
            }
            VkBufferDeviceAddressInfo const info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = handle};
            return vkGetBufferDeviceAddress(device, &info);
        }
    } // namespace

    descriptor_heap::~descriptor_heap() {
        // The buffers are RAII, so there is nothing to free here - but the class is non-copyable and non-movable
        // on purpose (a heap is device state with an address other structures encode), and this definition is
        // where that intent is stated.
    }

    bool descriptor_heap::init(vma_allocator& allocator, VkDevice const device, heap_limits const& limits) noexcept {
        if (device == VK_NULL_HANDLE || limits.max_resource_size == 0 || limits.max_sampler_size == 0) {
            return false; // no device, or a device that published no heap limits: nothing to lay out
        }
        this->write_resource_descriptors = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(vkGetDeviceProcAddr(device, "vkWriteResourceDescriptorsEXT"));
        this->write_sampler_descriptors = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(vkGetDeviceProcAddr(device, "vkWriteSamplerDescriptorsEXT"));
        // Resolved but NOT required for init: a heap that cannot push data is still a usable heap for descriptors
        // read from a fixed offset, so this does not decide whether the heap exists - push_data() refuses instead.
        this->cmd_push_data = reinterpret_cast<PFN_vkCmdPushDataEXT>(vkGetDeviceProcAddr(device, "vkCmdPushDataEXT"));
        this->bind_resource_heap = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(vkGetDeviceProcAddr(device, "vkCmdBindResourceHeapEXT"));
        this->bind_sampler_heap = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(vkGetDeviceProcAddr(device, "vkCmdBindSamplerHeapEXT"));
        if (this->write_resource_descriptors == nullptr || this->write_sampler_descriptors == nullptr || this->bind_resource_heap == nullptr || this->bind_sampler_heap == nullptr) {
            // The extension entry points come from the device rather than from the link line, the same rule the
            // acceleration-structure module follows: vulkan-1's import library exports no extension command.
            deren::utility::log("descriptor heap: the device did not publish the heap entry points; the heap is the only binding model this renderer has, so it cannot render without it");
            return false;
        }
        this->configured_limits = limits;
        this->logical_device = device;

        // BOTH HEAPS ARE HOST_VISIBLE, and that is the model rather than a shortcut: a descriptor is written by
        // the application (vkWriteResourceDescriptorsEXT writes through a host range) and read by the device.
        // DESCRIPTOR_HEAP_BIT_EXT is what makes the buffer a legal heap at all, and the device address is what a
        // mapping's heapOffset is relative to.
        VkBufferUsageFlags const heap_usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        this->resource_heap_size = limits.max_resource_size < resource_working_size ? limits.max_resource_size : resource_working_size;
        this->sampler_heap_size = limits.max_sampler_size < sampler_working_size ? limits.max_sampler_size : sampler_working_size;
        // ZERO-FILLED CONTENTS, for the ordinary reason that a heap holds no descriptors until one is written.
        // (The crash that led to this line was NOT the data pointer: it was ORDERING. A heap is two buffers from
        // the allocator, and core creates it after vma.init() for that reason - create_buffer before the
        // allocator exists is an access violation with no log line, no validation message and no allocation
        // error, which is why this took so long to find. See vulkan/core/core.cpp.)
        std::vector<uint8_t> const zeroed_resource(static_cast<std::size_t>(this->resource_heap_size), 0u);
        std::vector<uint8_t> const zeroed_sampler(static_cast<std::size_t>(this->sampler_heap_size), 0u);
        this->resource_heap = allocator.create_buffer(zeroed_resource.data(), zeroed_resource.size(), buffer_type::storage_coherent, heap_usage);
        this->sampler_heap = allocator.create_buffer(zeroed_sampler.data(), zeroed_sampler.size(), buffer_type::storage_coherent, heap_usage);
        if (!this->resource_heap.valid() || !this->sampler_heap.valid()) {
            deren::utility::log("descriptor heap: the heap allocations failed; the heap is the only binding model this renderer has, so it cannot render without it");
            this->destroy();
            return false;
        }
        this->resource_heap_address = address_of(allocator, logical_device, this->resource_heap);
        // THE MAPPED POINTER, which is NOT the address above: a descriptor is written through a HOST address
        // (VkHostAddressRangeEXT), so this is what write_descriptors needs. A heap that is not mapped cannot be
        // written by the host at all, so a missing mapping disables the heap instead of crashing on first write.
        auto const* const resource_detail = this->resource_heap.valid() ? allocator.get_buffer_detail(this->resource_heap.handle()) : nullptr;
        this->resource_mapped = resource_detail != nullptr ? resource_detail->allocation_info.pMappedData : nullptr;
        // ... and the sampler heap's, for the same reason: vkWriteSamplerDescriptorsEXT also takes a HOST range.
        // A null here is not fatal at this point - write_samplers() refuses - which keeps this a one-line mirror
        // of the resource side rather than a second failure path.
        auto const* const sampler_detail = this->sampler_heap.valid() ? allocator.get_buffer_detail(this->sampler_heap.handle()) : nullptr;
        this->sampler_mapped = sampler_detail != nullptr ? sampler_detail->allocation_info.pMappedData : nullptr;
        if (this->resource_mapped == nullptr) {
            deren::utility::log("descriptor heap: the resource heap is not mapped; the heap is the only binding model this renderer has, so it cannot render without it");
            this->destroy();
            return false;
        }
        this->sampler_heap_address = address_of(allocator, logical_device, this->sampler_heap);
        if (this->resource_heap_address == 0 || this->sampler_heap_address == 0) {
            deren::utility::log("descriptor heap: the heap buffers have no device address; the heap is the only binding model this renderer has, so it cannot render without it");
            this->destroy();
            return false;
        }
        // The alignment is a property of the ADDRESS, not of the buffer, exactly as the micromap's data address
        // was: a heap binding whose address is not a multiple of resourceHeapAlignment is invalid. VMA's
        // device-side allocations are normally 256-byte aligned, which satisfies both numbers here (64 and 32),
        // so this is a check rather than a fixup - and a failed check disables the heap instead of binding it.
        if (limits.resource_alignment != 0 && (this->resource_heap_address % limits.resource_alignment) != 0) {
            deren::utility::log("descriptor heap: the resource heap address {} is not a multiple of the required alignment {}", this->resource_heap_address, limits.resource_alignment);
            this->destroy();
            return false;
        }
        if (limits.sampler_alignment != 0 && (this->sampler_heap_address % limits.sampler_alignment) != 0) {
            deren::utility::log("descriptor heap: the sampler heap address {} is not a multiple of the required alignment {}", this->sampler_heap_address, limits.sampler_alignment);
            this->destroy();
            return false;
        }

        deren::utility::log("SUCCESS: descriptor heap created (resource {} KiB at 0x{:x}, sampler {} KiB at 0x{:x}; strides buffer {} B, image {} B, sampler {} B)",
                            this->resource_heap_size / 1024,
                            this->resource_heap_address,
                            this->sampler_heap_size / 1024,
                            this->sampler_heap_address,
                            limits.buffer_descriptor_size,
                            limits.image_descriptor_size,
                            limits.sampler_descriptor_size);
        return true;
    }

    void descriptor_heap::destroy() noexcept {
        this->resource_heap = {};
        this->sampler_heap = {};
        this->resource_heap_address = 0;
        this->sampler_heap_address = 0;
        this->resource_heap_size = 0;
        this->sampler_heap_size = 0;
        this->next_free = 0;
    }

    bool descriptor_heap::write_descriptors(VkDeviceSize const descriptors_offset, std::span<VkResourceDescriptorInfoEXT const> const infos) noexcept {
        if (!this->ready() || infos.empty() || descriptors_offset >= this->resource_heap_size) {
            return false;
        }
        // The host range is the heap's own memory at the offset the descriptors go to: the call writes
        // descriptorCount descriptors of the declared types there, which is why the destination must be big
        // enough for the sum of their strides.
        VkDeviceSize needed = 0;
        for (VkResourceDescriptorInfoEXT const& info : infos) {
            needed += this->descriptor_stride(info.type);
        }
        if (descriptors_offset + needed > this->resource_heap_size) {
            deren::utility::log("descriptor heap: a write of {} descriptors ({} B) at offset {} does not fit the {} B resource heap", infos.size(), needed, descriptors_offset, this->resource_heap_size);
            return false;
        }
        // THE HOST RANGE IS A HOST POINTER, i.e. the heap's MAPPED memory at the offset the descriptors go to - not
        // the heap's device address re-cast as a pointer, which is what this did first and which crashes the
        // process as soon as a driver accepts the descriptor and writes through it.
        VkHostAddressRangeEXT host_range = {};
        host_range.address = static_cast<uint8_t*>(this->resource_mapped) + descriptors_offset;
        host_range.size = static_cast<std::size_t>(needed);
        return this->write_resource_descriptors(this->logical_device, static_cast<uint32_t>(infos.size()), infos.data(), &host_range) == VK_SUCCESS;
    }

    void descriptor_heap::bind_infos(VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler) const noexcept {
        resource = {};
        resource.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        resource.heapRange.address = this->resource_heap_address;
        resource.heapRange.size = this->resource_heap_size;
        resource.reservedRangeOffset = 0;
        resource.reservedRangeSize = this->configured_limits.resource_reserved;

        sampler = {};
        sampler.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        sampler.heapRange.address = this->sampler_heap_address;
        sampler.heapRange.size = this->sampler_heap_size;
        sampler.reservedRangeOffset = 0;
        // The reserved window is the reason a combined image sampler can live in the resource heap with its
        // sampler taken from here: minSamplerHeapReservedRangeWithEmbedded is the floor for exactly that.
        sampler.reservedRangeSize = this->configured_limits.sampler_reserved_with_embedded;
    }

    void descriptor_heap::record_bind(VkCommandBuffer const command_buffer) const noexcept {
        if (!this->ready() || command_buffer == VK_NULL_HANDLE) {
            return;
        }
        // The same two bind infos a secondary inherits (see bind_infos): one definition, two destinations.
        VkBindHeapInfoEXT resource_info = {};
        VkBindHeapInfoEXT sampler_info = {};
        this->bind_infos(resource_info, sampler_info);
        this->bind_resource_heap(command_buffer, &resource_info);
        this->bind_sampler_heap(command_buffer, &sampler_info);
    }

    uint32_t descriptor_heap::descriptor_stride(VkDescriptorType const type) const noexcept {
        switch (type) { // NOLINT(*-switch-missing-default-case) - the default below is what an unhandled type gets
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return this->configured_limits.image_descriptor_size;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return this->configured_limits.buffer_descriptor_size;
        default:
            return this->configured_limits.image_descriptor_size; // textures dominate this renderer's bindings
        }
    }

    bool descriptor_heap::write_image(VkDeviceSize const offset_bytes, VkImageViewCreateInfo const& view, VkImageLayout const layout, VkDescriptorType const type) noexcept {
        VkImageDescriptorInfoEXT image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT;
        image_info.pNext = nullptr;
        image_info.pView = &view; // the VIEW TO CREATE, not an existing VkImageView (see the header's note)
        image_info.layout = layout;
        VkResourceDescriptorInfoEXT const info = {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                                                  .pNext = nullptr,
                                                  .type = type,
                                                  .data = {.pImage = &image_info}};
        return this->write_descriptors(offset_bytes, std::span<VkResourceDescriptorInfoEXT const>(&info, 1));
    }

    bool descriptor_heap::write_buffer(VkDeviceSize const offset_bytes, VkDeviceAddress const address, VkDeviceSize const size, VkDescriptorType const type) noexcept {
        VkDeviceAddressRangeEXT const range = {.address = address, .size = size};
        VkResourceDescriptorInfoEXT const info = {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                                                  .pNext = nullptr,
                                                  .type = type,
                                                  .data = {.pAddressRange = &range}};
        return this->write_descriptors(offset_bytes, std::span<VkResourceDescriptorInfoEXT const>(&info, 1));
    }

    bool descriptor_heap::write_samplers(VkDeviceSize const descriptors_offset, std::span<VkSamplerCreateInfo const> const samplers) noexcept {
        if (!this->ready() || this->write_sampler_descriptors == nullptr || this->sampler_mapped == nullptr || samplers.empty()) {
            return false;
        }
        VkDeviceSize const stride = this->configured_limits.sampler_descriptor_size != 0 ? this->configured_limits.sampler_descriptor_size : 1u;
        VkDeviceSize const bytes = stride * samplers.size();
        if (descriptors_offset + bytes > this->sampler_heap_size) {
            deren::utility::log("descriptor heap: a write of {} sampler descriptors ({} B at {}) does not fit the {} B sampler heap", samplers.size(), bytes, descriptors_offset, this->sampler_heap_size);
            return false;
        }
        // A HOST ADDRESS, not the heap's device address: vkWriteSamplerDescriptorsEXT takes the same
        // VkHostAddressRangeEXT a resource write does, and writing through the device address is the mistake that
        // crashes the process the moment a driver dereferences it (see write_descriptors).
        VkHostAddressRangeEXT const range = {
            .address = static_cast<uint8_t*>(this->sampler_mapped) + descriptors_offset,
            .size = bytes,
        };
        return this->write_sampler_descriptors(this->logical_device, static_cast<uint32_t>(samplers.size()), samplers.data(), &range) == VK_SUCCESS;
    }

    bool descriptor_heap::push_data(VkCommandBuffer const command_buffer, uint32_t const offset, std::span<std::byte const> const data) const noexcept {
        if (!this->ready() || this->cmd_push_data == nullptr || command_buffer == VK_NULL_HANDLE || data.empty()) {
            return false;
        }
        if (static_cast<VkDeviceSize>(offset) + data.size() > this->configured_limits.max_push_data) {
            deren::utility::log("descriptor heap: a push of {} B at offset {} exceeds the {} B push-data window", data.size(), offset, this->configured_limits.max_push_data);
            return false;
        }
        VkPushDataInfoEXT const info = {
            .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
            .pNext = nullptr,
            .offset = offset,
            // A HOST address range, like every other write in this extension (see write_descriptors).
            .data = {.address = const_cast<std::byte*>(data.data()), .size = data.size()},
        };
        this->cmd_push_data(command_buffer, &info);
        return true;
    }

    VkDeviceSize descriptor_heap::reserve(uint32_t const count, VkDescriptorType const type) noexcept {
        if (!this->ready() || count == 0) {
            return VK_WHOLE_SIZE;
        }
        VkDeviceSize const stride = this->descriptor_stride(type);
        VkDeviceSize const offset = this->next_free != 0 ? this->next_free : this->usable_offset();
        VkDeviceSize const end = offset + stride * count;
        if (end > this->resource_heap_size) {
            deren::utility::log("descriptor heap: a reservation of {} descriptors ({} B) does not fit the {} B resource heap", count, stride * count, this->resource_heap_size);
            return VK_WHOLE_SIZE;
        }
        this->next_free = end;
        return offset;
    }

    VkDeviceSize descriptor_heap::reserve_bytes(VkDeviceSize const bytes, VkDeviceSize const alignment) noexcept {
        if (!this->ready() || bytes == 0) {
            return VK_WHOLE_SIZE;
        }
        VkDeviceSize const step = alignment != 0 ? alignment : 1u;
        VkDeviceSize const cursor = this->next_free != 0 ? this->next_free : this->usable_offset();
        VkDeviceSize const offset = ((cursor + step - 1u) / step) * step;
        if (offset + bytes > this->resource_heap_size) {
            deren::utility::log("descriptor heap: a reservation of {} B does not fit the {} B resource heap", bytes, this->resource_heap_size);
            return VK_WHOLE_SIZE;
        }
        this->next_free = offset + bytes;
        return offset;
    }
} // namespace deren::vulkan