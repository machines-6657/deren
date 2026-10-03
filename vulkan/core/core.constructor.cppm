// ============================================================================
// module: deren.vulkan.core:constructor  - the IMPLEMENTATION PARTITION that builds a core
//
// What is here is the initialisation run that was already one cohesive block in core.cpp: the two
// constructors, the window/instance/surface/device/swapchain/image-view steps, the depth and render
// target creation, the command pool, the sync objects and the timestamp query pool. What is NOT here
// is anything that runs per frame (begin_gpu_timing and its neighbours stayed in core.cpp), the heap
// layout, the pipelines or the queries - those are not construction.
//
// THE TWO IMPORTS ON THIS LINE AND THE glm INCLUDE ARE NOT DECORATION. A partition does NOT see the
// primary interface, and imports are NOT transitive: the declarations come from `import :declarations;`, and
// `utility` (whose log() this code calls) is imported by :declarations NON-exported, so it has to be imported
// again here. The glm include is the one core.cpp explains at length: without it this translation unit
// sees TWO operator new(size_t, align_val_t) declarations under -fno-exceptions with the vendored std
// module and resolves NEITHER.
// ============================================================================
module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

// LOAD-BEARING, and it is the same trap chores.cpp documents at length: with -fno-exceptions and the vendored
// std module, a TU that instantiates std::vector sees TWO 'operator new(size_t, align_val_t)' declarations -
// module std's and the textual libc++ copy baked into utility:data_block.pcm - and resolves neither, which is
// "call to operator new is ambiguous" at allocate.h. This file instantiates plenty of std::vector (the device
// extension-name list, the descriptor pool sizes), and it began seeing both the moment deren.vulkan.core gained an
// import edge it did not have before: descriptor_heap, whose own module carries a textual Vulkan header in its
// global module fragment. Textually including glm here makes clang MERGE the two copies, exactly as it does for
// chores.cpp and vulkan/animation/controller.cpp. Do not remove this include to "clean up".
#include <glm/glm.hpp>

module deren.vulkan.core:constructor;

import :declarations;
// THE CONTRACT'S CREATION STRUCTURE IS IMPORTED HERE, NOT VIA :declarations: a partition does not see
// the primary interface and imports are NOT transitive (see the banner above), so the delegating
// constructor below - whose parameter is deren::promise::rhi::create_info - needs this edge.
import deren.promise.rhi;
import deren.utility;
import deren.vulkan.core.pipeline;
import :init_utils;
import deren.vulkan.constant_init;

// ============================================================================
// THE ABI GUARD, AND WHY THERE IS NO LONGER A TRANSLATION HERE.
//
// `deren::promise::rhi::create_info` is the contract's ONE creation structure and the backend's
// constructor takes it directly (plan_rhi_v4.md §4.1 item 3: the renderer's creation parameters
// cross as a POD structure). It used to be two structures plus a `to_backend_create_info()` mapping
// between them - the portable shape and this backend's own `core_create_info` - and that pair is
// gone: one structure means one place to add the next field, and the initialisation run below reads
// the boundary's fields directly.
//
// WHAT REMAINS IS THE GUARD, NOT A MAPPING. `struct_size` is the structure's ABI guard
// (rhi.core_desc.cppm's "EVOLUTION" note: fields are only ever APPENDED, never reordered), so a
// value that differs from this build's size means the caller compiled a different shape. The
// conservative answer the guard exists for is taken and logged instead of assumed: the caller's
// structure is treated as TOO SHORT, and a field is copied only when its whole extent lies inside
// the bytes the caller declares. A shorter structure is an older caller; a longer one is a newer
// caller whose appended tail this backend has never heard of - either way only the common prefix is
// read and everything else keeps THIS build's default.
// ============================================================================
namespace {

    // THE APPEND-ONLY GUARD ITSELF LIVES IN core.declarations.cppm (`covered_by`) because the buffer
    // descriptor is read through the same rule in core.api_core.cpp: one implementation of the
    // arithmetic, two callers.

    /// the ABI guard applied to the contract's own structure: a default-initialised copy that takes a
    /// field only when the caller's declared size covers it
    deren::promise::rhi::create_info sanitize_create_info(deren::promise::rhi::create_info const& desc) {
        using rhi_create_info = deren::promise::rhi::create_info;

        uint32_t const declared = desc.struct_size;
        uint32_t const known = static_cast<uint32_t>(sizeof(rhi_create_info));
        if (declared != known) {
            deren::utility::log("core: the RHI create_info is {} B here and {} B in the caller -> the caller's structure is treated as TOO SHORT: a field whose whole extent is not inside those {} B keeps this build's default",
                                known, declared, declared);
        }

        deren::promise::rhi::create_info options = {};

        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, window_title), sizeof(rhi_create_info::window_title)) && desc.window_title != nullptr) {
            // A NULL POINTER KEEPS THE DEFAULT rather than being stored: the title is `char const*`
            // (a std::string cannot cross the boundary) and a null one is not an empty title but the
            // absence of an answer. The pointer itself is BORROWED until the constructor returns -
            // `init_window` hands it to GLFW, which copies it into the window - so nothing here keeps
            // it: `create_options` holds the pointer and the note on that member says why it is never
            // read again.
            options.window_title = desc.window_title;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, window_width), sizeof(rhi_create_info::window_width))) {
            options.window_width = desc.window_width;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, window_height), sizeof(rhi_create_info::window_height))) {
            options.window_height = desc.window_height;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, render_scale), sizeof(rhi_create_info::render_scale))) {
            options.render_scale = desc.render_scale;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, vsync), sizeof(rhi_create_info::vsync))) {
            options.vsync = desc.vsync;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, validation_layers), sizeof(rhi_create_info::validation_layers))) {
            options.validation_layers = desc.validation_layers;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, window_visible), sizeof(rhi_create_info::window_visible))) {
            options.window_visible = desc.window_visible;
        }
        if (deren::vulkan::covered_by(declared, offsetof(rhi_create_info, native_window), sizeof(rhi_create_info::native_window))) {
            // A CALLER-PROVIDED WINDOW IS BOUND, NEVER CREATED AND NEVER DESTROYED. `void*` is the
            // contract's spelling because the contract does not know what a window system is; for THIS
            // backend it is a GLFWwindow* the caller has already made Vulkan-capable. It is copied
            // through as the contract's own `void*` - the constructor below reinterprets it - and the
            // size, title and window_visible fields are IGNORED in that mode, as the field documents.
            //
            // THE CALLER ALSO OWNS GLFW'S LIFETIME IN THAT MODE: it initialised GLFW before this core was
            // constructed (the window could not exist otherwise) and it is the caller that terminates it -
            // after this core and its surface are destroyed. This backend therefore does not initialise
            // GLFW on this path and never terminates it on any path.
            options.native_window = desc.native_window;
        }

        return options;
    }

} // namespace

namespace deren::vulkan {
    // core
    core::core()
        : core(deren::promise::rhi::create_info{}) {
    }

