#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "media_types.hpp"
#include "options.hpp"

class AudioRuntime {
public:
    ~AudioRuntime();
    void start(const Options &options, AudioOutput &output);
    void stop();

private:
    std::atomic_bool running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};
