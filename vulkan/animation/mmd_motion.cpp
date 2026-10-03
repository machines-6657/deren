/**
 * @file mmd_motion.cpp
 * @brief VMD parser implementation for deren.vulkan.animation.mmd_motion
 *
 * Layout per the OpenMMD VMD specification (only 16 of a bone key's 64 interpolation bytes are
 * used) plus babylon-mmd's field map (src/Loader/Parser/vmdObject.ts) for the [4][4][4] indexing
 * and the trailing property-keyframe section.  Both were checked against a real 20 MB motion:
 * with those offsets every one of its 193525 keys decodes to MMD's default linear curve
 * (20,20,107,107) on all four channels, and the file parses to exactly its last byte.
 */
module;
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

module deren.vulkan.animation.mmd_motion;

import deren.vstd;
import deren.utility;

namespace deren::vulkan::animation {
    namespace {

        constexpr std::size_t vmd_header_bytes = 30;
        constexpr std::size_t vmd_model_name_bytes = 20;
        constexpr std::size_t vmd_name_bytes = 15;
        constexpr std::size_t vmd_bone_record_bytes = 111;
        constexpr std::size_t vmd_morph_record_bytes = 23;
        constexpr std::size_t vmd_camera_record_bytes = 61;
        constexpr std::size_t vmd_light_record_bytes = 28;
        constexpr std::size_t vmd_self_shadow_record_bytes = 9;
        constexpr std::size_t vmd_ik_name_bytes = 20;
        constexpr std::size_t vmd_ik_state_bytes = vmd_ik_name_bytes + 1;
        constexpr std::size_t vmd_property_header_bytes = 9; // frame u32 + visibility u8 + ik state count u32
        constexpr std::string_view vmd_signature = "Vocaloid Motion Data 0002";

        /**
         * Byte offsets of (x1, y1, x2, y2) inside a bone key's 64-byte interpolation block, per channel
         * (0 = X, 1 = Y, 2 = Z, 3 = R).
         *
         * The block is a [4][4][4] array that stores each value several times over, and the four channels
         * are NOT at a uniform stride: bytes 2 and 3 carry the physics-toggle flags, so Z's x1 and R's x1
         * live at 17 and 18 instead.  Reading every channel at the uniform stride (i.e. 2 and 3) still
         * yields a well-formed monotone curve, so the error is invisible in the output - with MMD's
         * default linear keys it silently reports (0,20,107,107) for Z and R instead of (20,20,107,107).
         */
        constexpr std::size_t vmd_bezier_x1[4] = {0, 1, 17, 18};
        constexpr std::size_t vmd_bezier_y1[4] = {4, 5, 6, 7};
        constexpr std::size_t vmd_bezier_x2[4] = {8, 9, 10, 11};
        constexpr std::size_t vmd_bezier_y2[4] = {12, 13, 14, 15};

