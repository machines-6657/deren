// Headless unit tests: THE DOCUMENTS ARE PART OF THE BUILD =====================================
// The durable reasoning of this project lives in `docs/*.md`, and since the mesh migration those
// files are also doxygen INPUT: they are rendered into the HTML manual and, through docs/latex, into
// refman.pdf. That makes them a BUILD INPUT whose failure modes nobody sees in a diff - which is what
// this test is for. Five invariants for the documents, plus one for the CI workflow, and each one is
// a mistake that has actually happened here or would be silent:
//
//  1. EVERY `docs/*.md` IS IN Doxyfile's INPUT, or named in the exception list below with a reason.
//     MEASURED: nine of twelve were outside it until commit 0ba6d22 - mesh_shaders.md (the mesh
//     migration's own record), slang_migration.md and seven more were simply not in the manual, and
//     nothing anywhere said so. A directory input is not the fix: `docs/` also holds doxygen's own
//     output and two sets of third-party material, which is why the list is explicit.
//  2. EVERY INPUT DOCUMENT IS ASCII. The manual's LaTeX runs these files through pdflatex, and a
//     non-ASCII character aborts or corrupts that build (the rule docs/shaders.md states). The four
//     documents added in 0ba6d22 had to be converted first - em dashes, arrows, section signs, a
//     box-drawing tree and U+1D40 - and `docs/mainpage.md` still had three em dashes. CI has no
//     LaTeX, so the checkable half of that rule is checked here, by byte.
//  3. EVERY `](#anchor)` LINK RESOLVES to a heading in the SAME file. doxygen accepts an anchor it
//     cannot resolve without a warning of its own, so renaming a heading leaves a link that quietly
//     goes nowhere - and this repo's documents cross-reference themselves by section heavily.
//  4. CODE FENCES AND `@code`/`@endcode` ARE BALANCED. A truncated document, or a diff that eats one
//     fence, changes what doxygen thinks is code: the manual is mangled rather than the build failing.
//  5. EVERY `heap_slots_<name>` THE DOCUMENTS NAME IS A DECLARED SLOT, in the host's grid or in the
//     shaders' constants. A renamed slot leaves the documents describing a resource that does not
//     exist, and a heap has no reflection to notice: those constants ARE the source of truth (which is
//     the same reason test_render_resources compares the two files that hold them).
//  6. THE CI WORKFLOW'S TEST LIST EQUALS CMakeLists' `VR_TEST_TARGETS`. The workflow says so in a
//     comment and the comment is right about why: a target missing from the `--target` list is an
//     executable ctest cannot find, and the job goes red as "Not Run". MEASURED: it had already
//     drifted - ten targets in CMake, nine in the ASan job (`test_meshlet` missing) - which is the
//     same failure the comment describes happening twice before this list existed.
//
// NOT CHECKED HERE, deliberately: whether a link's TARGET FILE exists (the documents link to code
// they describe, and several name files that were deliberately removed and marked as history - a rule
// for that needs the history to be machine-readable first), and anything about the rendered manual,
// which needs doxygen and pdflatex and is therefore the build_docs step's business rather than CI's.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
    /// `docs/*.md` that is deliberately NOT in Doxyfile's INPUT, with the reason it is not. Empty on
    /// purpose: every document on disk is in the manual today, and adding a name here is the explicit
    /// decision that it should not be - which is the point of the list being here rather than implied.
    struct exception_entry {
        char const* name;
        char const* reason;
    };
    // `std::array<..., 0>`, not `exception_entry[]`: the list is empty BY DESIGN above, and a zero-length array is
    // an MSVC error (C2466 + C2131) where clang accepts it as an extension. The range-for below reads it unchanged.
    constexpr std::array<exception_entry, 0> not_in_manual = {};

    /// `heap_slots_<name>` spellings the documents use that are NOT slot names, with the reason.
    struct identifier_exception {
        char const* name;
        char const* reason;
    };
    constexpr identifier_exception not_a_slot[] = {
        {"heap_slots_bloom_lN", "a placeholder for the four bloom levels in a table, not a declaration"},
    };

    std::string read_text(std::string const& path) {
        std::ifstream file(path, std::ios::binary);
        CHECK_MSG(file.is_open(), path.c_str());
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::vector<std::string> read_lines(std::string const& path) {
        std::vector<std::string> lines;
        std::istringstream stream(read_text(path));
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
        return lines;
    }

    std::string trim(std::string text) {
        auto const not_space = [](uint8_t const c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
        text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
        return text;
    }

    /// THE ANCHOR A MARKDOWN HEADING GETS: lowercase, everything that is not a letter, a digit, a space
    /// or a hyphen dropped, spaces turned into hyphens. "## 8. Animations" answers `#8-animations`.
    std::string heading_anchor(std::string heading) {
        std::string anchor;
        for (char const c : trim(std::move(heading))) {
            auto const byte = static_cast<uint8_t>(c);
            if (byte >= 'A' && byte <= 'Z') {
                anchor.push_back(static_cast<char>(byte - 'A' + 'a'));
            } else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-') {
                anchor.push_back(c);
            } else if (byte == ' ' || byte == '\t') {
                anchor.push_back('-');
            }
        }
        return anchor;
    }

    /// every `heap_slots_<name>` spelling inside @p text, deduplicated. The name is read as [A-Za-z0-9_]
    /// rather than as the lowercase convention, so that a document's METAVARIABLE (`heap_slots_bloom_lN`,
    /// where the capital N stands for a level) is read whole and can be named in the exception list
    /// exactly as the document spells it.
    std::set<std::string> slots_named_in(std::string const& text) {
        std::set<std::string> names;
        std::size_t at = text.find("heap_slots_");
        while (at != std::string::npos) {
            std::size_t end = at + std::string("heap_slots_").size();
            while (end < text.size()) {
                char const c = text[end];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
                    ++end;
                } else {
                    break;
                }
            }
            names.insert(text.substr(at, end - at));
            at = text.find("heap_slots_", end);
        }
        return names;
    }

    /// the lines of Doxyfile's INPUT value, continuations joined: every token it lists
    std::vector<std::string> doxygen_input_tokens(std::string const& doxyfile_text) {
        std::vector<std::string> tokens;
        std::istringstream stream(doxyfile_text);
        std::string line;
        bool in_input = false;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!in_input) {
                std::string const trimmed = trim(line);
                std::size_t const equals = trimmed.find('=');
                if (equals == std::string::npos || trim(trimmed.substr(0, equals)) != "INPUT") {
                    continue; // not the INPUT tag (INPUT_ENCODING and friends are different tags)
                }
                in_input = true;
                line = trimmed.substr(equals + 1u);
            }
            bool continues = false;
            std::string const trimmed = trim(line);
            if (!trimmed.empty() && trimmed.back() == '\\') {
                continues = true;
                line = trimmed.substr(0, trimmed.size() - 1u);
            } else if (!trimmed.empty()) {
                line = trimmed;
            }
            std::istringstream values(line);
            std::string token;
            while (values >> token) {
                tokens.push_back(token);
            }
            if (!continues) {
                break;
            }
        }
        return tokens;
    }

    /// the `test_*` targets CMakeLists' VR_TEST_TARGETS declares
    std::set<std::string> vr_test_targets(std::string const& cmake_text) {
        std::set<std::string> targets;
        std::istringstream stream(cmake_text);
        std::string line;
        while (std::getline(stream, line)) {
            std::size_t const at = line.find("VR_TEST_TARGETS");
            if (at == std::string::npos || line.find("set(", at == 0 ? 0u : at - 4u) == std::string::npos) {
                continue;
            }
            std::size_t token = line.find("test_", at);
            while (token != std::string::npos) {
                std::size_t end = token;
                while (end < line.size()) {
                    char const c = line[end];
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                        ++end;
                    } else {
                        break;
                    }
                }
                targets.insert(line.substr(token, end - token));
                token = line.find("test_", end);
            }
            if (!targets.empty()) {
                break;
            }
        }
        return targets;
    }

    /// the `test_*` targets the CI workflow builds: EVERY `--target` block's value, merged. Merging rather than
    /// taking the first is the fix for a measured own-goal: the workflow's FIRST `--target` is the clang-format
    /// job's (`cmake --build build --target clang-format-check`), whose value is on its own line and ends the
    /// block - so a first-only parser read an empty list and reported every target as missing.
    std::set<std::string> ci_built_targets(std::string const& workflow_text) {
        std::set<std::string> targets;
        std::vector<std::string> const lines = [&workflow_text] {
            std::vector<std::string> out;
            std::istringstream stream(workflow_text);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                out.push_back(line);
            }
            return out;
        }();
        auto collect = [&targets](std::string const& text) {
            std::size_t token = text.find("test_");
            while (token != std::string::npos) {
                std::size_t end = token;
                while (end < text.size()) {
                    char const c = text[end];
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                        ++end;
                    } else {
                        break;
                    }
                }
                targets.insert(text.substr(token, end - token));
                token = text.find("test_", end);
            }
        };
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::size_t const marker = lines[i].find("--target");
            if (marker == std::string::npos) {
                continue;
            }
            collect(lines[i].substr(marker + std::string("--target").size())); // a value on the same line
            // ... and the value continues on the following lines until the YAML block ends: a blank line, a
            // comment, or a line that is a `key:` at any indent (the next step of the job)
            for (std::size_t j = i + 1u; j < lines.size(); ++j) {
                std::string const trimmed = trim(lines[j]);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed.find(':') != std::string::npos) {
                    break;
                }
                collect(trimmed);
            }
        }
        return targets;
    }

    bool is_exception(std::string const& name) {
        for (exception_entry const& entry : not_in_manual) {
            if (name == entry.name) {
                return true;
            }
        }
        return false;
    }

    bool is_not_a_slot(std::string const& name) {
        for (identifier_exception const& entry : not_a_slot) {
            if (name == entry.name) {
                return true;
            }
        }
        return false;
    }
} // namespace

