#include "mainwidget.h"

#include <cmath>

#include <OpenGLBuffer>
#include <OpenGLFunctions>
#include <OpenGLInstanceGroup>
#include <OpenGLShaderProgram>
#include <OpenGLUniformBufferObject>
#include <OpenGLVertexArrayObject>
#include <OpenGLFramebufferObject>
#include <OpenGLRenderbufferObject>
#include <OpenGLTexture>
#include <OpenGLInstance>
#include <OpenGLMaterial>
#include <OpenGLPointLight>
#include <OpenGLPointLightGroup>

#include <KCamera3D>
#include <OpenGLDebugDraw>
#include <KInputManager>
#include <KMatrix4x4>
#include <KPanGesture>
#include <KPinchGesture>
#include <KPointF>
#include <KTransform3D>
#include <KVector2D>
#include <KVertex>

#include <Qt>
#include <QElapsedTimer>
#include <QFileDialog>
#include <OpenGLMesh>
#include <KHalfEdgeMesh>
#include <KLinq>
#include <OpenGLUniformBufferManager>

static uint64_t sg_count = 0;
#define DEFERRED_TEXTURES 4

enum DeferredData
{
  DepthData,
  LinearDepthData,
  PositionData,
  NormalData,
  DiffuseData,
  SpecularData,
  VelocityData,
  AmbientPass,
  MotionBlurPass,
  LightPass,
  DeferredDataCount
};

/*******************************************************************************
 * MainWidgetPrivate
 ******************************************************************************/
class MainWidgetPrivate : protected OpenGLFunctions
{
public:
  MainWidgetPrivate(MainWidget *parent);
  void initializeGL();
  void loadObj(const QString &fileName);
  void openObj();
  void drawBoundaries();
  void updateBackbuffer(int w, int h);
  void drawBackbuffer();
  void constructDeferredTexture(OpenGLTexture &t, OpenGLInternalFormat f);

  // Transformations
  KCamera3D m_camera;
  KCamera3D m_cameraPrev;
  KTransform3D m_transform;
  KMatrix4x4 m_projection;
  float m_depthFar;
  float m_depthNear;
  float m_depthDiff;

  // OpenGL State Information
  float m_width;
  float m_height;
  OpenGLMesh *m_openGLMesh;
  KHalfEdgeMesh *m_halfEdgeMesh;
  KHalfEdgeMesh *m_quad;
  KHalfEdgeMesh *m_floor;
  OpenGLMesh *m_quadGL;
  typedef std::tuple<KVector3D,KVector3D> QueryResultType;
  std::vector<QueryResultType> m_boundaries;
  OpenGLShaderProgram *m_program;
  OpenGLShaderProgram *m_textureDrawer;
  OpenGLUniformBufferObject m_matrixBlock;
  OpenGLInstanceGroup *m_instanceGroup;
  OpenGLInstanceGroup *m_floorGroup;
  OpenGLInstance *m_floorInstance;
  OpenGLShaderProgram *m_pointLightProgram;
  OpenGLPointLightGroup *m_pointLightGroup;
  std::vector<OpenGLPointLight*> m_pointLights;
  bool m_paused;

  // GBuffer
  DeferredData m_buffer;
  OpenGLShaderProgram *m_deferredPrograms[DeferredDataCount];
  OpenGLFramebufferObject *m_deferredBuffer;
  OpenGLFramebufferObject *m_lightBuffer;
  OpenGLTexture m_deferredTextures[DEFERRED_TEXTURES];
  OpenGLTexture m_backBuffer;
  OpenGLTexture m_depthTexture;
  std::vector<OpenGLInstance*> m_instances;
  float m_ambientColor[4];

  // Touch Information
  float m_dragVelocity;
  KVector3D m_dragAxis;

  // Parent
  MainWidget *m_parent;
};

