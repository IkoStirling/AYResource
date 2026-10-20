#pragma once

#include "AYResource/IConverter.h"
#include "ImportNativePath.h"
#include <AYIO/PathSafety.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>

namespace ayt::resource::detail {

// Converters see an empty staging directory, so their resume/skip heuristics
// cannot reuse obsolete payloads. Publication is sequential, not a multi-file
// reader snapshot. Failures roll back replaced files; failed rollback retains
// the recovery tree. The caller must serialize imports into the same root.
class ImportOutputTransaction {
public:
    explicit ImportOutputTransaction(const std::string& output)
    {
        static std::atomic<unsigned long long> serial{0};
        _output = importNativePath(std::filesystem::u8path(output));
        std::filesystem::create_directories(_output);
        const auto name = ".ayimport-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++serial);
        _work = _output.parent_path() / name;
        if (_work.parent_path() != _output.parent_path()
            || !std::filesystem::create_directory(_work))
            throw std::runtime_error("cannot create isolated import staging directory");
        _stage = _work / "stage";
        std::filesystem::create_directory(_stage);
    }
    ~ImportOutputTransaction()
    {
        if (!_retain && !_work.empty()) {
            std::error_code error;
            std::filesystem::remove_all(_work, error);
        }
    }
    std::string stagingRoot() const
    {
        return importPortablePathText(_stage);
    }
    void validate(const ConversionResult& result, bool requireClosedDependencies = false) const
    {
        if (result.resources.empty())
            throw std::runtime_error("source conversion produced no resources");
        std::set<std::string> produced;
        for (const auto& resource : result.resources)
            if (!produced.insert(resource.path).second)
                throw std::runtime_error("converter reported duplicate output: " + resource.path);
        if (requireClosedDependencies) for (const auto& dependency : result.dependencies)
            if (produced.contains(dependency.from) && !produced.contains(dependency.to))
                throw std::runtime_error("converter left an unpublished dependency: " + dependency.to);
        for (const auto& resource : result.resources) {
            const auto relative = std::filesystem::u8path(resource.path);
            requireRelative(relative);
            const auto path = importNativePath(_stage / relative);
            if (!std::filesystem::is_regular_file(path)
                || std::filesystem::file_size(path) == 0)
            {
                std::string written;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(_stage)) {
                    if (entry.is_regular_file()) {
                        if (written.size() > 1024) { written += "..."; break; }
                        written += importPortablePathText(entry.path().lexically_relative(_stage)) + "; ";
                    }
                }
                throw std::runtime_error("converter did not write resource: " + resource.path
                    + " (staged: " + written + ")");
            }
        }
    }
    void publish(const std::string& finalMarker = {})
    {
        namespace fs = std::filesystem;
        struct Entry { fs::path relative; bool hadPrevious = false; bool installed = false; };
        std::vector<Entry> entries;
        for (const auto& file : fs::recursive_directory_iterator(_stage)) {
            if (file.is_symlink()) throw std::runtime_error("import output contains a symlink");
            if (!file.is_regular_file()) continue;
            const auto relative = file.path().lexically_relative(_stage);
            requireRelative(relative);
            const auto destination = _output / relative;
            for (auto parent = destination; parent != _output; parent = parent.parent_path()) {
                if (fs::is_symlink(parent))
                    throw std::runtime_error("import target contains a symlink");
            }
            if (fs::exists(destination) && !fs::is_regular_file(destination))
                throw std::runtime_error("import target is not a regular file");
            entries.push_back({relative});
        }
        const auto marker = fs::u8path(finalMarker);
        if (!finalMarker.empty()) requireRelative(marker);
        std::sort(entries.begin(), entries.end(), [&marker](const auto& first, const auto& second) {
            const bool firstMarker = first.relative == marker;
            const bool secondMarker = second.relative == marker;
            if (firstMarker != secondMarker) return !firstMarker;
            // Metadata is the final publication marker observed by the Editor.
            const bool firstMetadata = first.relative.extension() == ".dep";
            const bool secondMetadata = second.relative.extension() == ".dep";
            return firstMetadata == secondMetadata ? first.relative < second.relative : !firstMetadata;
        });
        try {
            for (auto& entry : entries) {
                const auto destination = _output / entry.relative;
                fs::create_directories(destination.parent_path());
                if (fs::exists(destination)) {
                    const auto backup = _work / "previous" / entry.relative;
                    fs::create_directories(backup.parent_path());
                    fs::rename(destination, backup);
                    entry.hadPrevious = true;
                }
                fs::rename(_stage / entry.relative, destination);
                entry.installed = true;
            }
        } catch (...) {
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                std::error_code error;
                const auto destination = _output / entry->relative;
                if (entry->installed) fs::remove(destination, error);
                if (error) _retain = true;
                error.clear();
                if (entry->hadPrevious)
                    fs::rename(_work / "previous" / entry->relative, destination, error);
                if (error) _retain = true;
            }
            if (_retain) throw std::runtime_error("import publication rollback failed; recovery retained at "
                + _work.string());
            throw;
        }
    }
private:
    static void requireRelative(const std::filesystem::path& path)
    {
        const auto name = path.generic_u8string();
        if (!ayt::io::path::isPortableRelative(std::string_view(
                reinterpret_cast<const char*>(name.data()), name.size())))
            throw std::runtime_error("converter output must stay within the import root");
    }
    std::filesystem::path _output, _work, _stage;
    bool _retain = false;
};

} // namespace ayt::resource::detail
