// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/texture_gles.h"

#include <optional>

#include "flutter/fml/mapping.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/allocation.h"
#include "impeller/base/config.h"
#include "impeller/base/validation.h"

namespace impeller {

static TextureGLES::Type GetTextureTypeFromDescriptor(
    const TextureDescriptor& desc) {
  const auto usage = static_cast<TextureUsageMask>(desc.usage);
  const auto render_target =
      static_cast<TextureUsageMask>(TextureUsage::kRenderTarget);
  if (usage == render_target) {
    return TextureGLES::Type::kRenderBuffer;
  }
  return TextureGLES::Type::kTexture;
}

HandleType ToHandleType(TextureGLES::Type type) {
  switch (type) {
    case TextureGLES::Type::kTexture:
      return HandleType::kTexture;
    case TextureGLES::Type::kRenderBuffer:
      return HandleType::kRenderBuffer;
  }
  FML_UNREACHABLE();
}

TextureGLES::TextureGLES(ReactorGLES::Ref reactor, TextureDescriptor desc)
    : TextureGLES(std::move(reactor), std::move(desc), false) {}

TextureGLES::TextureGLES(ReactorGLES::Ref reactor,
                         TextureDescriptor desc,
                         enum IsWrapped wrapped)
    : TextureGLES(std::move(reactor), std::move(desc), true) {}

TextureGLES::TextureGLES(std::shared_ptr<ReactorGLES> reactor,
                         TextureDescriptor desc,
                         bool is_wrapped)
    : Texture(std::move(desc)),
      reactor_(reactor),
      type_(GetTextureTypeFromDescriptor(GetTextureDescriptor())),
      handle_(reactor_->CreateHandle(ToHandleType(type_))),
      is_wrapped_(is_wrapped) {
  if (!GetTextureDescriptor().IsValid()) {
    return;
  }
  is_valid_ = true;
}

// |Texture|
TextureGLES::~TextureGLES() {
  reactor_->CollectHandle(std::move(handle_));
}

// |Texture|
bool TextureGLES::IsValid() const {
  return is_valid_;
}

// |Texture|
void TextureGLES::SetLabel(const std::string_view& label) {
  label_ = std::string{label.data(), label.size()};
  if (contents_initialized_) {
    reactor_->SetDebugLabel(handle_, label_);
  }
}

struct TexImage2DData {
  GLint internal_format = 0;
  GLenum external_format = GL_NONE;
  GLenum type = GL_NONE;
  std::shared_ptr<const fml::Mapping> data;

  TexImage2DData(PixelFormat pixel_format) {
    switch (pixel_format) {
      case PixelFormat::kA8UNormInt:
        internal_format = GL_ALPHA;
        external_format = GL_ALPHA;
        type = GL_UNSIGNED_BYTE;
        break;
      case PixelFormat::kR8G8B8A8UNormInt:
      case PixelFormat::kB8G8R8A8UNormInt:
      case PixelFormat::kR8G8B8A8UNormIntSRGB:
      case PixelFormat::kB8G8R8A8UNormIntSRGB:
        internal_format = GL_RGBA;
        external_format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
        break;
      case PixelFormat::kUnknown:
      case PixelFormat::kS8UInt:
        return;
    }
    is_valid_ = true;
  }

  TexImage2DData(PixelFormat pixel_format,
                 std::shared_ptr<const fml::Mapping> mapping) {
    switch (pixel_format) {
      case PixelFormat::kUnknown:
        return;
      case PixelFormat::kA8UNormInt: {
        internal_format = GL_ALPHA;
        external_format = GL_ALPHA;
        type = GL_UNSIGNED_BYTE;
        data = std::move(mapping);
        break;
      }
      case PixelFormat::kR8G8B8A8UNormInt: {
        internal_format = GL_RGBA;
        external_format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
        data = std::move(mapping);
        break;
      }
      case PixelFormat::kR8G8B8A8UNormIntSRGB:
        return;
      case PixelFormat::kB8G8R8A8UNormInt:
        return;
      case PixelFormat::kB8G8R8A8UNormIntSRGB:
        return;
      case PixelFormat::kS8UInt:
        return;
    }
    is_valid_ = true;
  }