        [[nodiscard]] std::uint32_t read_u32(std::uint8_t const* p) noexcept {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
                   (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
        }

        [[nodiscard]] float read_f32(std::uint8_t const* p) noexcept {
            std::uint32_t const bits = read_u32(p);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        /** VMD pads fixed-width name fields with NULs; the Shift-JIS bytes themselves are preserved. */
        [[nodiscard]] std::string read_name(std::uint8_t const* p, std::size_t width) {
            std::size_t n = 0;
            while (n < width && p[n] != 0u) {
                ++n;
            }
            return std::string(reinterpret_cast<char const*>(p), n);
        }

        /** @brief one channel's control points, read at the offsets the format actually uses */
        [[nodiscard]] mmd_bezier read_bezier(std::uint8_t const* block, std::size_t channel) noexcept {
            return mmd_bezier{static_cast<float>(block[vmd_bezier_x1[channel]]) / 127.0f,
                              static_cast<float>(block[vmd_bezier_y1[channel]]) / 127.0f,
                              static_cast<float>(block[vmd_bezier_x2[channel]]) / 127.0f,
                              static_cast<float>(block[vmd_bezier_y2[channel]]) / 127.0f};
        }

        /** @brief index of the first key at or after @p frame, assuming @p frames is sorted ascending */
        template <class key>
        [[nodiscard]] std::size_t upper_index(std::vector<key> const& keys, float frame) noexcept {
            std::size_t hi = 1;
            while (hi < keys.size() && static_cast<float>(keys[hi].frame) < frame) {
                ++hi;
            }
            return hi;
        }

    } // namespace

    mmd_bone_track const* mmd_motion::bone(std::string_view name) const noexcept {
        for (auto const& track : bones) {
            if (track.name == name) {
                return &track;
            }
        }
        return nullptr;
    }

    bool mmd_motion::sample_bone(std::string_view name, float frame, mmd_pose& out) const noexcept {
        mmd_bone_track const* const track = bone(name);
        if (track == nullptr || track->keys.empty()) {
            return false;
        }
        auto const& keys = track->keys;
        if (frame <= static_cast<float>(keys.front().frame)) {
            out.translation = keys.front().translation;
            out.rotation = keys.front().rotation;
            return true;
        }
        if (frame >= static_cast<float>(keys.back().frame)) {
            out.translation = keys.back().translation;
            out.rotation = keys.back().rotation;
            return true;
        }
        std::size_t const hi = upper_index(keys, frame);
        mmd_bone_key const& a = keys[hi - 1];
        mmd_bone_key const& b = keys[hi];
        float const span = static_cast<float>(b.frame) - static_cast<float>(a.frame);
        float const s = span > 0.0f ? (frame - static_cast<float>(a.frame)) / span : 0.0f;
        // Each channel is eased separately: MMD stores one bezier per axis, so the three position
        // components do not share a fraction, and the quaternion has a fourth curve of its own.
        out.translation = glm::vec3(glm::mix(a.translation.x, b.translation.x, a.ease_x.evaluate(s)),
                                    glm::mix(a.translation.y, b.translation.y, a.ease_y.evaluate(s)),
                                    glm::mix(a.translation.z, b.translation.z, a.ease_z.evaluate(s)));
        out.rotation = glm::slerp(a.rotation, b.rotation, a.ease_r.evaluate(s));
        return true;
    }

    float mmd_motion::sample_morph(std::string_view name, float frame) const noexcept {
        for (auto const& track : morphs) {
            if (track.name != name) {
                continue;
            }
            auto const& keys = track.keys;
            if (keys.empty()) {
                return 0.0f;
            }
            if (frame <= static_cast<float>(keys.front().frame)) {
                return keys.front().weight;
            }
            if (frame >= static_cast<float>(keys.back().frame)) {
                return keys.back().weight;
            }
            std::size_t const hi = upper_index(keys, frame);
            mmd_morph_key const& a = keys[hi - 1];
            mmd_morph_key const& b = keys[hi];
            float const span = static_cast<float>(b.frame) - static_cast<float>(a.frame);
            float const s = span > 0.0f ? (frame - static_cast<float>(a.frame)) / span : 0.0f;
            return glm::mix(a.weight, b.weight, s); // morph channels carry no easing curve
        }
        return 0.0f;
    }

    std::string escape_mmd_name(std::string_view raw) {
        static constexpr char digits[] = "0123456789abcdef";
        // A C hex escape swallows every hex digit that follows it, so "\x89E" is the single value
        // 0x89E rather than two bytes - which matters because a retarget table is generated from
        // this output.  When the byte after an escape would render as a literal hex digit the two
        // are separated by an empty literal, a no-op in C: "\x89""E".  Only printable ASCII can
        // collide, since every hex digit character is printable and every other byte renders as
        // "\x..", which begins with a backslash.
        auto const is_hex_digit = [](uint8_t const c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            auto const byte = static_cast<uint8_t>(raw[i]);
            if (byte >= 0x20u && byte < 0x7fu && byte != '\\' && byte != '"') {
                out.push_back(static_cast<char>(byte));
                continue;
            }
            out.push_back('\\');
            out.push_back('x');
            out.push_back(digits[(byte >> 4u) & 0xfu]);
            out.push_back(digits[byte & 0xfu]);
            if (i + 1 < raw.size() && is_hex_digit(static_cast<uint8_t>(raw[i + 1]))) {
                out += "\"\"";
            }
        }
        return out;
    }

    std::optional<mmd_motion> parse_mmd_motion(std::vector<std::uint8_t> const& bytes) {
        std::uint8_t const* const d = bytes.data();
        std::size_t const size = bytes.size();
        if (size < vmd_header_bytes + vmd_model_name_bytes) {
            return std::nullopt;
        }
        if (std::memcmp(d, vmd_signature.data(), vmd_signature.size()) != 0) {
            return std::nullopt;
        }

        mmd_motion motion;
        motion.model_name = read_name(d + vmd_header_bytes, vmd_model_name_bytes);
        std::size_t o = vmd_header_bytes + vmd_model_name_bytes;

        // --- bone keyframes ---------------------------------------------------------------
        if (o + 4 > size) {
            return std::nullopt;
        }
        std::uint32_t const bone_count = read_u32(d + o);
        o += 4;
        if (bone_count > (size - o) / vmd_bone_record_bytes) {
            return std::nullopt;
        }
        std::unordered_map<std::string, std::size_t> bone_track;
        for (std::uint32_t i = 0; i < bone_count; ++i) {
            std::uint8_t const* const rec = d + o;
            std::string name = read_name(rec, vmd_name_bytes);
            mmd_bone_key key;
            key.frame = read_u32(rec + 15);
            key.translation = glm::vec3(read_f32(rec + 19), read_f32(rec + 23), read_f32(rec + 27));
            // the file stores the quaternion as x, y, z, w; glm::quat's constructor takes w first
            key.rotation = glm::quat(read_f32(rec + 43), read_f32(rec + 31), read_f32(rec + 35),
                                     read_f32(rec + 39));
            key.ease_x = read_bezier(rec + 47, 0);
            key.ease_y = read_bezier(rec + 47, 1);
            key.ease_z = read_bezier(rec + 47, 2);
            key.ease_r = read_bezier(rec + 47, 3);
            key.physics_flags = static_cast<std::uint16_t>(rec[47 + 2]) |
                                (static_cast<std::uint16_t>(rec[47 + 3]) << 8u);
            auto const [it, inserted] = bone_track.try_emplace(name, motion.bones.size());
            if (inserted) {
                motion.bones.push_back(mmd_bone_track{std::move(name), {}});
            }
            motion.bones[it->second].keys.push_back(key);
            o += vmd_bone_record_bytes;
        }

        // --- morph keyframes --------------------------------------------------------------
        if (o + 4 > size) {
            return std::nullopt;
        }
        std::uint32_t const morph_count = read_u32(d + o);
        o += 4;
        if (morph_count > (size - o) / vmd_morph_record_bytes) {
            return std::nullopt;
        }
        std::unordered_map<std::string, std::size_t> morph_track;
        for (std::uint32_t i = 0; i < morph_count; ++i) {
            std::uint8_t const* const rec = d + o;
            std::string name = read_name(rec, vmd_name_bytes);
            mmd_morph_key key;
            key.frame = read_u32(rec + 15);
            key.weight = read_f32(rec + 19);
            auto const [it, inserted] = morph_track.try_emplace(name, motion.morphs.size());
            if (inserted) {
                motion.morphs.push_back(mmd_morph_track{std::move(name), {}});
            }
            motion.morphs[it->second].keys.push_back(key);
            o += vmd_morph_record_bytes;
        }

        // --- camera, light and self-shadow sections: sized but not consumed ----------------
        auto const skip_section = [&](std::size_t record_bytes) {
            if (o + 4 > size) {
                return false;
            }
            std::uint32_t const count = read_u32(d + o);
            o += 4;
            if (count > (size - o) / record_bytes) {
                return false;
            }
            o += static_cast<std::size_t>(count) * record_bytes;
            return true;
        };
        if (!skip_section(vmd_camera_record_bytes) || !skip_section(vmd_light_record_bytes) ||
            !skip_section(vmd_self_shadow_record_bytes)) {
            return std::nullopt;
        }

        // --- property keyframes (visibility + IK on/off states) ---------------------------
        // This section is not a bare IK list: every entry is a property keyframe carrying a
        // visibility flag and its own list of IK states.  Treating it as "count then 21-byte IK
        // records" swallows the 9-byte header and then reports a perfectly plausible record holding
        // an impossible frame number - 27266 in a motion that only reaches frame 3954.
        if (o + 4 <= size) {
            std::uint32_t const property_count = read_u32(d + o);
            o += 4;
            for (std::uint32_t i = 0; i < property_count; ++i) {
                if (o + vmd_property_header_bytes > size) {
                    return std::nullopt;
                }
                std::uint32_t const frame = read_u32(d + o);
                std::uint32_t const ik_state_count = read_u32(d + o + 5); // byte 4 is visibility
                o += vmd_property_header_bytes;
                if (ik_state_count > (size - o) / vmd_ik_state_bytes) {
                    return std::nullopt;
                }
                for (std::uint32_t j = 0; j < ik_state_count; ++j) {
                    mmd_ik_event event;
                    event.name = read_name(d + o, vmd_ik_name_bytes);
                    event.frame = frame;
                    event.enabled = d[o + vmd_ik_name_bytes] != 0u;
                    motion.ik.push_back(std::move(event));
                    o += vmd_ik_state_bytes;
                }
            }
        }
        motion.unparsed_bytes = size - o;

        // Keyframes are not guaranteed to be grouped or ordered in the file, so group them above and
        // order them here; the last frame any track reaches is the timeline length.
        std::uint32_t last = 0;
        for (auto& track : motion.bones) {
            std::sort(track.keys.begin(), track.keys.end(),
                      [](mmd_bone_key const& a, mmd_bone_key const& b) { return a.frame < b.frame; });
            if (!track.keys.empty() && track.keys.back().frame > last) {
                last = track.keys.back().frame;
            }
        }
        for (auto& track : motion.morphs) {
            std::sort(track.keys.begin(), track.keys.end(),
                      [](mmd_morph_key const& a, mmd_morph_key const& b) { return a.frame < b.frame; });
            if (!track.keys.empty() && track.keys.back().frame > last) {
                last = track.keys.back().frame;
            }
        }
        motion.last_frame = last;
        return motion;
    }

    std::optional<mmd_motion> load_mmd_motion(std::string const& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            return std::nullopt;
        }
        return parse_mmd_motion(bytes);
    }