    // THE ONE CONSTRUCTION. The contract's structure goes through the ABI guard ONCE, in the member
    // initialiser, and what the run below reads is this core's own copy of it: there is no second
    // spelling to translate into, and the initialisation run exists exactly once. The guard is a
    // member initialiser rather than a delegating constructor because the two spellings collapsed
    // into one signature - `core(create_info const&)` is the only constructor left to delegate to.
    core::core(deren::promise::rhi::create_info const& desc)
        : create_options{sanitize_create_info(desc)} {
        // THE RENDER SCALE BEFORE ANYTHING IS CREATED: `init_swap_chain` below sets `swap_chain_extent`,
        // which `render_extent()` multiplies, and `create_depth_resources` / `create_render_targets` are
        // created with the result. Clamped rather than rejected: 1.0 is the historic behaviour and the
        // value a caller writes by omission, a value above 1.0 would ask the render chain for MORE pixels
        // than are presented (a supersample this renderer's resolve does not implement), and a zero or
        // negative scale is an invalid extent rather than a small frame.
        this->render_scale = std::clamp(this->create_options.render_scale, 0.1f, 1.0f);
        if (this->render_scale != this->create_options.render_scale) {
            deren::utility::log("core: render_scale {} clamped to {} (the supported range is 0.1 .. 1.0)", this->create_options.render_scale, this->render_scale);
        }
        if (this->create_options.native_window != nullptr) {
            // caller-provided window: bind to it as-is - no glfwInit / glfwCreateWindow here and
            // no glfwDestroyWindow cleanup (ownership stays with the caller). The `void*` is the
            // backend's own reinterpretation of the contract's opaque handle.
            window = reinterpret_cast<GLFWwindow*>(this->create_options.native_window);
        } else {
            // the title is read HERE and nowhere else: it is the contract's borrowed `char const*`
            // and GLFW copies the text into the window during this call (the member note says so).
            init_window(this->create_options.window_width, this->create_options.window_height,
                        this->create_options.window_title != nullptr ? this->create_options.window_title : "");
        }
        init_instance();
        init_surface();
        init_device_and_queue();
        init_swap_chain();
        init_image_views();
        create_depth_resources();
        color_format = swap_chain_image_format;
        create_render_targets(); // the scene's render targets: the post-process pass input
        create_command_pool();
        create_samplers(); // the shared samplers a declaration picks by hint
        create_sync_objects();
        create_timestamp_query_pool(); // GPU pass timings (a no-op on devices that cannot timestamp)

        vma.init(this->instance, this->logical_device, this->physical_device, this->graphics_queue_handle, this->graphics_queue_family_index);
        this->register_cleanup([this] {
            vma.destroy();
        });

        // ---- THE DESCRIPTOR HEAP (VK_EXT_descriptor_heap), created HERE because this is the first point at which
        //      the allocator exists: the heap is two buffers, the extension asks for ONE heap for the
        //      application's lifetime (binding a new one costs a pipeline flush), and every pass reads the
        //      descriptors in it. A failure - no entry points, an allocation, an address that misses the required
        //      alignment - logs and leaves the renderer without a binding model, which is what the false return
        //      means: the heap is the only one it has, so it cannot render without it.
        if (this->descriptor_heap_limits.max_resource_size != 0) {
            if (this->descriptor_heaps.init(this->vma, this->logical_device, this->descriptor_heap_limits)) {
                // ---- THE SLOT GRID, RESERVED FIRST BECAUSE ITS BASE IS FIXED (see core.cppm's heap_slots), and
                //      that is the whole reason it comes before the blocks below: a heap-native shader bakes
                //      `array[heap_slots_x + i]`, so slot 0 has to land at 1 MiB on every device. The cursor
                //      starts at the implementation's reserved window (94 KiB here), so the distance up to 1 MiB
                //      is reserved first - alignment-1 bookkeeping inside this allocator, not an API constraint -
                //      and the grid is then reserved with the stride as its alignment, which is what a descriptor
                //      write needs.
                VkDeviceSize const grid_base_bytes = static_cast<VkDeviceSize>(heap_slot_base) * heap_slot_stride;
                VkDeviceSize const grid_bytes = static_cast<VkDeviceSize>(heap_slot_count) * heap_slot_stride;
                // The RESOURCE grid needs the 64 B stride; the SAMPLER grid is a separate heap with its own fixed
                // base (64 KiB - the API caps that heap at 128 KiB, so it cannot use the resource grid's 1 MiB)
                // and the device's own sampler stride. Both are CHECKED rather than assumed, because a device
                // that missed either would make the shaders' baked base indices address the wrong descriptors -
                // and that is silent in validation, which is why the heap path is refused instead.
                bool const descriptors_fit = this->descriptor_heaps.limits().buffer_descriptor_size <= heap_slot_stride &&
                                             this->descriptor_heaps.limits().image_descriptor_size <= heap_slot_stride &&
                                             this->descriptor_heaps.limits().sampler_descriptor_size <= heap_sampler_stride &&
                                             this->descriptor_heaps.limits().sampler_reserved_with_embedded <= static_cast<VkDeviceSize>(heap_sampler_base) * heap_sampler_stride;
                if (descriptors_fit) {
                    // ALIGN_UP IS THE WHOLE MECHANISM: the cursor starts at the implementation's reserved window
                    // (94 KiB on this device, logged here), and reserving the grid with 1 MiB as its ALIGNMENT
                    // puts slot 0 at 1 MiB in one step. The first version reserved the distance up to 1 MiB and
                    // then the grid; it landed at the window instead, which is the measured reason this is one
                    // call rather than two - a filler reservation is a second thing that can silently not happen.
                    this->heap_grid_offset = this->descriptor_heaps.reserve_bytes(grid_bytes, grid_base_bytes);
                    deren::utility::log("descriptor heap: the reserved window ends at {} B, so the grid's 1 MiB base is {} B away", this->descriptor_heaps.usable_offset(), grid_base_bytes);
                }
                if (descriptors_fit && this->heap_grid_offset == grid_base_bytes) {
                    deren::utility::log("descriptor heap: slot grid at {} ({} slots x {} B; textures {} materials {} tlas {} camera {} light {} clusters {}/{} gbuffer {} env {} lut {})",
                                        this->heap_grid_offset,
                                        heap_slot_count,
                                        heap_slot_stride,
                                        heap_slots::textures - heap_slot_base,
                                        heap_slots::materials - heap_slot_base,
                                        heap_slots::tlas - heap_slot_base,
                                        heap_slots::scene_camera - heap_slot_base,
                                        heap_slots::scene_light - heap_slot_base,
                                        heap_slots::cluster_counts - heap_slot_base,
                                        heap_slots::cluster_indices - heap_slot_base,
                                        heap_slots::gbuffer_albedo - heap_slot_base,
                                        heap_slots::env_cube - heap_slot_base,
                                        heap_slots::brdf_lut - heap_slot_base);
                } else {
                    deren::utility::log("descriptor heap: NO slot grid (base {} B, expected {} B, descriptors {} {} {} B against the {} B stride), so the heap stays unused",
                                        this->heap_grid_offset,
                                        grid_base_bytes,
                                        this->descriptor_heaps.limits().buffer_descriptor_size,
                                        this->descriptor_heaps.limits().image_descriptor_size,
                                        this->descriptor_heaps.limits().sampler_descriptor_size,
                                        static_cast<uint64_t>(heap_slot_stride));
                    this->heap_grid_offset = VK_WHOLE_SIZE;
                }
                // THE LAYOUT, reserved once and published (see core.cppm): the runtime WRITES the contents, the
                // pipeline builders build the mappings, and both read these two numbers rather than computing their
                // own - the one way this design has of being wrong without anything saying so.
                this->heap_texture_array_offset = this->descriptor_heaps.reserve(scene_texture_capacity, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
                this->heap_material_table_offset = this->descriptor_heaps.reserve(1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                deren::utility::log("descriptor heap: layout reserved (texture array at {}, material table at {})", this->heap_texture_array_offset, this->heap_material_table_offset);

                // THE SAMPLERS COME LAST BECAUSE THE HEAP DID NOT EXIST WHEN THEY WERE MADE: create_samplers()
                // ran earlier in this constructor and kept the create infos (core.cppm's shared_sampler_infos),
                // and a heap sampler descriptor IS such a create info - the driver creates the sampler inside the
                // heap, exactly as it creates a view inside a heap image descriptor. Six of them, on the SAMPLER
                // heap's own grid, in the order shaders/heap_slots.glsl names them.
                if (descriptors_fit) {
                    VkDeviceSize const sampler_grid = static_cast<VkDeviceSize>(heap_sampler_base) * heap_sampler_stride;
                    uint32_t written = 0;
                    for (uint32_t index = 0; index < this->shared_sampler_infos.size(); ++index) {
                        VkSamplerCreateInfo const& info = this->shared_sampler_infos[index];
                        if (info.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO) {
                            continue; // a sampler that never got created is not written, and validation would say so
                        }
                        if (this->descriptor_heaps.write_samplers(sampler_grid + index * heap_sampler_stride, std::span<VkSamplerCreateInfo const>(&info, 1))) {
                            ++written;
                        }
                    }
                    deren::utility::log("descriptor heap: {} shared samplers written to the sampler grid at {}", written, sampler_grid);
                }
            } else {
                deren::utility::log("descriptor heap: not created; the heap is the only binding model this renderer has, so it cannot render without it");
            }
        }

        // ---- S2 BATCH 2: THE CONTRACT'S VIEWS, THE FRAME'S COMMAND BUFFERS, THE READ-BACK RELEASE --
        //
        // THE FRAME'S PRIMARY COMMAND BUFFERS ARE ALLOCATED HERE NOW (they moved out of the runtime:
        // see core.declarations.cppm). The SHAPE is unchanged - one primary per frame slot, allocated
        // once and reused every frame - and the runtime still begins, ends and submits them; what
        // changed is who owns the RAII objects, which is what lets `begin_commands()` hand out the
        // frame's list.
        this->commands_view.owner = this;
        this->frame_image_view.owner = this;
        this->readback_slot_view.owner = this;
        this->escape_view.owner = this;
        this->address_view.owner = this;
        this->frame_command_buffers.reserve(static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT));
        for (int32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
            this->frame_command_buffers.push_back(this->make_command_buffer());
        }
        // THE VIEWS THAT OWN DEVICE MEMORY HAVE TO BE EMPTIED BEFORE THE DEVICE GOES AWAY, and that is
        // what this cleanup is for: `vk_command_buffer` frees itself through the device and its pool,
        // and `vk_buffer` through VMA's allocator - do_cleanup() destroys both from the destructor
        // BODY, i.e. before any member destructor runs. do_cleanup() is LIFO and this is the LAST
        // cleanup registered in this constructor, so it runs FIRST.
        this->register_cleanup([this] {
            this->frame_command_buffers.clear();
            this->readback_slot_buffer.reset();
            this->readback_slot_handle = VK_NULL_HANDLE;
            this->readback_slot_mapped = nullptr;
            this->readback_slot_size = 0;
        });
        // ---- THE ABILITY INVARIANT, CHECKED AT STARTUP (batch-1 spec §4.3, gate G2) -----------------
        // `abilities()` may only report a bit whose `query_extension()` answers with an object of that
        // kind: "reported but not retrievable" and "retrievable but not reported" are both backend bugs,
        // and both are NAMED startup failures here rather than log lines - a broken invariant is not a
        // missing device feature. THIS BACKEND NOW ANNOUNCES `vulkan_escape`, so the loop runs on a
        // NON-EMPTY set for the first time and `query_extension(vulkan_escape)` must answer with the
        // escape object (it does: core.api_core.cpp) - a bit without its object refuses to start.
        // The test half of the same gate is check_core_contract() in tests/test_dynamic_link.cpp.
        for (deren::promise::rhi::extension_kind const kind : deren::promise::rhi::all_extension_kinds()) {
            bool const announced = deren::promise::rhi::has_ability(this->abilities(), kind);
            deren::promise::rhi::extension* const ability = this->query_extension(kind);
            bool const serves = ability != nullptr && ability->kind() == kind;
            if (announced != serves) {
                deren::utility::log("rhi: abilities() and query_extension() disagree about extension_kind {} (announced {}, answers {})",
                                    static_cast<std::uint32_t>(kind),
                                    announced,
                                    serves);
                deren::utility::panic("rhi: abilities()/query_extension() are inconsistent");
            }
        }
    };

    core::~core() {
        vkDeviceWaitIdle(this->logical_device);
        this->do_cleanup();
    }

    // self-owned window path: only taken when create_options.native_window is null (a caller-provided
    // window skips glfwInit/glfwCreateWindow entirely and registers no destroy cleanup).
    //
    // GLFW'S LIFETIME BELONGS TO WHOEVER INITIALISED IT. An application that owns its window (main.cpp)
    // calls glfwInit before this core exists and glfwTerminate after it is gone, so this path is not
    // reached there - and when it IS reached (no caller window) the core initialises GLFW here and never
    // terminates it. glfwInit is idempotent: a caller that already initialised GLFW makes this a no-op.
    void core::init_window(int32_t const width, int32_t const height, std::string_view const window_name) noexcept {
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE); // resizing recreates the swapchain via render_frame's OUT_OF_DATE handling
        // ---- AND WHETHER ANYONE IS EVER SHOWN THIS WINDOW, which is a creation-time answer: GLFW has no
        //      "create it and never show it" afterwards that does not flash. A scripted capture passes
        //      window_visible = false, and the reason is not cosmetic: `--capture-frames` is run by the
        //      gate (20 launches) and by the A/B batches (dozens more), with nobody watching, and the
        //      hidden-console STARTUPINFO the capture scripts use is a CONSOLE mechanism - a GUI process
        //      ignores it, which is why the flashing survived it. What the window shows cannot matter to
        //      the result: the screenshot is a vkCmdCopyImageToBuffer read-back of the SWAPCHAIN IMAGE
        //      (the read-back copy recorded inside the frame - runtime.frames.cppm's end_recording), i.e.
        //      of the render target, not of the window, so no
        //      presented-window state can change a byte of it - measured, not asserted: the gate's ten
        //      references and the four recorded A/B anchors are byte-identical either way.
        //      GLFW_VISIBLE before glfwCreateWindow is the whole mechanism; GLFW's default is TRUE.
        glfwWindowHint(GLFW_VISIBLE, this->create_options.window_visible ? GLFW_TRUE : GLFW_FALSE);

        window = glfwCreateWindow(
            width,
            height,
            window_name.empty() ? "vulkan" : window_name.data(),
            nullptr,
            nullptr);

        if (window != nullptr) {
            // ---- BELT AND BRACES, and each is needed for its own reason: `glfwShowWindow` is called for
            //      the visible case because a platform is allowed to ignore the hint as well, and
            //      `glfwHideWindow` for the hidden case for the same one from the other side. Calling
            //      them unconditionally makes the window's visibility a property of this flag on every
            //      platform rather than of the platform's hint handling. Both are idempotent.
            if (this->create_options.window_visible) {
                glfwShowWindow(window);
            } else {
                glfwHideWindow(window);
            }
        }

        register_cleanup([this] {
            if (window) {
                glfwDestroyWindow(window);
            }
        });
    }

