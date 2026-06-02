#include <chrono>
#include <string>

class FpsTool {
public:
    FpsTool()
        : frameCount(0),
          timeElapsed(0.0f),
          fps(0) {}

    // 每帧调用一次
    void Calculate(float deltaTime) {
        frameCount++;
        timeElapsed += deltaTime;

        if (timeElapsed >= 1.0f) {
            fps = frameCount / timeElapsed;
            frameCount = 0;
            timeElapsed = 0.0f;
        }
    }

    float getFPS() const { return fps; }

    std::string getTitle(const std::string& baseTitle = "VulkanRenderer") const {
        return baseTitle + " - FPS: " + std::to_string(fps);
    }

private:
    int frameCount;
    float timeElapsed;
    int fps;
};
