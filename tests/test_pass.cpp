// Headless unit tests: deren.vulkan.pass (the framework, no device) =================
// The framework's whole value is a CONTRACT: create once per generation, resolve per frame, skip an inactive
// pass without resolving it, apply the behaviour before recording, mark once per stage, and tell every pass
// when the swapchain was rebuilt. Each of those is something this renderer does by hand today, and does
// inconsistently - the manual reset list missed per-image descriptor families once, and the viewport resync is a
// hand-kept pipeline list. So this test asserts the contract with a FAKE host: no device, no Vulkan call, and
// every assertion is about what the runner DID, in what order, and what it deliberately did not do.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp> // the frame constants carry glm types (see deren.vulkan.frame_constants)
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

import deren.vulkan.pass;
import deren.vulkan.pass.chain;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;
import deren.vulkan.bindings;

namespace {
    namespace vp = deren::vulkan::pass;
    namespace rr = deren::vulkan::render_resource;

    /// The own binding every fake pass carries: one sampled image of its OWN at binding 0. What a pass reaches
    /// through the frame's heap is not a declaration the fake host has to spell out - the framework has no set to
    /// resolve it into (every stage reads the descriptor heap).
    std::array<rr::pass_binding, 1> const fake_own_bindings = {{{.binding = 0,
                                                                 .owner = rr::binding_owner::own,
                                                                 .kind = rr::binding_kind::sampled_image,
                                                                 .resource = rr::resource_id::ml_trace,
                                                                 .access = rr::binding_access::read,
                                                                 .sampler = rr::sampler_hint::gbuffer}}};

    /// the declaration every fake pass carries, renamed to the pass it belongs to
    rr::pass_io named_io(std::string_view const name, std::span<rr::render_target const> const targets = {}) {
        return rr::pass_io{
            .name = name,
            .bindings = fake_own_bindings,
            .targets = targets,
            .push = std::nullopt,
        };
    }

    /// fake handles, so a resolved pass can be told apart from an unresolved one without a device
    /// (not `constexpr`: a handle comes from `reinterpret_cast`, which is not a constant expression)
    VkCommandBuffer const fake_cmd = reinterpret_cast<VkCommandBuffer>(0xC0);
    VkDevice const fake_device = reinterpret_cast<VkDevice>(0xDD);
    VkSampler const fake_shadow_sampler = reinterpret_cast<VkSampler>(0x22);
    std::array<VkPipeline, 4> const fake_pipelines = {
        reinterpret_cast<VkPipeline>(0x1), reinterpret_cast<VkPipeline>(0x2), reinterpret_cast<VkPipeline>(0x3), reinterpret_cast<VkPipeline>(0x4)};
    /// the push block the fake host composes: raw bytes, as a real host does (the framework has no pass's type)
    std::array<std::byte, 4> const fake_push = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    /// the per-image view lists the fake host resolves (one entry per swapchain image of its fake frame): what a
    /// pass that owns a per-image descriptor family is handed, so the test can assert the runner passes them on
    std::array<VkImageView, 3> const fake_per_image = {
        reinterpret_cast<VkImageView>(0xA0), reinterpret_cast<VkImageView>(0xA1), reinterpret_cast<VkImageView>(0xA2)};

    /// the host's own state: the log every callback writes, plus what the fake host answers
    struct host_state {
        std::vector<std::string> log;
        vp::frame_identity frame = {.image_index = 3, .slot = 1, .image_count = 3, .extent = {640, 480}};
        std::vector<std::string_view> active_features = {"gi"};
        std::string_view failing_pass = {}; // resolve returns false for this pass's name
        std::array<vp::resolved_binding, 9> own = {};
        /// what a pass was handed at create time, recorded so the create interface can be asserted
        VkDevice created_with_device = VK_NULL_HANDLE;
        VkSampler created_with_sampler = VK_NULL_HANDLE;
        /// WHAT THE RESOLVER CAN READ OFF THE DECLARATION, recorded here because the extent rule is applied by
        /// the HOST (see host_resolve): a rule that names a resource and one of its elements is only usable if
        /// both halves of the pair reach the host that has to map them to a size.
        rr::resource_id last_extent_of = rr::resource_id::none;
        uint16_t last_extent_element = 0;
    };

    vp::frame_identity host_frame(void* ctx) {
        return static_cast<host_state*>(ctx)->frame;
    }

    bool host_feature_active(void* ctx, std::string_view const feature) {
        auto const& active = static_cast<host_state*>(ctx)->active_features;
        return std::find(active.begin(), active.end(), feature) != active.end();
    }

    /// the resolver a real runtime would write: the extent rule is applied HERE, from the declaration
    bool host_resolve(void* ctx, vp::frame_pass const& pass, vp::resolved_io& out) {
        auto& state = *static_cast<host_state*>(ctx);
        state.log.emplace_back(std::string("resolve:") + std::string(pass.io().name));
        if (pass.io().name == state.failing_pass) {
            return false;
        }
        vp::behaviour const& behaviour = pass.behaviour();
        state.last_extent_of = behaviour.extent_of;
        state.last_extent_element = behaviour.extent_of_element;
        out.frame = state.frame;
        out.cmd = fake_cmd;
        out.own = state.own;
        out.pipelines = std::span<VkPipeline const>(fake_pipelines.data(), behaviour.pipelines.size());
        out.push = fake_push;
        // the declared render targets, resolved the way an own binding is: the view for the instance, the
        // image for a barrier
        for (std::size_t t = 0; t < pass.io().targets.size() && t < out.target_storage.size(); ++t) {
            out.target_storage[t] = {.view = reinterpret_cast<VkImageView>(0x70 + t), .buffer = VK_NULL_HANDLE, .image = reinterpret_cast<VkImage>(0x80 + t)};
        }
        out.targets = std::span<vp::resolved_binding const>(out.target_storage.data(), pass.io().targets.size());
        // THE PER-IMAGE VIEW LISTS: what a pass that owns a per-image descriptor family needs (its write callback
        // is handed an image index, and `own` only carries the current frame's handles). A synthetic host fills
        // them here so the test can assert the runner hands them through untouched - the framework does not
        // interpret them.
        for (std::size_t k = 0; k < pass.io().bindings.size() && k < out.own_per_image.size(); ++k) {
            out.own_per_image[k] = std::span<VkImageView const>(fake_per_image.data(), fake_per_image.size());
        }
        out.extent = behaviour.extent == vp::extent_rule::half ? VkExtent2D{state.frame.extent.width / 2u, state.frame.extent.height / 2u} : state.frame.extent;
        return true;
    }