MainWidgetPrivate::MainWidgetPrivate(MainWidget *parent) :
  m_openGLMesh(Q_NULLPTR), m_halfEdgeMesh(Q_NULLPTR),
  m_program(Q_NULLPTR), m_parent(parent), m_width(1), m_height(1),
  m_buffer(LightPass), m_paused(false)
{
  m_deferredBuffer = 0;
  m_ambientColor[0] = m_ambientColor[1] = m_ambientColor[2] = 0.2f;
  m_ambientColor[3] = 1.0f;
}

void MainWidgetPrivate::initializeGL()
{
  initializeOpenGLFunctions();
}

void MainWidgetPrivate::loadObj(const QString &fileName)
{
  // Remove old mesh
  bool oldValue = m_paused;
  m_paused = true;
  delete m_openGLMesh;
  delete m_halfEdgeMesh;
  m_boundaries.clear();

  // Initialize an object
  quint64 ms;
  QElapsedTimer timer;
  {
    {
      timer.start();
      m_halfEdgeMesh = new KHalfEdgeMesh(m_parent, fileName);
      ms = timer.elapsed();
      qDebug() << "Create HalfEdgeMesh (sec):" << float(ms) / 1e3f;
    }
    {
      m_parent->makeCurrent();
      timer.start();
      m_openGLMesh = m_halfEdgeMesh->createOpenGLMesh(OpenGLMesh::Contiguous | OpenGLMesh::Interleaved);
      m_instanceGroup->setMesh(m_openGLMesh);
      ms = timer.elapsed();
      qDebug() << "Create OpenGLMesh (sec)  :" << float(ms) / 1e3f;
    }
    auto query =
      SELECT
        FROM ( edge : m_halfEdgeMesh->halfEdges() )
        WHERE ( edge.face == 0 )
        JOIN ( m_halfEdgeMesh->vertex(edge.to)->position,
               m_halfEdgeMesh->vertex(m_halfEdgeMesh->halfEdge(edge.next)->to)->position );
    {
      timer.start();
      m_boundaries = query();
      ms = timer.elapsed();
      qDebug() << "Mesh Query Time (sec)    :" << float(ms) / 1e3f;
    }
    qDebug() << "--------------------------------------";
    qDebug() << "Mesh Vertexes  :" << m_halfEdgeMesh->vertices().size();
    qDebug() << "Mesh Faces     :" << m_halfEdgeMesh->faces().size();
    qDebug() << "Mesh HalfEdges :" << m_halfEdgeMesh->halfEdges().size();
    qDebug() << "Boundary Edges :" << m_boundaries.size();
    qDebug() << "Polygons /Frame:" << m_halfEdgeMesh->faces().size() * sg_count;
  }

  m_paused = oldValue;
}

void MainWidgetPrivate::openObj()
{
  QString fileName = QFileDialog::getOpenFileName(
    m_parent, m_parent->tr("Open Model"),
    ".",
    m_parent->tr("Wavefront Object File (*.obj))")
  );
  if (!fileName.isNull())
  {
    loadObj(fileName);
  }
}

void MainWidgetPrivate::drawBoundaries()
{
  KMatrix4x4 const &modelToWorld = m_transform.toMatrix();
  for (QueryResultType const &line : m_boundaries)
  {
    QVector3D origin = modelToWorld * std::get<0>(line);
    QVector3D to     = modelToWorld * std::get<1>(line);
    OpenGLDebugDraw::World::drawLine(origin, to, Qt::red);
  }
}

