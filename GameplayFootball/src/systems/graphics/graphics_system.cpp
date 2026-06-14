// written by bastiaan konings schuiling 2008 - 2014
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "graphics_system.hpp"

#include "graphics_scene.hpp"

#include "base/log.hpp"
#include "base/utils.hpp"

#include "managers/resourcemanagerpool.hpp"

#include "rendering/r3d_messages.hpp"
#include "rendering/mock_renderer3d.hpp"

namespace blunted {

  GraphicsSystem::GraphicsSystem() : systemType(e_SystemType_Graphics) {
    renderer3DTask = NULL;
    task = NULL;
  }

  GraphicsSystem::~GraphicsSystem() {
  }

  void GraphicsSystem::Initialize(const Properties &config) {
    bool doRender = config.GetBool("render", true);
    int w = config.GetInt("context_x", 1280);
    int h = config.GetInt("context_y", 720);
    Initialize(doRender, w, h);
  }

  void GraphicsSystem::Initialize(bool render, int width, int height) {

    textureResourceManager = boost::shared_ptr < ResourceManager<Texture> > (new ResourceManager<Texture>("texture"));
    vertexBufferResourceManager = boost::shared_ptr < ResourceManager<VertexBuffer> > (new ResourceManager<VertexBuffer>("vertexbuffer"));
    ResourceManagerPool::GetInstance().RegisterManager(e_ResourceType_Texture, textureResourceManager);
    ResourceManagerPool::GetInstance().RegisterManager(e_ResourceType_VertexBuffer, vertexBufferResourceManager);

    // start thread for renderer
    if (!render) {
      renderer3DTask = new MockRenderer3D();
    } else {
      renderer3DTask = new OpenGLRenderer3D();
    }
    this->width = width;
    this->height = height;
    bpp = 32;
    bool fullscreen = false;
    renderer3DTask->Run();

    boost::intrusive_ptr<Renderer3DMessage_CreateContext> createContext(new Renderer3DMessage_CreateContext(width, height, bpp, fullscreen));
    renderer3DTask->messageQueue.PushMessage(createContext);
    createContext->Wait();

    if (!createContext->success) {
      Log(e_FatalError, "GraphicsSystem", "Initialize", "Could not create context");
    } else {
      Log(e_Notice, "GraphicsSystem", "Initialize", "Created context, resolution " + int_to_str(width) + " * " + int_to_str(height) + " @ " + int_to_str(bpp) + " bpp");
    }

    task = new GraphicsTask(this);
    if (render) {
      task->Run();
    }
  }

  void GraphicsSystem::Exit() {
    // shutdown system task (only if it was started)
    if (task && task->thread.joinable()) {
      boost::intrusive_ptr<Message_Shutdown> shutdown(new Message_Shutdown());
      task->messageQueue.PushMessage(shutdown);
      shutdown->Wait();

      task->Join();
    }
    delete task;
    task = NULL;

    textureResourceManager.reset();
    vertexBufferResourceManager.reset();

    // shutdown renderer thread
    boost::intrusive_ptr<Message_Shutdown> R3Dshutdown(new Message_Shutdown());
    renderer3DTask->messageQueue.PushMessage(R3Dshutdown);
    R3Dshutdown->Wait();

    renderer3DTask->Join();
    delete renderer3DTask;
    renderer3DTask = NULL;
  }

  e_SystemType GraphicsSystem::GetSystemType() const {
    return systemType;
  }

  ISystemScene *GraphicsSystem::CreateSystemScene(boost::shared_ptr<IScene> scene) {
    if (scene->GetSceneType() == e_SceneType_Scene2D) {
      GraphicsScene *graphicsScene = new GraphicsScene(this);
      scene->Attach(graphicsScene->GetInterpreter(e_SceneType_Scene2D));
      return graphicsScene;
    }
    if (scene->GetSceneType() == e_SceneType_Scene3D) {
      GraphicsScene *graphicsScene = new GraphicsScene(this);
      scene->Attach(graphicsScene->GetInterpreter(e_SceneType_Scene3D));
      return graphicsScene;
    }
    return NULL;
  }

  ISystemTask *GraphicsSystem::GetTask() {
    return task;
  }

  Renderer3D *GraphicsSystem::GetRenderer3D() {
    return renderer3DTask;
  }

  MessageQueue<Overlay2DQueueEntry> &GraphicsSystem::GetOverlay2DQueue() {
    return overlay2DQueue;
  }

}