    void host_apply_behaviour(void* ctx, vp::frame_pass const& pass, vp::resolved_io const&) {
        auto& state = *static_cast<host_state*>(ctx);
        state.log.emplace_back(std::string("behaviour:") + std::string(pass.io().name) +
                               (pass.behaviour().resync_viewport ? ":resync" : ":plain"));
    }

    void host_mark_begin(void* ctx, std::string_view const name) {
        static_cast<host_state*>(ctx)->log.emplace_back(std::string("mark_begin:") + std::string(name));
    }

    void host_mark_end(void* ctx, std::string_view const name) {
        static_cast<host_state*>(ctx)->log.emplace_back(std::string("mark_end:") + std::string(name));
    }

    vp::pass_host make_host(host_state& state) {
        return vp::pass_host{
            .context = &state,
            .frame = host_frame,
            .feature_active = host_feature_active,
            .resolve = host_resolve,
            .apply_behaviour = host_apply_behaviour,
            .mark_begin = host_mark_begin,
            .mark_end = host_mark_end,
        };
    }

    /// the create-time context: what a pass builds itself from, and the point of the split is that ANY owner can
    /// fill it - this fake one below needs no device, no runtime and no frame, which is exactly what the test
    /// asserts a pass may rely on
    std::span<uint8_t const> fake_shader(void* /*owner*/, std::string_view const name) {
        static std::array<uint8_t, 3> const bytes = {0x03, 0x02, 0x23};
        return name == "fake.comp.spv" ? std::span<uint8_t const>(bytes) : std::span<uint8_t const>{};
    }

    vp::pass_context make_context() {
        return vp::pass_context{
            .device = fake_device,
            .samplers = {.shadow = fake_shadow_sampler},
            .shader = fake_shader,
            .swap_chain_image_format = VK_FORMAT_B8G8R8A8_SRGB,
            .depth_format = VK_FORMAT_D32_SFLOAT,
            .owner = nullptr,
        };
    }

    /// position of an entry in the log, or a large number when it is absent (so ordering asserts read clearly)
    std::size_t at(std::vector<std::string> const& log, std::string_view const entry) {
        auto const it = std::find(log.begin(), log.end(), entry);
        return it == log.end() ? std::size_t{9999} : static_cast<std::size_t>(it - log.begin());
    }

    bool has(std::vector<std::string> const& log, std::string_view const entry) {
        return at(log, entry) != 9999;
    }

    /// a declaration the schema must refuse: a binding whose resource was never set
    rr::pass_io const& bad_io() {
        static constexpr std::array<rr::pass_binding, 1> bindings = {{
            {.binding = 0, .owner = rr::binding_owner::own, .kind = rr::binding_kind::sampled_image, .resource = rr::resource_id::none, .sampler = rr::sampler_hint::post},
        }};
        static constexpr rr::pass_io io = {.name = "bad", .bindings = bindings, .push = std::nullopt};
        return io;
    }

    /// a pass that records what the runner did to it, in the order it did it
    class fake_pass final : public vp::frame_pass {
    public:
        fake_pass(rr::pass_io io, vp::behaviour const behaviour, std::string_view const feature, host_state& state)
            : io_decl(io)
            , behaviour_decl(behaviour)
            , feature_name(feature)
            , state_ptr(&state) {
        }

        [[nodiscard]] rr::pass_io const& io() const noexcept override {
            return io_decl;
        }
        [[nodiscard]] vp::behaviour const& behaviour() const noexcept override {
            return behaviour_decl;
        }
        [[nodiscard]] std::string_view feature() const noexcept override {
            return feature_name;
        }
        /// the framework's generic readiness question: a fake that says it did not build anything (see the
        /// `chain.ready(name)` checks) - the DEFAULT (a pass that builds nothing of its own) is the interface's `true`
        [[nodiscard]] bool ready() const noexcept override {
            return this->is_ready;
        }
        bool is_ready = true;
        void create(vp::pass_context const& context) override {
            state_ptr->log.emplace_back(std::string("create:") + std::string(io_decl.name));
            state_ptr->created_with_device = context.device;
            state_ptr->created_with_sampler = context.samplers.of(rr::sampler_hint::shadow);
        }
        void on_swapchain_recreated(vp::pass_host const&) override {
            state_ptr->log.emplace_back(std::string("recreate:") + std::string(io_decl.name));
        }
        void record(vp::resolved_io const& io) override {
            state_ptr->log.emplace_back(std::string("record:") + std::string(io_decl.name));
            last_cmd = io.cmd;
            last_pipelines = io.pipelines.size();
            last_push_size = io.push.size();
            last_extent = io.extent;
            last_image_index = io.frame.image_index;
            last_slot = io.frame.slot;
            last_image_count = io.frame.image_count;
            last_targets = io.targets.size();
            last_target_view = io.targets.empty() ? VK_NULL_HANDLE : io.targets[0].view;
            last_target_image = io.targets.empty() ? VK_NULL_HANDLE : io.targets[0].image;
            // the per-image view lists, as the pass received them (see resolved_io::own_per_image)
            last_per_image_first = io.own_per_image.empty() || io.own_per_image[0].empty() ? VK_NULL_HANDLE : io.own_per_image[0][0];
            last_per_image_length = io.own_per_image.empty() ? 0 : io.own_per_image[0].size();
        }