  bool IsValid() const { return is_valid_; }

 private:
  bool is_valid_ = false;
};

// |Texture|
bool TextureGLES::OnSetContents(const uint8_t* contents,
                                size_t length,
                                size_t slice) {
  return OnSetContents(CreateMappingWithCopy(contents, length), slice);
}

// |Texture|
bool TextureGLES::OnSetContents(std::shared_ptr<const fml::Mapping> mapping,
                                size_t slice) {
  if (!mapping) {
    return false;
  }

  if (mapping->GetSize() == 0u) {
    return true;
  }

  if (mapping->GetMapping() == nullptr) {
    return false;
  }

  if (GetType() != Type::kTexture) {
    VALIDATION_LOG << "Incorrect texture usage flags for setting contents on "
                      "this texture object.";
    return false;
  }

  if (is_wrapped_) {
    VALIDATION_LOG << "Cannot set the contents of a wrapped texture.";
    return false;
  }

  const auto& tex_descriptor = GetTextureDescriptor();

  if (tex_descriptor.size.IsEmpty()) {
    return true;
  }

  if (!tex_descriptor.IsValid()) {
    return false;
  }

  if (mapping->GetSize() < tex_descriptor.GetByteSizeOfBaseMipLevel()) {
    return false;
  }

  GLenum texture_type;
  GLenum texture_target;
  switch (tex_descriptor.type) {
    case TextureType::kTexture2D:
      texture_type = GL_TEXTURE_2D;
      texture_target = GL_TEXTURE_2D;
      break;
    case TextureType::kTexture2DMultisample:
      VALIDATION_LOG << "Multisample texture uploading is not supported for "
                        "the OpenGLES backend.";
      return false;
    case TextureType::kTextureCube:
      texture_type = GL_TEXTURE_CUBE_MAP;
      texture_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice;
      break;
  }

  auto data = std::make_shared<TexImage2DData>(tex_descriptor.format,
                                               std::move(mapping));
  if (!data || !data->IsValid()) {
    VALIDATION_LOG << "Invalid texture format.";
    return false;
  }

  ReactorGLES::Operation texture_upload = [handle = handle_,            //
                                           data,                        //
                                           size = tex_descriptor.size,  //
                                           texture_type,                //
                                           texture_target               //
  ](const auto& reactor) {
    auto gl_handle = reactor.GetGLHandle(handle);
    if (!gl_handle.has_value()) {
      VALIDATION_LOG
          << "Texture was collected before it could be uploaded to the GPU.";
      return;
    }
    const auto& gl = reactor.GetProcTable();
    gl.BindTexture(texture_type, gl_handle.value());
    const GLvoid* tex_data = nullptr;
    if (data->data) {
      tex_data = data->data->GetMapping();
    }

    {
      TRACE_EVENT1("impeller", "TexImage2DUpload", "Bytes",
                   std::to_string(data->data->GetSize()).c_str());
      gl.TexImage2D(texture_target,         // target
                    0u,                     // LOD level
                    data->internal_format,  // internal format
                    size.width,             // width
                    size.height,            // height
                    0u,                     // border
                    data->external_format,  // external format
                    data->type,             // type
                    tex_data                // data
      );
    }
  };

  contents_initialized_ = reactor_->AddOperation(texture_upload);
  if (contents_initialized_) {
    reactor_->SetDebugLabel(handle_, label_);
  }
  return contents_initialized_;
}

// |Texture|
ISize TextureGLES::GetSize() const {
  return GetTextureDescriptor().size;
}

static std::optional<GLenum> ToRenderBufferFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::kB8G8R8A8UNormInt:
    case PixelFormat::kR8G8B8A8UNormInt:
      return GL_RGBA4;
    case PixelFormat::kS8UInt:
      return GL_STENCIL_INDEX8;
    case PixelFormat::kUnknown:
    case PixelFormat::kA8UNormInt:
    case PixelFormat::kR8G8B8A8UNormIntSRGB:
    case PixelFormat::kB8G8R8A8UNormIntSRGB:
      return std::nullopt;
  }
  FML_UNREACHABLE();
}