    namespace {

        /**
         * The MMD standard humanoid subset, as (VMD name bytes, joint name).
         *
         * Every byte string here was read out of a real motion rather than typed from memory, and
         * test_mmd_motion_real_file_when_available asserts that each entry matches a bone in that
         * motion when VR_MMD_MOTION points at it.  That check is the point: comparing against a
         * synthetic file would only prove the table agrees with itself, so a mistyped byte would
         * survive it.
         */
        constexpr mmd_bone_alias mmd_alias_table[] = {
            {"\x91\x53\x82\xc4\x82\xcc\x90\x65", "root"},                                       // 全ての親
            {"\x83\x5a\x83\x93\x83\x5e\x81\x5b", "center"},                                     // センター
            {"\x83\x4f\x83\x8b\x81\x5b\x83\x75", "groove"},                                     // グルーブ
            {"\x8d\x98", "waist"},                                                              // 腰
            {"\x89\xba\x94\xbc\x90\x67", "lower_body"},                                         // 下半身
            {"\x8d\x98\x83\x4c\x83\x83\x83\x93\x83\x5a\x83\x8b\x8d\xb6", "waist_cancel_u5de6"}, // 腰キャンセル左
            {"\x8d\xb6\x91\xab", "leg_l"},                                                      // 左足
            {"\x8d\xb6\x82\xd0\x82\xb4", "knee_l"},                                             // 左ひざ
            {"\x8d\xb6\x91\xab\x8e\xf1", "ankle_l"},                                            // 左足首
            {"\x8d\x98\x83\x4c\x83\x83\x83\x93\x83\x5a\x83\x8b\x89\x45", "waist_cancel_u53f3"}, // 腰キャンセル右
            {"\x89\x45\x91\xab", "leg_r"},                                                      // 右足
            {"\x89\x45\x82\xd0\x82\xb4", "knee_r"},                                             // 右ひざ
            {"\x89\x45\x91\xab\x8e\xf1", "ankle_r"},                                            // 右足首
            {"\x8f\xe3\x94\xbc\x90\x67", "upper_body"},                                         // 上半身
            {"\x8f\xe3\x94\xbc\x90\x67\x32", "upper_body2"},                                    // 上半身2
            {"\x8d\xb6\x8c\xa8", "shoulder_l"},                                                 // 左肩
            {"\x8d\xb6\x98\x72", "arm_l"},                                                      // 左腕
            {"\x8d\xb6\x98\x72\x9d\x80", "arm_twist_l"},                                        // 左腕捩
            {"\x8d\xb6\x98\x72\x9d\x80\x31", "arm_twist_l_1"},                                  // 左腕捩1
            {"\x8d\xb6\x98\x72\x9d\x80\x32", "arm_twist_l_2"},                                  // 左腕捩2
            {"\x8d\xb6\x98\x72\x9d\x80\x33", "arm_twist_l_3"},                                  // 左腕捩3
            {"\x8d\xb6\x82\xd0\x82\xb6", "elbow_l"},                                            // 左ひじ
            {"\x8d\xb6\x8e\xe8\x9d\x80", "wrist_twist_l"},                                      // 左手捩
            {"\x8d\xb6\x8e\xe8\x9d\x80\x31", "wrist_twist_l_1"},                                // 左手捩1
            {"\x8d\xb6\x8e\xe8\x9d\x80\x32", "wrist_twist_l_2"},                                // 左手捩2
            {"\x8d\xb6\x8e\xe8\x9d\x80\x33", "wrist_twist_l_3"},                                // 左手捩3
            {"\x8d\xb6\x8e\xe8\x8e\xf1", "wrist_l"},                                            // 左手首
            {"\x8d\xb6\x90\x6c\x8e\x77\x82\x50", "finger_index_l_1"},                           // 左人指１
            {"\x8d\xb6\x90\x6c\x8e\x77\x82\x51", "finger_index_l_2"},                           // 左人指２
            {"\x8d\xb6\x90\x6c\x8e\x77\x82\x52", "finger_index_l_3"},                           // 左人指３
            {"\x8d\xb6\x92\x86\x8e\x77\x82\x50", "finger_middle_l_1"},                          // 左中指１
            {"\x8d\xb6\x92\x86\x8e\x77\x82\x51", "finger_middle_l_2"},                          // 左中指２
            {"\x8d\xb6\x92\x86\x8e\x77\x82\x52", "finger_middle_l_3"},                          // 左中指３
            {"\x8d\xb6\x8f\xac\x8e\x77\x82\x50", "finger_pinky_l_1"},                           // 左小指１
            {"\x8d\xb6\x8f\xac\x8e\x77\x82\x51", "finger_pinky_l_2"},                           // 左小指２
            {"\x8d\xb6\x8f\xac\x8e\x77\x82\x52", "finger_pinky_l_3"},                           // 左小指３
            {"\x8d\xb6\x96\xf2\x8e\x77\x82\x50", "finger_ring_l_1"},                            // 左薬指１
            {"\x8d\xb6\x96\xf2\x8e\x77\x82\x51", "finger_ring_l_2"},                            // 左薬指２
            {"\x8d\xb6\x96\xf2\x8e\x77\x82\x52", "finger_ring_l_3"},                            // 左薬指３
            {"\x8d\xb6\x90\x65\x8e\x77\x82\x4f", "finger_thumb_l_0"},                           // 左親指０
            {"\x8d\xb6\x90\x65\x8e\x77\x82\x50", "finger_thumb_l_1"},                           // 左親指１
            {"\x8d\xb6\x90\x65\x8e\x77\x82\x51", "finger_thumb_l_2"},                           // 左親指２
            {"\x89\x45\x8c\xa8", "shoulder_r"},                                                 // 右肩
            {"\x89\x45\x98\x72", "arm_r"},                                                      // 右腕
            {"\x89\x45\x98\x72\x9d\x80", "arm_twist_r"},                                        // 右腕捩
            {"\x89\x45\x98\x72\x9d\x80\x31", "arm_twist_r_1"},                                  // 右腕捩1
            {"\x89\x45\x98\x72\x9d\x80\x32", "arm_twist_r_2"},                                  // 右腕捩2
            {"\x89\x45\x98\x72\x9d\x80\x33", "arm_twist_r_3"},                                  // 右腕捩3
            {"\x89\x45\x82\xd0\x82\xb6", "elbow_r"},                                            // 右ひじ
            {"\x89\x45\x8e\xe8\x9d\x80", "wrist_twist_r"},                                      // 右手捩
            {"\x89\x45\x8e\xe8\x9d\x80\x31", "wrist_twist_r_1"},                                // 右手捩1
            {"\x89\x45\x8e\xe8\x9d\x80\x32", "wrist_twist_r_2"},                                // 右手捩2
            {"\x89\x45\x8e\xe8\x9d\x80\x33", "wrist_twist_r_3"},                                // 右手捩3
            {"\x89\x45\x8e\xe8\x8e\xf1", "wrist_r"},                                            // 右手首
            {"\x89\x45\x90\x6c\x8e\x77\x82\x50", "finger_index_r_1"},                           // 右人指１
            {"\x89\x45\x90\x6c\x8e\x77\x82\x51", "finger_index_r_2"},                           // 右人指２
            {"\x89\x45\x90\x6c\x8e\x77\x82\x52", "finger_index_r_3"},                           // 右人指３
            {"\x89\x45\x92\x86\x8e\x77\x82\x50", "finger_middle_r_1"},                          // 右中指１
            {"\x89\x45\x92\x86\x8e\x77\x82\x51", "finger_middle_r_2"},                          // 右中指２
            {"\x89\x45\x92\x86\x8e\x77\x82\x52", "finger_middle_r_3"},                          // 右中指３
            {"\x89\x45\x8f\xac\x8e\x77\x82\x50", "finger_pinky_r_1"},                           // 右小指１
            {"\x89\x45\x8f\xac\x8e\x77\x82\x51", "finger_pinky_r_2"},                           // 右小指２
            {"\x89\x45\x8f\xac\x8e\x77\x82\x52", "finger_pinky_r_3"},                           // 右小指３
            {"\x89\x45\x96\xf2\x8e\x77\x82\x50", "finger_ring_r_1"},                            // 右薬指１
            {"\x89\x45\x96\xf2\x8e\x77\x82\x51", "finger_ring_r_2"},                            // 右薬指２
            {"\x89\x45\x96\xf2\x8e\x77\x82\x52", "finger_ring_r_3"},                            // 右薬指３
            {"\x89\x45\x90\x65\x8e\x77\x82\x4f", "finger_thumb_r_0"},                           // 右親指０
            {"\x89\x45\x90\x65\x8e\x77\x82\x50", "finger_thumb_r_1"},                           // 右親指１
            {"\x89\x45\x90\x65\x8e\x77\x82\x51", "finger_thumb_r_2"},                           // 右親指２
            {"\x89\x45\x82\xc2\x82\xdc\x90\xe6", "u53f3_u3064_u307e_u5148"},                    // 右つま先
            {"\x89\x45\x91\xab\x49\x4b\x90\x65", "ik_leg_parent_r"},                            // 右足IK親
            {"\x89\x45\x91\xab\x82\x68\x82\x6a", "ik_leg_r"},                                   // 右足ＩＫ
            {"\x89\x45\x82\xc2\x82\xdc\x90\xe6\x82\x68\x82\x6a", "ik_toe_r"},                   // 右つま先ＩＫ
            {"\x8d\xb6\x82\xc2\x82\xdc\x90\xe6", "u5de6_u3064_u307e_u5148"},                    // 左つま先
            {"\x8d\xb6\x91\xab\x49\x4b\x90\x65", "ik_leg_parent_l"},                            // 左足IK親
            {"\x8d\xb6\x91\xab\x82\x68\x82\x6a", "ik_leg_l"},                                   // 左足ＩＫ
            {"\x8d\xb6\x82\xc2\x82\xdc\x90\xe6\x82\x68\x82\x6a", "ik_toe_l"},                   // 左つま先ＩＫ
            {"\x8e\xf1", "neck"},                                                               // 首
            {"\x93\xaa", "head"},                                                               // 頭
            {"\x89\x45\x91\xab\x44", "leg_r_u0044"},                                            // 右足D
            {"\x89\x45\x82\xd0\x82\xb4\x44", "knee_r_u0044"},                                   // 右ひざD
            {"\x89\x45\x91\xab\x8e\xf1\x44", "ankle_r_u0044"},                                  // 右足首D
            {"\x89\x45\x91\xab\x90\xe6\x45\x58", "toe_ex_r"},                                   // 右足先EX
            {"\x8d\xb6\x91\xab\x44", "leg_l_u0044"},                                            // 左足D
            {"\x8d\xb6\x82\xd0\x82\xb4\x44", "knee_l_u0044"},                                   // 左ひざD
            {"\x8d\xb6\x91\xab\x8e\xf1\x44", "ankle_l_u0044"},                                  // 左足首D
            {"\x8d\xb6\x91\xab\x90\xe6\x45\x58", "toe_ex_l"},                                   // 左足先EX
            {"\x97\xbc\x96\xda", "u4e21_u76ee"},                                                // 両目
            {"\x8d\xb6\x96\xda", "u5de6_u76ee"},                                                // 左目
            {"\x89\x45\x96\xda", "u53f3_u76ee"},                                                // 右目
        };

    } // namespace

