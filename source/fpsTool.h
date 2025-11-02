#include <chrono>
#include <string>

class FpsTool {
public:
    FpsTool()
        : frameCount(0),
          lastUpdate(std::chrono::steady_clock::now()),
          fps(0.0f) {}

    // 每帧调用一次
    void Calculate() {
        frameCount++;
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = now - lastUpdate;

        if (elapsed.count() >= 1.0f) {
            fps = frameCount / elapsed.count();
            frameCount = 0;
            lastUpdate = now;
        }
    }

    float getFPS() const { return fps; }

    std::string getTitle(const std::string& baseTitle = "VulkanRenderer") const {
        return baseTitle + " - FPS: " + std::to_string(fps);
    }

private:
    int frameCount;
    std::chrono::steady_clock::time_point lastUpdate;
    float fps;
};