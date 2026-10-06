#include <AYResource/ProjectBuild.h>
#include <AYTest.h>
#if AY_RESOURCE_TEST_SCRIPT
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/TaskEvents.h>
#include <AYScript/ScriptRuntimeBridge.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
using namespace ayt::resource;
namespace
{
namespace fs = std::filesystem;
struct Fixture
{
    fs::path root = fs::temp_directory_path() /
                    ("project_logia_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ProjectBuildProfile profile;
    Fixture()
    {
        profile.schemaVersion = 1;
        profile.id = "logia";
        profile.platform = "windows";
        profile.architecture = "x64";
        profile.configuration = "Debug";
        profile.package.output = "out/game";
        put("main.logia",
            "script Main { import \"modules/a.logia\" on_start() { self.hp=value() } }");
        put("modules/a.logia",
            "script A { import \"b.logia\" function value():int { return nested() } }");
        put("modules/b.logia", "script B { function nested():int { return 7 } }");
    }
    void put(const std::string &path, const std::string &text)
    {
        auto file = root / "Assets" / path;
        fs::create_directories(file.parent_path());
        std::ofstream(file) << text;
    }
    ~Fixture()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
    ProjectBuildPlan plan()
    {
        return ProjectBuildPlanner::create(profile, root.string());
    }
};
} // namespace
TEST_SUITE(ProjectLogiaTests)
TEST_CASE(import_closure_is_portable_and_published_without_author_sources)
{
    Fixture files;
    files.profile.content.defaultStorage = ProjectAssetStorage::Pak;
    auto plan = files.plan();
    CHECK(plan.valid());
    auto main = std::find_if(plan.assets.begin(), plan.assets.end(),
                             [](const auto &asset) { return asset.logicalPath == "main.logia"; });
    CHECK(main != plan.assets.end());
    if (main == plan.assets.end())
        return;
    CHECK(main->scriptDependencies ==
          std::vector<std::string>({"modules/a.logia", "modules/b.logia"}));
    auto built = ProjectBuildExecutor::execute(plan);
    CHECK(built.ok);
    if (!built.ok)
        return;
    fs::rename(files.root / "Assets", files.root / "MovedAuthorAssets");
    auto path = fs::path(built.outputDirectory) / "Content/main.logia";
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    auto ctx =
        ayt::script::logia::checkedLogiaHostContext(ayt::script::logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    ctx.sourcePath = path.string();
    ctx.assetRoot = path.parent_path().string();
    ayt::script::LogiaRuntimeBridge bridge;
    std::vector<ayt::script::logia::CompilerError> errors;
    CHECK(bridge.loadScript("Main", source, ctx, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 7.0, 0.001);
}
TEST_CASE(published_imported_task_accepts_arguments_and_consumes_event_payload)
{
    Fixture files;
    files.put("main.logia", "script Main { import \"modules/a.logia\" on_start() { tasks.start(deliver,5) } }");
    files.put("modules/a.logia", R"logia(script A {
        function deliver(base:int) {
            await wait_event("task_complete") into (id:int,ok:bool)
            if ok { self.hp=base+id }
        }
    })logia");
    auto plan = files.plan();
    CHECK(plan.valid());
    auto built = ProjectBuildExecutor::execute(plan);
    CHECK(built.ok);
    if (!built.ok) return;
    fs::rename(files.root / "Assets", files.root / "MovedAuthorAssets");
    const auto path = fs::path(built.outputDirectory) / "Content/main.logia";
    std::ifstream input(path);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    auto ctx = ayt::script::logia::checkedLogiaHostContext(ayt::script::logia::actorLogiaHostContext());
    ctx.actorProperties = {{"hp", "float"}};
    ctx.sourcePath = path.string(); ctx.assetRoot = path.parent_path().string();
    ayt::script::LogiaRuntimeBridge bridge;
    std::vector<ayt::script::logia::CompilerError> errors;
    CHECK(bridge.loadScript("Main", source, ctx, errors));
    ayt::entity::ActorScriptComponent actor;
    actor.properties["hp"] = 0.0;
    CHECK(bridge.callActorLifecycle("Main", "on_start", actor));
    bridge.tickTasks(0);
    ayt::event::EventBus::instance().emit(ayt::event::TaskCompleteEvent{17, true});
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 0.0, 0.001);
    bridge.tickTasks(0);
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]), 22.0, 0.001);
}
TEST_CASE(excluded_or_packed_imports_are_rejected)
{
    Fixture files;
    files.profile.content.rules.push_back(
        {"modules/**", ProjectAssetTransform::Exclude, ProjectAssetStorage::Loose});
    CHECK(!files.plan().valid());
    files.profile.content.rules[0].transform = ProjectAssetTransform::Raw;
    files.profile.content.rules[0].storage = ProjectAssetStorage::Pak;
    CHECK(!files.plan().valid());
}
TEST_CASE(missing_cycles_escape_and_stateful_modules_are_rejected)
{
    Fixture files;
    files.put("modules/b.logia", "script B { import \"a.logia\" }");
    CHECK(!files.plan().valid());
    files.put("modules/b.logia", "script B { state v:int=1 }");
    CHECK(!files.plan().valid());
    files.put("modules/b.logia", "script B { import \"../../outside.logia\" }");
    CHECK(!files.plan().valid());
    fs::remove(files.root / "Assets/modules/b.logia");
    CHECK(!files.plan().valid());
}
TEST_CASE(execution_rechecks_changed_imports_and_preserves_previous_output)
{
    Fixture files;
    auto plan = files.plan();
    auto first = ProjectBuildExecutor::execute(plan);
    CHECK(first.ok);
    files.put("modules/extra.logia", "script Extra { function extra():int { return 1 } }");
    files.put("modules/b.logia",
              "script B { import \"extra.logia\" function nested():int { return extra() } }");
    auto failed = ProjectBuildExecutor::execute(plan);
    CHECK(!failed.ok);
    CHECK(fs::exists(files.root / "out/game/Content/modules/b.logia"));
    auto replanned = files.plan();
    CHECK(replanned.valid());
    CHECK(ProjectBuildExecutor::execute(replanned).ok);
}
TEST_SUITE_END
#endif
