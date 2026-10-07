#include <gtest/gtest.h>
#include <lua.hpp>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "OS/File.hpp"
#include "Scripting/LuaScript.hpp"
#include "Utils/Log.hpp"

// what the importer does to a source, without the importer: tests don't link ResourceCompiler
static auto compile(std::string_view source, const char* chunk_name) -> std::vector<u8> {
  auto bytecode = std::vector<u8>{};
  auto* state = luaL_newstate();
  EXPECT_EQ(luaL_loadbufferx(state, source.data(), source.size(), chunk_name, "t"), LUA_OK);
  lua_dump(
    state,
    [](lua_State*, const void* data, usize size, void* user_data) -> i32 {
      auto* out = static_cast<std::vector<u8>*>(user_data);
      out->insert(out->end(), static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
      return 0;
    },
    &bytecode,
    0
  );
  lua_close(state);

  return bytecode;
}

static auto write_script_pack(const std::filesystem::path& path, std::vector<u8> bytecode, const ox::UUID& uuid)
  -> bool {
  auto file = ox::AssetFile{};
  file.add_entry(
    ox::ScriptData{.name = path.filename().string(), .bytecode = std::move(bytecode)},
    ox::PackedUUID::pack(uuid)
  );

  return file.pack(path);
}

// runs the chunk in a bare state and hands back the global `answer` it sets
static auto run_answer(std::span<const u8> bytecode) -> i64 {
  auto* state = luaL_newstate();
  const auto* chars = reinterpret_cast<const char*>(bytecode.data());
  EXPECT_EQ(luaL_loadbufferx(state, chars, bytecode.size(), "=test", "b"), LUA_OK);
  EXPECT_EQ(lua_pcall(state, 0, 0, 0), LUA_OK);
  lua_getglobal(state, "answer");
  const auto answer = static_cast<i64>(lua_tointeger(state, -1));
  lua_close(state);

  return answer;
}

// runs without an app, so nothing is mounted and the registry keeps physical paths
class ScriptAssetTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    directory = std::filesystem::temp_directory_path() / "ox_script_asset_test";
    std::filesystem::create_directories(directory);
    asset_man = std::make_unique<ox::AssetManager>();
    ASSERT_TRUE(asset_man->init().has_value());
  }

  void TearDown() override {
    EXPECT_TRUE(asset_man->deinit().has_value());
    asset_man.reset();

    auto error = std::error_code{};
    std::filesystem::remove_all(directory, error);
  }

  std::unique_ptr<ox::AssetManager> asset_man = nullptr;
  std::filesystem::path directory = {};
};

TEST(ScriptPackTest, BytecodeRoundTripsThroughAPack) {
  const auto path = std::filesystem::temp_directory_path() / "ox_script_pack_test.oxpack";
  const auto uuid = ox::UUID::generate_random();
  const auto bytecode = compile("answer = 42", "@assets_dir/answer.lua");
  ASSERT_TRUE(write_script_pack(path, bytecode, uuid));

  auto read = ox::AssetFile::unpack(path);
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->entries.size(), 1);
  EXPECT_EQ(read->entries[0].type, ox::AssetType::Script);
  EXPECT_EQ(read->entries[0].uuid.unpack(), uuid);

  const auto* script = std::get_if<ox::ScriptData>(&read->entries[0].data);
  ASSERT_NE(script, nullptr);
  EXPECT_EQ(script->name, path.filename().string());
  EXPECT_EQ(script->bytecode, bytecode);
  EXPECT_EQ(run_answer(script->bytecode), 42);

  std::filesystem::remove(path);
}

TEST_F(ScriptAssetTest, ACookedScriptLoadsItsBytecodeAndKeepsItsSourcePath) {
  const auto uuid = ox::UUID::generate_random();
  const auto pack_path = directory / "answer.oxpack";
  const auto source_path = directory / "answer.lua";
  ASSERT_TRUE(write_script_pack(pack_path, compile("answer = 7", "@answer.lua"), uuid));
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Script, pack_path, source_path));

  ASSERT_TRUE(asset_man->load_asset(uuid));
  {
    auto script = asset_man->get_script(uuid);
    ASSERT_TRUE(script);
    EXPECT_EQ(script->path, source_path);
    EXPECT_FALSE(script->source.has_value());
    EXPECT_EQ(run_answer(script->bytecode), 7);
  }

  asset_man->unload_asset(uuid);
}

TEST_F(ScriptAssetTest, ASourceScriptLoadsFromItsFile) {
  const auto source_path = directory / "plain.lua";
  {
    auto file = ox::File(source_path, ox::FileAccess::Write);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(std::string_view("answer = 1"));
  }

  const auto uuid = asset_man->create_asset(ox::AssetType::Script, source_path);
  ASSERT_TRUE(asset_man->load_asset(uuid));
  {
    auto script = asset_man->get_script(uuid);
    ASSERT_TRUE(script);
    EXPECT_EQ(script->path, source_path);
    EXPECT_TRUE(script->bytecode.empty());
  }

  asset_man->unload_asset(uuid);
}

TEST_F(ScriptAssetTest, APackWithoutAScriptFailsToLoad) {
  const auto uuid = ox::UUID::generate_random();
  const auto pack_path = directory / "empty.oxpack";
  auto file = ox::AssetFile{};
  ASSERT_TRUE(file.pack(pack_path));
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Script, pack_path, directory / "empty.lua"));

  EXPECT_FALSE(asset_man->load_asset(uuid));
  EXPECT_FALSE(asset_man->is_loaded(uuid));
}

TEST_F(ScriptAssetTest, ReloadingPicksUpAPackCookedAgain) {
  const auto uuid = ox::UUID::generate_random();
  const auto pack_path = directory / "reload.oxpack";
  ASSERT_TRUE(write_script_pack(pack_path, compile("answer = 1", "@reload.lua"), uuid));
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Script, pack_path, directory / "reload.lua"));
  ASSERT_TRUE(asset_man->load_asset(uuid));

  ASSERT_TRUE(write_script_pack(pack_path, compile("answer = 2", "@reload.lua"), uuid));
  ASSERT_TRUE(asset_man->reload_script(uuid));
  {
    auto script = asset_man->get_script(uuid);
    ASSERT_TRUE(script);
    EXPECT_EQ(run_answer(script->bytecode), 2);
  }

  asset_man->unload_asset(uuid);
}

TEST_F(ScriptAssetTest, OnlyALoadedScriptReloads) {
  const auto uuid = ox::UUID::generate_random();
  const auto pack_path = directory / "unloaded.oxpack";
  ASSERT_TRUE(write_script_pack(pack_path, compile("answer = 1", "@unloaded.lua"), uuid));
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Script, pack_path, directory / "unloaded.lua"));

  EXPECT_FALSE(asset_man->reload_script(uuid));
  EXPECT_FALSE(asset_man->reload_script(ox::UUID::generate_random()));
}