void MainWidgetPrivate::updateBackbuffer(int w, int h)
{
  m_width = w;
  m_height = h;

  // GBuffer Texture Storage
  for (int i = 0; i < DEFERRED_TEXTURES; ++i)
  {
    constructDeferredTexture(m_deferredTextures[i], OpenGLInternalFormat::Rgba32F);
  }

  // Backbuffer Texture
  constructDeferredTexture(m_backBuffer, OpenGLInternalFormat::Rgba32F);
  constructDeferredTexture(m_depthTexture, OpenGLInternalFormat::Depth32F);

  // GBuffer Framebuffer
  m_deferredBuffer->bind();
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment0, m_backBuffer);
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment1, m_deferredTextures[0]);
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment2, m_deferredTextures[1]);
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment3, m_deferredTextures[2]);
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment4, m_deferredTextures[3]);
  m_deferredBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::DepthAttachment,  m_depthTexture);
  m_deferredBuffer->drawBuffers(OpenGLFramebufferObject::ColorAttachment0, OpenGLFramebufferObject::ColorAttachment1, OpenGLFramebufferObject::ColorAttachment2, OpenGLFramebufferObject::ColorAttachment3, OpenGLFramebufferObject::ColorAttachment4);

  // Check Framebuffer validity
  switch(m_deferredBuffer->status())
  {
  case OpenGLFramebufferObject::Complete:
    break;
  case OpenGLFramebufferObject::IncompleteAttachment:
    qFatal("Incomplete Attachment");
    break;
  case OpenGLFramebufferObject::IncompleteMissingAttachment:
    qFatal("Incomplete Missing Attachment");
    break;
  case OpenGLFramebufferObject::IncompleteDrawBuffer:
    qFatal("Incomplete Draw Buffer");
    break;
  case OpenGLFramebufferObject::IncompleteReadBuffer:
    qFatal("Incomplete Read Buffer");
    break;
  case OpenGLFramebufferObject::Unsupported:
    qFatal("Unsupported");
    break;
  }

  m_deferredBuffer->release();

  // Light Buffer
  m_lightBuffer->bind();
  m_lightBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::ColorAttachment0, m_deferredTextures[3]);
  m_lightBuffer->attachTexture2D(OpenGLFramebufferObject::TargetDraw, OpenGLFramebufferObject::DepthAttachment, m_depthTexture);
  m_lightBuffer->drawBuffers(OpenGLFramebufferObject::ColorAttachment0);

  // Check Framebuffer validity
  switch(m_lightBuffer->status())
  {
  case OpenGLFramebufferObject::Complete:
    break;
  case OpenGLFramebufferObject::IncompleteAttachment:
    qFatal("Incomplete Attachment");
    break;
  case OpenGLFramebufferObject::IncompleteMissingAttachment:
    qFatal("Incomplete Missing Attachment");
    break;
  case OpenGLFramebufferObject::IncompleteDrawBuffer:
    qFatal("Incomplete Draw Buffer");
    break;
  case OpenGLFramebufferObject::IncompleteReadBuffer:
    qFatal("Incomplete Read Buffer");
    break;
  case OpenGLFramebufferObject::Unsupported:
    qFatal("Unsupported");
    break;
  }

  m_lightBuffer->release();
}

void MainWidgetPrivate::drawBackbuffer()
{
  OpenGLMarkerScoped _("Present G Buffer");
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glActiveTexture(GL_TEXTURE0);
  m_deferredTextures[0].bind();
  glActiveTexture(GL_TEXTURE1);
  m_deferredTextures[1].bind();
  glActiveTexture(GL_TEXTURE2);
  m_deferredTextures[2].bind();
  glActiveTexture(GL_TEXTURE3);
  m_backBuffer.bind();
  glActiveTexture(GL_TEXTURE4);
  m_deferredTextures[3].bind();
  glActiveTexture(GL_TEXTURE5);
  m_depthTexture.bind();
  if (m_buffer == LightPass || m_buffer == MotionBlurPass)
  {
    OpenGLMarkerScoped _("Light Pass");
    m_lightBuffer->bind();
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthFunc(GL_GREATER);
    m_pointLightProgram->bind();
    m_pointLightGroup->draw();
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, m_parent->defaultFramebufferObject());
  }
  m_deferredPrograms[m_buffer]->bind();
  m_quadGL->draw();
  m_deferredPrograms[m_buffer]->release();
  m_backBuffer.release();
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void MainWidgetPrivate::constructDeferredTexture(OpenGLTexture &t, OpenGLInternalFormat f)
{
  t.create(OpenGLTexture::Texture2D);
  t.bind();
  t.setInternalFormat(f);
  t.setWrapMode(OpenGLTexture::DirectionS, OpenGLTexture::ClampToEdge);
  t.setWrapMode(OpenGLTexture::DirectionT, OpenGLTexture::ClampToEdge);
  t.setFilter(OpenGLTexture::Magnification, OpenGLTexture::Nearest);
  t.setFilter(OpenGLTexture::Minification, OpenGLTexture::Nearest);
  t.setSize(m_width, m_height);
  t.allocate();
  t.release();
}

