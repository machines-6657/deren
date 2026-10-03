// The toon material sidecar's implementation: the tab-separated format, and the two rules the header states -
// an explicit `_Use` flag decides a feature, and a missing file is an EMPTY sidecar rather than a failure.
//
// WHY A MALFORMED ROW IS AN ERROR rather than a skipped line, which is the one judgement call in this file: a
// sidecar is written by an asset pipeline, so a row with the wrong column count means the pipeline and this
// reader disagree about the format - and the consequence of skipping it is a material that silently loses its
// ramp, which shows up as "the character looks slightly wrong" rather than as anything a build or a log would
// catch. Failing with the line number turns that into one line to read. Blank lines and the header are
// different: they are EXPECTED, so they are counted and passed over.

module;

#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

module deren.toon_material_sidecar;

import deren.vstd;

namespace deren::toon {

    std::string_view material_sidecar::slot(std::string_view const slot_name) const noexcept {
        auto const found = this->slots.find(slot_name);
        return found == this->slots.end() ? std::string_view{} : std::string_view(found->second);
    }

    float material_sidecar::scalar(std::string_view const scalar_name, float const fallback) const noexcept {
        auto const found = this->scalars.find(scalar_name);
        return found == this->scalars.end() ? fallback : found->second;
    }

    bool material_sidecar::enabled(std::string_view const slot_name) const noexcept {
        // THE CONVENTION IS THE FILE'S OWN: a slot `_DiffRampMap` is switched on by `_UseDiffRampMap`, and the
        // prefix replaces the slot's leading underscore rather than being inserted after it. A name that does
        // not start with one gets the prefix directly, so both spellings resolve.
        //
        // IT IS A CONVENTION AND NOT A LAW, which is why `enabled_by_flag` exists: `_MatcapTex` is switched on
        // by `_UseMatcap`. This builds the conventional name and asks the same question the explicit form does.
        std::string flag;
        flag.reserve(enable_flag_prefix.size() + slot_name.size());
        flag.append(enable_flag_prefix);
        flag.append(slot_name.starts_with('_') ? slot_name.substr(1) : slot_name);
        return this->enabled_by_flag(flag);
    }

    bool material_sidecar::enabled_by_flag(std::string_view const flag_name) const noexcept {
        // FALSE WHEN THE FLAG IS ABSENT, which is the safe answer and the second rule of the header: an
        // artist's switch is off unless it was switched on, and a consumer that inferred from the slot's
        // presence would turn on exactly the features the sidecar exists to keep off.
        return this->scalar(flag_name, 0.0f) > 0.5f;
    }

    material_sidecar const* sidecar::find(std::string_view const material_name) const noexcept {
        for (material_sidecar const& material : this->materials) {
            if (material.name == material_name) {
                return &material;
            }
        }
        return nullptr;
    }

    std::filesystem::path sidecar_path_for(std::filesystem::path const& model_path) {
        // `operator+=` APPENDS TO THE FILENAME rather than replacing the extension, which is what makes
        // `x.glb` become `x.glb.toon.tsv` instead of `x.toon.tsv`.
        std::filesystem::path result = model_path;
        result += std::string(sidecar_suffix);
        return result;
    }

    namespace {

        /// @brief split @p line on tabs into at most four fields
        /// @return how many fields the line had, so the caller can tell the header from a malformed row
        std::size_t split_row(std::string_view const line, std::array<std::string_view, 4>& out) noexcept {
            std::size_t count = 0;
            std::size_t start = 0;
            while (count < out.size()) {
                std::size_t const tab = line.find('\t', start);
                if (tab == std::string_view::npos) {
                    out[count++] = line.substr(start);
                    return count;
                }
                out[count++] = line.substr(start, tab - start);
                start = tab + 1;
            }
            // more than four fields: report five so the caller's `!= 4` test fires without a fifth slot
            return line.find('\t', start) == std::string_view::npos ? count : count + 1;
        }

