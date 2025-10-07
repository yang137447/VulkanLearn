#include <Eigen/Dense>

class Matrix
{
public:
    static Matrix& GetInstance()
    {
        static Matrix instance;
        return instance;
    }

    void SetModelTransform(Eigen::Vector3f position, Eigen::Vector3f rotation, Eigen::Vector3f scale);
    void SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up);
    void SetProjection(float fov, float aspect, float near, float far);

    Eigen::Matrix4f& GetModelMatrix();
    Eigen::Matrix4f& GetViewMatrix();
    Eigen::Matrix4f& GetProjectionMatrix();


private:
    Matrix();
    void InitMatrix();

    Eigen::Matrix4f modelMatrix;
    Eigen::Matrix4f viewMatrix;
    Eigen::Matrix4f projectionMatrix;
    Eigen::Matrix4f ndcMatrix;
};