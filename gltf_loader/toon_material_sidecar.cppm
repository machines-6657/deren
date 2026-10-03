// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file gltf_loader/toon_material_sidecar.cppm
 * @brief The TOON MATERIAL SIDECAR: the `.toon.tsv` that sits beside a model and describes its toon
 *        materials, read into something a renderer can query.
 * @defgroup gltf_toon_material_sidecar Toon Material Sidecar
 *
 * WHY IT IS A MODULE OF ITS OWN rather than more code in `gltf_loader`: what it reads is NOT glTF. A glTF
 * material carries the five texture slots and the six factors the spec defines - base colour,
 * metallic-roughness, normal, occlusion, emissive - and a toon character needs a SECOND set that glTF has no
 * concept of: a diffuse ramp, a shadow LUT, a specular ramp, a matcap, a face SDF. Those live in a sidecar
 * file the asset pipeline writes next to the model, keyed by MATERIAL NAME, and their names are the asset
 * pipeline's rather than the spec's. Reading it is therefore a different job from reading glTF, and it is
 * kept separate for the same reason `vulkan_loader`-agnostic `vulkancorekit` does not link `gltf_loader`:
 * two formats, two concerns, two modules.
 *
 * THE FILE, which is tab-separated with a header row and four columns:
 * @code
 * material                 kind    name              value
 * M_actor_zhuangfy_body_01 slot    _DiffRampMap      T_actor_common_body_01_RD
 * M_actor_zhuangfy_body_01 slot    _ShadowLutTex     T_actor_commonfemaleskincolor01_lut_D
 * M_actor_zhuangfy_body_01 float   _UseDiffRampMap   1.0
 * M_actor_zhuangfy_body_01 float   _OutlineWidth     0.6
 * @endcode
 * The `value` column means two different things by `kind`: for `slot` it is a TEXTURE NAME (a bare asset name,
 * not a path and not an index), and for `float` it is a number. `color` appears in the asset pipeline's
 * vocabulary and is carried as text, because this module does not know what a consumer would do with it.
 *
 * THE TWO RULES THIS MODULE EXISTS TO KEEP, both of them the asset pipeline's stated contract rather than
 * this module's invention:
 *
 *  1. A MATERIAL'S FEATURE IS SWITCHED ON BY AN EXPLICIT `_Use<Slot>` FLAG, NEVER INFERRED FROM THE SLOT
 *     BEING PRESENT. `enabled()` implements exactly that, and it is why the sidecar carries a flag for every
 *     optional slot: a ramp declared but not enabled must stay off, and a consumer that inferred would
 *     silently turn on a feature the artist turned off.
 *  2. A MODEL WITH NO SIDECAR IS NOT AN ERROR. It is the normal case for every model that is not a
 *     character, so `load_sidecar` returns an EMPTY sidecar rather than a failure - and a consumer reads
 *     `.materials.empty()` as "no toon materials here", which is a fact rather than an error to handle.
 *
 * WHAT IT DOES NOT DO YET: it does not resolve a texture NAME to anything. The sidecar says
 * `T_actor_common_body_01_RD`; finding the image that name refers to - inside the glTF's images, or beside
 * it on disk - is the consumer's job and the next step. This module's contract ends at "the file says this".
 */

module;

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module deren.toon_material_sidecar;

import deren.vstd;

export namespace deren::toon {

    /// @brief the suffix the asset pipeline writes beside a model: `<model path>` + this
    /// @note INCLUDING the model's own extension, so `x.glb` is described by `x.glb.toon.tsv`. That is the
    ///       convention the files on disk follow, and reproducing it rather than inventing a cleaner one is
    ///       what lets this module find them without a search path or a manifest.
    inline constexpr std::string_view sidecar_suffix = ".toon.tsv";

    /// @brief the prefix of the flag that switches a slot on: `_DiffRampMap` is enabled by `_UseDiffRampMap`
    inline constexpr std::string_view enable_flag_prefix = "_Use";

    /**
     * @brief one material's toon description, as the sidecar declares it
     *
     * The two maps are keyed by the SIDECAR's names, which are the asset pipeline's: `_DiffRampMap` rather
     * than "diffuse ramp". Keeping them as written is deliberate - a translation table here would be a second
     * vocabulary to keep in step with the file, and the file is the contract.
     */
    struct material_sidecar {
        /// the material this entry describes, which is the name a glTF material must match by
        std::string name = {};
        /// kind `slot`: slot name -> texture NAME (a bare asset name, not a path and not an index)
        std::map<std::string, std::string, std::less<>> slots = {};
        /// kind `float`: name -> value. Booleans are floats in this file (`_Use*` is 0.0 or 1.0)
        std::map<std::string, float, std::less<>> scalars = {};
        /// kind `color` and anything else the pipeline writes: kept verbatim rather than dropped, so an
        /// unknown kind is visible to a consumer instead of silently missing
        std::map<std::string, std::string, std::less<>> others = {};