    std::vector<mmd_bone_alias> const& mmd_bone_aliases() {
        static std::vector<mmd_bone_alias> const table(std::begin(mmd_alias_table),
                                                       std::end(mmd_alias_table));
        return table;
    }

    std::int32_t mmd_retarget::joint_of(std::size_t const bone_index) const noexcept {
        return bone_index < joint_of_bone.size() ? joint_of_bone[bone_index] : -1;
    }

    mmd_retarget build_mmd_retarget(mmd_motion const& motion,
                                    std::vector<std::string> const& joint_names) {
        std::unordered_map<std::string_view, std::size_t> joint_index;
        for (std::size_t i = 0; i < joint_names.size(); ++i) {
            joint_index.try_emplace(joint_names[i], i); // the first joint of a name wins
        }
        mmd_retarget result;
        result.joint_of_bone.assign(motion.bones.size(), -1);
        for (std::size_t bone = 0; bone < motion.bones.size(); ++bone) {
            for (mmd_bone_alias const& alias : mmd_bone_aliases()) {
                if (alias.mmd_name != motion.bones[bone].name) {
                    continue;
                }
                auto const joint = joint_index.find(alias.joint_name);
                if (joint != joint_index.end()) {
                    result.joint_of_bone[bone] = static_cast<std::int32_t>(joint->second);
                }
                break;
            }
            if (result.joint_of_bone[bone] < 0) {
                result.unresolved_bones.push_back(bone);
            } else {
                ++result.mapped;
            }
        }
        result.unmapped = result.unresolved_bones.size();
        return result;
    }

