#pragma once
#include <thread>
#include <utility>
namespace pipeline {
// libc++ supplied by older Apple SDKs lacks std::jthread.
class JoiningThread {
  public:
    template <class Function>
    explicit JoiningThread(Function&& function) : thread_(std::forward<Function>(function)) {}
    ~JoiningThread() {
        if (thread_.joinable())
            thread_.join();
    }
    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    void join() {
        if (thread_.joinable())
            thread_.join();
    }

  private:
    std::thread thread_;
};
} // namespace pipeline