void TextureGLES::InitializeContentsIfNecessary() const {
  if (!IsValid()) {
    return;
  }
  if (contents_initialized_) {
    return;
  }
  contents_initialized_ = true;

  if (is_wrapped_) {
    return;
  }

  auto size = GetSize();

  if (size.IsEmpty()) {
    return;
  }

  const auto& gl = reactor_->GetProcTable();
  auto handle = reactor_->GetGLHandle(handle_);
  if (!handle.has_value()) {
    VALIDATION_LOG << "Could not initialize the contents of texture.";
    return;
  }

  switch (type_) {
    case Type::kTexture: {
      TexImage2DData tex_data(GetTextureDescriptor().format);
      if (!tex_data.IsValid()) {
        VALIDATION_LOG << "Invalid format for texture image.";
        return;
      }
      gl.BindTexture(GL_TEXTURE_2D, handle.value());
      {
        TRACE_EVENT0("impeller", "TexImage2DInitialization");
        gl.TexImage2D(GL_TEXTURE_2D,  // target
                      0u,             // LOD level (base mip level size checked)
                      tex_data.internal_format,  // internal format
                      size.width,                // width
                      size.height,               // height
                      0u,                        // border
                      tex_data.external_format,  // format
                      tex_data.type,             // type
                      nullptr                    // data
        );
      }

    } break;
    case Type::kRenderBuffer:
      auto render_buffer_format =
          ToRenderBufferFormat(GetTextureDescriptor().format);
      if (!render_buffer_format.has_value()) {
        VALIDATION_LOG << "Invalid format for render-buffer image.";
        return;
      }
      gl.BindRenderbuffer(GL_RENDERBUFFER, handle.value());
      {
        TRACE_EVENT0("impeller", "RenderBufferStorageInitialization");
        gl.RenderbufferStorage(GL_RENDERBUFFER,               // target
                               render_buffer_format.value(),  // internal format
                               size.width,                    // width
                               size.height                    // height
        );
      }

      break;
  }
  reactor_->SetDebugLabel(handle_, label_);
}

bool TextureGLES::Bind() const {
  if (!IsValid()) {
    return false;
  }
  auto handle = reactor_->GetGLHandle(handle_);
  if (!handle.has_value()) {
    return false;
  }
  const auto& gl = reactor_->GetProcTable();
  switch (type_) {
    case Type::kTexture:
      gl.BindTexture(GL_TEXTURE_2D, handle.value());
      break;
    case Type::kRenderBuffer:
      gl.BindRenderbuffer(GL_RENDERBUFFER, handle.value());
      break;
  }
  InitializeContentsIfNecessary();
  return true;
}

TextureGLES::Type TextureGLES::GetType() const {
  return type_;
}

static GLenum ToAttachmentPoint(TextureGLES::AttachmentPoint point) {
  switch (point) {
    case TextureGLES::AttachmentPoint::kColor0:
      return GL_COLOR_ATTACHMENT0;
    case TextureGLES::AttachmentPoint::kDepth:
      return GL_DEPTH_ATTACHMENT;
    case TextureGLES::AttachmentPoint::kStencil:
      return GL_STENCIL_ATTACHMENT;
  }
}

bool TextureGLES::SetAsFramebufferAttachment(GLuint fbo,
                                             AttachmentPoint point) const {
  if (!IsValid()) {
    return false;
  }
  InitializeContentsIfNecessary();
  auto handle = reactor_->GetGLHandle(handle_);
  if (!handle.has_value()) {
    return false;
  }
  const auto& gl = reactor_->GetProcTable();
  switch (type_) {
    case Type::kTexture:
      gl.FramebufferTexture2D(GL_FRAMEBUFFER,            // target
                              ToAttachmentPoint(point),  // attachment
                              GL_TEXTURE_2D,             // textarget
                              handle.value(),            // texture
                              0                          // level
      );
      break;
    case Type::kRenderBuffer:
      gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,            // target
                                 ToAttachmentPoint(point),  // attachment
                                 GL_RENDERBUFFER,  // render-buffer target
                                 handle.value()    // render-buffer
      );
      break;
  }
  return true;
}

}  // namespace impeller
