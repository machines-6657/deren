// The demo's implementation: the two callbacks the runtime asks for, and nothing else. Every branch below is a
// STAGE of this application's frame - the same names the runtime's frame loop uses for its stage structs - and what
// it does is what the runtime used to do inline: hand each pass its frame (built from the runtime's own data) and
// run the frame's ordering rules that belong to that stage.
//
// Moved out of `runtime.cpp` UNCHANGED in behaviour, and the order inside each branch is deliberate: the frame is
// set FIRST, then the ordering rules run, exactly where the two used to sit relative to `record_stage` - so the
// command stream is identical and the capture gate decides the move.

module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <glm/glm.hpp>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

module deren.vulkan.render_start_demo;

import deren.vulkan.constant_init;
import deren.utility;

namespace deren::vulkan {

    std::size_t render_start_demo::attach(runtime& self) noexcept {
        this->runtime_owner = &self;
        // ---- THE CHAIN, CONSTRUCTED HERE ----
        // The same passes the renderer used to construct for itself, in the order their create step must run in (the
        // chain's order IS that order). THE G-BUFFER DEBUG VIEW COMES FIRST, which used to be a CREATE-ORDER
        // constraint - the passes that read the G-buffer asked the owner for the G-buffer set's layout while they
        // were being created. That is gone with the layouts: a pass builds only its own pipeline now.
        this->chain.emplace<pass::gbuffer_debug_pass>();
        this->chain.emplace<pass::shadow_pass>();
        this->chain.emplace<pass::scene_pass>();
        this->chain.emplace<pass::transparent_pass>();
        // ... and the toon character stage, right after the blended geometry: it re-shades the OPAQUE leaves
        // over the lit frame, at depth-EQUAL, so it must come after the lighting stage that produced what it
        // overwrites and after the blends that composite over the same pixels.
        this->chain.emplace<pass::character_forward_pass>();
        // ... and the toon stage's SECOND rim: a fullscreen additive contour from the depth, right after the
        // surface it outlines and before the resolve, so the anti-aliasing sees it in the frame it belongs to.
        this->chain.emplace<pass::toon_screen_rim_pass>();
        // ... and the REWRITTEN chain's rim, in the SAME slot of the chain's order (right after the surface stage
        // it outlines, before the resolve) - the two are alternatives at run time, and their order only matters on
        // a frame neither records, where it does not.
        this->chain.emplace<pass::goo_rim_pass>();
        this->chain.emplace<pass::megalights_trace_pass>();
        this->chain.emplace<pass::megalights_temporal_pass>();
        this->chain.emplace<pass::taa_pass>();
        this->chain.emplace<pass::rt_shadow_pass>();
        this->chain.emplace<pass::cluster_pass>();
        this->chain.emplace<pass::deferred_pass>();
        this->chain.emplace<pass::post_composite_pass>();
        // ... the bloom chain: FOUR instances of ONE class, one per level. The LEVEL is what differs - the target it
        // writes, the transition it declares, its extent and the `mode` lane of its push block - and the order IS
        // the chain: each level reads the one before it.
        this->chain.emplace<pass::post_bloom_pass>(0u);
        this->chain.emplace<pass::post_bloom_pass>(1u);
        this->chain.emplace<pass::post_bloom_pass>(2u);
        this->chain.emplace<pass::post_bloom_pass>(3u);
        this->chain.emplace<pass::fxaa_pass>();
        // ... and the RESOLVE, after it: the render chain's display-referred image onto the presented swapchain.
        // It is the frame's LAST writer on the frames the chain runs below the output size, and it is mutually
        // exclusive with the FXAA pass above (see runtime::post_fxaa_active), so the two are alternatives in the
        // frame loop rather than a stack.
        this->chain.emplace<pass::upscale_pass>();
        this->passes = &this->chain;
        // LOOKED UP BY THE NAME THE DECLARATION CARRIES, which is the only key a chain gives: a cast is what turns
        // the declaration's owner into the type whose frame it wants. A pass this build does not have (its
        // declaration missing, or a variant of this app) stays null and is simply never fed.
        this->cluster = this->find<pass::cluster_pass>("cluster");
        this->shadow = this->find<pass::shadow_pass>("shadow");
        this->scene = this->find<pass::scene_pass>("scene");
        this->transparent = this->find<pass::transparent_pass>("transparent");
        this->character_forward = this->find<pass::character_forward_pass>("character_forward");
        this->toon_screen_rim = this->find<pass::toon_screen_rim_pass>("toon_screen_rim");
        this->goo_rim = this->find<pass::goo_rim_pass>("goo_rim");
        this->rt_shadow = this->find<pass::rt_shadow_pass>("rt_shadow");
        this->deferred = this->find<pass::deferred_pass>("deferred");
        this->taa = this->find<pass::taa_pass>("taa");
        this->gbuffer_debug_pass = this->find<pass::gbuffer_debug_pass>("gbuffer-debug");
        this->megalights_trace = this->find<pass::megalights_trace_pass>("megalights_trace");
        this->megalights_temporal = this->find<pass::megalights_temporal_pass>("megalights_temporal");
        this->composite = this->find<pass::post_composite_pass>("post_composite");
        this->fxaa = this->find<pass::fxaa_pass>("fxaa");
        this->upscale = this->find<pass::upscale_pass>("upscale");

        std::size_t found = 0;
        found += this->cluster != nullptr ? 1u : 0u;
        found += this->shadow != nullptr ? 1u : 0u;
        found += this->scene != nullptr ? 1u : 0u;
        found += this->transparent != nullptr ? 1u : 0u;
        found += this->character_forward != nullptr ? 1u : 0u;
        found += this->rt_shadow != nullptr ? 1u : 0u;
        found += this->deferred != nullptr ? 1u : 0u;
        found += this->taa != nullptr ? 1u : 0u;
        found += this->gbuffer_debug_pass != nullptr ? 1u : 0u;
        found += this->composite != nullptr ? 1u : 0u;
        found += this->fxaa != nullptr ? 1u : 0u;
        if (found != 15) {
            // NOT a fatal error: this demo is one application's chain, and a build of it that lacks a pass (a
            // shader that did not compile is the usual reason - that pass's own create step says why) renders
            // without it. Saying so once at startup is what keeps "the pass did not run" from looking like a
            // rendering bug. NO FORMAT ARGUMENT, and that is not a style choice: `deren::utility::log` with one crashes
            // THIS translation unit's code generation (clang 22.1.8, `EmitBuiltinNewDeleteCall` - a toolchain bug
            // this branch has seen twice).
            deren::utility::log("render_start_demo: a pass this demo wires is missing from the chain - it is not fed a frame and will not record");
        }
        // ---- AND HAND IT OVER ----
        // `set_pass_chain` binds this chain into the runtime's own frame structure (the stage sequence, the marks,
        // the renderer's work between the stages) and takes this demo's wiring for everything the runtime does not
        // know about those passes: their frames, the stage preambles, the results, the feature table.
        self.set_pass_chain(this->chain, this->wiring());
        // ---- THE TWO HOOKS THE PASSES CARRY, INSTALLED ONCE ----
        // NEITHER IS A PER-FRAME VALUE, which is why neither belongs in a frame any more: the overlay's draw is a
        // property of this renderer and of WHICH pass is the frame's last writer (the composite and the FXAA pass
        // both hold it, and each frame's own decision says which of them uses it), and the reflection's recording
        // is this application's second signal through the temporal pass's one pipeline. `attach` is where both are
        // known, so they are set here and the frame loop never writes into a pass's frame again.
        if (this->composite != nullptr) {
            this->composite->set_overlay(self.overlay_draw());
        }
        if (this->fxaa != nullptr) {
            this->fxaa->set_overlay(self.overlay_draw());
        }
        // ... and the resolve holds it too: on a frame the render chain is smaller than the output, THIS is the
        // frame's last writer, so the overlay has to be drawn inside its instance (at the OUTPUT extent - the
        // other reason it belongs here and not in the composite, whose instance covers the render extent).
        if (this->upscale != nullptr) {
            this->upscale->set_overlay(self.overlay_draw());
        }
        return found;
    }

