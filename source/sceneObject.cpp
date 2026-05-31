#include "sceneObject.h"
#include <memory>
#include <iostream>

#include "renderableObject.h"
#include "commonFunction.h"

void SceneNode::SetPosition(Eigen::Vector3f& position)
{
    this->position = position;
}

void SceneNode::SetRotation(Eigen::Vector3f& rotation)
{
    quaternion = CommonFunction::RotationToQuat(rotation);
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf& quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::QuatToRotation(quaternion);
    //std::cout << "rotation: " << rotation.x() << " " << rotation.y() << " " << rotation.z() << std::endl;
}

void SceneNode::SetDeltaRotation(Eigen::Vector3f& deltaRotation)
{
    Eigen::Quaternionf deltaRotationQuaternion = CommonFunction::RotationToQuat(deltaRotation);
    quaternion = quaternion * deltaRotationQuaternion;
    quaternion = CommonFunction::RemoveRoll(quaternion);
    quaternion.normalize();
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetScale(Eigen::Vector3f& scale)
{
    this->scale = scale;
}

Eigen::Vector3f SceneNode::GetForwardVector() const
{
    return quaternion * Eigen::Vector3f(0.0f, 0.0f, -1.0f);
}

Eigen::Vector3f SceneNode::GetRightVector() const
{
    return quaternion * Eigen::Vector3f(1.0f, 0.0f, 0.0f);
}

Eigen::Vector3f SceneNode::GetUpVector() const
{
    return quaternion * Eigen::Vector3f(0.0f, 1.0f, 0.0f);
}

void SceneNode::SetTransform(Eigen::Vector3f& position, Eigen::Vector3f& rotation, Eigen::Vector3f& scale)
{
    SetPosition(position);
    SetRotation(rotation);
    SetScale(scale);

    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

void SceneNode::UpdateModelMatrix()
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

void DirectionalLight::SetColor(Eigen::Vector3f& color)
{
    this->color = color;
}
void DirectionalLight::SetIntensity(float intensity)
{
    this->intensity = intensity;
}

// PointLight 的 SetColor 和 SetIntensity 方法已内联在头文件中
Camera::Camera()
{
    isOrthographic = false;

    SetCamera(Eigen::Vector3f(0.0f, 2.0f, 2.0f), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f));

    SetProjection(
        90.0f, 
        static_cast<float>(CommonFunction::GetWindowSize().x()) / static_cast<float>(CommonFunction::GetWindowSize().y()), 
        0.1f, 10.0f);

    //Vulkan设备空间XYZ三个轴范围分别是 -1.0～+1.0、+1.0～-1.0、0.0～+1.0
    ndcMatrix << 
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -0.5f, 0.5f,
        0.0f, 0.0f, 0.0f, 1.0f;
}
void Camera::SetSize(float size)
{
    this->size = size;
}
void Camera::SetHFOV(float fov)
{
    this->hFov = fov;
}

void Camera::SetClip(float near, float far)
{
    this->clipNear = near;
    this->clipFar = far;
}
void Camera::EnableOrthographic(bool enable)
{
    isOrthographic = enable;
}
void Camera::SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up)
{
    const Eigen::Vector3f& f = -1.0 * (lookAtPosition - cameraPosition).normalized();
    const Eigen::Vector3f& r = up.cross(f).normalized();
    const Eigen::Vector3f& u = f.cross(r).normalized();
    const Eigen::Vector3f& p = cameraPosition;

    static Eigen::Matrix4f matrix01;
    matrix01 <<
        r.x(), r.y(), r.z(), 0.0f,
        u.x(), u.y(), u.z(), 0.0f,
        f.x(), f.y(), f.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;
    static Eigen::Matrix4f matrix02;
    matrix02 <<
        1.0f, 0.0f, 0.0f, -p.x(),
        0.0f, 1.0f, 0.0f, -p.y(),
        0.0f, 0.0f, 1.0f, -p.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix = matrix01 * matrix02;

    SetPosition(cameraPosition);
    SetRotation(Eigen::Quaternionf(matrix01.block<3,3>(0,0).transpose()));
}

void Camera::SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f cameraRotation)
{
    Eigen::Vector3f position = cameraPosition;

    //对于旋转处理，遵循 Yaw Pitch Roll 的顺序,即 先绕Y轴旋转，再绕X轴旋转，最后绕Z轴旋转
    Eigen::Matrix4f rotationMatrix = CommonFunction::RotationToMatrix(cameraRotation);

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, -position.x(),
        0.0f, 1.0f, 0.0f, -position.y(),
        0.0f, 0.0f, 1.0f, -position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix =  rotationMatrix.transpose() * translationMatrix;

    SetPosition(cameraPosition);
    SetRotation(Eigen::Quaternionf(rotationMatrix.block<3,3>(0,0)));
}

void Camera::SetProjection(float fov, float aspect, float near, float far)
{
    float n = -1.0f * near;
    float f = -1.0f * far;
    float fovRad = fov * M_PI / 180.0f; 
    float k = -1.0f / std::tan(fovRad / 2.0f);
    projectionMatrix <<
        -k, 0.0f, 0.0f, 0.0f,
        0.0f, -aspect * k , 0.0f, 0.0f,
        0.0f, 0.0f, -(n + f)/(n-f), 2.0f * n * f / (n - f),
        0.0f, 0.0f, -1.0f, 0.0f;

    SetHFOV(fov);
    SetClip(near, far);
}

void Camera::SetOrthographic(float size, float aspect, float near, float far)
{
    float n = -1.0f * near;
    float f = -1.0f * far;
    projectionMatrix <<
        2.0f/size, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f*aspect/size , 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f/(n-f), -1.0f*(n + f)/(n - f),
        0.0f, 0.0f, 0.0f, 1.0f;

    SetSize(size);
    SetClip(near, far);
}

Eigen::Matrix4f& Camera::GetViewMatrix()
{
    updateViewMatrix();
    return viewMatrix;
}
Eigen::Matrix4f& Camera::GetProjectionMatrix()
{
    static Eigen::Matrix4f matrix;
    matrix = ndcMatrix * projectionMatrix;
    return matrix;
}

void Camera::updateViewMatrix()
{
    //对于旋转处理，遵循 Yaw Pitch Roll 的顺序,即 先绕Y轴旋转，再绕X轴旋转，最后绕Z轴旋转
    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, -position.x(),
        0.0f, 1.0f, 0.0f, -position.y(),
        0.0f, 0.0f, 1.0f, -position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix =  rotationMatrix.transpose() * translationMatrix;
}

SceneObject::SceneObject()
{
}

SceneObject::SceneObject(std::shared_ptr<RenderableObject> renderableObject, std::shared_ptr<MaterialInstance> materialInstance)
{
    SetRenderableObject(renderableObject);
    SetMaterialInstance(materialInstance);
}

SceneObject::~SceneObject()
{
}

void SceneObject::SetRenderableObject(std::shared_ptr<RenderableObject> renderableObject)
{
    this->renderableObject = renderableObject;
}

void SceneObject::SetMaterialInstance(std::shared_ptr<MaterialInstance> materialInstance)
{
    this->materialInstance = materialInstance;
}