        VkCommandBuffer last_cmd = VK_NULL_HANDLE;
        std::size_t last_pipelines = 0;
        std::size_t last_push_size = 0;
        VkExtent2D last_extent = {0, 0};
        uint32_t last_image_index = 0;
        uint32_t last_slot = 0;
        uint32_t last_image_count = 0;
        std::size_t last_targets = 0;
        VkImageView last_target_view = VK_NULL_HANDLE;
        VkImage last_target_image = VK_NULL_HANDLE;
        VkImageView last_per_image_first = VK_NULL_HANDLE;
        std::size_t last_per_image_length = 0;

    private:
        // the members below cannot take the interface's plain names (`io()`, `behaviour()`, `feature()` are the
        // accessors, and the constructor's parameters already use them), so each carries a distinguishing suffix
        rr::pass_io io_decl;
        vp::behaviour behaviour_decl;
        std::string_view feature_name;
        host_state* state_ptr;
    };

    // the names are `deren.vulkan.runtime`'s own pipeline keys, which is what makes this cost nothing new
    constexpr std::array<std::string_view, 1> compute_pipeline_names = {"megalights_trace"};
    constexpr std::array<std::string_view, 2> fullscreen_pipeline_names = {"post_composite", "fxaa"};
    /// the fullscreen fake pass also declares one render TARGET: an attachment is a use that cannot be a
    /// descriptor, so it is declared in its own list (see vulkan.render_resource::render_target)
    constexpr rr::render_target fullscreen_target = {.resource = rr::resource_id::hdr, .element = 0};
    constexpr std::array<rr::render_target, 1> fullscreen_targets = {fullscreen_target};
    constexpr vp::behaviour compute_behaviour = {.kind = vp::behaviour_kind::compute, .group_size_x = 4, .group_size_y = 4, .group_size_z = 4, .extent = vp::extent_rule::resource, .pipelines = compute_pipeline_names};
    constexpr vp::behaviour fullscreen_behaviour = {.kind = vp::behaviour_kind::fullscreen, .extent = vp::extent_rule::half, .pipelines = fullscreen_pipeline_names, .resync_viewport = true};
    /// a behaviour whose extent rule names a RESOURCE **and one element of it**: the framework carries the pair,
    /// and the HOST is the layer that maps it to a size (the bloom chain's four levels are the first real user -
    /// see vp::behaviour::extent_of_element)
    constexpr vp::behaviour level_behaviour = {.kind = vp::behaviour_kind::fullscreen,
                                               .extent = vp::extent_rule::resource,
                                               .extent_of = rr::resource_id::bloom,
                                               .extent_of_element = 2,
                                               .pipelines = fullscreen_pipeline_names};
} // namespace

