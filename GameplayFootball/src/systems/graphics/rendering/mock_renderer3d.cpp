// DZFoot Headless — MockRenderer3D stub implementation

#include "mock_renderer3d.hpp"
#include "base/log.hpp"
#include <cassert>
#include <vector>
#include <deque>
#include <list>

namespace blunted {

  MockRenderer3D::MockRenderer3D() {
    View v;
    v.x = 0; v.y = 0;
    v.width = 10; v.height = 10;
    v.target = e_ViewRenderTarget_Context;
    v.targetTexID = 0;
    v.gBufferID = 0;
    v.accumBufferID = 0;
    views.push_back(v);
  }

  MockRenderer3D::~MockRenderer3D() {}

  void MockRenderer3D::SwapBuffers() {}

  void MockRenderer3D::SetMatrix(const std::string &shaderUniformName, const Matrix4 &matrix) {}

  void MockRenderer3D::RenderOverlay2D(const std::vector<Overlay2DQueueEntry> &overlay2DQueue) {}
  void MockRenderer3D::RenderOverlay2D() {}
  void MockRenderer3D::RenderLights(std::deque<LightQueueEntry> &lightQueue, const Matrix4 &projectionMatrix, const Matrix4 &viewMatrix) {}

  bool MockRenderer3D::CreateContext(int width, int height, int bpp, bool fullscreen) {
    context_width = width; context_height = height; context_bpp = bpp;
    return true;
  }

  void MockRenderer3D::Exit() {}

  int MockRenderer3D::CreateView(float x_percent, float y_percent, float width_percent, float height_percent) {
    View v; v.x = (int)(x_percent * context_width); v.y = (int)(y_percent * context_height);
    v.width = (int)(width_percent * context_width); v.height = (int)(height_percent * context_height);
    v.target = e_ViewRenderTarget_Context; v.targetTexID = 0;
    v.gBufferID = 0; v.accumBufferID = 0;
    views.push_back(v);
    return (int)views.size() - 1;
  }

  View &MockRenderer3D::GetView(int viewID) {
    assert(viewID >= 0 && viewID < (int)views.size());
    return views.at(viewID);
  }

  void MockRenderer3D::DeleteView(int viewID) {}

  void MockRenderer3D::SetCullingMode(e_CullingMode cullingMode) {}
  void MockRenderer3D::SetBlendingMode(e_BlendingMode blendingMode) {}
  void MockRenderer3D::SetDepthFunction(e_DepthFunction depthFunction) {}
  void MockRenderer3D::SetDepthTesting(bool OnOff) {}
  void MockRenderer3D::SetDepthMask(bool OnOff) {}
  void MockRenderer3D::SetBlendingFunction(e_BlendingFunction blendingFunction1, e_BlendingFunction blendingFunction2) {}
  void MockRenderer3D::SetTextureMode(e_TextureMode textureMode) {}
  void MockRenderer3D::SetColor(const Vector3 &color, float alpha) {}
  void MockRenderer3D::SetColorMask(bool r, bool g, bool b, bool alpha) {}

  void MockRenderer3D::ClearBuffer(const Vector3 &color, bool clearDepth, bool clearColor) {}

  Matrix4 MockRenderer3D::CreatePerspectiveMatrix(float aspectRatio, float nearCap, float farCap) { return Matrix4(); }
  Matrix4 MockRenderer3D::CreateOrthoMatrix(float left, float right, float bottom, float top, float nearCap, float farCap) { return Matrix4(); }

  VertexBufferID MockRenderer3D::CreateVertexBuffer(float *vertices, unsigned int verticesDataSize, std::vector<unsigned int> indices, e_VertexBufferUsage usage) { return VertexBufferID(); }
  void MockRenderer3D::UpdateVertexBuffer(VertexBufferID vertexBufferID, float *vertices, unsigned int verticesDataSize) {}
  void MockRenderer3D::DeleteVertexBuffer(VertexBufferID vertexBufferID) {}
  void MockRenderer3D::RenderVertexBuffer(const std::deque<VertexBufferQueueEntry> &vertexBufferQueue, e_RenderMode renderMode) {}
  void MockRenderer3D::RenderAABB(std::list<VertexBufferQueueEntry> &vertexBufferQueue) {}
  void MockRenderer3D::RenderAABB(std::list<LightQueueEntry> &lightQueue) {}