int32_t main() {
    std::filesystem::path const root = VR_TEST_SOURCE_DIR;
    std::filesystem::path const docs = root / "docs";

    // ---- 1. every document is in the manual, and everything in the manual exists ----
    std::vector<std::string> const inputs = doxygen_input_tokens(read_text((root / "Doxyfile").string()));
    std::set<std::string> input_docs;
    for (std::string const& token : inputs) {
        std::string const prefix = "./docs/";
        if (token.rfind(prefix, 0) == 0 && token.ends_with(".md")) {
            input_docs.insert(token.substr(prefix.size()));
        }
    }
    CHECK_MSG(!input_docs.empty(), "Doxyfile's INPUT lists at least one ./docs/*.md");
    std::vector<std::string> on_disk;
    for (auto const& file : std::filesystem::directory_iterator(docs)) {
        if (file.is_regular_file() && file.path().extension() == ".md") {
            on_disk.push_back(file.path().filename().string());
        }
    }
    CHECK_MSG(!on_disk.empty(), "docs/ holds markdown documents");
    for (std::string const& name : on_disk) {
        if (is_exception(name)) {
            continue;
        }
        CHECK_MSG(input_docs.contains(name), ("a document is not in Doxyfile's INPUT, so it is not in the manual (add it, or add it to this test's exception list with a reason): docs/" + name).c_str());
    }
    for (std::string const& name : input_docs) {
        CHECK_MSG(std::filesystem::exists(docs / name), ("Doxyfile's INPUT names a document that does not exist: docs/" + name).c_str());
    }

    // ---- 2..5 are per document ----
    std::size_t checked_anchors = 0;
    for (std::string const& name : input_docs) {
        std::filesystem::path const path = docs / name;
        if (!std::filesystem::exists(path)) {
            continue; // reported above
        }
        std::string const text = read_text(path.string());
        std::string const label = "docs/" + name;

        // 2. ASCII: the manual's LaTeX feeds these files to pdflatex, which the project's own rule says
        //    must never see a non-ASCII byte (docs/shaders.md). Report WHERE, so the fix is one edit.
        {
            std::size_t line = 1;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n') {
                    ++line;
                    continue;
                }
                auto const byte = static_cast<uint8_t>(text[i]);
                if (byte >= 0x80u) {
                    std::string const where = label + ":" + std::to_string(line) + " has a non-ASCII byte (the LaTeX manual runs these documents through pdflatex)";
                    CHECK_MSG(false, where.c_str());
                    break;
                }
            }
            CHECK_MSG(true, (label + " is ASCII").c_str());
        }

        // 3. anchors resolve inside the document, headings and doxygen sections both answering
        {
            std::set<std::string> anchors;
            for (std::string const& line : read_lines(path.string())) {
                std::string const trimmed = trim(line);
                if (!trimmed.empty() && trimmed[0] == '#') {
                    std::size_t hashes = 0;
                    while (hashes < trimmed.size() && trimmed[hashes] == '#') {
                        ++hashes;
                    }
                    if (hashes <= 6u) {
                        std::string title = trim(trimmed.substr(hashes));
                        while (!title.empty() && title.back() == '#') {
                            title.pop_back();
                        }
                        anchors.insert(heading_anchor(title));
                    }
                }
                for (std::string const marker : {"@section ", "@subsection "}) {
                    std::size_t const at = trimmed.find(marker);
                    if (at == std::string::npos || at != 0u) {
                        continue;
                    }
                    std::istringstream value(trimmed.substr(std::string(marker).size()));
                    std::string name_token;
                    value >> name_token;
                    anchors.insert(name_token);
                }
            }
            std::size_t at = text.find("](#");
            while (at != std::string::npos) {
                std::size_t const end = text.find(')', at);
                if (end == std::string::npos) {
                    break;
                }
                // `](#anchor)`: the three bytes before the anchor are ']', '(' and '#', and the stored
                // anchors carry no '#'
                std::string const anchor = text.substr(at + 3u, end - at - 3u);
                ++checked_anchors;
                CHECK_MSG(anchors.contains(anchor), ("a markdown anchor does not resolve to a heading in the same document: " + label + " -> " + anchor).c_str());
                at = text.find("](#", end);
            }
        }

        // 4. fences and doxygen code blocks are balanced: an odd count means doxygen reads prose as code
        {
            std::size_t fences = 0;
            std::size_t code = 0;
            std::size_t endcode = 0;
            for (std::string const& line : read_lines(path.string())) {
                std::string const trimmed = trim(line);
                if (trimmed.rfind("```", 0) == 0) {
                    ++fences;
                }
                std::size_t at = trimmed.find("@code");
                while (at != std::string::npos) {
                    ++code;
                    at = trimmed.find("@code", at + 5u);
                }
                at = trimmed.find("@endcode");
                while (at != std::string::npos) {
                    ++endcode;
                    at = trimmed.find("@endcode", at + 8u);
                }
            }
            CHECK_MSG(fences % 2u == 0u, ("a code fence is unclosed, so doxygen reads prose as code: " + label).c_str());
            CHECK_MSG(code == endcode, ("@code and @endcode are unbalanced: " + label).c_str());
        }

        // 5. the slot names the documents quote exist: the host grid and the shaders' constants are the
        //    two files that declare them (test_render_resources owns the comparison between those two)
        {
            std::string const grid = read_text((root / "vulkan" / "core" / "core.declarations.cppm").string()) +
                                     read_text((root / "shaders" / "heap_slot_constants.glsl").string());
            std::set<std::string> const declared = slots_named_in(grid);
            for (std::string const& slot : slots_named_in(text)) {
                if (is_not_a_slot(slot)) {
                    continue;
                }
                CHECK_MSG(declared.contains(slot), ("a document names a heap slot that no file declares: " + slot + " (in " + label + ")").c_str());
            }
        }
    }
    CHECK_MSG(checked_anchors >= 1u, "the documents contain at least one markdown anchor link to check");

    // ---- 6. the CI workflow builds every target CMake registers (and nothing CMake does not) ----
    {
        std::set<std::string> const registered = vr_test_targets(read_text((root / "CMakeLists.txt").string()));
        std::set<std::string> const built = ci_built_targets(read_text((root / ".github" / "workflows" / "ci.yml").string()));
        CHECK_MSG(registered.size() >= 10u, "VR_TEST_TARGETS parses as the full list of targets");
        CHECK_MSG(!built.empty(), "the CI workflow's --target list parses");
        for (std::string const& target : registered) {
            CHECK_MSG(built.contains(target), ("a test target CMake registers is not built by the CI workflow, so ctest there reports it as Not Run: " + target).c_str());
        }
        for (std::string const& target : built) {
            CHECK_MSG(registered.contains(target), ("the CI workflow builds a test target CMake does not register: " + target).c_str());
        }
    }

    return deren::vk_test::finish("test_docs");
}
