/****************************************************************************
Copyright (c) 2008-2010 Ricardo Quesada
Copyright (c) 2010-2012 cocos2d-x.org
Copyright (c) 2011      Zynga Inc.
Copyright (c) 2013-2014 Chukong Technologies Inc.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/

#include "2d/CCScene.h"
#include "base/CCDirector.h"
#include "base/CCCamera.h"
#include "base/CCEventDispatcher.h"
#include "base/CCEventListenerCustom.h"
#include "renderer/CCRenderer.h"
#include "deprecated/CCString.h"

#if CC_USE_PHYSICS
#include "physics/CCPhysicsWorld.h"
#endif

NS_CC_BEGIN

Scene::Scene()
#if CC_USE_PHYSICS
: _physicsWorld(nullptr)
#endif
{
    _ignoreAnchorPointForPosition = true;
    setAnchorPoint(Vec2(0.5f, 0.5f));
    
    //create default camera
    _defaultCamera = Camera::create();
    addChild(_defaultCamera);
    
    _leftVRCamera = _rightVRCamera = nullptr;
    
    _event = Director::getInstance()->getEventDispatcher()->addCustomEventListener(Director::EVENT_PROJECTION_CHANGED, std::bind(&Scene::onProjectionChanged, this, std::placeholders::_1));
    _event->retain();
}

Scene::~Scene()
{
#if CC_USE_PHYSICS
    CC_SAFE_DELETE(_physicsWorld);
#endif
    Director::getInstance()->getEventDispatcher()->removeEventListener(_event);
    CC_SAFE_RELEASE(_event);
}

bool Scene::init()
{
    auto size = Director::getInstance()->getWinSize();
    return initWithSize(size);
}

bool Scene::initWithSize(const Size& size)
{
    setContentSize(size);
    return true;
}

Scene* Scene::create()
{
    Scene *ret = new (std::nothrow) Scene();
    if (ret && ret->init())
    {
        ret->autorelease();
        return ret;
    }
    else
    {
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
}

Scene* Scene::createWithSize(const Size& size)
{
    Scene *ret = new (std::nothrow) Scene();
    if (ret && ret->initWithSize(size))
    {
        ret->autorelease();
        return ret;
    }
    else
    {
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
}

std::string Scene::getDescription() const
{
    return StringUtils::format("<Scene | tag = %d>", _tag);
}

Scene* Scene::getScene() const
{
    // FIX ME: should use const_case<> to fix compiling error
    return const_cast<Scene*>(this);
}

void Scene::onProjectionChanged(EventCustom* event)
{
    if (_defaultCamera)
    {
        _defaultCamera->initDefault();
    }
}

void Scene::render(Renderer* renderer)
{
    auto director = Director::getInstance();
    Camera* defaultCamera = nullptr;
    Rect viewrect(0.0f, 0.0f, 1.0f, 1.0f);
    for (const auto& camera : _cameras)
    {
        Camera::_visitingCamera = camera;
        if (Camera::_visitingCamera->getCameraFlag() == CameraFlag::DEFAULT)
        {
            defaultCamera = Camera::_visitingCamera;
            continue;
        }
        
        director->pushMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION);
        director->loadMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION, Camera::_visitingCamera->getViewProjectionMatrix());
        
        
        const auto& cameraViewRect = camera->getNormalizeViewPortRect();
        if (viewrect.origin.x != cameraViewRect.origin.x || viewrect.origin.y != cameraViewRect.origin.y
            || viewrect.size.width != cameraViewRect.size.width || viewrect.size.height != cameraViewRect.size.height)
        {
            director->setViewport();
            viewrect = cameraViewRect;
        }
        //visit the scene
        visit(renderer, Mat4::IDENTITY, 0);
        renderer->render();
        
        director->popMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION);
    }
    //draw with default camera
    if (defaultCamera)
    {
        Camera::_visitingCamera = defaultCamera;
        director->pushMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION);
        director->loadMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION, Camera::_visitingCamera->getViewProjectionMatrix());
        
        //visit the scene
        visit(renderer, Mat4::IDENTITY, 0);
        const auto& cameraViewRect = defaultCamera->getNormalizeViewPortRect();
        if (viewrect.origin.x != cameraViewRect.origin.x || viewrect.origin.y != cameraViewRect.origin.y
            || viewrect.size.width != cameraViewRect.size.width || viewrect.size.height != cameraViewRect.size.height)
        {
            director->setViewport();
            viewrect = cameraViewRect;
        }
        renderer->render();
        
        director->popMatrix(MATRIX_STACK_TYPE::MATRIX_STACK_PROJECTION);
    }
    Camera::_visitingCamera = nullptr;
    
    // reset view port
    if (viewrect.origin.x != 0.f || viewrect.origin.y != 0.f || viewrect.size.width != 1.f|| viewrect.size.height != 1.f)
        director->setViewport();
}

void Scene::enableVR(float distanceBetweenEyes, CameraFlag cameraflag)
{
    auto s = Director::getInstance()->getWinSize();
    if (_leftVRCamera == nullptr)
    {
        float ratio = (GLfloat)s.width * 0.5f / s.height;
        _leftVRCamera = Camera::createPerspective(60, ratio, 1.0f, 1000.0f);
        _rightVRCamera = Camera::createPerspective(60, ratio, 1.0f, 1000.0f);
        
        _leftVRCamera->setNormalizedViewPortRect(0.f, 0.f, 0.5f, 1.f);
        _rightVRCamera->setNormalizedViewPortRect(0.5f, 0.f, 0.5f, 1.f);
        addChild(_leftVRCamera);
        addChild(_rightVRCamera);
        _leftVRCamera->setScene(this);
        _rightVRCamera->setScene(this);
    }
    _leftVRCamera->setCameraFlag(cameraflag);
    _rightVRCamera->setCameraFlag(cameraflag);
    
    _leftVRCamera->setPosition3D(_defaultCamera->getPosition3D() - Vec3(distanceBetweenEyes / 2.f, 0.f, 0.f));
    _rightVRCamera->setPosition3D(_defaultCamera->getPosition3D() + Vec3(distanceBetweenEyes / 2.f, 0.f, 0.f));
    
    setVRHeadPosAndRot(_defaultCamera->getPosition3D(), _defaultCamera->getRotation3D());
}

void Scene::disableVR()
{
    if (_leftVRCamera && _rightVRCamera)
    {
        removeChild(_leftVRCamera);
        removeChild(_rightVRCamera);
        _leftVRCamera = _rightVRCamera = nullptr;
    }
}

void Scene::setVRHeadPosAndRot(const Vec3& pos, const Vec3& rot)
{
    if (_leftVRCamera && _rightVRCamera)
    {
        float distanceBetweenEyes = (_leftVRCamera->getPosition3D() - _rightVRCamera->getPosition3D()).length();
        
        _leftVRCamera->setRotation3D(rot);
        _rightVRCamera->setRotation3D(rot);
        auto mat = _leftVRCamera->getNodeToParentTransform();
        Vec3 offset(mat.m[0], mat.m[1], mat.m[2]);
        offset.normalize();
        offset *= distanceBetweenEyes / 2.f;
        
        _leftVRCamera->setPosition3D(pos - offset);
        _rightVRCamera->setPosition3D(pos + offset);
    }
}

#if CC_USE_PHYSICS
void Scene::addChild(Node* child, int zOrder, int tag)
{
    Node::addChild(child, zOrder, tag);
    addChildToPhysicsWorld(child);
}

void Scene::addChild(Node* child, int zOrder, const std::string &name)
{
    Node::addChild(child, zOrder, name);
    addChildToPhysicsWorld(child);
}

void Scene::update(float delta)
{
    Node::update(delta);
    if (nullptr != _physicsWorld && _physicsWorld->isAutoStep())
    {
        _physicsWorld->update(delta);
    }
}

Scene* Scene::createWithPhysics()
{
    Scene *ret = new (std::nothrow) Scene();
    if (ret && ret->initWithPhysics())
    {
        ret->autorelease();
        return ret;
    }
    else
    {
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
}

bool Scene::initWithPhysics()
{
    bool ret = false;
    do
    {
        Director * director;
        CC_BREAK_IF( ! (director = Director::getInstance()) );
        
        this->setContentSize(director->getWinSize());
        CC_BREAK_IF(! (_physicsWorld = PhysicsWorld::construct(*this)));
        
        this->scheduleUpdate();
        // success
        ret = true;
    } while (0);
    return ret;
}

void Scene::addChildToPhysicsWorld(Node* child)
{
    if (_physicsWorld)
    {
        std::function<void(Node*)> addToPhysicsWorldFunc = nullptr;
        addToPhysicsWorldFunc = [this, &addToPhysicsWorldFunc](Node* node) -> void
        {
            if (node->getPhysicsBody())
            {
                _physicsWorld->addBody(node->getPhysicsBody());
            }
            
            auto& children = node->getChildren();
            for( const auto &n : children) {
                addToPhysicsWorldFunc(n);
            }
        };
        
        addToPhysicsWorldFunc(child);
    }
}

#endif

NS_CC_END