    mmd_two_bone_solution solve_two_bone(glm::vec3 const& hip, glm::vec3 const& knee,
                                         glm::vec3 const& ankle, glm::vec3 const& target,
                                         glm::vec3 const& pole) {
        mmd_two_bone_solution out;
        float const upper_length = glm::length(knee - hip);
        float const lower_length = glm::length(ankle - knee);
        glm::vec3 const to_target = target - hip;
        float const distance = glm::length(to_target);
        if (upper_length <= 1e-6f || lower_length <= 1e-6f || distance <= 1e-6f) {
            return out;
        }
        // the chain cannot change length, so an out-of-reach target is pulled onto the sphere it
        // CAN reach - and said so, rather than silently returning a straight limb
        float const max_reach = upper_length + lower_length;
        float const min_reach = std::fabs(upper_length - lower_length);
        if (distance > max_reach || distance < min_reach) {
            out.clamped = true;
        }
        float const d = std::min(std::max(distance, min_reach + 1e-4f), max_reach - 1e-4f);
        glm::vec3 const axis = to_target / distance;
        // the plane: everything perpendicular to the chain axis, with the pole projected into it
        glm::vec3 bend = pole - axis * glm::dot(pole, axis);
        if (glm::length(bend) <= 1e-5f) {
            glm::vec3 const fallback = std::fabs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                                : glm::vec3(1.0f, 0.0f, 0.0f);
            bend = fallback - axis * glm::dot(fallback, axis);
        }
        bend = glm::normalize(bend);
        // law of cosines: the angle at the hip between the chain and the hip -> target direction
        float const cosine = (d * d + upper_length * upper_length - lower_length * lower_length) /
                             (2.0f * d * upper_length);
        float const angle = std::acos(std::min(1.0f, std::max(-1.0f, cosine)));
        out.upper = axis * std::cos(angle) + bend * std::sin(angle);
        glm::vec3 const reach = hip + out.upper * upper_length;
        glm::vec3 const rest = target - reach;
        out.lower = glm::length(rest) > 1e-6f ? glm::normalize(rest) : out.upper;
        return out;
    }
    namespace {

