#include "AYResource.h"
#include "AYTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

// Consumer TU: only the public umbrella + propagated interface headers.
// Internal paths (assetsImpl / Loader / Converter) must not be required here.

using namespace ayt::resource;
namespace fs = std::filesystem;

#ifndef AY_RUNTIME_ROOT
#define AY_RUNTIME_ROOT ""
#endif

namespace {

bool containsForbiddenInclude(const std::string& text)
{
    // Match common spellings used across the tree.
    static const char* kNeedles[] = {
        "assetsImpl/",
        "assetsImpl\\",
        "include/AYResource/Loader/",
        "include\\Loader\\",
        "include/Converter/",
        "include\\Converter\\",
        "\"Loader/",
        "<Loader/",
        "\"Converter/",
        "<Converter/",
    };
    for (const char* n : kNeedles) {
        if (text.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool shouldSkipPath(const fs::path& p)
{
    std::string s = p.generic_string();
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    // Module-private trees / non-production surfaces.
    if (s.find("/AYResource/") != std::string::npos) {
        return true;
    }
    if (s.find("/unittest/") != std::string::npos) {
        return true;
    }
    if (s.find("/demo/") != std::string::npos) {
        return true;
    }
    // L2→L3 bridge may cast to concrete types (design §8.3).
    if (s.find("/AYRenderer/src/detail/") != std::string::npos) {
        return true;
    }
    return false;
}

std::string toRuntimeRelative(const fs::path& runtimeRoot, const fs::path& file)
{
    fs::path rel = fs::relative(file, runtimeRoot);
    std::string s = rel.generic_string();
    return s;
}

std::unordered_set<std::string> loadAllowlist(const fs::path& path)
{
    std::unordered_set<std::string> out;
    std::ifstream in(path);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        for (char& c : line) {
            if (c == '\\') {
                c = '/';
            }
        }
        out.insert(line);
    }
    return out;
}

} // namespace

TEST_SUITE(PublicApiSurfaceTests)

TEST_CASE(umbrella_header_compiles_without_internal_includes)
{
    static_assert(MAJOR_VERSION >= 1);
    ResourceManager& manager = ResourceManager::instance();
    (void)manager;
    CHECK(true);
}

TEST_CASE(production_modules_do_not_include_private_headers_except_allowlist)
{
    const fs::path runtimeRoot = fs::path(AY_RUNTIME_ROOT);
    if (AY_RUNTIME_ROOT[0] == '\0' || !fs::exists(runtimeRoot)) {
        // Configured builds always define AY_RUNTIME_ROOT; skip only if misconfigured.
        CHECK(false);
        return;
    }

    const fs::path allowPath = runtimeRoot / "AYResource" / "docs" / "private-include-allowlist.txt";
    const auto allow = loadAllowlist(allowPath);
    CHECK(fs::exists(allowPath));

    std::vector<std::string> violations;
    for (fs::recursive_directory_iterator it(runtimeRoot), end; it != end; ++it) {
        if (!it->is_regular_file()) {
            continue;
        }
        const fs::path& p = it->path();
        const auto ext = p.extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") {
            continue;
        }
        if (shouldSkipPath(p)) {
            continue;
        }

        std::ifstream in(p, std::ios::binary);
        if (!in) {
            continue;
        }
        std::ostringstream oss;
        oss << in.rdbuf();
        const std::string text = oss.str();
        if (!containsForbiddenInclude(text)) {
            continue;
        }

        const std::string rel = toRuntimeRelative(runtimeRoot, p);
        if (allow.count(rel) == 0) {
            violations.push_back(rel);
        }
    }

    if (!violations.empty()) {
        std::fprintf(stderr,
                     "[PublicApiSurface] private include outside allowlist (%zu):\n",
                     violations.size());
        for (const auto& v : violations) {
            std::fprintf(stderr, "  - %s\n", v.c_str());
        }
        std::fprintf(stderr,
                     "Prefer interface/AYResource/assetsDefs/I*.h, or add to "
                     "AYResource/docs/private-include-allowlist.txt with review.\n");
    }
    CHECK(violations.empty());
}

TEST_SUITE_END