int32_t main() {
    using namespace deren::vulkan::pass;

    host_state state;
    pass_host const host = make_host(state);

    fake_pass probe{named_io("probe"), compute_behaviour, "gi", state};
    fake_pass tail{named_io("tail", fullscreen_targets), fullscreen_behaviour, "gi", state};
    fake_pass gated{named_io("gated"), compute_behaviour, "off", state};
    fake_pass bad{bad_io(), compute_behaviour, "gi", state};

    // ---- build: every declaration is validated first, and a bad one refuses the whole stage ----
    {
        std::array<frame_pass*, 2> passes = {&probe, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = create_stage(st, make_context());
        CHECK(report.created == 2);
        CHECK(report.rejected.empty());
        CHECK(at(state.log, "create:probe") < at(state.log, "create:tail")); // declaration order, not container order
    }
    {
        std::array<frame_pass*, 2> passes = {&probe, &bad};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = create_stage(st, make_context());
        CHECK(report.rejected == "bad"); // the pass's own name, for a startup message that says which one
        CHECK(report.created == 1);      // and the stage stops there rather than running a bad declaration
        CHECK(!has(state.log, "create:bad"));
    }
    {
        std::array<frame_pass*, 3> passes = {&probe, nullptr, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        CHECK(create_stage(st, make_context()).created == 2); // a null slot is skipped, not counted
    }

    // ---- record: the order of resolve, behaviour and record, and the stage's mark around all of it ----
    {
        std::array<frame_pass*, 2> passes = {&probe, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = record_stage(st, host);
        CHECK(report.recorded == 2);
        CHECK(report.skipped_inactive == 0);
        CHECK(report.marked == 1);
        CHECK(state.log.front() == "mark_begin:scene"); // the stage owns the mark: first and last
        CHECK(state.log.back() == "mark_end:scene");
        // per pass: resolve, then the behaviour's mechanical part, then record
        CHECK(at(state.log, "resolve:probe") < at(state.log, "behaviour:probe:plain"));
        CHECK(at(state.log, "behaviour:probe:plain") < at(state.log, "record:probe"));
        CHECK(at(state.log, "record:probe") < at(state.log, "resolve:tail"));
        // ... and the behaviour is passed through, not interpreted: the fullscreen pass asked for a resync
        CHECK(has(state.log, "behaviour:tail:resync"));
    }

    // ---- what a pass is GIVEN, which is the reason it needs no device state of its own ----
    {
        std::array<frame_pass*, 2> passes = {&probe, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const given = record_stage(st, host);
        CHECK(given.recorded == 2);
        CHECK(probe.last_cmd == fake_cmd);  // the command buffer is handed out per frame...
        CHECK(probe.last_image_index == 3); // ... and both frame counters, kept apart
        CHECK(probe.last_slot == 1);
        CHECK(probe.last_pipelines == 1);      // one declared name -> one resolved pipeline
        CHECK(tail.last_pipelines == 2);       // two names -> two, in the declared order
        CHECK(probe.last_extent.width == 640); // extent_rule::resource, with a fake host that hands the frame's
        CHECK(tail.last_extent.width == 320);  // extent_rule::half is half of it, applied by the resolver
        // ... the host-composed push block as raw bytes, and the GENERATION's image count - which is what a pass
        // sizes per-image state from, and is not the image index
        CHECK(probe.last_push_size == fake_push.size());
        CHECK(probe.last_image_count == 3);
        // ... and the images it RENDERS INTO, declared apart from the bindings because an attachment is bound
        // by a rendering instance and not by a set: a compute pass declares none, the fullscreen one gets its
        // view and its image resolved
        CHECK(probe.last_targets == 0);
        CHECK(tail.last_targets == 1);
        CHECK(tail.last_target_view == reinterpret_cast<VkImageView>(0x70));
        CHECK(tail.last_target_image == reinterpret_cast<VkImage>(0x80));
        // ... and the PER-IMAGE VIEW LISTS reached the pass untouched: a pass that owns per-image state reads
        // each image's own handles from them, which `own` (the current frame's) cannot supply - see
        // resolved_io::own_per_image
        CHECK(probe.last_per_image_length == 3); // one entry per swapchain image of the frame
        CHECK(probe.last_per_image_first != VK_NULL_HANDLE);
    }

    // ---- the extent rule's (resource, element) PAIR reaches the host, which is the only layer that can map it
    //      to a size: the framework carries a declaration, it does not interpret one ----
    {
        fake_pass level{named_io("bloom_level"), level_behaviour, "gi", state};
        std::array<frame_pass*, 1> passes = {&level};
        stage const st = {.name = "post", .passes = passes};
        state.log.clear();
        run_report const report = record_stage(st, host);
        CHECK(report.recorded == 1);
        CHECK(state.last_extent_of == rr::resource_id::bloom); // the family the rule names...
        CHECK(state.last_extent_element == 2);                 // ... and WHICH of its images
        // ... and a declaration that names no element says 0 rather than an arbitrary level: the field is
        // additive, so every declaration written before it existed still means exactly what it meant
        CHECK(fullscreen_behaviour.extent_of_element == 0);
        CHECK(compute_behaviour.extent_of_element == 0);
        CHECK(fullscreen_behaviour.extent_of == rr::resource_id::none);
    }

    // ---- the create-time context on its own: a pass is built from THIS and nothing else ----
    {
        vp::pass_context const context = make_context();
        // a pass is built from the CONTEXT alone: no frame, no runner, no runtime - the property that makes a
        // pass constructible outside this renderer
        CHECK(context.device == fake_device);
        CHECK(context.samplers.of(rr::sampler_hint::shadow) == fake_shadow_sampler);
        // ... and it carries NO set layout and NO pipeline layout: every stage is heap-native, so a pass builds
        // its pipeline with a null layout and reaches its descriptors through the frame's heap. The context is
        // the create-time facts a pass cannot derive, and nothing that could bind a set.
        CHECK(context.shader(context.owner, "fake.comp.spv").size() == 3);
        CHECK(context.shader(context.owner, "missing.comp.spv").empty());
        // ... and the SURFACE's format, which a pipeline that renders into the swapchain must be created with:
        // a session-stable device fact the host hands over rather than one a pass could guess (the extent, which
        // DOES change, is deliberately not here - a pass that bakes one rebuilds in on_swapchain_recreated)
        CHECK(context.swap_chain_image_format == VK_FORMAT_B8G8R8A8_SRGB);
        // ... and the DEPTH format, the second session-stable format - the shadow pass's pipeline has a depth
        // attachment and no colour one, so the surface's format is the wrong fact for it
        CHECK(context.depth_format == VK_FORMAT_D32_SFLOAT);
    }

    // ---- what a pass is given at CREATE time: a device, the five samplers, and two lookups - and nothing that
    //      allocates or runs a frame ----
    {
        std::array<frame_pass*, 1> passes = {&probe};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        state.created_with_device = VK_NULL_HANDLE;
        state.created_with_sampler = VK_NULL_HANDLE;
        run_report const built = create_stage(st, make_context());
        CHECK(built.created == 1);
        CHECK(state.created_with_device == fake_device);
        CHECK(state.created_with_sampler == fake_shadow_sampler); // chosen by hint, never named by the pass
    }

    // ---- an inactive feature is skipped WITHOUT being resolved: what makes an off feature byte-exact ----
    {
        std::array<frame_pass*, 2> passes = {&probe, &gated};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = record_stage(st, host);
        CHECK(report.recorded == 1);
        CHECK(report.skipped_inactive == 1);
        CHECK(!has(state.log, "resolve:gated")); // not resolved...
        CHECK(!has(state.log, "record:gated"));  // ... and not recorded
    }

    // ---- a frame that cannot resolve a pass skips it, and does not apply its behaviour either ----
    {
        std::array<frame_pass*, 2> passes = {&probe, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        state.failing_pass = "tail";
        run_report const report = record_stage(st, host);
        state.failing_pass = {};
        CHECK(report.recorded == 1);
        CHECK(report.skipped_unresolved == 1);
        CHECK(!has(state.log, "behaviour:tail:resync")); // a pass recorded with unresolved handles is worse
        CHECK(!has(state.log, "record:tail"));
    }

    // ---- a stage that nests inside another's instance can decline its own mark pair ----
    {
        std::array<frame_pass*, 1> passes = {&probe};
        stage const st = {.name = "nested", .passes = passes, .marks = false};
        state.log.clear();
        run_report const report = record_stage(st, host);
        CHECK(report.marked == 0);
        CHECK(!has(state.log, "mark_begin:nested"));
        CHECK(has(state.log, "record:probe"));
    }

    // ---- a rebuilt swapchain tells EVERY pass: the hazard the manual reset list keeps missing ----
    {
        std::array<frame_pass*, 3> passes = {&probe, &tail, &gated};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = recreate_stage(st, host);
        CHECK(report.recreated == 3);
        CHECK(has(state.log, "recreate:probe"));
        CHECK(has(state.log, "recreate:tail"));
        CHECK(has(state.log, "recreate:gated")); // even the pass this frame skipped
    }

    // ---- the declaration -> Vulkan mapping, which is what the generator builds a layout from ----
    // These live in this test rather than in test_render_resources because they are Vulkan-typed: the
    // description layer itself stays pure CPU, and everything that has to name a VkDescriptorType lives on the
    // bindings side. No device is created - an enum mapping needs none - so this still runs in CI.
    {
        using namespace deren::vulkan::bindings;
        CHECK(descriptor_type_of(rr::binding_kind::sampled_image) == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        CHECK(descriptor_type_of(rr::binding_kind::storage_image) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        CHECK(descriptor_type_of(rr::binding_kind::sampler) == VK_DESCRIPTOR_TYPE_SAMPLER);
        CHECK(descriptor_type_of(rr::binding_kind::uniform_buffer) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        CHECK(descriptor_type_of(rr::binding_kind::storage_buffer) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        CHECK(descriptor_type_of(rr::binding_kind::input_attachment) == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
        CHECK(descriptor_type_of(rr::binding_kind::acceleration_structure) == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        CHECK(stage_flags_of(rr::stage_flag::compute) == VK_SHADER_STAGE_COMPUTE_BIT);
        CHECK(stage_flags_of(rr::stage_flag::fragment | rr::stage_flag::compute) == (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT));
        CHECK(stage_flags_of(rr::stage_flag::none) == 0u);
        // (the `image_layout_of` mapping pins stood here; the mapping went with the enum when every image
        // became GENERAL - see docs/unified_image_layouts.md)
        // the sampler CHOICE a declaration makes instead of a handle
        rr::shared::sampler_set const samplers = {.gbuffer = reinterpret_cast<VkSampler>(0x11), .shadow = reinterpret_cast<VkSampler>(0x22)};
        CHECK(samplers.of(rr::sampler_hint::shadow) == reinterpret_cast<VkSampler>(0x22));
        CHECK(samplers.of(rr::sampler_hint::gbuffer) == reinterpret_cast<VkSampler>(0x11));
        CHECK(samplers.of(rr::sampler_hint::none) == VK_NULL_HANDLE); // "no sampler", which the validator enforces
        // a declaration's own bindings are exactly the bindings its shader declares, in order - checked against the
        // stochastic punctual lighting chain's temporal resolve, which is the shape the pipeline builder this
        // test covers still builds from (that declaration went with the removed chain)
        uint32_t own = 0;
        for (rr::pass_binding const& b : rr::megalights_temporal_io.bindings) {
            if (b.owner == rr::binding_owner::own) {
                CHECK(b.binding == own); // contiguous from zero: the index IS the binding number
                ++own;
            }
        }
        CHECK(own == 5);
    }

    // ---- THE CHAIN: a value that holds a run of passes and its ORDER, and nothing else - it must behave
    //      exactly like the separate stages it replaces (same per-pass feature gate, resolver and record),
    //      because that is the whole claim its header makes ----
    {
        pass_chain chain{"gi"};
        CHECK(chain.empty());
        CHECK(chain.name() == "gi");
        chain.add(probe);
        chain.add(tail);
        CHECK(chain.size() == 2);
        // the lookup is by the DECLARATION's name, so a caller never has to know the chain's order to find one
        CHECK(chain.find("tail") == static_cast<frame_pass*>(&tail));
        CHECK(chain.find("nothing") == nullptr);
        // ... and the stage it hands the runner is its own name, its own list, and its own mark policy
        CHECK(chain.as_stage().name == "gi");
        CHECK(chain.as_stage().passes.size() == 2);
        CHECK(!chain.as_stage().marks);

        state.log.clear();
        run_report const created = chain.init(make_context());
        CHECK(created.created == 2);
        CHECK(created.rejected.empty());
        CHECK(at(state.log, "create:probe") < at(state.log, "create:tail")); // the ORDER OF THE add CALLS

        state.log.clear();
        run_report const recorded = chain.record(host);
        CHECK(recorded.recorded == 2);
        // the order of resolve/behaviour/record is the runner's, unchanged by the chain: resolve then behaviour
        // then the pass, per pass, in chain order
        CHECK(at(state.log, "resolve:probe") < at(state.log, "behaviour:probe:plain"));
        CHECK(at(state.log, "behaviour:probe:plain") < at(state.log, "record:probe"));
        CHECK(at(state.log, "record:probe") < at(state.log, "resolve:tail"));
    }
    {
        // a pass whose feature is off is skipped INSIDE the chain, exactly as it was inside its own stage - a
        // chain that could skip differently would be a scheduler, which this class deliberately is not
        pass_chain chain{"gi"};
        chain.add(probe);
        chain.add(gated); // feature "off"
        state.log.clear();
        run_report const recorded = chain.record(host);
        CHECK(recorded.recorded == 1);
        CHECK(recorded.skipped_inactive == 1);
        CHECK(!has(state.log, "record:gated"));
    }

    // ---- `chain.ready(name)`: the readiness question asked in the DECLARATION's vocabulary, which is the only key
    //      an owner that holds a chain instead of typed members has. It is what the renderer's feature registry asks
    //      now ("the knob is on AND the pass built its pipeline") ----
    {
        pass_chain chain{"probe"};
        chain.add(probe);
        chain.add(tail);
        CHECK(chain.ready("probe")); // the fake says it is ready
        CHECK(chain.ready("tail"));
        CHECK(!chain.ready("absent")); // a name no pass in this chain declares is not "ready"
        // ... and a pass that says it did not build what it records with is not ready
        probe.is_ready = false;
        CHECK(!chain.ready("probe"));
        CHECK(chain.ready("tail")); // ... independently of its neighbour
        probe.is_ready = true;
    }

    // ---- the chain also KEPT objects that are not passes (`keep`, whose only users were the renderer's two jobs).
    //      THE PROMISE WENT WITH THE FUNCTION: those two jobs are ordinary members of the renderer now, because they
    //      are the only non-pass GPU-owning objects left, and a container that exists to hold exactly two objects of
    //      two known types is more machinery than two members. Nothing tests it any more; `emplace`, the owning form
    //      for a PASS, is covered by the stage and list assertions above ----

    // ---- the resource table: what EXISTS, keyed the way a DECLARATION names it (resource + element + instance).
    //      The instance rule is the schema's own SCOPE, and it is one function so the publisher and the reader
    //      cannot disagree - which is the whole reason this container exists instead of sixteen resolvers ----
    {
        vp::frame_identity const frame = {.image_index = 2, .slot = 1, .image_count = 3, .extent = {64, 64}};
        CHECK(vp::instance_for(rr::resource_scope::per_swapchain_image, frame) == 2);
        CHECK(vp::instance_for(rr::resource_scope::per_frame_slot, frame) == 1);
        CHECK(vp::instance_for(rr::resource_scope::device_wide, frame) == 0); // one instance, whatever the frame says

        vp::resource_table table;
        VkImageView const view_a = reinterpret_cast<VkImageView>(0x1000);
        VkImage const image_a = reinterpret_cast<VkImage>(0x2000);
        VkImageView const view_b = reinterpret_cast<VkImageView>(0x3000);
        VkImage const image_b = reinterpret_cast<VkImage>(0x4000);
        VkBuffer const buffer_a = reinterpret_cast<VkBuffer>(0x5000);

        // an empty table answers all-null rather than failing: "the owner does not have it" and "this frame
        // cannot use it" are different statements, and only the second is a pass's business
        CHECK(table.size() == 0);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view == VK_NULL_HANDLE);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).image == VK_NULL_HANDLE);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).buffer == VK_NULL_HANDLE);

        // one entry per IMAGE, which is what makes `own_per_image` and the bloom levels expressible
        table.publish(rr::resource_id::hdr, 0, 0, {.view = view_a, .image = image_a});
        table.publish(rr::resource_id::hdr, 0, 1, {.view = view_b, .image = image_b});
        CHECK(table.size() == 2);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view == view_a);
        CHECK(table.find(rr::resource_id::hdr, 0, 1).view == view_b);
        CHECK(table.find(rr::resource_id::hdr, 0, 2).view == VK_NULL_HANDLE); // no third image this generation
        CHECK(table.instances_of(rr::resource_id::hdr, 0) == 2);
        // the ELEMENT is part of the key: the bloom chain's four levels are four entries of one family
        table.publish(rr::resource_id::bloom, 3, 0, {.view = view_a, .image = image_a});
        CHECK(table.size() == 3);
        CHECK(table.find(rr::resource_id::bloom, 3, 0).view == view_a);
        CHECK(table.find(rr::resource_id::bloom, 2, 0).view == VK_NULL_HANDLE);
        CHECK(table.instances_of(rr::resource_id::bloom, 3) == 1);
        // a buffer-only family carries the buffer and leaves the image lanes null, which is how a barrier
        // buffer is told apart from an image with a view
        table.publish(rr::resource_id::cluster_counts, 0, 1, {.buffer = buffer_a});
        CHECK(table.find(rr::resource_id::cluster_counts, 0, 1).buffer == buffer_a);
        CHECK(table.find(rr::resource_id::cluster_counts, 0, 1).view == VK_NULL_HANDLE);
        CHECK(table.find(rr::resource_id::cluster_counts, 1, 1).buffer == VK_NULL_HANDLE);

        // publishing the same key REPLACES it: a frame that re-publishes an alias (scene_color is TAA-or-HDR)
        // ends with the last value rather than with two entries that disagree
        table.publish(rr::resource_id::hdr, 0, 0, {.view = view_b, .image = image_b});
        CHECK(table.size() == 4);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view == view_b);

        // ... and `clear` is what makes the next frame's publication the whole truth
        table.clear();
        CHECK(table.size() == 0);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view == VK_NULL_HANDLE);
    }

    // ---- the frame constants ride with the resolved I/O: a value, defaulted, so a pass that reads it before
    //      the frame loop has filled it reads zeros rather than whatever was on the stack ----
    {
        vp::resolved_io io = {};
        CHECK(io.constants.scene_radius == 0.0f);
        CHECK(io.constants.view == glm::mat4(1.0f)); // the identity, not the frame's camera: nothing has run yet
    }

    // ---- what `frame_pass::resolve` does unless a pass overrides it: the DECLARATION-driven resolution, whose
    //      only source of handles is the resource table. This is the piece the renderer's sixteen hand-written
    //      `resolve_*_pass` functions are being replaced by, one pass at a time, and the rules it enforces are
    //      the ones every one of them wrote by hand: a missing resource means the pass does not run, the frame's
    //      instance comes from the SCHEMA's scope, and the push block is the pass's own business ----
    {
        /// a pass whose declaration is all it needs. The TAA resolve's declaration is reused rather than
        /// invented: four own bindings (all per-swapchain-image), one render target, and a declared push size
        /// that the resolver must NOT fill.
        struct declared_pass final : vp::frame_pass {
            rr::pass_io const* declaration = &rr::taa_io;
            vp::behaviour how = {.kind = vp::behaviour_kind::fullscreen, .extent = vp::extent_rule::full};
            VkPipeline owned_pipeline = VK_NULL_HANDLE;
            [[nodiscard]] rr::pass_io const& io() const noexcept override {
                return *this->declaration;
            }
            [[nodiscard]] vp::behaviour const& behaviour() const noexcept override {
                return this->how;
            }
            [[nodiscard]] std::string_view feature() const noexcept override {
                return {};
            }
            // the passes that build their OWN pipeline answer this, and the resolver must prefer it over a
            // registry entry that happens to share a `behaviour::pipelines` name
            [[nodiscard]] VkPipeline pipeline() const noexcept override {
                return this->owned_pipeline;
            }
            void create(vp::pass_context const&) override {
            }
            void on_swapchain_recreated(vp::pass_host const&) override {
            }
            void record(vp::resolved_io const&) override {
            }
        };
        /// the owner side of a resolve_context: a table plus the lookups the framework declares
        struct resolver_owner {
            vp::resource_table table;
            VkExtent2D resource_extent = {7, 9};
            VkPipeline pipeline = reinterpret_cast<VkPipeline>(0x77);
            std::string_view unknown_pipeline = {};
        };
        resolver_owner owner;
        vp::frame_identity const frame = {.image_index = 1, .slot = 0, .image_count = 2, .extent = {64, 32}};
        vp::resolve_context const context = {
            .resources = &owner.table,
            .frame = frame,
            .cmd = fake_cmd,
            .extent_of = [](void* o, rr::resource_id, uint32_t) { return static_cast<resolver_owner*>(o)->resource_extent; },
            .pipeline =
                [](void* o, std::string_view const name) -> vp::owned_pipeline {
                // the owner answers a NAME with a pipeline, or with nothing for a name it does not own
                return name == static_cast<resolver_owner*>(o)->unknown_pipeline
                           ? vp::owned_pipeline{}
                           : vp::owned_pipeline{.pipeline = static_cast<resolver_owner*>(o)->pipeline};
            },
            .owner = &owner,
        };
        declared_pass pass;
        vp::resolved_io io = {};
        // the framework's DEFAULT readiness answer: this pass overrides `ready()` nowhere, because it builds nothing
        // the run has to wait for - a pass with nothing of its own has nothing to be unready ABOUT (see
        // frame_pass::ready)
        CHECK(pass.ready());

        // AN EMPTY TABLE: the frame does not have what the declaration names, so the pass does not run at all -
        // the rule that replaced every resolver's `if (images.empty() || index >= count) return false;`
        CHECK(!pass.resolve(context, io));

        // ... publish this frame's four bindings and its target AS FAMILIES (the run of views and images the owner
        // already holds - the shape `publish_family` exists for), and the SAME declaration resolves
        for (uint32_t image = 0; image < 2; ++image) {
            for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
                uint64_t const tag = static_cast<uint64_t>(id) + image;
                owner.table.publish(id, 0, image, {.view = reinterpret_cast<VkImageView>(0x8000 + tag), .image = reinterpret_cast<VkImage>(0x9000 + tag)});
            }
        }
        VkImageView const hdr_view = reinterpret_cast<VkImageView>(0xAA);
        owner.table.publish(rr::resource_id::hdr, 0, frame.image_index, {.view = hdr_view, .image = reinterpret_cast<VkImage>(0xBB)});
        CHECK(pass.resolve(context, io));
        CHECK(io.cmd == fake_cmd);
        CHECK(io.frame.image_index == frame.image_index);
        CHECK(io.own.size() == 4); // the four own bindings, in their own binding order
        CHECK(io.own[0].image == reinterpret_cast<VkImage>(0x9000 + static_cast<uint64_t>(rr::resource_id::scene_color) + frame.image_index));
        CHECK(io.own[3].image == reinterpret_cast<VkImage>(0x9000 + static_cast<uint64_t>(rr::resource_id::gbuffer_depth) + frame.image_index));
        CHECK(io.targets.size() == 1);
        CHECK(io.targets[0].view == hdr_view);
        CHECK(io.extent.width == 64 && io.extent.height == 32); // the declaration's rule is `full`
        CHECK(io.push.empty());                                 // a pass composes its own push block
        CHECK(io.barrier_images.empty() && io.barrier_buffers.empty());
        // THE PER-IMAGE CHANNEL is empty here (these were published instance by instance), which is the shape the
        // one consumer reads: it checks each span's length before indexing it
        CHECK(io.own_per_image[0].empty());

        // ... and published AS A FAMILY, the same declaration also hands the pass every image's view - the run
        // `views_of` returns, in the owner's own storage, which is what a per-image descriptor family writes from
        std::array<VkImageView, 2> const family_views = {reinterpret_cast<VkImageView>(0xF0), reinterpret_cast<VkImageView>(0xF1)};
        std::array<VkImage, 2> const family_images = {reinterpret_cast<VkImage>(0xE0), reinterpret_cast<VkImage>(0xE1)};
        owner.table.clear();
        for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
            owner.table.publish_family(id, 0, family_views, family_images);
        }
        owner.table.publish_family(rr::resource_id::hdr, 0, family_views, family_images);
        CHECK(pass.resolve(context, io));
        CHECK(io.own_per_image[0].size() == 2);                                   // one view per image of the generation
        CHECK(io.own_per_image[0][1] == family_views[1]);                         // ... in instance order
        CHECK(io.own_per_image[3].data() == family_views.data());                 // and it is the OWNER's run, not a copy
        CHECK(io.own[frame.image_index].view == family_views[frame.image_index]); // find() answers from the family too
        CHECK(io.targets[0].image == family_images[frame.image_index]);
        CHECK(owner.table.instances_of(rr::resource_id::hdr, 0) == 2);
        CHECK(owner.table.size() == 5); // the four bindings' families + the target's, one entry each

        // A MISSING TARGET is the same statement as a missing binding: do not record the pass
        owner.table.clear();
        for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
            owner.table.publish_family(id, 0, family_views, family_images);
        }
        CHECK(!pass.resolve(context, io));

        // THE EXTENT RULES, applied by the framework from the behaviour: half is the formula the half-size images
        // are created with, `resource` is the owner's answer, and `none` means the pass sizes its own work
        owner.table.publish_family(rr::resource_id::hdr, 0, family_views, family_images);
        pass.how.extent = vp::extent_rule::half;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 32 && io.extent.height == 16);
        pass.how.extent = vp::extent_rule::resource;
        pass.how.extent_of = rr::resource_id::bloom;
        pass.how.extent_of_element = 2;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 7 && io.extent.height == 9); // the owner's answer for that element
        pass.how.extent = vp::extent_rule::none;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 0 && io.extent.height == 0); // "I size my own work", not "the frame's size"

        // THE PIPELINES the behaviour names: resolved by name through the owner, and a name the owner cannot
        // answer means the frame cannot bind what the pass declared - so the pass does not run
        constexpr std::array<std::string_view, 1> pipeline_names = {"taa"};
        pass.how.extent = vp::extent_rule::full;
        pass.how.pipelines = pipeline_names;
        CHECK(pass.resolve(context, io));
        CHECK(io.pipelines.size() == 1);
        CHECK(io.pipelines[0] == owner.pipeline);
        owner.unknown_pipeline = "taa";
        CHECK(!pass.resolve(context, io));
        owner.unknown_pipeline = {};

        // ... and a pass that BUILT its own pipeline is handed its own, never the same-named registry entry: that
        // is the whole reason the interface asks the pass first
        pass.owned_pipeline = reinterpret_cast<VkPipeline>(0x1234);
        CHECK(pass.resolve(context, io));
        CHECK(io.pipelines.size() == 1);
        CHECK(io.pipelines[0] == pass.owned_pipeline);
        pass.owned_pipeline = VK_NULL_HANDLE;

        // A DECLARATION WITH NO BINDING OF ITS OWN IS STILL A DECLARATION: the framework has no set to hand a pass
        // (every stage reads its descriptors from the frame's heap), so such a declaration resolves with no own
        // binding and no pipeline, and the extent rule is still applied. (The shared-set entries this block used
        // to assert were declaration-only facts; they went with the vocabulary.)
        rr::pass_io const shared_only = {.name = "shared", .bindings = {}, .targets = {}, .push = std::nullopt};
        declared_pass shared_pass;
        shared_pass.declaration = &shared_only;
        vp::resolved_io shared_io = {};
        CHECK(shared_pass.resolve(context, shared_io));
        CHECK(shared_io.own.empty());                        // the declaration names no own binding
        CHECK(shared_io.pipelines.empty());                  // ... and no pipeline
        CHECK(shared_io.extent.width == frame.extent.width); // the extent rule is still applied
        CHECK(shared_pass.resolve(context, shared_io));      // resolving twice is idempotent (a per-frame contract)

        // The FAMILY/ELEMENT pair the post chain's stages used to name is gone with the shared-set vocabulary: those
        // stages now differ by the resource ELEMENT they render into and move, which the declarations carry as
        // `render_target::element` and `barrier_image::element` and the resolver hands over element by element.

        // A RUN OF ELEMENTS: ONE target entry claiming `count` consecutive elements resolves to ONE SLOT PER
        // ELEMENT - the shadow map's cascades are the case (`render_target::count`), and what makes it a
        // declaration fact rather than a resolver's is that the FRAME CAPS THE RUN: the family has the elements the
        // owner published (the layers the cascade knob asked for), and the pass renders what it is handed.
        std::array<rr::render_target, 1> const run_decl = {rr::render_target{.resource = rr::resource_id::shadow_map, .element = 0, .kind = rr::target_kind::depth, .count = 4}};
        rr::pass_io const run_io = {.name = "shadow", .bindings = {}, .targets = run_decl, .push = std::nullopt};
        declared_pass run_pass;
        run_pass.declaration = &run_io;
        // a per-FRAME-SLOT family, which is why the instance is the frame's SLOT rather than its image
        owner.table.clear();
        for (uint32_t layer = 0; layer < 3; ++layer) {
            // `0xC0 + layer` is 32 bits wide, and MSVC /W4 reports C4312 for a uint32_t reinterpret_cast to the
            // 64-bit VkImageView handle, so the fabricated handle is widened to pointer size first.
            owner.table.publish(rr::resource_id::shadow_map, layer, frame.slot,
                                {.view = reinterpret_cast<VkImageView>(static_cast<std::uintptr_t>(0xC0 + layer)), .image = reinterpret_cast<VkImage>(0xD0)});
        }
        vp::resolved_io run_out = {};
        CHECK(run_pass.resolve(context, run_out));
        CHECK(run_out.targets.size() == 3); // the run ends where the FRAME's elements end, not at the declaration's count
        CHECK(run_out.targets[0].view == reinterpret_cast<VkImageView>(0xC0));
        CHECK(run_out.targets[1].view == reinterpret_cast<VkImageView>(0xC1));
        CHECK(run_out.targets[2].view == reinterpret_cast<VkImageView>(0xC2));
        CHECK(run_out.targets[0].image == reinterpret_cast<VkImage>(0xD0)); // ONE image behind every layer of the run
        // ... a frame with ONE layer hands over one target (the single-shadow-map configuration: the same
        // declaration, a different frame) ...
        owner.table.clear();
        owner.table.publish(rr::resource_id::shadow_map, 0, frame.slot, {.view = reinterpret_cast<VkImageView>(0xC0), .image = reinterpret_cast<VkImage>(0xD0)});
        CHECK(run_pass.resolve(context, run_out));
        CHECK(run_out.targets.size() == 1);
        // ... and a frame with NONE does not run the pass at all, exactly like a missing single target
        owner.table.clear();
        CHECK(!run_pass.resolve(context, run_out));
    }

    return deren::vk_test::finish("test_pass");
}
