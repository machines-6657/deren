// Headless unit tests: THE SHADER PIPELINE'S SOURCES OF TRUTH ================================
// One .spv is named in FOUR places, and nothing in the build compares them:
//
//   1. CMakeLists.txt's `VR_SLANG_SOURCES` - the canonical list, and what `cmake --build` compiles;
//   2. shaders/compile_shaders.ps1 - the escape hatch for a machine without CMake;
//   3. shaders/compile_shaders.sh - its POSIX twin;
//   4. the C++ that LOADS the .spv by name at runtime (chores.cpp registers them, and a pass or the
//      runtime asks for one by name).
//
// A drift between 1 and 2/3 is silent: the build produces the file, the script does not, and the
// escape hatch quietly builds one shader fewer - which is exactly what was found the moment this test
// was written (`heap_probe.slang:mesh_main:mesh:heap_probe.mesh.spv` was in CMakeLists.txt and in
// neither script, so a machine using the scripts would have lost the mesh probe). A drift between 1
// and 4 is louder (a load logs "no shader"), but it is the same class of mistake and it is checkable
// here too.
//
// THIS IS THE MESH MIGRATION'S CI GATE, and it is deliberately able to FAIL: the list must contain the
// three MESH entries (a mesh stage is a different stage type, so its .spv name is new - deleting one is
// how "the mesh path" would silently stop being built), every `mesh_main` a .slang file declares must
// have an entry, and every entry's source must exist. The capture gate cannot run in CI at all (its
// references are tied to one machine's driver); this can.
#include "vk_test.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {
    /// the lines of a file, or an empty vector when it cannot be read (the CHECKs below report that)
    std::vector<std::string> read_lines(std::string const& path) {
        std::vector<std::string> lines;
        std::ifstream file(path);
        CHECK_MSG(file.is_open(), path.c_str());
        std::string line;
        while (std::getline(file, line)) {
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

    /// Is @p text one of the `<source>:<entry>:<stage>:<output>` entries? Four non-empty fields, a .slang
    /// source and a .spv output - the SHAPE is the contract, so a line that merely mentions a shader (a
    /// comment, a log message) cannot pass for an entry.
    bool is_entry(std::string const& text, std::string* const output = nullptr) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            std::size_t const colon = text.find(':', start);
            fields.push_back(text.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
        if (fields.size() != 4u || fields[0].empty() || fields[1].empty() || fields[2].empty() || fields[3].empty()) {
            return false;
        }
        if (fields[0].find(".slang") != fields[0].size() - 6u) {
            return false;
        }
        if (fields[3].find(".spv") != fields[3].size() - 4u) {
            return false;
        }
        if (output != nullptr) {
            *output = fields[3];
        }
        return true;
    }

    /// Every entry in @p path that is a quoted PowerShell list element or a bare list line, i.e. both scripts'
    /// spellings, with the trailing comma/quote stripped.
    std::set<std::string> entries_in_script(std::string const& path) {
        std::set<std::string> entries;
        for (std::string line : read_lines(path)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.front() == '"') {
                line.erase(line.begin());
            }
            if (!line.empty() && line.back() == ',') {
                line.pop_back();
            }
            if (!line.empty() && line.back() == '"') {
                line.pop_back();
            }
            line = trim(line);
            if (is_entry(line)) {
                entries.insert(line);
            }
        }
        return entries;
    }

    /// The entries of CMakeLists.txt's `set(VR_SLANG_SOURCES ...)` block: the block's own lines, from the
    /// `set(` to the closing `)`.
    std::set<std::string> cmake_entries(std::string const& path) {
        std::set<std::string> entries;
        bool inside = false;
        for (std::string line : read_lines(path)) {
            std::string const text = trim(line);
            if (!inside) {
                if (text.find("set(VR_SLANG_SOURCES") != std::string::npos) {
                    inside = true;
                }
                continue;
            }
            if (!text.empty() && text.front() == ')') {
                break;
            }
            if (is_entry(text)) {
                entries.insert(text);
            }
        }
        return entries;
    }

    /// The .spv names the runtime's app-side loader asks for, as `std::string_view`-style literals in
    /// chores.cpp (`load_shader(shaders_dir, "x.spv", ...)`, `register_shader("x.spv", ...)`).
    std::set<std::string> loaded_spv_names(std::string const& path) {
        std::set<std::string> names;
        for (std::string const& line : read_lines(path)) {
            for (std::string_view const marker : {".spv\"", ".spv\","}) {
                std::size_t at = line.find(marker);
                while (at != std::string::npos) {
                    // walk back to the opening quote of the literal
                    std::size_t const open = line.rfind('"', at);
                    if (open != std::string::npos) {
                        std::string const name = line.substr(open + 1u, at + 4u - open - 1u);
                        if (name.size() > 4u && name.find(".spv") != std::string::npos) {
                            names.insert(name);
                        }
                    }
                    at = line.find(marker, at + 1u);
                }
            }
        }
        return names;
    }
} // namespace