    void core::init_instance() noexcept {
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "deren"; // must agree with project() in CMakeLists.txt and the window title
        // version injected by CMake (project(VERSION) in CMakeLists.txt) - the single source
        app_info.applicationVersion = VK_MAKE_VERSION(DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);
        app_info.pEngineName = "";
        app_info.engineVersion = VK_MAKE_VERSION(DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        // get and set GLFW required extensions
        uint32_t glfw_extension_count = 0;
        char const** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        std::vector<char const*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);

        // validation layers + debug messenger: runtime switch from create_options (app_config's
        // [render] validation_layers; Debug defaults on, Release off - both overridable)
        bool const enable_validation = this->create_options.validation_layers;

        if (enable_validation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            deren::utility::log("add debug extension: {}", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        // print all extensions
        deren::utility::log("required instance extension ({}):", extensions.size());
        for (auto const& ext : extensions) {
            deren::utility::log("  - {}", ext);
        }

        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        // The escape's guard rail: the names this INSTANCE enabled, in this core's own storage. Taken
        // from the very list that went into VkInstanceCreateInfo, so "enabled" cannot drift from what
        // the escape reports, and the strings are literals/`VK_*_EXTENSION_NAME` - valid for the
        // context's lifetime.
        this->instance_extension_names = extensions;

        // enable validation_layers
        std::vector<char const*> validation_layers;

        if (enable_validation) {
            validation_layers = {
                "VK_LAYER_KHRONOS_validation",
            };

            // Check validation layer support
            if (check_validation_layer_support(validation_layers)) {
                create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
                create_info.ppEnabledLayerNames = validation_layers.data();
                deren::utility::log("validation layers enabled ( {} )", validation_layers.size());
                for (auto const& layer : validation_layers) {
                    deren::utility::log("  - {}", layer);
                }
            } else {
                deren::utility::log("warning: VK_LAYER_KHRONOS_validation disabled");
                create_info.enabledLayerCount = 0;
            }
        } else {
            deren::utility::log("validation layers disabled (config [render] validation_layers = false)");
            create_info.enabledLayerCount = 0;
        }

        if (VkResult result = vkCreateInstance(&create_info, nullptr, &this->instance); result != VK_SUCCESS) {
            // Provide more detailed error info
            std::string error_msg = "can not create vulkan instance, error code is: ";
            switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                error_msg += "VK_ERROR_OUT_OF_HOST_MEMORY";
                break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                error_msg += "VK_ERROR_OUT_OF_DEVICE_MEMORY";
                break;
            case VK_ERROR_INITIALIZATION_FAILED:
                error_msg += "VK_ERROR_INITIALIZATION_FAILED";
                break;
            case VK_ERROR_LAYER_NOT_PRESENT:
                error_msg += "VK_ERROR_LAYER_NOT_PRESENT";
                break;
            case VK_ERROR_EXTENSION_NOT_PRESENT:
                error_msg += "VK_ERROR_EXTENSION_NOT_PRESENT";
                break;
            case VK_ERROR_INCOMPATIBLE_DRIVER:
                error_msg += "VK_ERROR_INCOMPATIBLE_DRIVER";
                break;
            default:
                error_msg += std::to_string(static_cast<int32_t>(result));
                break;
            }
            deren::utility::panic(error_msg);
        }
        deren::utility::log("instance init succeeded");
        deren::utility::log("instance handler is 0x{:x}", reinterpret_cast<uint64_t>(this->instance));

        // register instance destruction first, then the messenger cleanup INSIDE the
        // validation branch below: cleanup runs LIFO, so the messenger (when created) is
        // destroyed before the instance it belongs to
        this->register_cleanup([this] {
            vkDestroyInstance(this->instance, nullptr);
        });

        if (enable_validation) {
            // get function pointer
            auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
                instance, "vkCreateDebugUtilsMessengerEXT"));
            auto vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
                instance, "vkDestroyDebugUtilsMessengerEXT"));

            if ((vkCreateDebugUtilsMessengerEXT == nullptr) || (vkDestroyDebugUtilsMessengerEXT == nullptr)) {
                deren::utility::panic("Failed to get debug utils function pointers");
            }
            VkDebugUtilsMessengerCreateInfoEXT debug_info = {};
            debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_info.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_info.pfnUserCallback = debug_callback;
            debug_info.pUserData = nullptr;

            if (vkCreateDebugUtilsMessengerEXT(instance, &debug_info, nullptr, &debug_messenger) != VK_SUCCESS) {
                deren::utility::error("create debug messenger failed");
            } else {
                deren::utility::log("create debug messenger succeeded");
            }
            this->register_cleanup([vkDestroyDebugUtilsMessengerEXT, this] {
                vkDestroyDebugUtilsMessengerEXT(this->instance, this->debug_messenger, nullptr);
            });
        }
    }

    void core::init_surface() noexcept {
        if (glfwCreateWindowSurface(this->instance, this->window, nullptr, &this->surface) != VK_SUCCESS) {
            deren::utility::panic("can not init surface");
        }

        this->register_cleanup([this] {
            vkDestroySurfaceKHR(this->instance, this->surface, nullptr);
        });
    }

    void core::init_device_and_queue() noexcept {
        this->physical_device = pick_suitable_device(this->instance, this->surface);

        // Collect device capabilities: one query through the feature/property pNext chains (see device_capabilities)
        device_capabilities capabilities;
        capabilities.query(this->physical_device);
        print_device_capabilities(capabilities);
        // keep the plain properties around (limits such as timestampPeriod drive renderer
        // decisions like the GPU pass timings) - the capabilities query already fetched them
        this->device_properties = capabilities.properties_2.properties;

        // 与选卡共用检查，尤其保证 descriptor heap 的着色器依赖也可用。
        auto const missing = capabilities.renderer_missing_requirements();
        if (!missing.empty()) {
            for (char const* requirement : missing) {
                deren::utility::error("Selected GPU is missing required capability: {}", requirement);
            }
            deren::utility::panic("Selected GPU no longer satisfies renderer requirements");
        }

        device_creation_info creation_info;

        creation_info.queue_families = find_queue_families(this->physical_device, this->surface);

        // Spelled as a string rather than the header macro so the build does not depend on how new the
        // Vulkan headers are; it is only ever pushed when the device advertises it.
        constexpr char const* fifo_latest_ready_extension = "VK_EXT_present_mode_fifo_latest_ready";

        creation_info.extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        // VK_EXT_present_mode_fifo_latest_ready is asked for only when the device has it, so the
        // extension is never enabled blindly and the present mode that needs it is only ever chosen
        // from the surface's own list (see choose_swap_present_mode).
        if (check_device_extension_support(physical_device, std::vector<char const*>{fifo_latest_ready_extension})) {
            creation_info.extensions.push_back(fifo_latest_ready_extension);
        }

        // Ray tracing is optional and opt-in per feature, not per build: the extensions are enabled
        // when the device has them, and the two feature structs are already in the pNext chain from
        // the capabilities query - query() leaves them out when either feature is missing, so
        // "enabled" and "available" cannot disagree. A device without them runs the raster paths
        // exactly as before, with ray_query_available false and the RT features refusing to turn on.
        constexpr std::array<char const*, 3> ray_tracing_extensions = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, // required by the AS extension itself
        };
        if (capabilities.ray_query_available) {
            for (char const* extension : ray_tracing_extensions) {
                creation_info.extensions.push_back(extension);
            }
        }
        // The RT pipeline and its maintenance1 features, then the micromap: each name is pushed only when
        // the capability query proved BOTH the extension and its feature, so an enabled extension and an
        // available feature cannot disagree (the rule the ray-query block above follows).
        constexpr std::array<char const*, 2> ray_tracing_pipeline_extensions = {
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
        };
        if (capabilities.ray_tracing_pipeline_available) {
            for (char const* extension : ray_tracing_pipeline_extensions) {
                creation_info.extensions.push_back(extension);
            }
        }
        if (capabilities.opacity_micromap_available) {
            creation_info.extensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        }
        // ... and the descriptor heap, which is independent of all of the above: it changes how BINDINGS are
        // stored (descriptors in a buffer the application writes) rather than how rays are traced. Its feature
        // struct is the first extension link in the query chain for the same reason, and it is pushed here on
        // the same rule - the capability query proved both the extension and its feature.
        if (capabilities.descriptor_heap_available) {
            // ITS DEPENDENCY FIRST, because the heap is not a legal enabled extension without one of the two
            // (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). The capability query resolved which of them
            // this device has and made the heap unavailable when it has neither, so this cannot push a name the
            // device does not support.
            creation_info.extensions.push_back(capabilities.descriptor_heap_extension_name);
            // ... and the heap's shaders' own dependency, without which no `descriptor_heap` declaration can be
            // turned into a shader module at all (see the capability layer): the extension whose SPIR-V declares
            // untyped pointers.
            creation_info.extensions.push_back(capabilities.untyped_pointers_dependency);
            creation_info.extensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        }
        // ... and mesh shaders, the other independent extension: it replaces the VERTEX stage with a MESH
        // stage (and, where the device has one, adds a TASK stage), so it neither needs nor is needed by
        // anything above. Its feature struct is the second independent link in the query chain on the same
        // rule - pushed only when the query proved both the extension and the meshShader feature, so an
        // enabled extension and an available feature cannot disagree. No dependency name is pushed with it:
        // VK_EXT_mesh_shader's dependencies (VK_KHR_spirv_1_4, VK_VERSION_1_2) are both satisfied by the 1.3
        // device this renderer creates, unlike the heap's own pair.
        if (capabilities.mesh_shader_available) {
            creation_info.extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }
        // GENERAL 布局是强制要求，上面的共用检查已确认扩展与功能均可用。
        creation_info.extensions.push_back(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);

        // 自有图像上传/读回同样要求 host image copy，GENERAL 的两个方向均已检查。
        // Swapchain 读回仍使用 copy command，因为其图像没有 HOST_TRANSFER usage。
        creation_info.extensions.push_back(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);

        if (!check_device_extension_support(physical_device, creation_info.extensions)) {
            deren::utility::panic("Required device extensions not supported");
        }

        // Features go through the pNext chain (device_capabilities query result, incl. all 1.1/1.2/1.3 supported features)
        // The escape's guard rail, device half: exactly the extension names this DEVICE was created
        // with (the list checked above), so a pass can ask "is the extension I am about to resolve an
        // entry point for actually enabled" and get the truth rather than a promise.
        this->device_extension_names = creation_info.extensions;
        creation_info.pNext = capabilities.device_pnext();

        auto const [device, graphics_family_index, present_family_index, graphics_queue, present_queue] = create_logical_device(physical_device, creation_info); // NOLINT(*-misplaced-const)

        this->logical_device = device;

        // ---- MESH SHADERS: the capability half (docs/mesh_shaders.md). The extension is enabled above only when
        //      the feature is there, so `mesh_shader_available` and "the feature struct is in the device chain" are
        //      the same fact; the dispatch command is an EXTENSION command the loader's import library does not
        //      export (`vkCmdDrawMeshTasksEXT` is an undefined symbol if called directly), which is why it is
        //      resolved here through vkGetDeviceProcAddr. Null means the extension is absent.
        this->mesh_shader_available = capabilities.mesh_shader_available;
        if (this->mesh_shader_available) {
            this->mesh_dispatch = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT"));
            // the indirect twin comes from the same extension and is resolved the same way; it answers null
            // independently, and the dispatch path treats that as "no indirect route" rather than as an error
            this->mesh_dispatch_indirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksIndirectEXT"));
        }

        // ---- HOST IMAGE COPY: the same shape as the mesh commands above (extension entry points fetched
        //      through vkGetDeviceProcAddr, because the loader's import library does not export them) - and
        //      the OPPOSITE POLICY: VK_EXT_host_image_copy is required, so a name that does not resolve is a
        //      NAMED STARTUP FAILURE rather than a quiet switch to another mechanism. BOTH directions are
        //      resolved here and checked: `vkCopyImageToMemoryEXT` (image -> host) and `vkCopyMemoryToImageEXT`
        //      (host -> image, the texture upload path), because "the extension is enabled" has to mean both
        //      are callable. See docs/host_image_copy.md.
        this->host_image_copy_available = capabilities.host_image_copy_available;
        this->copy_image_to_memory = reinterpret_cast<PFN_vkCopyImageToMemoryEXT>(vkGetDeviceProcAddr(device, "vkCopyImageToMemoryEXT"));
        this->copy_memory_to_image = reinterpret_cast<PFN_vkCopyMemoryToImageEXT>(vkGetDeviceProcAddr(device, "vkCopyMemoryToImageEXT"));
        if (this->copy_image_to_memory == nullptr) {
            deren::utility::panic("VK_EXT_host_image_copy is enabled but vkCopyImageToMemoryEXT did not resolve through vkGetDeviceProcAddr");
        }
        if (this->copy_memory_to_image == nullptr) {
            deren::utility::panic("VK_EXT_host_image_copy is enabled but vkCopyMemoryToImageEXT did not resolve through vkGetDeviceProcAddr");
        }
        // ... and the allocator gets it, because THE IMAGE UPLOAD IS THAT CALL: vma.cppm's
        // host_image_upload() copies each mip from the caller's memory into the image with it (the image
        // carries VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT for exactly that reason). Handing the resolved
        // pointer down keeps the capability check in ONE place - here, before anything is uploaded.
        this->vma.set_host_image_copy(this->copy_memory_to_image);

        // ---- THE DESCRIPTOR HEAP's LIMITS, recorded here and not created here: the heap's buffers come from the
        //      ALLOCATOR, and vma.init() runs at the END of the constructor (after every init_* step), so a
        //      create_buffer at this point is a call on an uninitialised allocator - which is an access violation
        //      with no log line, no validation message and no allocation error. The capabilities are in scope
        //      here and nowhere later, so this copies the numbers and the constructor does the creation.
        if (capabilities.descriptor_heap_available) {
            auto const& heap = capabilities.descriptor_heap_properties;
            this->descriptor_heap_limits = heap_limits{
                .max_resource_size = heap.maxResourceHeapSize,
                .max_sampler_size = heap.maxSamplerHeapSize,
                .resource_alignment = heap.resourceHeapAlignment,
                .sampler_alignment = heap.samplerHeapAlignment,
                .resource_reserved = heap.minResourceHeapReservedRange,
                .sampler_reserved_with_embedded = heap.minSamplerHeapReservedRangeWithEmbedded,
                .buffer_descriptor_size = static_cast<uint32_t>(heap.bufferDescriptorSize),
                .image_descriptor_size = static_cast<uint32_t>(heap.imageDescriptorSize),
                .sampler_descriptor_size = static_cast<uint32_t>(heap.samplerDescriptorSize),
                .max_push_data = static_cast<uint32_t>(heap.maxPushDataSize),
                .max_embedded_samplers = heap.maxDescriptorHeapEmbeddedSamplers,
            };
        }
        this->graphics_queue_handle = graphics_queue;
        this->present_queue_handle = present_queue;
        this->graphics_queue_family_index = graphics_family_index;
        this->present_queue_family_index = present_family_index;

        // What the device ended up with, for the passes that need it (see the members: the ray-traced
        // paths are skipped rather than broken on a device without them).
        this->ray_query_available = capabilities.ray_query_available;
        this->ray_tracing_pipeline_available = capabilities.ray_tracing_pipeline_available;
        this->opacity_micromap_available = capabilities.opacity_micromap_available;
        this->acceleration_structure_properties = capabilities.acceleration_structure_properties;
        this->opacity_micromap_properties = capabilities.opacity_micromap_properties;
        this->ray_tracing_pipeline_properties = capabilities.ray_tracing_pipeline_properties;

        deren::utility::log("device and queue init succeeded");

        register_cleanup([this] {
            if (this->logical_device != VK_NULL_HANDLE) {
                vkDestroyDevice(this->logical_device, nullptr);
            }
        });
    }

    void core::init_swap_chain() noexcept {
        auto const [capabilities, formats, present_modes] = query_swap_chain_support(this->physical_device, this->surface);

        // Add checks:
        if (formats.empty() || present_modes.empty()) {
            deren::utility::panic("Swap chain not adequately supported");
        }

        auto const [format, color_space] = choose_swap_surface_format(formats);
        VkPresentModeKHR const present_mode = choose_swap_present_mode(present_modes, this->create_options.vsync);
        {
            char const* name = "other";
            if (present_mode == present_mode_fifo_latest_ready) {
                name = "fifo-latest-ready";
            } else if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                name = "mailbox";
            } else if (present_mode == VK_PRESENT_MODE_FIFO_KHR) {
                name = "fifo";
            } else if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                name = "immediate";
            }
            // Logged because it decides whether the fps counter can be believed: with a vsync mode the
            // presented rate is the display's, not the renderer's.
            deren::utility::log("swapchain: present mode {} (vsync {}, {} modes offered)", name, this->create_options.vsync, present_modes.size());
        }

        // `create_options`' own window size is passed as the fallback extent: this is the size init_window
        // asked glfwCreateWindow for two calls up, and it is what choose_swap_extent uses when the surface
        // leaves the extent to the application AND the window reports no framebuffer (a hidden or not yet
        // mapped window can). See that function: the value is the caller's, not one invented there.
        VkExtent2D const extent = choose_swap_extent(capabilities, this->window, this->create_options.window_width, this->create_options.window_height);

        uint32_t image_count = capabilities.minImageCount + 1;

        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        image_count = std::max(image_count, capabilities.minImageCount);

        VkSwapchainCreateInfoKHR create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface;

        create_info.minImageCount = image_count;
        create_info.imageFormat = format;
        create_info.imageColorSpace = color_space;
        create_info.imageExtent = extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        // The F12 screenshot read-back copies FROM a swapchain image (vkCmdCopyImageToBuffer), which
        // requires the image to have been created with TRANSFER_SRC usage
        // (VUID-vkCmdCopyImageToBuffer-srcImage-00186). Ask for it when the surface supports it -
        // imageUsage has to stay a subset of the surface's supportedUsageFlags - and remember the
        // answer so the screenshot path can disable itself instead of performing an illegal copy on a
        // surface that does not.
        this->swapchain_transfer_src_supported = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        if (this->swapchain_transfer_src_supported) {
            create_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        } else {
            deren::utility::log("swapchain: surface does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT - screenshots disabled");
        }
        // ---- AND THE SURFACE FACT ABOUT HOST_TRANSFER, RECORDED AND NOT BRANCHED ON. A swapchain image's
        //      usage must be a subset of the surface's supportedUsageFlags
        //      (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276), and `vkCopyImageToMemoryEXT` needs its
        //      source image created with VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT - so where that bit is
        //      absent (every surface this renderer has been run on; the raw vulkaninfo output is in the
        //      batch report), a host copy of a swapchain image is impossible and the read-back uses the
        //      copy command (recorded inside the frame, before the present transition - see
        //      the read-back copy in runtime.frames.cppm's end_recording). That is a property of the
        //      SURFACE, not a capability the
        //      backend may require, so it selects nothing at run time: one line RECORDS which mechanism
        //      the read-back uses, and there is only one.
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT) == 0) {
            deren::utility::log("swapchain: the surface does not list VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT, so the read-back uses vkCmdCopyImageToBuffer "
                                "(the host image copy serves the images this renderer creates itself)");
        }

        queue_family_indices const indices = find_queue_families(this->physical_device, this->surface);
        if (!indices.compute_family || !indices.graphics_family || !indices.present_family) {
            deren::utility::panic("find queue family index failed");
        }

        uint32_t const queue_family_indices[] = {indices.graphics_family.value(), indices.present_family.value()};

        if (indices.graphics_family != indices.present_family) {
            create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            create_info.queueFamilyIndexCount = 2;
            create_info.pQueueFamilyIndices = queue_family_indices;
        } else {
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create_info.queueFamilyIndexCount = 0;     // Optional
            create_info.pQueueFamilyIndices = nullptr; // Optional
        }

        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = present_mode;
        create_info.clipped = VK_TRUE;
        create_info.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(logical_device, &create_info, nullptr, &this->swap_chain) != VK_SUCCESS) {
            deren::utility::panic("failed to create swap chain!");
        }

        vkGetSwapchainImagesKHR(logical_device, this->swap_chain, &image_count, nullptr);
        this->swap_chain_images.resize(image_count);
        vkGetSwapchainImagesKHR(logical_device, this->swap_chain, &image_count, this->swap_chain_images.data());

        this->swap_chain_image_format = format;
        this->swap_chain_extent = extent;

        register_cleanup([this] {
            if (swap_chain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(logical_device, swap_chain, nullptr);
                swap_chain = VK_NULL_HANDLE;
            }
        });
    }

    void core::init_image_views() noexcept {

        this->swap_chain_image_views.resize(this->swap_chain_images.size());
        for (size_t i = 0; i < this->swap_chain_images.size(); i++) {
            VkImageViewCreateInfo const create_info = make_image_view_info(this->swap_chain_images[i], swap_chain_image_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
            if (vkCreateImageView(logical_device, &create_info, nullptr, &this->swap_chain_image_views[i]) != VK_SUCCESS) {
                deren::utility::panic("failed to create image views!");
            }
        }
        register_cleanup([this] {
            for (auto const& image_view : this->swap_chain_image_views) {
                vkDestroyImageView(logical_device, image_view, nullptr);
            }
            this->swap_chain_image_views.clear();
        });
    }

    void core::create_depth_image(VkImage& image, VkDeviceMemory& image_memory, VkImageView& image_view) const noexcept {
        // Use the swapchain size stored in the class
        auto const& [width, height] = this->render_extent();

        // 1. Create images
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = this->depth_attachment_format;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(logical_device, &image_info, nullptr, &image) != VK_SUCCESS) {
            // No vkDestroyImage on this path: `image` is the caller's out-parameter and vkCreateImage
            // leaves it untouched when it fails, so destroying it here would pass a handle that was
            // never created.
            deren::utility::panic("failed to create depth image!");
        }

        // 2. Allocate memory
        VkMemoryRequirements mem_requirements;
        vkGetImageMemoryRequirements(logical_device, image, &mem_requirements);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_requirements.size;
        alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, this->physical_device);

        if (vkAllocateMemory(logical_device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
            deren::utility::panic("failed to allocate depth image memory!");
        }

        vkBindImageMemory(logical_device, image, image_memory, 0);

        // 3. Create image views
        VkImageViewCreateInfo const view_info = make_image_view_info(image, this->depth_attachment_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1);

        if (vkCreateImageView(logical_device, &view_info, nullptr, &image_view) != VK_SUCCESS) {
            deren::utility::panic("failed to create depth image view!");
        }
    }

    void core::create_depth_resources() noexcept {

        depth_attachment_format = find_depth_format(this->physical_device);

        // First test whether the depth format is valid
        if (depth_attachment_format == VK_FORMAT_UNDEFINED) {
            deren::utility::panic("can't find supported depth format");
        }

        // Single-sampled, always: the scene renders into a 1x G-buffer whose depth is its own, and
        // this main depth image is what the no-G-buffer fallback instance (and nothing else) uses.
        // There is no sample count to resolve here - the [render] msaa setting went with the forward
        // path, which was its only consumer.

        depth_images.resize(swap_chain_image_views.size());
        depth_image_views.resize(swap_chain_image_views.size());
        depth_image_memories.resize(swap_chain_image_views.size()); // make sure memory is allocated

        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            // Call create_depth_image directly, but make sure the arguments are correct
            create_depth_image(depth_images[i], depth_image_memories[i], depth_image_views[i]);
        }

        register_cleanup([this] {
            for (auto const& view : depth_image_views) {
                vkDestroyImageView(logical_device, view, nullptr);
            }
            for (auto const& image : depth_images) {
                vkDestroyImage(logical_device, image, nullptr);
            }
            for (auto const& memory : depth_image_memories) {
                vkFreeMemory(logical_device, memory, nullptr);
            }
            depth_image_views.clear();
            depth_images.clear();
            depth_image_memories.clear();
        });
    }

    void core::create_render_targets() {
        // EVERY TARGET BELOW IS CREATED AT THE RENDER EXTENT, not at the output's. With a render scale
        // below 1.0 the scene chain is deliberately smaller than the swapchain - that is the whole point of
        // `render_scale` - and the frame's last writer resolves it up to the output extent. The swapchain
        // images themselves are created in `init_swap_chain` and are untouched by this, which is what keeps
        // the presented size and the shaded size two different numbers. At the default scale the two
        // extents are equal by construction (`render_extent`), so this creates exactly what it always did.
        // The derived sizes below stay DERIVED - the half-size stochastic targets halve the render extent
        // and a bloom level is `render >> (level + 1)` - so they follow the scale instead of contradicting
        // it.
        VkExtent2D const render = this->render_extent();
        // One HDR scene target per swapchain image: the lighting stage (or the TAA resolve, when TAA
        // is on) writes it and the post-process pass samples it. TRANSFER_SRC as well, because the TAA
        // resolve copies the frame it wrote here into the history image (vkCmdCopyImage requires the
        // source to carry the usage flag).
        hdr_images.resize(swap_chain_image_views.size());
        hdr_image_memories.resize(swap_chain_image_views.size());
        hdr_image_views.resize(swap_chain_image_views.size());

        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                render.width,
                render.height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                hdr_images[i],
                hdr_image_memories[i]);

            hdr_image_views[i] = create_image_view(
                hdr_images[i],
                hdr_format,
                VK_IMAGE_ASPECT_COLOR_BIT,
                logical_device);
            // the heap's copy: this target is RENDERED into (an attachment is not a descriptor) and SAMPLED by the
            // post chain, so one sampled descriptor is what the grid needs - unlike the images a compute pass
            // writes, which need a storage descriptor beside it.
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(hdr_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::post_color + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the post HDR target for image {} did not reach grid slot {}", i, heap_slots::post_color + static_cast<uint32_t>(i));
                }
            }
        }

        // Display-referred (LDR) targets, one per swapchain image, same size and lifetime as the
        // HDR ones: the post composite renders into them when FXAA is enabled and the FXAA pass
        // samples them. Format is hdr_format (R16F) on purpose - the values are display range but
        // stored gamma-encoded so FXAA can threshold them without a per-tap decode.
        ldr_images.resize(swap_chain_image_views.size());
        ldr_image_memories.resize(swap_chain_image_views.size());
        ldr_image_views.resize(swap_chain_image_views.size());
        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                render.width,
                render.height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                ldr_images[i],
                ldr_image_memories[i]);

            ldr_image_views[i] = create_image_view(
                ldr_images[i],
                hdr_format,
                VK_IMAGE_ASPECT_COLOR_BIT,
                logical_device);
            // the heap's copy, at the array named for its reader: this is the display-referred target FXAA samples
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(ldr_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::display_color + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the display target for image {} did not reach grid slot {}", i, heap_slots::display_color + static_cast<uint32_t>(i));
                }
            }
        }

        // ---- G-buffer targets (see gbuffer_formats) + the pass's own 1x depth image ----
        // One set per swapchain image, single-sampled: a G-buffer cannot be multisampled without
        // per-sample shading, which is the trade that makes TAA the engine's anti-aliasing.
        // COLOR_ATTACHMENT | SAMPLED because the pass writes them as attachments and the
        // lighting/transparent/TAA/debug passes sample them.
        for (uint32_t target = 0; target < gbuffer_target_count; ++target) {
            std::vector<VkImage>& target_images = gbuffer_images[target];
            std::vector<VkDeviceMemory>& target_memories = gbuffer_image_memories[target];
            std::vector<VkImageView>& target_views = gbuffer_image_views[target];
            target_images.resize(swap_chain_image_views.size());
            target_memories.resize(swap_chain_image_views.size());
            target_views.resize(swap_chain_image_views.size());

            for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
                create_target_image(
                    render.width,
                    render.height,
                    gbuffer_formats[target],
                    VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    target_images[i],
                    target_memories[i]);

                target_views[i] = create_image_view(
                    target_images[i],
                    gbuffer_formats[target],
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    logical_device);

                // ... AND THE HEAP'S COPY OF THE SAME IMAGE, at the grid array a heap-native shader will name: the
                // descriptor is a view CREATE INFO rather than a view, so it is built from the same image, format
                // and aspect the line above used, and nothing has to be kept around for it. The three surface
                // targets are the first three entries of gbuffer_formats (see make_gbuffer_pipeline's format list:
                // albedo, normal, material), which is what the grid's three arrays are named after.
                if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                    uint32_t const heap_slot = target == 0u ? heap_slots::gbuffer_albedo : (target == 1u ? heap_slots::gbuffer_normal : heap_slots::gbuffer_material);
                    VkImageViewCreateInfo const heap_view = make_image_view_info(target_images[i], gbuffer_formats[target], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                    if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slot + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                        deren::utility::log("descriptor heap: the gbuffer target {} for image {} did not reach grid slot {}", target, i, heap_slot + static_cast<uint32_t>(i));
                    }
                }
            }
        }

        // Motion vectors + the TAA working image (the scene color the resolve reads): same extent and
        // lifetime as the G-buffer targets, single-sampled, written as attachments and sampled
        // afterwards.
        // The parameter is `slot_base` and NOT `heap_slot_base`: it would hide `core::heap_slots::heap_slot_base`
        // and MSVC /W4 reports C4458 (an error under /WX). The member keeps its name because the shaders'
        // own constant is called that - see the note on it in core.declarations.cppm.
        auto const create_sampled_target = [this, render](std::vector<VkImage>& images, std::vector<VkDeviceMemory>& memories, std::vector<VkImageView>& views, VkFormat const format, uint32_t const slot_base) {
            images.resize(swap_chain_image_views.size());
            memories.resize(swap_chain_image_views.size());
            views.resize(swap_chain_image_views.size());
            for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
                create_target_image(
                    render.width,
                    render.height,
                    format,
                    VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    images[i],
                    memories[i]);
                views[i] = create_image_view(images[i], format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);
                // the heap's copy, from the same format and aspect (see the G-buffer block above)
                if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                    VkImageViewCreateInfo const heap_view = make_image_view_info(images[i], format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                    if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(slot_base + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                        deren::utility::log("descriptor heap: the sampled target for image {} did not reach grid slot {}", i, slot_base + static_cast<uint32_t>(i));
                    }
                }
            }
        };
        // The slot each one takes is the grid array named for what READS it: the motion vectors, and the scene
        // colour - which is TAA's `current_color` input, i.e. exactly what heap_slots::taa_current is named after.
        create_sampled_target(velocity_images, velocity_image_memories, velocity_image_views, gbuffer_velocity_format, heap_slots::gbuffer_velocity);
        create_sampled_target(scene_color_images, scene_color_image_memories, scene_color_image_views, hdr_format, heap_slots::taa_current);

        // The resolved-history image only needs TRANSFER_DST (the runtime copies the resolved frame
        // into it) and SAMPLED (the next frame's resolve reads it).
        taa_history_images.resize(swap_chain_image_views.size());
        taa_history_image_memories.resize(swap_chain_image_views.size());
        taa_history_image_views.resize(swap_chain_image_views.size());
        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                render.width,
                render.height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                taa_history_images[i],
                taa_history_image_memories[i]);

            taa_history_image_views[i] = create_image_view(taa_history_images[i], hdr_format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);
            // the heap's copy, at the array TAA's history input is named for (see the sampled-target lambda above)
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(taa_history_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::taa_history + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the TAA history for image {} did not reach grid slot {}", i, heap_slots::taa_history + static_cast<uint32_t>(i));
                }
            }
        }

        // The stochastic punctual lighting chain's images are HALF resolution, one per swapchain image: the
        // trace, the history and the resolve all share these two extents.
        uint32_t const half_width = std::max(1u, render.width / 2u);
        uint32_t const half_height = std::max(1u, render.height / 2u);

        // The stochastic punctual lighting chain's raw estimate: the same allocation as the GI trace's
        // (half resolution, STORAGE for its writer and SAMPLED for the lighting stage that adds it), and
        // deliberately its own image rather than a reuse of the GI's - the two are different quantities
        // produced by different passes, and the GI's is written later in the frame than this one.
        ml_images.resize(swap_chain_image_views.size());
        ml_image_memories.resize(swap_chain_image_views.size());
        ml_image_views.resize(swap_chain_image_views.size());
        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                half_width,
                half_height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                ml_images[i],
                ml_image_memories[i]);

            ml_image_views[i] = create_image_view(ml_images[i], hdr_format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);
            // TWO descriptors for this image, because its compute pass WRITES it and the lighting stage SAMPLES it
            // (no single heap descriptor is both): sampled at the array the readers name, storage at its own slot.
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(ml_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::ml_trace + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the megalights trace for image {} did not reach grid slot {}", i, heap_slots::ml_trace + static_cast<uint32_t>(i));
                }
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::ml_trace_storage + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)) {
                    deren::utility::log("descriptor heap: the megalights trace STORAGE descriptor for image {} did not reach grid slot {}", i, heap_slots::ml_trace_storage + static_cast<uint32_t>(i));
                }
            }
        }

        // The stochastic chain's temporal resolve (docs/megalights.md): the accumulation - STORAGE for the
        // compute pass that writes it, SAMPLED for the lighting stage that adds it, TRANSFER_SRC because it is
        // what the next frame's history is copied FROM - and the history beside the chain's, written only by
        // that copy (TRANSFER_DST | SAMPLED and nothing else).
        ml_resolve_images.resize(swap_chain_image_views.size());
        ml_resolve_image_memories.resize(swap_chain_image_views.size());
        ml_resolve_image_views.resize(swap_chain_image_views.size());
        ml_history_images.resize(swap_chain_image_views.size());
        ml_history_image_memories.resize(swap_chain_image_views.size());
        ml_history_image_views.resize(swap_chain_image_views.size());
        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                half_width,
                half_height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                ml_resolve_images[i],
                ml_resolve_image_memories[i]);
            ml_resolve_image_views[i] = create_image_view(ml_resolve_images[i], hdr_format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);
            // ... and the resolve's pair, the same way the trace's is written above: sampled for the lighting stage
            // that adds it, storage for the compute pass that accumulates into it.
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(ml_resolve_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::ml_resolved + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the megalights resolve for image {} did not reach grid slot {}", i, heap_slots::ml_resolved + static_cast<uint32_t>(i));
                }
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::ml_resolved_storage + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)) {
                    deren::utility::log("descriptor heap: the megalights resolve STORAGE descriptor for image {} did not reach grid slot {}", i, heap_slots::ml_resolved_storage + static_cast<uint32_t>(i));
                }
            }

            create_target_image(
                half_width,
                half_height,
                hdr_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                ml_history_images[i],
                ml_history_image_memories[i]);
            ml_history_image_views[i] = create_image_view(ml_history_images[i], hdr_format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);
            // the heap's copy: the history is written by a TRANSFER and read as the temporal resolve's input, so
            // one sampled descriptor is all it needs (the copy is not a descriptor write)
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_view = make_image_view_info(ml_history_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::ml_history + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the megalights history for image {} did not reach grid slot {}", i, heap_slots::ml_history + static_cast<uint32_t>(i));
                }
            }
        }

        // The GI denoiser's resolve target, its history and the spatial filter's output were created here: three

        // The furnace verification mode's constant environment (see the member comment): TRANSFER_DST because
        // a clear is what gives it contents, SAMPLED because the IBL bindings will point at it.
        furnace_cube_images.assign(1, VK_NULL_HANDLE);
        furnace_cube_memories.assign(1, VK_NULL_HANDLE);
        furnace_cube_views.assign(1, VK_NULL_HANDLE);
        create_target_image_cube(
            1,
            hdr_format,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            furnace_cube_images[0],
            furnace_cube_memories[0]);
        furnace_cube_views[0] = create_image_view(furnace_cube_images[0], hdr_format, VK_IMAGE_ASPECT_COLOR_BIT, logical_device, VK_IMAGE_VIEW_TYPE_CUBE, 6);
        // The ray-traced sun visibility: FULL resolution (one ray per screen pixel) and one per FRAME
        // SLOT - see the member's comment for why the slot, not the swapchain image, is the right
        // lifetime. R16F rather than RGBA16F: the pass writes a single visibility factor, and the
        // deferred lighting stage multiplies the sun term by it. STORAGE for the compute pass that
        // writes it, SAMPLED for the lighting stage that reads it.
        rt_shadow_images.assign(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
        rt_shadow_image_memories.assign(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
        rt_shadow_image_views.assign(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
        for (uint32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            create_target_image(
                render.width,
                render.height,
                VK_FORMAT_R16_SFLOAT,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                rt_shadow_images[slot],
                rt_shadow_image_memories[slot]);
            rt_shadow_image_views[slot] = create_image_view(rt_shadow_images[slot], VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, logical_device);

            // THE HEAP'S COPIES OF THAT IMAGE - TWO of them, which is not redundancy: SAMPLED_IMAGE and
            // STORAGE_IMAGE are different descriptor kinds and one heap descriptor is never both, while this image
            // is WRITTEN by the visibility compute pass and SAMPLED by the lighting stage. A heap image descriptor
            // carries a view CREATE INFO rather than a view, so both are built from the same arguments
            // create_image_view used on the line above. The layouts differ for the same reason the types do.
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const visibility_view = make_image_view_info(rt_shadow_images[slot], VK_FORMAT_R16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::rt_visibility + slot) * heap_slot_stride, visibility_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the rt visibility SAMPLED descriptor did not reach grid slot {}", heap_slots::rt_visibility + slot);
                }
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::rt_visibility_storage + slot) * heap_slot_stride, visibility_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)) {
                    deren::utility::log("descriptor heap: the rt visibility STORAGE descriptor did not reach grid slot {}", heap_slots::rt_visibility_storage + slot);
                }
            }
        }

        gbuffer_depth_images.resize(swap_chain_image_views.size());
        gbuffer_depth_image_memories.resize(swap_chain_image_views.size());
        gbuffer_depth_image_views.resize(swap_chain_image_views.size());
        for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
            create_target_image(
                render.width,
                render.height,
                depth_attachment_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                gbuffer_depth_images[i],
                gbuffer_depth_image_memories[i]);

            // the DEPTH aspect (a color view of a depth image is invalid) - same raw-view
            // convention as the other per-image targets, whose destruction is registered below
            gbuffer_depth_image_views[i] = create_image_view(gbuffer_depth_images[i], depth_attachment_format, VK_IMAGE_ASPECT_DEPTH_BIT, logical_device);
            // ... and the heap's copy, with the DEPTH aspect and the depth format the view above used: the heap
            // descriptor is a create info, and a colour aspect on a depth image is a validation error rather than a
            // wrong picture (the shadow map paid for that one already). The deferred, post and TAA stages all sample
            // this image, which is why the grid array is named for the surface rather than for one reader.
            if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                VkImageViewCreateInfo const heap_depth_view = make_image_view_info(gbuffer_depth_images[i], depth_attachment_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(heap_slots::gbuffer_depth + static_cast<uint32_t>(i)) * heap_slot_stride, heap_depth_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                    deren::utility::log("descriptor heap: the gbuffer depth for image {} did not reach grid slot {}", i, heap_slots::gbuffer_depth + static_cast<uint32_t>(i));
                }
            }
        }

        // bloom targets: the 4-level chain (halved per level, min 1x1), same lifetime as the HDR
        // targets; each level gets one target per swapchain image
        for (uint32_t level = 0; level < bloom_level_count; ++level) {
            std::vector<VkImage>& level_images = bloom_images[level];
            std::vector<VkDeviceMemory>& level_memories = bloom_image_memories[level];
            std::vector<VkImageView>& level_views = bloom_image_views[level];
            uint32_t const level_width = std::max(1u, render.width >> (level + 1u));
            uint32_t const level_height = std::max(1u, render.height >> (level + 1u));

            level_images.resize(swap_chain_image_views.size());
            level_memories.resize(swap_chain_image_views.size());
            level_views.resize(swap_chain_image_views.size());
            for (size_t i = 0; i < swap_chain_image_views.size(); i++) {
                create_target_image(
                    level_width,
                    level_height,
                    hdr_format,
                    VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    level_images[i],
                    level_memories[i]);

                level_views[i] = create_image_view(
                    level_images[i],
                    hdr_format,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    logical_device);
                // the heap's copy: a bloom level is RENDERED into and SAMPLED by the next level and the composite,
                // so one sampled descriptor per level per image - and the grid packs the levels `heap_image_capacity`
                // apart, which is the same stride this loop's level index multiplies (see core.cppm's heap_slots).
                if (this->descriptor_heaps.ready() && this->heap_grid_offset != VK_WHOLE_SIZE) {
                    uint32_t const level_base = heap_slots::bloom_l0 + level * heap_image_capacity;
                    VkImageViewCreateInfo const heap_view = make_image_view_info(level_images[i], hdr_format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                    if (!this->descriptor_heaps.write_image(static_cast<VkDeviceSize>(level_base + static_cast<uint32_t>(i)) * heap_slot_stride, heap_view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) {
                        deren::utility::log("descriptor heap: bloom level {} for image {} did not reach grid slot {}", level, i, level_base + static_cast<uint32_t>(i));
                    }
                }
            }
        }

        // The teardown is registered ONCE, not once per swapchain generation: this function reruns on
        // every recreate_swap_chain(), and register_cleanup() *pushes* (LIFO), so registering
        // unconditionally grew the cleanup stack by one identical lambda per resize. The single
        // lambda destroys whatever the vectors hold at destruction time, which is what we want.
        if (this->resolve_cleanup_registered) {
            return;
        }
        this->resolve_cleanup_registered = true;

        register_cleanup([this] {
            for (auto const& view : hdr_image_views) {
                vkDestroyImageView(logical_device, view, nullptr);
            }
            for (auto const& memory : hdr_image_memories) {
                vkFreeMemory(logical_device, memory, nullptr);
            }
            for (auto const& image : hdr_images) {
                vkDestroyImage(logical_device, image, nullptr);
            }
            hdr_image_views.clear();
            hdr_image_memories.clear();
            hdr_images.clear();
            // the LDR (FXAA input) targets share this lifetime: they are (re)created with the
            // swapchain in exactly the same way, so they belong to the same cleanup registration
            for (auto const& view : ldr_image_views) {
                vkDestroyImageView(logical_device, view, nullptr);
            }
            for (auto const& memory : ldr_image_memories) {
                vkFreeMemory(logical_device, memory, nullptr);
            }
            for (auto const& image : ldr_images) {
                vkDestroyImage(logical_device, image, nullptr);
            }
            ldr_image_views.clear();
            ldr_image_memories.clear();
            ldr_images.clear();
            // the G-buffer targets + the pass's own depth image share this lifetime too
            for (auto const& target_views : gbuffer_image_views) {
                for (auto const& view : target_views) {
                    vkDestroyImageView(logical_device, view, nullptr);
                }
            }
            for (auto const& target_memories : gbuffer_image_memories) {
                for (auto const& memory : target_memories) {
                    vkFreeMemory(logical_device, memory, nullptr);
                }
            }
            for (auto const& target_images : gbuffer_images) {
                for (auto const& image : target_images) {
                    vkDestroyImage(logical_device, image, nullptr);
                }
            }
            gbuffer_image_views = {};
            gbuffer_image_memories = {};
            gbuffer_images = {};
            for (auto const& view : gbuffer_depth_image_views) {
                vkDestroyImageView(logical_device, view, nullptr);
            }
            gbuffer_depth_image_views.clear();
            for (auto const& memory : gbuffer_depth_image_memories) {
                vkFreeMemory(logical_device, memory, nullptr);
            }
            gbuffer_depth_image_memories.clear();
            for (auto const& image : gbuffer_depth_images) {
                vkDestroyImage(logical_device, image, nullptr);
            }
            gbuffer_depth_images.clear();
            // motion vectors + the TAA working images share the same lifetime (see
            // create_render_targets)
            auto const destroy_images = [this](std::vector<VkImage>& images, std::vector<VkDeviceMemory>& memories, std::vector<VkImageView>& views) {
                for (auto const& view : views) {
                    vkDestroyImageView(logical_device, view, nullptr);
                }
                for (auto const& memory : memories) {
                    vkFreeMemory(logical_device, memory, nullptr);
                }
                for (auto const& image : images) {
                    vkDestroyImage(logical_device, image, nullptr);
                }
                views.clear();
                memories.clear();
                images.clear();
            };
            destroy_images(velocity_images, velocity_image_memories, velocity_image_views);
            destroy_images(scene_color_images, scene_color_image_memories, scene_color_image_views);
            destroy_images(taa_history_images, taa_history_image_memories, taa_history_image_views);
            destroy_images(ml_images, ml_image_memories, ml_image_views);
            destroy_images(ml_resolve_images, ml_resolve_image_memories, ml_resolve_image_views);
            destroy_images(ml_history_images, ml_history_image_memories, ml_history_image_views);
            destroy_images(furnace_cube_images, furnace_cube_memories, furnace_cube_views);
            destroy_images(rt_shadow_images, rt_shadow_image_memories, rt_shadow_image_views);
            for (auto const& level_views : bloom_image_views) {
                for (auto const& view : level_views) {
                    vkDestroyImageView(logical_device, view, nullptr);
                }
            }
            for (auto const& level_memories : bloom_image_memories) {
                for (auto const& memory : level_memories) {
                    vkFreeMemory(logical_device, memory, nullptr);
                }
            }
            for (auto const& level_images : bloom_images) {
                for (auto const& image : level_images) {
                    vkDestroyImage(logical_device, image, nullptr);
                }
            }
            bloom_image_views = {};
            bloom_image_memories = {};
            bloom_images = {};
        });
    }

    void core::create_command_pool() noexcept {
        VkCommandPoolCreateInfo const pool_info = make_command_pool_info(graphics_queue_family_index);

        if (vkCreateCommandPool(logical_device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
            deren::utility::panic("failed to create command pool");
        }

        register_cleanup([this] {
            if (command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(logical_device, command_pool, nullptr);
            }
        });
    }

    void core::create_target_image(
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageTiling tiling,
        VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkImage& image,
        VkDeviceMemory& image_memory) const noexcept {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = width;
        image_info.extent.height = height;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = format;
        image_info.tiling = tiling;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = usage;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(logical_device, &image_info, nullptr, &image) != VK_SUCCESS) {
            deren::utility::panic("can't create target image");
        }

        VkMemoryRequirements mem_requirements;
        vkGetImageMemoryRequirements(logical_device, image, &mem_requirements);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_requirements.size;
        alloc_info.memoryTypeIndex = find_memory_type(
            mem_requirements.memoryTypeBits,
            properties,
            physical_device);

        if (vkAllocateMemory(logical_device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
            deren::utility::panic("can't allocate target image memory");
        }

        vkBindImageMemory(logical_device, image, image_memory, 0);
    }

    void core::create_target_image_3d(
        uint32_t width,
        uint32_t height,
        uint32_t depth,
        VkFormat format,
        VkImageTiling tiling,
        VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkImage& image,
        VkDeviceMemory& image_memory) const noexcept {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_3D; // the only field that differs from the 2D path
        image_info.extent.width = width;
        image_info.extent.height = height;
        image_info.extent.depth = depth;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = format;
        image_info.tiling = tiling;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = usage;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(logical_device, &image_info, nullptr, &image) != VK_SUCCESS) {
            deren::utility::panic("can't create 3D target image");
        }

        VkMemoryRequirements mem_requirements;
        vkGetImageMemoryRequirements(logical_device, image, &mem_requirements);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_requirements.size;
        alloc_info.memoryTypeIndex = find_memory_type(
            mem_requirements.memoryTypeBits,
            properties,
            physical_device);

        if (vkAllocateMemory(logical_device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
            deren::utility::panic("can't allocate 3D target image memory");
        }

        vkBindImageMemory(logical_device, image, image_memory, 0);
    }

    void core::create_target_image_cube(
        uint32_t size,
        VkFormat format,
        VkImageTiling tiling,
        VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkImage& image,
        VkDeviceMemory& image_memory) const noexcept {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; // what makes a six-layer array a cube
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = size;
        image_info.extent.height = size;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 6;
        image_info.format = format;
        image_info.tiling = tiling;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = usage;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(logical_device, &image_info, nullptr, &image) != VK_SUCCESS) {
            deren::utility::panic("can't create cube target image");
        }

        VkMemoryRequirements mem_requirements;
        vkGetImageMemoryRequirements(logical_device, image, &mem_requirements);

        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_requirements.size;
        alloc_info.memoryTypeIndex = find_memory_type(
            mem_requirements.memoryTypeBits,
            properties,
            physical_device);

        if (vkAllocateMemory(logical_device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
            deren::utility::panic("can't allocate cube target image memory");
        }

        vkBindImageMemory(logical_device, image, image_memory, 0);
    }

    void core::create_sync_objects() {
        // Per frame slot: one BINARY image-available semaphore (vkAcquireNextImageKHR requires
        // binary) + one TIMELINE semaphore that replaces the old per-slot fences and counts
        // completed submissions. vkQueuePresentKHR also requires a binary wait, but present may
        // run on a separate queue, so the present-ready gates are ONE BINARY PER SWAPCHAIN IMAGE
        // (an image is only re-acquired after its present finished, which keeps the per-image
        // gate safe across queues).
        image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        present_ready_semaphores.resize(swap_chain_images.size());
        frame_done_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        frame_done_values.assign(MAX_FRAMES_IN_FLIGHT, 0);

        VkSemaphoreTypeCreateInfo timeline_type = make_timeline_semaphore_type_info();
        VkSemaphoreCreateInfo binary_info = make_binary_semaphore_info();

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (vkCreateSemaphore(logical_device, &binary_info, nullptr, &image_available_semaphores[i]) != VK_SUCCESS) {
                deren::utility::panic("failed to create image-available semaphore");
            }
            VkSemaphoreCreateInfo timeline_info = binary_info;
            timeline_info.pNext = &timeline_type;
            if (vkCreateSemaphore(logical_device, &timeline_info, nullptr, &frame_done_semaphores[i]) != VK_SUCCESS) {
                deren::utility::panic("failed to create frame-done timeline semaphore");
            }
        }
        for (auto& semaphore : present_ready_semaphores) {
            if (vkCreateSemaphore(logical_device, &binary_info, nullptr, &semaphore) != VK_SUCCESS) {
                deren::utility::panic("failed to create present-ready semaphore");
            }
        }

        register_cleanup([this] {
            for (auto const& semaphore : image_available_semaphores) {
                vkDestroySemaphore(logical_device, semaphore, nullptr);
            }
            for (auto const& semaphore : present_ready_semaphores) {
                vkDestroySemaphore(logical_device, semaphore, nullptr);
            }
            for (auto const& semaphore : frame_done_semaphores) {
                vkDestroySemaphore(logical_device, semaphore, nullptr);
            }
        });
    }

    void core::create_timestamp_query_pool() noexcept {
        // Two device facts decide whether pass timings are possible: how wide the timestamp
        // counter of a graphics queue is (timestampValidBits - a family that cannot write
        // timestamps reports 0) and how long one tick takes (timestampPeriod, ns/tick).
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(this->physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(this->physical_device, &family_count, families.data());
        if (this->graphics_queue_family_index < families.size()) {
            this->timestamp_valid_bits = families[this->graphics_queue_family_index].timestampValidBits;
        }
        this->timestamp_period_ns = this->device_properties.limits.timestampPeriod;

        if (this->timestamp_valid_bits == 0 || this->timestamp_period_ns <= 0.0f) {
            deren::utility::log("gpu timing: unavailable on this queue ({} valid bits, {} ns/tick) - pass timings are off",
                                this->timestamp_valid_bits,
                                static_cast<double>(this->timestamp_period_ns));
            return;
        }

        VkQueryPoolCreateInfo const pool_info = make_query_pool_info(VK_QUERY_TYPE_TIMESTAMP, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * gpu_timing_mark_capacity);
        if (vkCreateQueryPool(this->logical_device, &pool_info, nullptr, &this->timestamp_query_pool) != VK_SUCCESS) {
            deren::utility::log("gpu timing: timestamp query pool creation failed - pass timings are off");
            this->timestamp_query_pool = VK_NULL_HANDLE;
            return;
        }
        this->gpu_timing_supported = true;
        deren::utility::log("gpu timing: {} marks/frame available ({} ns/tick, {} valid bits)",
                            gpu_timing_mark_capacity,
                            static_cast<double>(this->timestamp_period_ns),
                            this->timestamp_valid_bits);

        register_cleanup([this] {
            if (this->timestamp_query_pool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(this->logical_device, this->timestamp_query_pool, nullptr);
            }
        });
    }
} // namespace deren::vulkan