        /// @brief the texture name in @p slot_name, or an empty view when the material has no such slot
        /// @note the view points INTO this struct's storage: it stays valid as long as this material does
        [[nodiscard]] std::string_view slot(std::string_view slot_name) const noexcept;
        /// @brief the number in @p scalar_name, or @p fallback when the material has no such scalar
        [[nodiscard]] float scalar(std::string_view scalar_name, float fallback) const noexcept;
        /**
         * @brief whether the artist switched @p slot_name ON, which is the EXPLICIT flag and not the slot's
         *        presence
         * @param slot_name the slot's name as the sidecar writes it (`_DiffRampMap`)
         * @return the `_Use<Slot>` flag's value > 0.5, and FALSE when that flag is absent - the safe answer,
         *         because the contract is that a feature is off unless it was switched on
         */
        [[nodiscard]] bool enabled(std::string_view slot_name) const noexcept;
        /**
         * @brief whether the flag NAMED @p flag_name is switched on, for the slots whose flag is not
         *        `_Use<Slot>`
         *
         * WHY THIS EXISTS ALONGSIDE `enabled()`, and it is a fact about the asset pipeline rather than a
         * convenience: NOT EVERY SLOT'S FLAG IS `_Use<Slot>`. The ramp and LUT slots do follow it
         * (`_DiffRampMap`/`_UseDiffRampMap`, `_ShadowLutTex`/`_UseShadowLutTex`, `_SpecRampMap`/
         * `_UseSpecRampMap`), but the MATCAP slot `_MatcapTex` is switched on by `_UseMatcap` - the slot's
         * `Tex` suffix is not in the flag. A consumer that assumed the rule for every slot would ask for
         * `_UseMatcapTex`, find nothing, and answer "off" - correctly by this module's rules, and completely
         * wrongly by the asset pipeline's, silently, and only on the materials that have a matcap.
         *
         * The flag's name is therefore part of the ASSET PIPELINE'S VOCABULARY, exactly like the slot's name
         * is, and it belongs to the layer that knows that vocabulary rather than to a rule inferred here.
         * `enabled()` is the convenience for the slots that follow the convention and is built on this.
         *
         * @param flag_name the flag as the sidecar writes it, leading underscore included (`_UseMatcap`)
         * @return its value > 0.5, and FALSE when it is absent - the same safe answer as `enabled()`
         */
        [[nodiscard]] bool enabled_by_flag(std::string_view flag_name) const noexcept;
    };

    /// @brief a whole sidecar: one entry per material, in the file's order
    struct sidecar {
        std::vector<material_sidecar> materials = {};
        /// @brief how many lines were skipped: blank ones, the header, and any line that did not have the four
        ///        columns. Counted rather than logged here, because this module reports and its caller decides
        std::size_t skipped_lines = 0;
        /// @brief how many rows arrived for a material whose entry was ALREADY in the list, i.e. how many rows the
        ///        reader had to MERGE back into an existing entry (see `parse_sidecar`)
        ///
        /// A FILE'S ORDER IS NOT THE ONLY WAY TO WRITE ONE, and this counter is the evidence for that: a material's
        /// rows are usually contiguous, but APPENDING a row - the natural way to extend a sidecar, and how the
        /// rewritten chain's own rows were added - puts it in a second block at the end. The first version of this
        /// reader pushed a NEW entry whenever the name CHANGED, so such a file described one material TWICE and
        /// `find` returned the FIRST, i.e. the one WITHOUT the appended rows. MEASURED on
        /// `laevatain_goo.glb.toon.tsv`: `_UseGooBaseRamp` was in the file and absent from the entry the lookup
        /// received, so a whole shading arm never ran while every log line agreed the row was there. A non-zero
        /// count is therefore not an error - it is a file shape, seen and handled.
        std::size_t merged_rows = 0;

        /// @brief the entry for @p material_name, or nullptr when the sidecar does not describe it
        [[nodiscard]] material_sidecar const* find(std::string_view material_name) const noexcept;
        /// @brief whether this sidecar describes any toon material at all
        [[nodiscard]] bool empty() const noexcept {
            return this->materials.empty();
        }
    };

    /// @brief the path the sidecar for @p model_path would have (it need not exist)
    [[nodiscard]] std::filesystem::path sidecar_path_for(std::filesystem::path const& model_path);

    /**
     * @brief parse sidecar TEXT: the whole format, and the only place it is implemented
     * @param text the file's contents (LF or CRLF; blank lines and the header row are skipped)
     * @return the sidecar, or the reason the text could not be read
     * @note SEPARATE FROM load_sidecar SO THE FORMAT CAN BE TESTED WITHOUT A FILE, which is how the failure
     *       cases below are reachable: a malformed COLUMN COUNT is a different thing from a missing file, and
     *       only one of them is an error.
     */
    [[nodiscard]] std::expected<sidecar, std::string> parse_sidecar(std::string_view text);

    /**
     * @brief read the sidecar beside @p model_path
     * @return the parsed sidecar; an EMPTY one when the file does not exist (see the header's second rule);
     *         or the reason a file that DOES exist could not be read or parsed
     */
    [[nodiscard]] std::expected<sidecar, std::string> load_sidecar(std::filesystem::path const& model_path);

} // namespace deren::toon