int32_t main() {
    std::string const root = std::string(VR_TEST_SOURCE_DIR);

    std::set<std::string> const cmake = cmake_entries(root + "/CMakeLists.txt");
    std::set<std::string> const ps1 = entries_in_script(root + "/shaders/compile_shaders.ps1");
    std::set<std::string> const sh = entries_in_script(root + "/shaders/compile_shaders.sh");

    // ---- 1. The canonical list is non-empty and every source file it names exists ----
    CHECK_MSG(!cmake.empty(), "VR_SLANG_SOURCES parsed out of CMakeLists.txt");
    for (std::string const& entry : cmake) {
        std::string const source = entry.substr(0, entry.find(':'));
        CHECK_MSG(std::filesystem::exists(root + "/shaders/" + source), entry.c_str());
    }

    // ---- 2. The two scripts compile EXACTLY the canonical list (this is the drift that was found) ----
    auto const report_difference = [](char const* who, std::set<std::string> const& expected, std::set<std::string> const& actual) {
        for (std::string const& entry : expected) {
            CHECK_MSG(actual.contains(entry), (std::string(who) + " is missing " + entry).c_str());
        }
        for (std::string const& entry : actual) {
            CHECK_MSG(expected.contains(entry), (std::string(who) + " has an entry the build does not: " + entry).c_str());
        }
    };
    report_difference("compile_shaders.ps1", cmake, ps1);
    report_difference("compile_shaders.sh", cmake, sh);

    // ---- 3. THE MESH ENTRIES EXIST, and every `mesh_main` a source declares has one ----
    // A mesh stage is a different stage type, so its output name is NEW: deleting one of these is how the
    // mesh path would stop being built without anything failing. Both directions are checked - the names
    // this renderer's mesh passes load, and any `mesh_main` a .slang file grows in the future.
    std::set<std::string> outputs;
    for (std::string const& entry : cmake) {
        std::string output;
        CHECK(is_entry(entry, &output));
        outputs.insert(output);
        if (entry.find(":mesh_main:mesh:") != std::string::npos) {
            CHECK_MSG(entry.find(".mesh.spv") != std::string::npos, entry.c_str());
        }
    }
    for (std::string const& required : {"shadow.mesh.spv", "pbr.mesh.spv", "heap_probe.mesh.spv"}) {
        CHECK_MSG(outputs.contains(required), required.c_str());
    }
    for (auto const& file : std::filesystem::directory_iterator(root + "/shaders")) {
        if (!file.is_regular_file() || file.path().extension() != ".slang") {
            continue;
        }
        bool declares_mesh_entry = false;
        for (std::string const& line : read_lines(file.path().string())) {
            if (line.find("void mesh_main(") != std::string::npos) {
                declares_mesh_entry = true;
            }
        }
        if (!declares_mesh_entry) {
            continue;
        }
        std::string const source = file.path().filename().string();
        std::size_t const entries_for_source = static_cast<std::size_t>(std::count_if(cmake.begin(), cmake.end(), [&source](std::string const& entry) {
            return entry.rfind(source + ":mesh_main:mesh:", 0) == 0;
        }));
        CHECK_MSG(entries_for_source == 1u, ("declares mesh_main with no (or a duplicated) VR_SLANG_SOURCES entry: " + source).c_str());
    }

    // ---- 4. Every .spv the APP loads by name is produced by the list ----
    // The names chores.cpp hands to load_shader/register_shader are the ones the runtime answers, so one that
    // no rule produces is a shader that is silently empty at startup - which is what a renamed output looks
    // like from the app's side.
    for (std::string const& name : loaded_spv_names(root + "/chores.cpp")) {
        CHECK_MSG(outputs.contains(name), ("chores.cpp loads a .spv no VR_SLANG_SOURCES entry produces: " + name).c_str());
    }

    // ---- 5. EVERY #include A LEAF PULLS IN IS A BUILD DEPENDENCY OF THE RULE THAT COMPILES IT ----
    // This is the check that was missing when the mesh geometry fetch arrived: `mesh_geometry.slang` was included
    // by every geometry leaf and absent from CMakeLists' VR_SHADER_INCLUDES, so editing it left STALE SPIR-V in
    // the build tree - the two halves of one pipeline declared different push blocks, and nothing but a byte
    // comparison against the escape-hatch scripts' output showed it. The list is the transitive closure by hand,
    // so the set of includes found in shaders/ must be a SUBSET of it (a leaf's own include graph is what the
    // build cannot see).
    {
        std::set<std::string> listed;
        // The CMake variable's spelling, as a named marker rather than a hand-counted offset: the first version of
        // this skipped 18 characters of a 19-character marker, so every name in the set carried a leading '/' and
        // the check reported all eight includes as missing. A test whose own parser is off by one is a test that
        // says "the build is wrong" about the build being right.
        constexpr std::string_view include_marker = "VR_SHADER_SRC_DIR}/";
        for (std::string const& line : read_lines(root + "/CMakeLists.txt")) {
            std::size_t const marker = line.find(include_marker);
            if (marker == std::string::npos) {
                continue;
            }
            std::string const name = line.substr(marker + include_marker.size());
            std::size_t const end = name.find('"');
            if (end != std::string::npos) {
                listed.insert(name.substr(0, end));
            }
        }
        CHECK_MSG(listed.contains("heap_access.slang"), "VR_SHADER_INCLUDES lists heap_access.slang");
        std::set<std::string> found;
        for (auto const& file : std::filesystem::directory_iterator(root + "/shaders")) {
            if (!file.is_regular_file()) {
                continue;
            }
            std::string const extension = file.path().extension().string();
            if (extension != ".slang" && extension != ".glsl") {
                continue;
            }
            for (std::string const& line : read_lines(file.path().string())) {
                std::size_t const include = line.find("#include \"");
                if (include == std::string::npos) {
                    continue;
                }
                std::string const name = line.substr(include + 10u);
                std::size_t const end = name.find('"');
                if (end != std::string::npos) {
                    found.insert(name.substr(0, end));
                }
            }
        }
        CHECK_MSG(!found.empty(), "shaders/ declares #includes to check");
        for (std::string const& include : found) {
            // ... and the include exists, which a rename would otherwise leave as a compile error in the build
            // and (worse) as a silently skipped file in this test's own scan
            CHECK_MSG(std::filesystem::exists(root + "/shaders/" + include), include.c_str());
            CHECK_MSG(listed.contains(include), ("an included shader is not in CMakeLists' VR_SHADER_INCLUDES, so editing it leaves stale SPIR-V: " + include).c_str());
        }
    }

    // ---- 6. No retired GLSL STAGE source is left in shaders/ (the migration's own contract) ----
    // The archive lives in shaders/glsl.old/; a .vert/.frag/.comp in shaders/ would be a source nothing
    // compiles and a reader would take for live code. The shared bodies (.glsl, included by the leaves) are
    // not stage sources and stay.
    for (auto const& file : std::filesystem::directory_iterator(root + "/shaders")) {
        if (!file.is_regular_file()) {
            continue;
        }
        std::string const extension = file.path().extension().string();
        CHECK_MSG(extension != ".vert" && extension != ".frag" && extension != ".comp" && extension != ".mesh",
                  ("a retired GLSL stage source is in shaders/: " + file.path().filename().string()).c_str());
    }

    return deren::vk_test::finish("test_shader_sources");
}
