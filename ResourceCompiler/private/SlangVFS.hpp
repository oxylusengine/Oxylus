#pragma once

#include <Core/Types.hpp>
#include <OS/File.hpp>
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <atomic>
#include <slang.h>
#include <vector>

namespace ox::rc {
struct SlangBlob : ISlangBlob {
  std::vector<u8> m_data = {};
  std::atomic_uint32_t m_refCount = 1;

  ISlangUnknown* getInterface(const SlangUUID&) { return nullptr; }
  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    ISlangUnknown* intf = getInterface(uuid);
    if (intf) {
      addRef();
      *outObject = intf;
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_refCount; }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
    --m_refCount;
    if (m_refCount == 0) {
      delete this;
      return 0;
    }
    return m_refCount;
  }

  SlangBlob(const std::vector<u8>& data) : m_data(data) {}
  virtual ~SlangBlob() = default;
  SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() final { return m_data.data(); };
  SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() final { return m_data.size(); };
};

struct SlangVirtualFS : ISlangFileSystem {
  std::filesystem::path m_root_dir;
  std::atomic_uint32_t m_ref_count;
  ankerl::unordered_dense::map<std::string, std::vector<u8>> m_loaded_modules;

  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    ISlangUnknown* intf = getInterface(uuid);
    if (intf) {
      addRef();
      *outObject = intf;
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_ref_count; }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
    --m_ref_count;
    if (m_ref_count == 0) {
      delete this;
      return 0;
    }
    return m_ref_count;
  }

  SlangVirtualFS(std::filesystem::path root_dir) : m_root_dir(std::move(root_dir)), m_ref_count(1) {}
  virtual ~SlangVirtualFS() = default;

  ISlangUnknown* getInterface(const SlangUUID&) { return nullptr; }
  SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID&) final { return nullptr; }

  SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path_cstr, ISlangBlob** outBlob) final {
    auto path = std::filesystem::path(path_cstr);

    // /resources/shaders/path/to/xx.slang -> path.to.xx
    auto module_name = std::filesystem::relative(path, m_root_dir).replace_extension("").string();
    std::ranges::replace(module_name, static_cast<c8>(std::filesystem::path::preferred_separator), '.');

    auto it = m_loaded_modules.find(module_name);
    if (it == m_loaded_modules.end()) {
      auto file_bytes = File::to_bytes(path);
      if (!file_bytes.empty()) {
        auto new_it = m_loaded_modules.emplace(module_name, std::move(file_bytes));
        *outBlob = new SlangBlob(new_it.first->second);
        return SLANG_OK;
      } else {
        return SLANG_E_NOT_FOUND;
      }
    } else {
      *outBlob = new SlangBlob(it->second);
      return SLANG_OK;
    }

    return SLANG_E_NOT_FOUND;
  }
};

} // namespace ox::rc