/*******************************************************************************
 * MainWidget
 ******************************************************************************/
MainWidget::MainWidget(QWidget *parent) :
  OpenGLWidget(parent), m_private(new MainWidgetPrivate(this))
{
  P(MainWidgetPrivate);
  p.m_transform.scale(50.0f);
  p.m_transform.translate(0.0f, 0.0f, -150.0f);
  p.m_dragVelocity = 0.0f;
  OpenGLShaderProgram::addSharedIncludePath(":/resources/shaders");
  OpenGLShaderProgram::addSharedIncludePath(":/resources/shaders/ubo");
}

MainWidget::~MainWidget()
{
  makeCurrent();
  delete m_private;
}

static GLuint buffers[1];

/*******************************************************************************
 * OpenGL Methods
 ******************************************************************************/
void MainWidget::initializeGL()
{
  P(MainWidgetPrivate);
  p.initializeGL();
  OpenGLWidget::initializeGL();
  printVersionInformation();

  // Set global information
  glEnable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glClearDepth(1.0f);
  glDepthFunc(GL_LEQUAL);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  p.m_quad = new KHalfEdgeMesh(this, ":/resources/objects/quad.obj");
  p.m_quadGL = p.m_quad->createOpenGLMesh(OpenGLMesh::Contiguous);

  // Application-specific initialization
  {
    // Uniform Block Object
    p.m_matrixBlock.create();
    p.m_matrixBlock.bind(1);
    p.m_matrixBlock.setUsagePattern(OpenGLBuffer::DynamicDraw);
    p.m_matrixBlock.allocate(sizeof(GLfloat) * (16 * 10 + 4 + 5));
    OpenGLUniformBufferManager::addUniformBufferObject("GlobalBuffer", &p.m_matrixBlock);

    // Create Shader for GBuffer Pass
    p.m_program = new OpenGLShaderProgram(this);
    p.m_program->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/resources/shaders/gbuffer.vert");
    p.m_program->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/resources/shaders/gbuffer.frag");
    p.m_program->link();

    // Create Shader for point light pass
    p.m_pointLightProgram = new OpenGLShaderProgram(this);
    p.m_pointLightProgram->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/resources/shaders/lighting/pointLight.vert");
    p.m_pointLightProgram->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/resources/shaders/lighting/pointLight.frag");
    p.m_pointLightProgram->link();
    p.m_pointLightProgram->bind();
    p.m_pointLightProgram->setUniformValue("geometryTexture", 0);
    p.m_pointLightProgram->setUniformValue("materialTexture", 1);
    p.m_pointLightProgram->setUniformValue("dynamicsTexture", 2);
    p.m_pointLightProgram->setUniformValue("backbufferTexture", 3);
    p.m_pointLightProgram->setUniformValue("lightbufferTexture", 4);
    p.m_pointLightProgram->setUniformValue("depthTexture", 5);
    p.m_pointLightProgram->release();

    char const* fragFiles[DeferredDataCount] = {
      ":/resources/shaders/gbuffer/depth.frag",
      ":/resources/shaders/gbuffer/linearDepth.frag",
      ":/resources/shaders/gbuffer/position.frag",
      ":/resources/shaders/gbuffer/normal.frag",
      ":/resources/shaders/gbuffer/diffuse.frag",
      ":/resources/shaders/gbuffer/specular.frag",
      ":/resources/shaders/gbuffer/velocity.frag",
      ":/resources/shaders/gbuffer/ambient.frag",
      ":/resources/shaders/gbuffer/motion.frag",
      ":/resources/shaders/gbuffer/backbuffer.frag"
    };
    for (int i = 0; i < DeferredDataCount; ++i)
    {
      p.m_deferredPrograms[i] = new OpenGLShaderProgram(this);
      p.m_deferredPrograms[i]->addIncludePath(":/resources/shaders");
      p.m_deferredPrograms[i]->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/resources/shaders/gbuffer/main.vert");
      p.m_deferredPrograms[i]->addShaderFromSourceFile(QOpenGLShader::Fragment, fragFiles[i]);
      p.m_deferredPrograms[i]->link();
      p.m_deferredPrograms[i]->bind();
      p.m_deferredPrograms[i]->setUniformValue("geometryTexture", 0);
      p.m_deferredPrograms[i]->setUniformValue("materialTexture", 1);
      p.m_deferredPrograms[i]->setUniformValue("dynamicsTexture", 2);
      p.m_deferredPrograms[i]->setUniformValue("backbufferTexture", 3);
      p.m_deferredPrograms[i]->setUniformValue("lightbufferTexture", 4);
      p.m_deferredPrograms[i]->setUniformValue("depthTexture", 5);
      p.m_deferredPrograms[i]->release();
    }

    // Framebuffer Object
    p.m_deferredBuffer = new OpenGLFramebufferObject;
    p.m_deferredBuffer->create();
    p.m_lightBuffer = new OpenGLFramebufferObject;
    p.m_lightBuffer->create();

    p.m_pointLightGroup = new OpenGLPointLightGroup(this);
    KHalfEdgeMesh *mesh1 = new KHalfEdgeMesh(this, ":/resources/objects/pointLight.obj");
    OpenGLMesh * oglMesh1 = mesh1->createOpenGLMesh(OpenGLMesh::Contiguous);
    p.m_pointLightGroup->setMesh(oglMesh1);
    for (int i = 0; i < 10; ++i)
    {
      OpenGLPointLight *l = p.m_pointLightGroup->createLight();
      l->setRadius(10.0f);
      p.m_pointLights.push_back(l);
    }
    // Open OBJ
    p.m_instanceGroup = new OpenGLInstanceGroup(this);
    p.m_floorGroup = new OpenGLInstanceGroup(this);
    KHalfEdgeMesh *mesh = new KHalfEdgeMesh(this, ":/resources/objects/floor.obj");
    OpenGLMesh *oglMesh = mesh->createOpenGLMesh(OpenGLMesh::Contiguous);
    p.m_floorGroup->setMesh(oglMesh);
    p.m_floorInstance = p.m_floorGroup->createInstance();
    p.m_floorInstance->material().setDiffuse(0.0f, 0.0f, 1.0f);
    p.m_floorInstance->material().setSpecular(0.5f, 0.5f, 0.5f, 1.0f);
    p.m_floorInstance->transform().setScale(100.0f);
    p.m_floorInstance->transform().setTranslation(0.0f, -2.0f, 0.0f);
    p.loadObj(":/resources/objects/sphere.obj");

    // Initialize instances
    for (int level = 0; level < 1; ++level)
    {
      for (int deg = 0; deg < 360; deg += 30)
      {
        float cosine = std::cos(deg * 3.14159f / 180.0f);
        float sine = std::sin(deg * 3.14159f / 180.0f);
        OpenGLInstance * instance = p.m_instanceGroup->createInstance();
        instance->currentTransform().setScale(1.0f);
        instance->material().setDiffuse(deg / 360.0f, 1.0f - deg / 360.0f, 0.0f);
        instance->material().setSpecular(1.0f, 1.0f, 1.0f, 16.0f);
        instance->currentTransform().setTranslation(cosine * 15, level * 5, sine * 15);
        p.m_instances.push_back(instance);
        ++sg_count;
      }
    }
    qDebug() << "Instances: " << sg_count;

    // Release (unbind) all
    p.m_program->release();
  }

  OpenGLDebugDraw::initialize();
}