        /// @brief the number in @p text, or nothing when it is not entirely a number
        /// @note `from_chars` rather than `stof`: this build has no exceptions, and a malformed sidecar must be
        ///       a returned error rather than a terminate
        std::optional<float> parse_number(std::string_view const text) {
            if (text.empty()) {
                return std::nullopt;
            }
            float value = 0.0f;
            auto const* const begin = text.data();
            auto const* const end = text.data() + text.size();
            auto const parsed = std::from_chars(begin, end, value);
            // the WHOLE field has to be the number: `1.0 ` or `1.0x` is a format disagreement, not a value
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                return std::nullopt;
            }
            return value;
        }

    } // namespace

    std::expected<sidecar, std::string> parse_sidecar(std::string_view const text) {
        sidecar result;
        std::string_view remaining = text;
        std::size_t line_number = 0;
        while (!remaining.empty()) {
            ++line_number;
            std::size_t const newline = remaining.find('\n');
            std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
            remaining = newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1); // CRLF: the file is written on Windows
            }
            if (line.empty()) {
                ++result.skipped_lines;
                continue;
            }
            std::array<std::string_view, 4> fields = {};
            std::size_t const count = split_row(line, fields);
            if (count != 4) {
                return std::unexpected(std::format("line {}: expected four tab-separated columns, found {}: '{}'", line_number, count, line));
            }
            if (fields[0] == "material") {
                // THE HEADER, recognised by its first column rather than by being the first line: a file
                // without it is still readable, and one that is not first is still a header.
                ++result.skipped_lines;
                continue;
            }
            // A MATERIAL'S ROWS ARE FOUND BY NAME AND NOT BY POSITION, because they do not have to be contiguous.
            // The first version of this reader pushed a new entry whenever the name CHANGED - right for a file
            // written material by material, WRONG for one extended by appending a row, which is the natural way to
            // add one. Such a file described a single material TWICE, `find` returned the first entry, and the
            // appended rows were silently missing from it: MEASURED on `laevatain_goo.glb.toon.tsv`, whose step-4
            // rows sat in a second block, so `_UseGooBaseRamp` never reached the lookup and a whole shading arm did
            // not run while every log line agreed the row was in the file (see `sidecar::merged_rows`). The entries
            // still keep the file's FIRST-appearance order, and the merge is counted so the caller can report a
            // shape it handled rather than one it assumed away.
            material_sidecar* material = nullptr;
            for (material_sidecar& existing : result.materials) {
                if (existing.name == fields[0]) {
                    material = &existing;
                    ++result.merged_rows;
                    break;
                }
            }
            if (material == nullptr) {
                material_sidecar fresh;
                fresh.name = std::string(fields[0]);
                result.materials.push_back(std::move(fresh));
                material = &result.materials.back();
            }
            std::string const key(fields[2]);
            if (fields[1] == "slot") {
                material->slots.insert_or_assign(key, std::string(fields[3]));
            } else if (fields[1] == "float") {
                std::optional<float> const value = parse_number(fields[3]);
                if (!value.has_value()) {
                    return std::unexpected(std::format("line {}: '{}' is not a number (material '{}', field '{}')", line_number, fields[3], fields[0], fields[2]));
                }
                material->scalars.insert_or_assign(key, *value);
            } else {
                // `color` and anything the pipeline adds later: KEPT VERBATIM rather than dropped, so an
                // unknown kind reaches a consumer as data instead of as a silently missing field. Not an error,
                // because the format is the pipeline's to extend and this reader's to carry.
                material->others.insert_or_assign(key, std::string(fields[3]));
            }
        }
        return result;
    }

    std::expected<sidecar, std::string> load_sidecar(std::filesystem::path const& model_path) {
        std::filesystem::path const path = sidecar_path_for(model_path);
        std::error_code code;
        bool const exists = std::filesystem::exists(path, code);
        if (code || !exists) {
            // THE NORMAL CASE FOR EVERY NON-CHARACTER MODEL, and that is why it is not an error: the caller
            // reads an empty sidecar as "no toon materials here", which is a fact rather than a failure to
            // handle. A file that EXISTS but cannot be read is a different thing and falls through below.
            return sidecar{};
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected(std::format("toon sidecar '{}' exists but could not be opened", path.string()));
        }
        std::string const text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto parsed = parse_sidecar(text);
        if (!parsed.has_value()) {
            return std::unexpected(std::format("toon sidecar '{}': {}", path.string(), parsed.error()));
        }
        return parsed;
    }

} // namespace deren::toon