        /** @brief shortest-arc rotation taking @p from to @p to (both directions need not be unit) */
        glm::quat rotate_between(glm::vec3 const& from, glm::vec3 const& to) {
            glm::vec3 const a = glm::normalize(from);
            glm::vec3 const b = glm::normalize(to);
            float const cosine = glm::dot(a, b);
            if (cosine > 1.0f - 1e-6f) {
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (cosine < -1.0f + 1e-6f) { // opposite: any perpendicular axis will do
                glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
                if (glm::length(axis) < 1e-4f) {
                    axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
                }
                return glm::angleAxis(3.14159265358979f, glm::normalize(axis));
            }
            glm::vec3 const axis = glm::cross(a, b);
            return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
        }

    } // namespace

    mmd_ik_result solve_leg_ik(glm::quat const& hip_local, glm::vec3 const& knee_offset,
                               glm::quat const& knee_local, glm::vec3 const& ankle_offset,
                               glm::quat const& parent_world, glm::vec3 const& hip_world,
                               glm::vec3 const& target_world, glm::vec3 const& pole_world) {
        mmd_ik_result out;
        glm::quat const hip_world_rotation = parent_world * hip_local;
        glm::vec3 const knee_world = hip_world + hip_world_rotation * knee_offset;
        glm::quat const knee_world_rotation = hip_world_rotation * knee_local;
        glm::vec3 const ankle_world = knee_world + knee_world_rotation * ankle_offset;

        mmd_two_bone_solution const solved =
            solve_two_bone(hip_world, knee_world, ankle_world, target_world, pole_world);
        out.clamped = solved.clamped;

        glm::quat const hip_delta = rotate_between(knee_world - hip_world, solved.upper);
        out.hip_local = glm::normalize(glm::inverse(parent_world) * (hip_delta * hip_world_rotation));

        // the knee's parent is the hip's NEW world rotation, and the lower bone has already been
        // carried along by hip_delta, so its own delta is measured after that
        glm::quat const new_hip_world = parent_world * out.hip_local;
        glm::quat const knee_delta = rotate_between(hip_delta * (ankle_world - knee_world), solved.lower);
        out.knee_local =
            glm::normalize(glm::inverse(new_hip_world) * (knee_delta * hip_delta * knee_world_rotation));
        return out;
    }
    clip bake_mmd_clip(mmd_motion const& motion, mmd_retarget const& retarget,
                       mmd_bake_options const& options) {
        clip baked;
        baked.name = motion.model_name;
        float const rate = options.frames_per_second > 0.0f ? options.frames_per_second
                                                            : mmd_motion::frames_per_second;
        float const last = options.last_frame >= 0.0f ? options.last_frame
                                                      : static_cast<float>(motion.last_frame);
        // MMD frame numbers and clip seconds are related by the SOURCE rate, not the bake rate:
        // frame f always sits at f / 30 seconds, however finely it is sampled.
        float const frames_per_step = mmd_motion::frames_per_second / rate;
        std::size_t const steps = static_cast<std::size_t>(last / frames_per_step) + 1;

        for (std::size_t bone = 0; bone < motion.bones.size(); ++bone) {
            std::int32_t const joint = retarget.joint_of(bone);
            if (joint < 0) {
                continue; // no counterpart on this skeleton, so there is nothing to drive
            }
            sampler translation;
            translation.per_key = 3;
            translation.interp = interpolation::linear;
            sampler rotation;
            rotation.per_key = 4;
            rotation.interp = interpolation::linear;
            for (std::size_t step = 0; step < steps; ++step) {
                float const frame = static_cast<float>(step) * frames_per_step;
                mmd_pose pose;
                if (!motion.sample_bone(motion.bones[bone].name, frame, pose)) {
                    continue;
                }
                float const seconds = frame / mmd_motion::frames_per_second;
                // A VMD position is an OFFSET in MMD's own space, so it takes the same transform the
                // skeleton was rigged with before it can serve as a node translation.  Writing it
                // verbatim is what stretched the mesh into a spike: every driven joint's local
                // translation was replaced by a value ~13.6x too large and with Z un-negated.
                translation.times.push_back(seconds);
                translation.values.push_back(pose.translation.x * options.axis_sign.x * options.scale);
                translation.values.push_back(pose.translation.y * options.axis_sign.y * options.scale);
                translation.values.push_back(pose.translation.z * options.axis_sign.z * options.scale);
                // the controller stores rotation keys as x, y, z, w
                rotation.times.push_back(seconds);
                rotation.values.push_back(pose.rotation.x);
                rotation.values.push_back(pose.rotation.y);
                rotation.values.push_back(pose.rotation.z);
                rotation.values.push_back(pose.rotation.w);
            }
            if (translation.times.empty()) {
                continue;
            }
            auto const joint_index = static_cast<std::size_t>(joint);
            std::size_t const translation_sampler = baked.samplers.size();
            baked.samplers.push_back(std::move(translation));
            std::size_t const rotation_sampler = baked.samplers.size();
            baked.samplers.push_back(std::move(rotation));
            baked.channels.push_back(
                channel{channel_path::translation, translation_sampler, joint_index});
            baked.channels.push_back(channel{channel_path::rotation, rotation_sampler, joint_index});
        }
        return baked;
    }

} // namespace deren::vulkan::animation