  void MockRenderer3D::SetLight(const Vector3 &position, const Vector3 &color, float radius) {}

  int MockRenderer3D::CreateTexture(e_InternalPixelFormat internalPixelFormat, e_PixelFormat pixelFormat, int width, int height, bool alpha, bool repeat, bool mipmaps, bool filter, bool multisample, bool compareDepth) { return 0; }
  void MockRenderer3D::ResizeTexture(int textureID, SDL_Surface *source, e_InternalPixelFormat internalPixelFormat, e_PixelFormat pixelFormat, bool alpha, bool mipmaps) {}
  void MockRenderer3D::UpdateTexture(int textureID, SDL_Surface *source, bool alpha, bool mipmaps) {}
  void MockRenderer3D::DeleteTexture(int textureID) {}
  void MockRenderer3D::CopyFrameBufferToTexture(int textureID, int width, int height) {}
  void MockRenderer3D::BindTexture(int textureID) {}
  void MockRenderer3D::SetTextureUnit(int textureUnit) {}
  void MockRenderer3D::SetClientTextureUnit(int textureUnit) {}

  int MockRenderer3D::CreateFrameBuffer() { return 0; }
  void MockRenderer3D::DeleteFrameBuffer(int fbID) {}
  void MockRenderer3D::BindFrameBuffer(int fbID) {}
  void MockRenderer3D::SetFrameBufferRenderBuffer(e_TargetAttachment targetAttachment, int rbID) {}
  void MockRenderer3D::SetFrameBufferTexture2D(e_TargetAttachment targetAttachment, int texID) {}
  bool MockRenderer3D::CheckFrameBufferStatus() { return true; }
  void MockRenderer3D::SetFramebufferGammaCorrection(bool onOff) {}

  int MockRenderer3D::CreateRenderBuffer() { return 0; }
  void MockRenderer3D::DeleteRenderBuffer(int rbID) {}
  void MockRenderer3D::BindRenderBuffer(int rbID) {}
  void MockRenderer3D::SetRenderBufferStorage(e_InternalPixelFormat internalPixelFormat, int width, int height) {}

  void MockRenderer3D::SetRenderTargets(std::vector<e_TargetAttachment> targetAttachments) {}

  void MockRenderer3D::SetFOV(float angle) {}
  void MockRenderer3D::PushAttribute(int attr) {}
  void MockRenderer3D::PopAttribute() {}
  void MockRenderer3D::SetViewport(int x, int y, int width, int height) {}
  void MockRenderer3D::GetContextSize(int &width, int &height, int &bpp) { width = context_width; height = context_height; bpp = context_bpp; }
  void MockRenderer3D::SetPolygonOffset(float scale, float bias) {}

  void MockRenderer3D::LoadShader(const std::string &name, const std::string &filename) {}
  void MockRenderer3D::UseShader(const std::string &name) {}
  void MockRenderer3D::SetUniformInt(const std::string &shaderName, const std::string &varName, int value) {}
  void MockRenderer3D::SetUniformFloat(const std::string &shaderName, const std::string &varName, float value) {}
  void MockRenderer3D::SetUniformFloat2(const std::string &shaderName, const std::string &varName, float value1, float value2) {}
  void MockRenderer3D::SetUniformFloat3(const std::string &shaderName, const std::string &varName, float value1, float value2, float value3) {}
  void MockRenderer3D::SetUniformFloat3Array(const std::string &shaderName, const std::string &varName, int count, float *values) {}
  void MockRenderer3D::SetUniformMatrix4(const std::string &shaderName, const std::string &varName, const Matrix4 &mat) {}

  void MockRenderer3D::HDRCaptureOverallBrightness() {}
  float MockRenderer3D::HDRGetOverallBrightness() { return 1.0f; }

  void MockRenderer3D::operator()() {
    bool quit = false;
    while (!quit) {
      bool isMessage;
      boost::intrusive_ptr<Command> message = messageQueue.WaitForMessage(isMessage, 1);
      if (isMessage) {
        if (!message->Handle(this)) quit = true;
        message.reset();
      }
    }
  }

}
