#pragma once

// A render interface that stands in while the real one is still being created
// on a worker thread (GPU device or GL context, shader compilation), so the
// window, RmlUi, the fonts and the document can start in parallel (DESIGN.md
// 7.1 first-frame budget). Every call waits for the real interface first; in
// practice RmlUi only reaches the renderer at the first Render.

#include <RmlUi/Core/RenderInterface.h>

#include <functional>
#include <utility>

class deferred_render_interface final : public Rml::RenderInterface {
public:
  explicit deferred_render_interface(
      std::function<Rml::RenderInterface *()> resolve)
      : resolve_(std::move(resolve)) {}

  Rml::CompiledGeometryHandle
  CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                  Rml::Span<const int> indices) override {
    return real()->CompileGeometry(vertices, indices);
  }
  void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation,
                      Rml::TextureHandle texture) override {
    real()->RenderGeometry(geometry, translation, texture);
  }
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
    real()->ReleaseGeometry(geometry);
  }
  Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions,
                                 const Rml::String &source) override {
    return real()->LoadTexture(dimensions, source);
  }
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                     Rml::Vector2i dimensions) override {
    return real()->GenerateTexture(source, dimensions);
  }
  void ReleaseTexture(Rml::TextureHandle texture) override {
    real()->ReleaseTexture(texture);
  }
  void EnableScissorRegion(bool enable) override {
    real()->EnableScissorRegion(enable);
  }
  void SetScissorRegion(Rml::Rectanglei region) override {
    real()->SetScissorRegion(region);
  }
  void EnableClipMask(bool enable) override { real()->EnableClipMask(enable); }
  void RenderToClipMask(Rml::ClipMaskOperation operation,
                        Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation) override {
    real()->RenderToClipMask(operation, geometry, translation);
  }
  void SetTransform(const Rml::Matrix4f *transform) override {
    real()->SetTransform(transform);
  }
  Rml::LayerHandle PushLayer() override { return real()->PushLayer(); }
  void
  CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                  Rml::BlendMode blend_mode,
                  Rml::Span<const Rml::CompiledFilterHandle> filters) override {
    real()->CompositeLayers(source, destination, blend_mode, filters);
  }
  void PopLayer() override { real()->PopLayer(); }
  Rml::TextureHandle SaveLayerAsTexture() override {
    return real()->SaveLayerAsTexture();
  }
  Rml::CompiledFilterHandle SaveLayerAsMaskImage() override {
    return real()->SaveLayerAsMaskImage();
  }
  Rml::CompiledFilterHandle
  CompileFilter(const Rml::String &name,
                const Rml::Dictionary &parameters) override {
    return real()->CompileFilter(name, parameters);
  }
  void ReleaseFilter(Rml::CompiledFilterHandle filter) override {
    real()->ReleaseFilter(filter);
  }
  Rml::CompiledShaderHandle
  CompileShader(const Rml::String &name,
                const Rml::Dictionary &parameters) override {
    return real()->CompileShader(name, parameters);
  }
  void RenderShader(Rml::CompiledShaderHandle shader,
                    Rml::CompiledGeometryHandle geometry,
                    Rml::Vector2f translation,
                    Rml::TextureHandle texture) override {
    real()->RenderShader(shader, geometry, translation, texture);
  }
  void ReleaseShader(Rml::CompiledShaderHandle shader) override {
    real()->ReleaseShader(shader);
  }

private:
  Rml::RenderInterface *real() {
    if (!real_) {
      real_ = resolve_();
    }
    return real_;
  }
  std::function<Rml::RenderInterface *()> resolve_;
  Rml::RenderInterface *real_ = nullptr;
};