void MainWidget::resizeGL(int width, int height)
{
  P(MainWidgetPrivate);
  p.m_depthFar = 1000.0f;
  p.m_depthNear = 0.1f;
  p.m_projection.setToIdentity();
  p.m_projection.perspective(45.0f, width / float(height), p.m_depthNear, p.m_depthFar);
  p.updateBackbuffer(width, height);
  p.m_depthDiff = p.m_depthFar - p.m_depthNear;
  OpenGLWidget::resizeGL(width, height);
}

void MainWidget::paintGL()
{
  P(MainWidgetPrivate);

  if (!p.m_paused)
  {
    OpenGLProfiler::BeginFrame();
    {
      OpenGLMarkerScoped _("Total Render Time");
      p.m_program->bind();
      {
        OpenGLMarkerScoped _("Prepare Scene");
        p.m_matrixBlock.write(0, p.m_camera.toMatrix().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16, p.m_projection.constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 2, (p.m_projection * p.m_camera.toMatrix()).constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 3, p.m_camera.toMatrix().inverted().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 4, p.m_projection.inverted().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 5, (p.m_projection * p.m_camera.toMatrix()).inverted().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 6,  p.m_cameraPrev.toMatrix().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 7,  (p.m_projection * p.m_cameraPrev.toMatrix()).constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 8, p.m_cameraPrev.toMatrix().inverted().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 9, (p.m_projection * p.m_cameraPrev.toMatrix()).inverted().constData(), sizeof(float) * 16);
        p.m_matrixBlock.write(sizeof(float) * 16 * 10, p.m_ambientColor, sizeof(float) * 4);
        p.m_matrixBlock.write(sizeof(float) * (16 * 10 + 4), &p.m_depthFar, sizeof(float));
        p.m_matrixBlock.write(sizeof(float) * (16 * 10 + 5), &p.m_depthNear, sizeof(float));
        p.m_matrixBlock.write(sizeof(float) * (16 * 10 + 6), &p.m_depthDiff, sizeof(float));
        p.m_matrixBlock.write(sizeof(float) * (16 * 10 + 7), &p.m_width, sizeof(float));
        p.m_matrixBlock.write(sizeof(float) * (16 * 10 + 8), &p.m_height, sizeof(float));
        p.m_instanceGroup->update(p.m_camera.toMatrix(), p.m_cameraPrev.toMatrix());
        p.m_floorGroup->update(p.m_camera.toMatrix(), p.m_cameraPrev.toMatrix());
        p.m_pointLightGroup->update(p.m_projection, p.m_camera.toMatrix());
      }
      {
        OpenGLMarkerScoped _("Generate G Buffer");
        p.m_deferredBuffer->bind();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        p.m_floorGroup->draw();
        p.m_instanceGroup->draw();
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
      }
      p.m_program->release();
      p.drawBackbuffer();
    }
    OpenGLProfiler::EndFrame();
    OpenGLDebugDraw::draw();
    OpenGLWidget::paintGL();
  }
}

