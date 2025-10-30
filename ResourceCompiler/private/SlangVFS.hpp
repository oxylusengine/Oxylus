#pragma once

#include <Core/Types.hpp>
#include <OS/File.hpp>
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
    const auto path = std::string_view(path_cstr);

    const auto root_path = std::filesystem::relative(m_root_dir);
    const auto module_path = root_path / path;

    const auto result = File::to_string(module_path);
    if (!result.empty()) {
      *outBlob = new SlangBlob(std::vector<u8>{result.data(), (result.data() + result.size())});

      return SLANG_OK;
    }

    return SLANG_E_NOT_FOUND;
  }
};

} // namespace ox::rc
