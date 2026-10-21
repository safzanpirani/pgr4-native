#include "shader_cache.h"

#include <algorithm>
#include <chrono>

#include <xxhash.h>

#include <rex/logging.h>

namespace pgr4::native {

namespace {
double Us(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - since).count();
}
}  // namespace

ShaderCache::ShaderCache(const ShaderTranslatorConfig& config)
    : translator_(rex::ui::GraphicsProvider::GpuVendorID(config.vendor_id), config.bindless,
                  /*edram_rov_used=*/false, config.gamma_render_target_as_unorm8,
                  config.msaa_2x_supported) {}

ShaderCache::~ShaderCache() = default;

DxbcShader* ShaderCache::Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                              uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(guest_ucode, size_t(dword_count) * sizeof(uint32_t));
  std::lock_guard<std::mutex> lock(load_mutex_);
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  auto t = std::chrono::steady_clock::now();
  auto shader = std::make_unique<DxbcShader>(type, hash, guest_ucode, dword_count);
  shader->AnalyzeUcode(disasm_buffer_);
  stats_.analyze_us += Us(t);
  ++stats_.shaders;
  return shaders_.emplace(hash, std::move(shader)).first->second.get();
}

DxbcShader::Translation* ShaderCache::Translate(DxbcShader& shader, uint64_t modification) {
  bool is_new = false;
  DxbcShader::Translation* translation = shader.GetOrCreateTranslation(modification, &is_new);
  if (is_new) {
    auto t = std::chrono::steady_clock::now();
    translator_.TranslateAnalyzedShader(*translation);
    double us = Us(t);
    stats_.translate_us += us;
    stats_.max_translate_us = std::max(stats_.max_translate_us, us);
    ++stats_.translations;
    if (!translation->is_valid()) {
      ++stats_.failures;
      REXLOG_ERROR("pgr4 native: {} shader {:016X} modification {:016X} failed to translate",
                   shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vertex" : "pixel",
                   shader.ucode_data_hash(), modification);
    }
  }
  return translation->is_valid() ? translation : nullptr;
}

}  // namespace pgr4::native
