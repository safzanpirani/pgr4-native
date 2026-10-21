// Ucode -> DXBC cache of the native renderer. Shaders are keyed by the XXH3 hash of their
// guest (big-endian) ucode, the same ucode_data_hash the Xenos plugin uses in its dumps and in
// its shader storage (.xsh), and translations by (shader, modification).
// Not thread-safe: called from the thread that runs the D3D hooks.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/string/buffer.h>

#include "shader_translate.h"

namespace pgr4::native {

struct ShaderCacheStats {
  uint64_t shaders = 0;            // distinct ucode loaded
  uint64_t translations = 0;       // distinct (ucode, modification) translated
  uint64_t failures = 0;           // translations that came out invalid
  double analyze_us = 0;           // total ucode analysis time
  double translate_us = 0;         // total translation time
  double max_translate_us = 0;
};

class ShaderCache {
 public:
  explicit ShaderCache(const ShaderTranslatorConfig& config = {});
  ~ShaderCache();

  const DxbcShaderTranslator& translator() const { return translator_; }

  // ucode: dword_count dwords as they are in guest memory (big-endian). Analyzes new ucode.
  DxbcShader* Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                   uint32_t dword_count);

  // Returns the translation for the modification, translating it on first use; nullptr if it is
  // invalid (counted in stats().failures and logged once).
  DxbcShader::Translation* Translate(DxbcShader& shader, uint64_t modification);

  const ShaderCacheStats& stats() const { return stats_; }

 private:
  DxbcShaderTranslator translator_;
  rex::string::StringBuffer disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<DxbcShader>> shaders_;
  ShaderCacheStats stats_;
};

}  // namespace pgr4::native
