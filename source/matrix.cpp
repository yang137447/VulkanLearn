#include "matrix.h"
#include "settings.h"

Matrix::Matrix()
{
    InitMatrix();
}

void Matrix::InitMatrix()
{
    SetModelTransform(Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f));

    SetCamera(Eigen::Vector3f(0.0f, 2.0f, 2.0f), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f));

    SetProjection(90.0f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 10.0f);

    //Vulkan设备空间XYZ三个轴范围分别是 -1.0～+1.0、+1.0～-1.0、0.0～+1.0
    ndcMatrix << 
        -1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, -0.5f,
        0.0f, 0.0f, 0.0f, -1.0f;
}

void Matrix::SetModelTransform(Eigen::Vector3f position, Eigen::Vector3f rotation, Eigen::Vector3f scale)
{
    rotation = rotation * M_PI / 180.0f; //弧度制

    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationXMatrix;
    rotationXMatrix <<
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, std::cos(rotation.x()), -std::sin(rotation.x()), 0.0f,
        0.0f, std::sin(rotation.x()), std::cos(rotation.x()), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationYMatrix;
    rotationYMatrix <<
        std::cos(rotation.y()), 0.0f, std::sin(rotation.y()), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -std::sin(rotation.y()), 0.0f, std::cos(rotation.y()), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationZMatrix;
    rotationZMatrix <<
        std::cos(rotation.z()), -std::sin(rotation.z()), 0.0f, 0.0f,
        std::sin(rotation.z()), std::cos(rotation.z()), 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix;
    rotationMatrix = rotationZMatrix * rotationYMatrix * rotationXMatrix;

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

void Matrix::SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up)
{
    const Eigen::Vector3f& f = (cameraPosition - lookAtPosition).normalized();
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
}

void Matrix::SetProjection(float fov, float aspect, float near, float far)
{
    float n = -1.0f * near;
    float f = -1.0f * far;
    float fovRad = fov * M_PI / 180.0f; 
    float k = -1.0f / std::tan(fovRad / 2.0f);
    projectionMatrix <<
        k, 0.0f, 0.0f, 0.0f,
        0.0f, aspect * k , 0.0f, 0.0f,
        0.0f, 0.0f, (n + f)/(n-f), -2.0f * n * f / (n - f),
        0.0f, 0.0f, 1.0f, 0.0f;
}
Eigen::Matrix4f& Matrix::GetModelMatrix()
{
    // modelMatrix <<
    //     1.0f, 0.0f, 0.0f, 0.0f,
    //     0.0f, 1.0f, 0.0f, 0.0f,
    //     0.0f, 0.0f, 1.0f, 0.0f,
    //     0.0f, 0.0f, 0.0f, 1.0f;
    
    return modelMatrix;
}
Eigen::Matrix4f& Matrix::GetViewMatrix()
{
    return viewMatrix;
}
Eigen::Matrix4f& Matrix::GetProjectionMatrix()
{
    static Eigen::Matrix4f matrix;
    matrix = ndcMatrix * projectionMatrix;
    return matrix;
}