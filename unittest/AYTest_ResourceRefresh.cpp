#include <AYTest.h>
#include <AYResource/ResourceBootstrap.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/assetsImpl/Texture.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace ayt::resource;
namespace {
struct RefreshFixture {
    ResourceManager& manager = ResourceManager::instance();
    bool autoWatch = manager.autoWatchLoadedResources();
    std::filesystem::path first, second;
    RefreshFixture() {
        const auto name = "ay-refresh-report-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        first = std::filesystem::temp_directory_path() / (name + "-a.txr");
        second = std::filesystem::temp_directory_path() / (name + "-b.txr");
        manager.unloadAll(); manager.setOnHotReload({});
        manager.setAutoWatchLoadedResources(false);
    }
    ~RefreshFixture() {
        manager.setOnHotReload({}); manager.unloadAll();
        manager.setAutoWatchLoadedResources(autoWatch);
        std::error_code error;
        std::filesystem::remove(first, error);
        std::filesystem::remove(second, error);
    }
    bool write(const std::filesystem::path& path, uint32_t width) {
        Texture texture; texture.createSolidColor(width, 2, 71, 83, 109);
        std::vector<uint8_t> bytes;
        if (!texture.saveToBinary(bytes)) return false;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return bool(file);
    }
};
}

TEST_SUITE(ResourceRefreshTests)
TEST_CASE(cpu_publication_and_individual_consumer_outcomes_are_distinct)
{
    RefreshFixture f; CHECK(initializeLoaders()); CHECK(f.write(f.first, 2));
    const auto previous = f.manager.load<Texture>(f.first.string()); CHECK(bool(previous));
    CHECK(f.write(f.first, 4));
    int calls = 0;
    const auto token = f.manager.setRefreshObserver("test device", [&](const std::string& path) {
        ++calls;
        const auto current = f.manager.load<Texture>(path);
        CHECK(bool(current)); CHECK(current != previous);
        if (current) CHECK(current->getWidth() == 4);
        return std::vector<ResourceConsumerRefresh>{
            {ResourceRefreshState::Updated, "linear", 17, 31, {}},
            {ResourceRefreshState::Retained, "sRGB", 23, 31, "device rejected replacement"}};
    });
    const auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string(), f.first.string()});
    CHECK(report.cpuRefreshed); CHECK(report.error.empty()); CHECK(report.needsAttention());
    CHECK(calls == 1); CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) {
        CHECK(report.paths[0].consumer == "test device");
        const auto& outcomes = report.paths[0].outcomes;
        CHECK(outcomes.size() == 2);
        if (outcomes.size() == 2) {
            CHECK(outcomes[0].state == ResourceRefreshState::Updated);
            CHECK(outcomes[1].state == ResourceRefreshState::Retained);
            CHECK(outcomes[0].resourceId == 17); CHECK(outcomes[1].resourceId == 23);
            CHECK(outcomes[1].contextId == 31); CHECK(outcomes[1].variant == "sRGB");
        }
    }
    CHECK(previous && previous->getWidth() == 2);
    std::string error;
    CHECK(f.manager.refreshPublishedResources({f.first.string()}, &error));
    CHECK(error.empty()); CHECK(calls == 2); // Legacy bool still describes CPU success.
    CHECK(f.manager.clearRefreshObserver(token));
}
TEST_CASE(cpu_batch_failure_keeps_all_residents_and_sends_no_consumer_notifications)
{
    RefreshFixture f; CHECK(initializeLoaders());
    CHECK(f.write(f.first, 2)); CHECK(f.write(f.second, 2));
    const auto a = f.manager.load<Texture>(f.first.string());
    const auto b = f.manager.load<Texture>(f.second.string());
    CHECK(bool(a)); CHECK(bool(b)); CHECK(f.write(f.first, 4));
    { std::ofstream invalid(f.second, std::ios::binary | std::ios::trunc); invalid << "broken"; }
    int calls = 0;
    f.manager.setRefreshObserver("consumer", [&](const std::string&) {
        ++calls; return std::vector<ResourceConsumerRefresh>{{ResourceRefreshState::Updated}};
    });
    const auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string(), f.second.string()});
    CHECK_FALSE(report.cpuRefreshed); CHECK(!report.error.empty()); CHECK(report.needsAttention());
    CHECK(report.paths.empty()); CHECK(calls == 0);
    CHECK(f.manager.load<Texture>(f.first.string()) == a);
    CHECK(f.manager.load<Texture>(f.second.string()) == b);
}
TEST_CASE(observer_tokens_cannot_remove_newer_or_legacy_registrations)
{
    RefreshFixture f;
    const auto callback = [](const std::string&) {
        return std::vector<ResourceConsumerRefresh>{{ResourceRefreshState::NotResident}};
    };
    const auto first = f.manager.setRefreshObserver("first", callback);
    CHECK(first != 0); CHECK(f.manager.clearRefreshObserver(first));
    const auto second = f.manager.setRefreshObserver("second", callback);
    CHECK(second != 0); CHECK(first != second);
    CHECK_FALSE(f.manager.clearRefreshObserver(first));
    int legacy = 0; f.manager.setOnHotReload([&](const auto&) { ++legacy; });
    CHECK_FALSE(f.manager.clearRefreshObserver(second));
    auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK_FALSE(report.needsAttention()); CHECK(legacy == 1);
    CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::Unconfirmed);
    CHECK_FALSE(f.manager.clearRefreshObserver(0));
    f.manager.setOnHotReload({});
    report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK_FALSE(report.needsAttention());
    CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::NotHandled);
}
TEST_CASE(replacing_the_observer_inside_a_notification_affects_only_subsequent_paths)
{
    RefreshFixture f;
    int oldCalls = 0, newCalls = 0;
    auto keepAlive = std::make_shared<int>(41);
    std::weak_ptr<int> lifetime = keepAlive;
    f.manager.setRefreshObserver("old", [&, owned = keepAlive](const std::string&) {
        ++oldCalls;
        f.manager.setRefreshObserver("new", [&](const std::string&) {
            ++newCalls; return std::vector<ResourceConsumerRefresh>{{ResourceRefreshState::NotResident}};
        });
        CHECK(*owned == 41); CHECK(!lifetime.expired());
        return std::vector<ResourceConsumerRefresh>{{ResourceRefreshState::Updated}};
    });
    keepAlive.reset();
    const auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string(), f.second.string()});
    CHECK(report.cpuRefreshed); CHECK_FALSE(report.needsAttention());
    CHECK(oldCalls == 1); CHECK(newCalls == 1); CHECK(report.paths.size() == 2);
    CHECK(lifetime.expired());
    if (report.paths.size() == 2) {
        CHECK(report.paths[0].consumer == "old"); CHECK(report.paths[1].consumer == "new");
    }
}
TEST_CASE(consumer_exceptions_and_empty_results_are_not_reported_as_success)
{
    RefreshFixture f;
    f.manager.setRefreshObserver("throwing", [](const std::string&) -> std::vector<ResourceConsumerRefresh> {
        throw std::runtime_error("upload interrupted");
    });
    auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK(report.needsAttention()); CHECK(report.error.empty());
    CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) {
        CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::Failed);
        CHECK(report.paths[0].outcomes[0].diagnostic == "upload interrupted");
    }
    f.manager.setRefreshObserver("empty", [](const std::string&) { return std::vector<ResourceConsumerRefresh>{}; });
    report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK_FALSE(report.needsAttention());
    if (report.paths.size() == 1) CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::Unconfirmed);
}
TEST_CASE(observer_output_is_bounded_and_excess_results_leave_an_explicit_failure)
{
    RefreshFixture f;
    f.manager.setRefreshObserver(std::string(1024, 'c'), [](const std::string&) {
        return std::vector<ResourceConsumerRefresh>(65,
            {ResourceRefreshState::Updated, std::string(2048, 'v'), 3, 7, std::string(4096, 'd')});
    });
    const auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK(report.needsAttention()); CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) {
        const auto& path = report.paths[0]; CHECK(path.consumer.size() == 128); CHECK(path.outcomes.size() == 64);
        CHECK(path.outcomes.back().state == ResourceRefreshState::Failed);
        CHECK(path.outcomes.front().variant.size() == 128); CHECK(path.outcomes.front().diagnostic.size() == 1024);
    }
}
TEST_CASE(invalid_states_and_nonstandard_exceptions_remain_explicit_failures)
{
    RefreshFixture f;
    f.manager.setRefreshObserver("invalid", [](const std::string&) {
        return std::vector<ResourceConsumerRefresh>{{static_cast<ResourceRefreshState>(255)}};
    });
    auto report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK(report.needsAttention()); CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::Failed);
    f.manager.setOnHotReload([](const std::string&) { throw 7; });
    report = f.manager.refreshPublishedResourcesWithReport({f.first.string()});
    CHECK(report.cpuRefreshed); CHECK(report.needsAttention()); CHECK(report.paths.size() == 1);
    if (report.paths.size() == 1) CHECK(report.paths[0].outcomes[0].state == ResourceRefreshState::Failed);
    std::string error;
    CHECK(f.manager.refreshPublishedResources({f.first.string()}, &error));
    CHECK(error.empty()); // CPU-only compatibility does not claim callback success.
}
TEST_CASE(empty_batches_are_successful_without_calling_or_loading_a_consumer)
{
    RefreshFixture f; int calls = 0;
    f.manager.setRefreshObserver("consumer", [&](const std::string&) {
        ++calls; return std::vector<ResourceConsumerRefresh>{{ResourceRefreshState::Updated}};
    });
    const auto report = f.manager.refreshPublishedResourcesWithReport({{}, {}});
    CHECK(report.cpuRefreshed); CHECK_FALSE(report.needsAttention()); CHECK(report.paths.empty());
    CHECK(report.error.empty()); CHECK(calls == 0); CHECK(f.manager.getResourceCount() == 0);
}
TEST_SUITE_END
