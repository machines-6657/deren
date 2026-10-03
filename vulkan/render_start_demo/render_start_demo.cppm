// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/render_start_demo/render_start_demo.cppm
 * @brief THE EXAMPLE: this repository's own pass chain, wired from OUTSIDE the renderer.
 * @defgroup vulkan_render_start_demo Render Start Demo
 *
 * WHY THIS MODULE EXISTS. `deren.vulkan.runtime` is a frame loop: it owns the device, the pacing, the images and the
 * frame's constants, and it records whatever chain of passes it is given. Everything that is specific to THIS
 * application's renderer - which passes there are, in what order, what each one is handed per frame, and which
 * knob belongs to which one - used to live inside the runtime as sixteen typed members and eighty-nine call sites.
 * It lives here now, and the runtime knows the chain only by the names its declarations carry.
 *
 * WHAT IT IS: the app's wiring, and deliberately not a framework. It looks the passes up by declaration name, keeps
 * the typed references, and implements the two callbacks the runtime asks for (`prepare`, `collect`) by switching on
 * the STAGE name - which is the frame's own structure, so the seam needs no new vocabulary. A second application
 * that wants a different chain writes a module like this one; nothing in `deren.vulkan.pass` or `deren.vulkan.runtime` changes.
 *
 * WHERE IT IS HEADED: the knobs and their setters move here next (today they are
 * still the runtime's public API and forward into the passes); the descriptor families that used to be built from a
 * pass's set layout are gone with the heap (the runtime writes it and a pass owns only a pipeline); and finally the
 * CONSTRUCTION - at which point the runtime is handed the chain through `set_pass_chain` instead of owning it, and
 * the transitional `runtime::passes()` accessor goes away.
 */

module;

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.render_start_demo;

import deren.vulkan.runtime;
import deren.vulkan.constant_init;
import deren.vulkan.pass;
import deren.vulkan.pass.chain; // pass_chain: the chain the runtime owns its passes in, looked up by declaration name
import deren.vulkan.pass.cluster;
import deren.vulkan.pass.deferred;
import deren.vulkan.pass.fxaa;
import deren.vulkan.pass.geometry_buffer_debug;
import deren.vulkan.pass.post;
import deren.vulkan.pass.ray_traced_shadow;
import deren.vulkan.pass.scene;
import deren.vulkan.pass.shadow;
import deren.vulkan.pass.megalights_trace;
import deren.vulkan.pass.megalights_temporal;
import deren.vulkan.pass.taa;
import deren.vulkan.pass.transparent;
import deren.vulkan.pass.character_forward; // the toon character stage: the OPAQUE leaves re-shaded over the lit frame
import deren.vulkan.pass.toon_screen_rim;   // ... and its second rim: a fullscreen additive contour from the depth
import deren.vulkan.pass.goo_rim;           // ... and the REWRITTEN chain's rim: the same shape, the Goo reference's values
import deren.vulkan.pass.upscale;           // the resolve: the render chain's LDR image -> the presented swapchain

export namespace deren::vulkan {

    /**
     * @brief the demo's wiring: this app's passes, found by declaration name, fed stage by stage
     *
     * @note the object must outlive the renderer's recording (the runtime holds pointers into it), which is what a
     *       local in the application's own frame scope gives it
     */
    class render_start_demo {
    public:
        /**
         * @brief look this renderer's passes up by the names their declarations carry, and remember the runtime
         * @return how many of the passes it knows about were found; a pass that is missing is simply not fed, and
         *         a pass that is not fed records nothing - which is the seam's own failure mode rather than a crash
         */
        std::size_t attach(runtime& self) noexcept;

        /// @brief the two callbacks the runtime asks for, with this demo as their context
        [[nodiscard]] runtime::chain_wiring wiring() noexcept {
            return runtime::chain_wiring{.owner = this,
                                         .prepare = &render_start_demo::prepare,
                                         .collect = &render_start_demo::collect,
                                         .feature_active = &render_start_demo::feature_active,
                                         .feature_available = &render_start_demo::feature_available,
                                         .recreated = &render_start_demo::recreated};
        }

        // =============================================================================================
        // THE APP'S KNOBS for the passes this demo owns.
        //
        // EACH OF THESE IS THE SPLIT THE HANDOVER FORCES, and it is a real one: the renderer needs some flags for its
        // OWN policy (it jitters the projection for TAA, builds the structures for GI, picks the scene target), while
        // the VALUES are read by one pass each. So the demo forwards the flag half to the runtime and keeps the value
        // half on the pass - one owner per value, and the app calls one setter rather than two.
        // =============================================================================================
        /// @brief TAA on/off (`runtime::set_taa_enabled`) + the resolve's two blend weights (the pass's)
        void set_taa(bool enabled, float blend_static, float blend_min) noexcept;
        /// @brief GI on/off + the tracer's ray budget (intensity, reach as a fraction of the scene radius, rays, steps)
        /**
         * @brief stochastic punctual lighting: the flag goes to the runtime, the estimator's parameters to the pass
         * @param enabled true = the punctual lights are sampled and ray-traced instead of added unshadowed
         * @param samples samples per half-resolution pixel (the pass clamps to 1..4, its shader's bound)
         * @param min_weight the minimum sample weight below which a light's sampling weight rolls to zero
         * @param bias_floor / @param bias_grazing the ray origin's self-intersection offset at normal and at
         *        grazing incidence (a world-space length, like every other bias in this engine)
         * @note the split is the runtime-flag one: the FLAG is the runtime's policy (it is what tells the deferred
         *       lighting stage whether the punctual lights were already handled), the numbers are the pass's.
         */
        void set_megalights(bool enabled, uint32_t samples, float min_weight, float bias_floor, float bias_grazing) noexcept;
        /// @brief the emitter's angular radius (radians): 0 keeps the shadows hard
        void set_megalights_light_angle(float radians) noexcept;
        /// @brief the resolve's accumulation policy: the history's relative depth tolerance and the frame-count cap
        void set_megalights_accumulation(float depth_tolerance, float max_frames, float spatial_sigma) noexcept;
        /// @brief the same policy's history depth tolerance, moved on its own so a frame can A/B it
        void set_megalights_history_tolerance(float depth_tolerance) noexcept;
        /// @brief the spatial filter's width in GI texels (0 = a pass-through); the pass clamps it
        /// @brief the composite's joint-bilateral GI upsample switch
        /// @brief the multi-bounce gain the tracer re-emits at a hit (the pass clamps it to [0, 1])
        /// @brief the world-space cache: its flag (the runtime's), its rate and round count (the pass's) and the
        ///        tracer's gain over it
        /// @brief which G-buffer channel the debug view shows (the pass's own parameter)
        void set_gbuffer_channel(int32_t channel) noexcept;
        /// @brief the flat render mode, which the lighting stage's own parameter decides
        ///
        /// NOTE the whole setter moved here rather than its value half, and that is the honest shape: the render mode
        /// is a property of the LIGHTING STAGE (the shader returns the stored albedo), and the renderer's other
        /// features ask that pass for it - so there was never a second copy in the runtime to keep.
        void set_unlit(bool unlit) noexcept;
        /// @brief screen-space AO: the switch and its three shaping values, all of them the lighting pass's
        ///
        /// The SESSION-level diagnostic travels with it ("why does this switch do nothing?"), asked through the
        /// runtime's own answers rather than by reaching into it.
        void set_ssao(bool enabled, float radius, float intensity, uint32_t samples) noexcept;
        /// @brief WHICH FILTER resolves a scaled render chain onto the output (`[render] upscale`)
        ///
        /// @param name "easu" (FSR 1's upsampler, the default) or "linear" (the bilinear reference it is measured
        ///        against). IT TAKES THE CONFIG'S SPELLING rather than a pass enum, and that is the layering
        ///        rather than convenience: the app hands its setting over without having to know the pass
        ///        namespace, and the one mapping from a name to a filter lives next to the pass that implements it.
        ///        An unknown spelling resolves to EASU and says so in the log: a value that looks like a setting
        ///        but selects nothing is what makes "which filter produced this frame?" unanswerable.
        void set_upscale_filter(std::string_view name) noexcept;

    private:
        /// give every pass of the named stage its frame, and run the frame's ordering rules for that stage
        static void prepare(void* owner, runtime::frame_services const& services, std::string_view stage);
        /// report the stage's results back (see runtime::frame_results)
        static void collect(void* owner, std::string_view stage, runtime::frame_results& out);
        /**
         * @brief the REFLECTION's own accumulation, recorded at the end of the temporal pass's recording
         *
         * THE APP'S ONE DELIBERATE EXCEPTION, and it lives here rather than in the pass for a reason a declaration
         * cannot express: this application resolves TWO SIGNALS - the diffuse bounce and the glossy reflection -
         * through the temporal pass's ONE pipeline, and each needs its own list of images in the same seven slots.
         * The pass does the first and calls back for the second (its frame's `record_reflection`),
         * which is what keeps the chain contiguous.
         */
        /// @brief the reflection's per-image sets, on the temporal pass's layout (see record_reflection)
        /// one signal's accumulation: the barriers, the dispatch, the history copy and the hand-backs (mode 1)
        /**
         * @brief THE FEATURE TABLE: what runs this frame, composed from the runtime's facts and this demo's passes
         *
         * Moved out of `runtime::active_features` / `runtime::feature_active` UNCHANGED: the runtime reports the
         * facts (`runtime::feature_facts`) and this answers, which is the split every one of those answers was
         * already expressing by hand - "the knob AND the pass built its pipeline AND the frame has a surface".
         */
        static bool feature_active(void* owner, runtime::feature_facts const& facts, std::string_view name);
        /// ... and the different question "could this feature ever run this SESSION" (the overlay's menu + the log)
        static bool feature_available(void* owner, runtime::feature_facts const& facts, std::string_view name);
        /// the swapchain was rebuilt: retire the family this demo keeps outside the chain
        static void recreated(void* owner);

        /// the typed references, looked up once by `attach` (a pass whose declaration is missing stays null)
        template <typename PassT>
        [[nodiscard]] PassT* find(std::string_view const name) noexcept {
            return static_cast<PassT*>(this->passes != nullptr ? this->passes->find(name) : nullptr);
        }

        pass::pass_chain* passes = nullptr;
        /**
         * THIS APPLICATION'S CHAIN, which this demo OWNS: `attach` constructs the passes into it and hands it to the
         * runtime (`set_pass_chain`), so the runtime holds no pass of its own and this object owns them all.
         */
        pass::pass_chain chain{"render"};
        /// named `runtime_owner` rather than `runtime`: the type of this very pointer is `deren::vulkan::runtime`, and
        /// a member called `runtime` would hide that type inside this class's scope
        runtime* runtime_owner = nullptr; // the flag halves of the knobs above are the runtime's policy
        /**
         * The frame's services, as last handed to `prepare`, and the reflection's family.
         *
         * The SERVICES are cached because the reflection's callback is carried by a PASS's frame - whose signature
         * the pass owns (`record_reflection(owner, cmd, history_valid)`) - so the toolkit it needs (the device, the
         * frame's table, its constants) has to be reachable from the demo rather than passed through the pass. They
         * are a value of pointers to the runtime, valid for the frame that set them.
         */
        pass::cluster_pass* cluster = nullptr;
        pass::shadow_pass* shadow = nullptr;
        pass::scene_pass* scene = nullptr;
        pass::transparent_pass* transparent = nullptr;
        /// THE TOON CHARACTER STAGE: the same OPAQUE leaves again, through the character-forward pipeline, over
        /// the lit frame. Owned by the chain and looked up by its declaration name like every other pass here.
        pass::character_forward_pass* character_forward = nullptr;
        /// THE TOON STAGE'S SECOND RIM: a fullscreen additive contour sampling the depth, run right after the
        /// surface stage. Gated by the SAME `character_forward` feature - the contour means nothing without the
        /// shading it outlines, so the two are one switch.
        pass::toon_screen_rim_pass* toon_screen_rim = nullptr;
        /// THE REWRITTEN CHAIN'S RIM (`deren.vulkan.pass.goo_rim`): the same fullscreen additive shape as the contour
        /// above, with the Goo reference's numbers instead of the article's, and MUTUALLY EXCLUSIVE with it - the
        /// feature table below answers this one with `goo_toon_active()` and that one with its negation.
        pass::goo_rim_pass* goo_rim = nullptr;
        pass::rt_shadow_pass* rt_shadow = nullptr;
        pass::deferred_pass* deferred = nullptr;
        pass::taa_pass* taa = nullptr;
        /// named `gbuffer_debug_pass` rather than `gbuffer_debug`: `feature_active` keeps a local
        /// `gbuffer_debug`, and a member of that name would be hidden by it - MSVC /W4 reports C4458, which /WX makes an error
        pass::gbuffer_debug_pass* gbuffer_debug_pass = nullptr;
        /// the stochastic punctual lighting pass (docs/megalights.md): the estimator's parameters live on the
        /// pass, and this owner is what forwards them
        pass::megalights_trace_pass* megalights_trace = nullptr;
        /// the chain's temporal resolve (docs/megalights.md): the accumulation policy is the pass's
        pass::megalights_temporal_pass* megalights_temporal = nullptr;
        pass::post_composite_pass* composite = nullptr;
        pass::fxaa_pass* fxaa = nullptr;
        /// THE RESOLVE (deren.vulkan.pass.upscale): the render chain's LDR image -> the presented swapchain, the
        /// frame's last writer whenever the chain runs below the output size. It is fed no frame by this owner
        /// - the overlay hook it carries is installed once in `attach`, like the composite's and FXAA's - so
        /// this view exists for that hook and for the feature table's relay.
        pass::upscale_pass* upscale = nullptr;
    };

} // namespace deren::vulkan