void MainWidget::teardownGL()
{
  OpenGLDebugDraw::teardown();
  OpenGLWidget::teardownGL();
}

/*******************************************************************************
 * Events
 ******************************************************************************/
void MainWidget::updateEvent(KUpdateEvent *event)
{
  P(MainWidgetPrivate);
  (void)event;
  p.m_cameraPrev = p.m_camera;

  // Update instances
  float angle = 0.0f;
  KVector3D translation;
  for (OpenGLInstance *instance : p.m_instances)
  {
    instance->currentTransform().rotate(angle, 0.0f, 0.0, 3.0f);
    angle += 1.0f;
  }

  static float f = 0.0f;
  f += 0.0016f;
  angle = f;
  for (OpenGLPointLight *instance : p.m_pointLights)
  {
    instance->setTranslation(cos(angle) * 17.0f, 0.0f, sin(angle) * 17.0f);
    angle += 2 * 3.1415926 / p.m_pointLights.size();
  }

  // Camera Transformation
  if (KInputManager::buttonPressed(Qt::RightButton))
  {
    float transSpeed = 3.0f;
    float rotSpeed   = 0.5f;

    if (KInputManager::keyPressed(Qt::Key_Control))
    {
      transSpeed = 1.0f;
    }

    // Handle rotations
    p.m_camera.rotate(-rotSpeed * KInputManager::mouseDelta().x(), KCamera3D::LocalUp);
    p.m_camera.rotate(-rotSpeed * KInputManager::mouseDelta().y(), p.m_camera.right());

    // Handle translations
    QVector3D translation;
    if (KInputManager::keyPressed(Qt::Key_W))
    {
      translation += p.m_camera.forward();
    }
    if (KInputManager::keyPressed(Qt::Key_S))
    {
      translation -= p.m_camera.forward();
    }
    if (KInputManager::keyPressed(Qt::Key_A))
    {
      translation -= p.m_camera.right();
    }
    if (KInputManager::keyPressed(Qt::Key_D))
    {
      translation += p.m_camera.right();
    }
    if (KInputManager::keyPressed(Qt::Key_E))
    {
      translation -= p.m_camera.up();
    }
    if (KInputManager::keyPressed(Qt::Key_Q))
    {
      translation += p.m_camera.up();
    }
    p.m_camera.translate(transSpeed * translation);
  }

  if (KInputManager::keyPressed(Qt::Key_Control))
  {
    if (KInputManager::keyTriggered(Qt::Key_O))
    {
      p.openObj();
    }
  }

  // Change Buffer
  if (KInputManager::keyPressed(Qt::Key_0))
  {
    p.m_buffer = (DeferredData)LightPass;
  }
  if (KInputManager::keyPressed(Qt::Key_1))
  {
    p.m_buffer = (DeferredData)0;
  }
  if (KInputManager::keyPressed(Qt::Key_2))
  {
    p.m_buffer = (DeferredData)1;
  }
  if (KInputManager::keyPressed(Qt::Key_3))
  {
    p.m_buffer = (DeferredData)2;
  }
  if (KInputManager::keyPressed(Qt::Key_4))
  {
    p.m_buffer = (DeferredData)3;
  }
  if (KInputManager::keyPressed(Qt::Key_5))
  {
    p.m_buffer = (DeferredData)4;
  }
  if (KInputManager::keyPressed(Qt::Key_6))
  {
    p.m_buffer = (DeferredData)5;
  }
  if (KInputManager::keyPressed(Qt::Key_7))
  {
    p.m_buffer = (DeferredData)6;
  }
  if (KInputManager::keyPressed(Qt::Key_8))
  {
    p.m_buffer = (DeferredData)7;
  }
  if (KInputManager::keyPressed(Qt::Key_9))
  {
    p.m_buffer = (DeferredData)8;
  }

  // Pinching will grow/shrink
  KPinchGesture pinch;
  if (KInputManager::pinchGesture(&pinch))
  {
    p.m_transform.scale(pinch.scaleFactor());
    p.m_transform.rotate(pinch.lastRotationAngle() - pinch.rotationAngle(), 0.0f, 0.0f, 1.0f);
  }

  // Panning will translate
  KPanGesture pan;
  if (KInputManager::panGesture(&pan))
  {
    KVector3D delta = KVector3D(pan.delta().x(), -pan.delta().y(), 0.0f) * 0.1f;
    p.m_transform.translate(delta);
  }

  // Touching will rotate
  if (KInputManager::touchCount() == 1)
  {
    KTouchPoint touch = KInputManager::touchPoint(0);
    KPointF delta = touch.pos() - touch.lastPos();
    KVector3D axis(delta.y(), delta.x(), 0.0f);
    switch (touch.state())
    {
    case Qt::TouchPointPressed:
      p.m_dragVelocity = 0.0f;
      break;
    case Qt::TouchPointMoved:
      p.m_dragAxis = p.m_camera.rotation().rotatedVector(axis);
      p.m_dragVelocity = axis.length() * 0.1f;
      p.m_dragAxis.normalize();
      break;
    default:
      break;
    }
  }

  // Rotate from drag gesture
  p.m_dragVelocity *= 0.9f;
  p.m_transform.rotate(p.m_dragVelocity, p.m_dragAxis);
}