    void render_start_demo::prepare(void* const owner, runtime::frame_services const& services, std::string_view const stage) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        // THE FRAME'S TOOLKIT, cached for the callbacks that are carried by a PASS's frame and therefore cannot be
        // handed it (the reflection's recording - see the member's own note).
        // ... and the one fact the renderer's own policy reads about a pass it no longer holds: whether the lighting
        // stage is in the flat render mode, which is what makes a screen-space effect pointless on such a frame. Published on
        // every stage prepare rather than once, because it is one bool and the app can flip it between frames.
        if (self.runtime_owner != nullptr && self.deferred != nullptr) {
            self.runtime_owner->set_scene_unlit(self.deferred->unlit());
        }
        // The stage names are the frame's own structure (the same names the runtime's stage structs carry), so this
        // switch is the frame ORDER written once, where the passes live. WHAT IS NOT HERE ANY MORE: the frames.
        // Every pass builds its own before this runs (see frame_pass::prepare_frame), so what is left per stage is
        // only what is genuinely this owner's - the frame-ORDER duties (whose first sample publishes an image) and
        // the one answer no one else can give a pass (whether this frame's structures exist).
        // THE THREE FRAMES THIS DEMO STILL HANDS OVER, and they are the three the runtime still builds (see
        // `frame_services`): each carries the renderer's own recording machinery - the per-slot secondary buffers
        // for the scene and the transparent pass, the per-cascade secondaries for the shadow - so the frame cannot
        // be composed by the pass alone yet. Every other frame is the pass's own.
        if (stage == "shadow") {
            if (self.shadow != nullptr) {
                self.shadow->set_frame(services.make_shadow_frame(services.owner));
            }
        } else if (stage == "scene") {
            if (self.scene != nullptr) {
                self.scene->set_frame(services.make_scene_frame(services.owner));
            }
        } else if (stage == "transparent") {
            if (self.transparent != nullptr) {
                self.transparent->set_frame(services.make_transparent_frame(services.owner));
            }
        } else if (stage == "character_forward") {
            // NO FRAME-ORDER DUTY of its own: this stage's two declared targets were both published long before
            // it runs (the lighting stage samples the depth, so `ensure_gbuffer_depth_sampled` is already what the
            // stage that first read it did), and the pass itself owns the two transitions between "sampled" and
            // "attachment" that it needs. So this branch hands over the frame - the leaf list and the pipeline
            // name - and nothing else.
            if (self.character_forward != nullptr) {
                self.character_forward->set_frame(services.make_character_forward_frame(services.owner));
            }
        } else if (stage == "toon_screen_rim") {
            // THIS STAGE'S FRAME-ORDER DUTY, and it is the reason it declares no barrier images: it SAMPLES the
            // G-buffer (the depth it compares and the albedo it lightens), so whoever samples that surface FIRST
            // this frame has to publish the G-buffer instance's attachment writes. Gated on the same predicate
            // the runner gates THIS STAGE on - which is the pass's own feature name, `toon_screen_rim`, and NOT
            // the surface stage's: the two were one name until the rewritten chain needed to silence the contour
            // without silencing the stage that draws the character, and a preamble gated on the wrong one would
            // touch the G-buffer on a frame this pass does not record.
            if (services.feature_active != nullptr && services.feature_active(services.owner, "toon_screen_rim")) {
                static_cast<void>(services.ensure_gbuffer_targets_sampled(services.owner, services.cmd, services.image_index));
                static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
            }
        } else if (stage == "goo_rim") {
            // THE SAME FRAME-ORDER DUTY as the article's contour above, and the same predicate shape: this stage
            // samples the G-buffer (the depth it compares, the normal and the material id it reads), so whoever
            // samples that surface FIRST this frame publishes the G-buffer instance's attachment writes. Gated on
            // THE PASS'S OWN FEATURE NAME, which the table below answers with `goo_toon_active()` - so a frame with
            // `[render] goo_toon = false` never touches the surface here either.
            if (services.feature_active != nullptr && services.feature_active(services.owner, "goo_rim")) {
                static_cast<void>(services.ensure_gbuffer_targets_sampled(services.owner, services.cmd, services.image_index));
                static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
            }
        } else if (stage == "rt_shadow") {
            // THIS STAGE'S FRAME-ORDER DUTY: it may be the first sampler of the stored surface this frame, and
            // whoever samples it FIRST publishes the G-buffer instance's attachment writes. Gated on the same
            // predicate the runner gates the stage on, so the frame never touches them on a frame it does not run.
            if (services.feature_active != nullptr && services.feature_active(services.owner, "rt_shadow")) {
                static_cast<void>(services.ensure_gbuffer_targets_sampled(services.owner, services.cmd, services.image_index));
                static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
            }
        } else if (stage == "megalights") {
            // THE SAME FRAME-ORDER DUTY as the ray-traced shadow's above, and it is the same constraint: this stage
            // runs between the G-buffer pass and the lighting stage, so it may be the FIRST sampler of the stored
            // surface this frame - the estimator reads the albedo, the normal, the material and the depth to evaluate
            // its sampled lights, and the G-buffer instance's attachment writes have to be published before those
            // reads. Gated on the same predicate the runner gates the stage on, so a frame that does not run it
            // touches nothing. (Measured: without this the pass dispatched against images still in their attachment
            // layout, which the validation layer reported as a descriptor/ layout mismatch and which left the
            // estimate at zero.)
            if (services.feature_active != nullptr && services.feature_active(services.owner, "megalights")) {
                static_cast<void>(services.ensure_gbuffer_targets_sampled(services.owner, services.cmd, services.image_index));
                static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
                // ... AND THE MOTION-VECTOR TARGET, which this stage's second pass is the first sampler of: the
                // resolve reprojects its history with it. This is the ordering rule the chain runs between its
                // two halves; here both passes are in one stage, so the rule runs before either records - correct
                // for the same reason, because nothing between them writes that target.
                static_cast<void>(services.ensure_velocity_sampled(services.owner, services.cmd, services.image_index));
            }
        } else if (stage == "deferred") {
            // ... and the same frame-order duty as the ray-traced shadow's above, for the same reason. Its frame is
            // the pass's own now (see pass::deferred_frame / frame_pass::prepare_frame).
            static_cast<void>(services.ensure_gbuffer_targets_sampled(services.owner, services.cmd, services.image_index));
            static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
        } else if (stage == "taa" || stage == "gbuffer_debug") {
            // BOTH STAGES HAND THE STORED SURFACE TO SAMPLERS THIS FRAME, and both are gated exactly as the runner
            // gates the stage. The motion-vector flag is CLEARED rather than published: the chain later in the
            // frame is what will publish that image, and clearing it here is what stops this stage's own accessor
            // from claiming it (and, on the debug view's path, from letting a later pass transition it twice).
            if (services.feature_active != nullptr && services.feature_active(services.owner, stage == "taa" ? "taa" : "gbuffer-debug")) {
                if (stage == "taa") {
                    // PUBLISHED, NOT CLEARED, for the TAA stage: its resolve samples the motion vectors and the
                    // host owns that transition now (the pass used to record the barrier itself, unconditionally -
                    // see vulkan/pass/taa.cpp). `ensure_velocity_sampled` does it only while the G-buffer's flag
                    // is armed and consumes it, so whichever stage samples the velocity first publishes it and
                    // the later ones are no-ops: the property that broke when the stochastic punctual lighting
                    // chain started sampling it before this stage.
                    static_cast<void>(services.ensure_velocity_sampled(services.owner, services.cmd, services.image_index));
                } else {
                    // The debug view only needs the flag out of its own accessor's way - its pass reads no motion
                    // vector - so clearing stays right for it.
                    services.require_velocity_publish(services.owner, services.image_index);
                }
                static_cast<void>(services.ensure_gbuffer_depth_sampled(services.owner, services.cmd, services.image_index));
            }
        } else if (stage == "post_composite") {
            // No frame work left: the composite composes its own (which target it writes, who draws the overlay,
            // whether this frame's bloom sum exists), and the overlay hook was installed once in `attach`.
        } else if (stage == "fxaa") {
            // ... and neither has the FXAA pass, whose frame is the overlay hook it was given in `attach`.
        } else if (stage == "upscale") {
            // ... AND NEITHER HAS THE RESOLVE, and that is the whole answer rather than an omission: its frame is
            // the overlay hook it was given in `attach`, it owns the two transitions it records (the LDR image it
            // reads and the swapchain it writes), and every value it pushes is either its own or the frame's -
            // so there is no frame-ORDER duty for this owner to run before it, which is exactly the shape the
            // composite's and FXAA's stages above have.
        }
        // A stage with no entry above is a stage whose pass wants nothing from this owner: the world-space probe
        // cache's declaration resolves every value it needs, and the frames of every other stage are the passes'
        // own (see frame_pass::prepare_frame).
    }

    void render_start_demo::collect(void* const owner, std::string_view const stage, runtime::frame_results& out) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        if (stage == "megalights") {
            // THE ONE ANSWER THIS STAGE GIVES THE FRAME LOOP: whether the temporal resolve wrote its accumulation,
            // which is what sets this IMAGE's history flag for the next frame. The chain's other answer
            // (`megalights_resolved`) is the run report's, because the lighting stage has to act on it in the SAME
            // frame - a distinction the two names keep: this one is about the next frame, that one about this one.
            out.megalights_temporal_resolved = self.megalights_temporal != nullptr && self.megalights_temporal->resolved();
            return;
        }
        if (stage == "taa") {
            // The camera UBO's `prev_view_proj` is only advanced when the resolve actually wrote a history: a
            // resolve that bailed out (no descriptor set) must not claim one.
            out.taa_wrote_history = self.taa != nullptr && self.taa->wrote_history();
        }
    }

    // =================================================================================================
    // THE APP'S KNOBS: the flag half goes to the runtime (its policy), the value half to the pass
    // =================================================================================================
    // The CLAMPS travel with the values (they are the same fact), which is why the argument for each of them now
    // lives in the pass that owns it rather than in the setter below.

    void render_start_demo::set_taa(bool const enabled, float const blend_static, float const blend_min) noexcept {
        bool const turned_on = this->runtime_owner != nullptr && this->runtime_owner->set_taa_enabled(enabled);
        if (this->taa == nullptr) {
            return;
        }
        this->taa->set_blend(blend_static, blend_min);
        if (turned_on) {
            // THE PASS's HALF OF THE OFF -> ON EDGE: whether each image's history holds anything is the pass's own
            // state, and the renderer's half (the matrix history and the jitter index) is what its setter just reset.
            this->taa->reset_history();
        }
    }

    void render_start_demo::set_megalights(bool const enabled, uint32_t const samples, float const min_weight, float const bias_floor, float const bias_grazing) noexcept {
        // The same split every knob with a runtime-side flag makes: the FLAG is the runtime's (it decides whether the deferred lighting
        // stage adds the punctual lights itself, so it is frame state the renderer publishes), the estimator's
        // NUMBERS are the pass's and are clamped there. The return value (the off -> on edge) is ignored: there is
        // no accumulation to restart until the temporal resolve lands.
        if (this->runtime_owner != nullptr) {
            static_cast<void>(this->runtime_owner->set_megalights_enabled(enabled));
        }
        if (this->megalights_trace != nullptr) {
            this->megalights_trace->set_estimator(samples, min_weight, bias_floor, bias_grazing);
        }
    }

    void render_start_demo::set_megalights_light_angle(float const radians) noexcept {
        // The estimator owns the angle, so this forwards like the other two setters do.
        if (this->megalights_trace != nullptr) {
            this->megalights_trace->set_light_angle(radians);
        }
    }

    void render_start_demo::set_megalights_accumulation(float const depth_tolerance, float const max_frames, float const spatial_sigma) noexcept {
        // The policy is the PASS's (see megalights_temporal_pass::set_accumulation), so this forwards the way the
        // estimator's own setter does.
        if (this->megalights_temporal != nullptr) {
            this->megalights_temporal->set_accumulation(depth_tolerance, max_frames);
            this->megalights_temporal->set_spatial(spatial_sigma);
        }
    }

    void render_start_demo::set_megalights_history_tolerance(float const depth_tolerance) noexcept {
        // The pass owns the accumulated value, so the tolerance moves by re-stating the policy it is part of:
        // the two other lanes keep what the pass already holds (see megalights_temporal_pass).
        if (this->megalights_temporal != nullptr) {
            this->megalights_temporal->set_accumulation(depth_tolerance, this->megalights_temporal->max_frames());
        }
    }

    void render_start_demo::set_gbuffer_channel(int32_t const channel) noexcept {
        if (this->gbuffer_debug_pass != nullptr) {
            this->gbuffer_debug_pass->set_channel(channel);
        }
    }

    void render_start_demo::set_unlit(bool const unlit) noexcept {
        if (this->deferred != nullptr) {
            this->deferred->set_unlit(unlit);
        }
        // ... and the renderer's own policy reads it (see set_scene_unlit): publish it here as well as per stage, so
        // a toggle that arrives between two frames is not one frame late.
        if (this->runtime_owner != nullptr) {
            this->runtime_owner->set_scene_unlit(unlit);
        }
    }

    void render_start_demo::set_upscale_filter(std::string_view const name) noexcept {
        // ONE MAPPING from the config's spelling to the pass's filter, here and nowhere else. EASU is the
        // fallback for an empty or unrecognised value, and the unrecognised case is LOGGED rather than silently
        // absorbed: a config key that appears to select a filter while the frame is produced by another one is
        // exactly the kind of dead setting this repository deletes instead of documenting.
        pass::upscale_filter filter = pass::upscale_filter::easu;
        if (name == "linear") {
            filter = pass::upscale_filter::linear;
        } else if (!name.empty() && name != "easu") {
            deren::utility::log("render_start_demo: [render] upscale = \"{}\" is not a filter this renderer has (linear, easu); using easu", name);
        }
        if (this->upscale != nullptr) {
            this->upscale->set_filter(filter);
        }
    }

    void render_start_demo::set_ssao(bool const enabled, float const radius, float const intensity, uint32_t const samples) noexcept {
        // CPU-side only (the same rule as the other render-mode knobs): the values are pushed with the lighting stage
        // each frame, so they are safe to change mid-run. THE VALUES AND THEIR CLAMPS ARE THE PASS'S (see
        // deferred_pass::set_ssao); the diagnostic below is about the SESSION rather than about the pass, which is
        // why it asks the runtime's registry and reports through the runtime's once-per-session logger.
        if (this->deferred != nullptr) {
            this->deferred->set_ssao(enabled, radius, intensity, samples);
        }
        if (enabled && this->runtime_owner != nullptr && !this->runtime_owner->feature_active("deferred")) {
            this->runtime_owner->warn_missing_feature("ssao", "screen-space AO has no effect: the G-buffer pass or its lighting stage was not created (see the startup log)");
        }
    }

    // =================================================================================================
    // THE FEATURE TABLE (moved out of runtime::active_features / feature_active / feature_available)
    // =================================================================================================
    // Every substitution below is the same one: `this-><runtime member>` became a FIELD of the facts the runtime
    // hands over, and `this->pass_ready("name")` became a question to the typed reference this demo found - so the
    // composition is unchanged and the two inputs it is made of now sit on their own sides of the seam.

    bool render_start_demo::feature_active(void* const owner, runtime::feature_facts const& facts, std::string_view const name) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        // the composed answers, each read by more than one branch below
        bool const gbuffer_debug = facts.gbuffer_debug && facts.gbuffer_pipeline && self.gbuffer_debug_pass != nullptr && self.gbuffer_debug_pass->ready();
        bool const shaded_scene = !gbuffer_debug && self.deferred != nullptr && self.deferred->ready() && facts.gbuffer_pipeline;
        // THE FLAT RENDER FLAG AND THE SSAO SWITCH LIVE IN THE LIGHTING PASS (they are its parameters), so the table
        // ASKS it - the same shape the other "ask the pass" answers below use. One copy of each
        // value, and the pass that pushes them is the one that owns them.
        bool const unlit = self.deferred != nullptr && self.deferred->unlit();

        // THE TWO GATES THAT USED TO BE THE FIRST LINE OF A RESOLVER. Both passes were "always active" before S3,
        // with the renderer's resolver returning false to skip them; the skip is the same, but the question now has
        // one name and one answer. What is NOT in these answers is the OTHER half of those old gates - whether this
        // frame's target generation exists: that is what the resource table already says, so a frame whose images
        // are not there fails the pass's own resolution. Two mechanisms, two questions.
        if (name == "scene") {
            return facts.gbuffer_pass;
        }
        if (name == "transparent") {
            return facts.transparent_pending;
        }
        if (name == "character_forward") {
            // THE RUNTIME'S COMPOSED PREDICATE (the knob AND this frame's opaque leaf list, see
            // feature_facts::character_forward_pending) AND the pass being ready. This ONE gate answers for the
            // SURFACE pass; the screen-space rim asks under its own name below, because the rewritten toon chain
            // must not wear that contour as well as the Goo rim (see `toon_screen_rim_pass::feature`).
            return facts.character_forward_pending && self.character_forward != nullptr && self.character_forward->ready();
        }
        if (name == "toon_screen_rim") {
            // THE SAME PREDICATE AS THE STAGE ABOVE PLUS "THE REWRITTEN CHAIN IS NOT THE ONE DRAWING" - and that
            // second half is the whole reason this is a name of its own.
            //
            // WHY THE REWRITTEN CHAIN MUST SILENCE IT: this pass is a fullscreen ADDITIVE contour (the article's
            // second rim, `toon_screen_rim.slang`), and the chain the rewrite follows has no such contour - its
            // screen-space piece is `DepthRim`, which step 2 defers. Left running with `goo_toon` on, the frame
            // would carry the article's contour AND the reference's rim, which is the same "two rims" defect
            // step 2 suppresses inside `toon_diffuse`, one stage over.
            //
            // `goo_toon_active()` IS THE RUNTIME'S OWN ANSWER to "is the rewritten chain drawing THIS frame",
            // and it is deliberately the same predicate `make_character_forward_frame` uses to pick the pipeline
            // name (`goo_toon_on && the pipeline exists`): the two must not be able to disagree, or a knob turned
            // on without the pipeline would silence the article's rim and draw nothing in its place.
            bool const goo_toon_active = self.runtime_owner != nullptr && self.runtime_owner->goo_toon_active();
            return facts.character_forward_pending && self.toon_screen_rim != nullptr && self.toon_screen_rim->ready() && !goo_toon_active;
        }
        if (name == "goo_rim") {
            // THE REWRITTEN CHAIN'S RIM, and it is the SAME predicate as the branch above with the sign of the
            // last term flipped - which is the whole reason the two are one name each rather than one name with a
            // mode: `feature_active` is asked once per NAME, so "the article's contour is off" and "the Goo rim is
            // on" have to be two answers that cannot drift apart. Both would be wrong the other way: a Goo rim
            // drawn over the old chain's frame would double the rim the old chain already computes inside
            // `toon_diffuse`, and an article contour left on under `goo_toon` is the same defect one stage over.
            //
            // IT IS ALSO THE GATE THE FRAME LOOP ASKS BEFORE RECORDING THE STAGE AT ALL (see runtime.frames.cppm),
            // so with `[render] goo_toon = false` the pass is not resolved, not recorded, and its stage preamble
            // does not publish the G-buffer - which is what makes every pre-existing capture scenario byte
            // identical rather than "identical because the shader wrote zero".
            bool const goo_toon_active = self.runtime_owner != nullptr && self.runtime_owner->goo_toon_active();
            return facts.character_forward_pending && self.goo_rim != nullptr && self.goo_rim->ready() && goo_toon_active;
        }
        if (name == "gbuffer-debug") {
            return gbuffer_debug;
        }
        if (name == "megalights") {
            // THE STOCHASTIC PUNCTUAL LIGHTING PASS'S OWN GATE: the runtime's composed predicate (the knob, the
            // deferred shading path, and the flat-render-mode exclusion - the pass evaluates the BRDF from the
            // G-buffer, and the flat mode's lighting stage returns the stored albedo instead) AND the pass having
            // built its pipeline. The same predicate is what the frame loop asks before recording the stage, so
            // the runner and the loop cannot disagree about whether the punctual lights were handled this frame.
            return facts.megalights && self.megalights_trace != nullptr && self.megalights_trace->ready();
        }
        if (name == "taa") {
            return facts.taa && self.taa != nullptr && self.taa->ready() && shaded_scene;
        }
        if (name == "fxaa") {
            return facts.fxaa;
        }
        if (name == "upscale") {
            // THE RUNTIME'S COMPOSED PREDICATE (`post_upscale_active`: the render chain is smaller than the
            // output AND the pass built its pipeline), relayed exactly as `fxaa` is - the runtime is what knows
            // the render scale and the pass registry, and this owner is what knows the chain. Nothing of the
            // pass's own is added here: its `ready()` half is already inside the fact, so the runner's gate and
            // the composite's target choice cannot disagree about whether the resolve runs.
            return facts.upscale;
        }
        if (name == "shadow") {
            // The shadow map is only read by the shading stages. The flat render mode samples nothing, so recording
            // the pass would be pure waste - it measured 0.22 ms of a 0.5 ms frame.
            return facts.shadow && self.shadow != nullptr && self.shadow->ready() && !unlit;
        }
        if (name == "rt_shadow") {
            // THE PASS'S GATE, in full: the knob and the extension PLUS "this frame's structure is built for the
            // slot". The second half is a FACT rather than a resource-table entry because an acceleration structure
            // is not a `resolved_binding` - it has a device address and no view, buffer or image.
            return facts.rt_shadow && self.rt_shadow != nullptr && self.rt_shadow->ready() && facts.structures_ready;
        }
        if (name == "clustered") {
            // Same argument as the shadow's: flat shading reads no light list, and with no active punctual light
            // there is nothing to sort in the first place.
            return facts.clustered && self.cluster != nullptr && self.cluster->ready() && facts.punctual_lights > 0.5f && !unlit;
        }
        if (name == "ssao") {
            return self.deferred != nullptr && self.deferred->ssao_enabled() && shaded_scene; // shader-side gate
        }
        if (name == "bloom") {
            return facts.bloom && self.composite != nullptr && self.composite->ready() && !gbuffer_debug;
        }
        if (name == "deferred") {
            // THE LIGHTING STAGE'S OWN GATE, and it is deliberately the SAME predicate the frame loop's branch uses:
            // the runner asks this before resolving the pass, and the frame loop asks it before recording the stage,
            // so one answer means the two cannot disagree about whether the lighting runs this frame.
            return facts.deferred_lit;
        }
        if (name == "unlit") {
            return unlit;
        }
        return false;
    }

    bool render_start_demo::feature_available(void* const owner, runtime::feature_facts const& facts, std::string_view const name) {
        // "CAN this feature run at all this session", which the overlay's menu and `log_feature_status()` ask - the
        // different question from `feature_active` above, and the one whose absent branch was a bound bug (the SSAO
        // group was never offered because `deferred` answered false).
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        if (name == "gbuffer-debug") {
            return facts.gbuffer_pipeline && self.gbuffer_debug_pass != nullptr && self.gbuffer_debug_pass->ready();
        }
        if (name == "deferred") {
            return self.deferred != nullptr && self.deferred->ready();
        }
        if (name == "taa") {
            return self.taa != nullptr && self.taa->ready();
        }
        if (name == "fxaa") {
            return self.fxaa != nullptr && self.fxaa->ready();
        }
        if (name == "character_forward") {
            // CAN it run at all, which is a different question from whether it is on: the overlay offers the
            // switch only when the chain has the pass AND the renderer registered the pipeline it binds. That
            // second half is the one that can be false - the pipeline needs the mesh stage, so a device without
            // VK_EXT_mesh_shader creates none - and an offered switch that would do nothing is exactly the bug
            // this function's header records.
            return self.character_forward != nullptr && self.character_forward->ready() && self.runtime_owner != nullptr && self.runtime_owner->character_forward_ready();
        }
        if (name == "shadow") {
            return self.shadow != nullptr && self.shadow->ready();
        }
        if (name == "clustered") {
            return self.cluster != nullptr && self.cluster->ready();
        }
        if (name == "megalights") {
            // BOTH passes, and not just the tracer: what the lighting stage adds is the temporal resolve's
            // ACCUMULATION, so a chain whose
            // resolve did not build has nothing to show and the overlay must not offer a switch that would do
            // nothing. (This branch was MISSING when the widgets were added, which is why they were invisible:
            // every `visible_when` on them was false.)
            return self.megalights_trace != nullptr && self.megalights_trace->ready() && self.megalights_temporal != nullptr && self.megalights_temporal->ready();
        }
        return false;
    }

    // =================================================================================================

    void render_start_demo::recreated([[maybe_unused]] void* const owner) {
        // This demo keeps no descriptor family of its own: every pass in the chain retires its own, which the
        // runner does through recreate_stage, so there is nothing left here to reset.
    }

} // namespace deren::vulkan
