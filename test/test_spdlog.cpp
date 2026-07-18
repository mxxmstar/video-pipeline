#include <iostream>

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

int main() {
    try {
        auto logger = spdlog::stdout_color_mt("video_pipeline_test");
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);

        SPDLOG_DEBUG("debug message");
        SPDLOG_INFO("info message");
        SPDLOG_WARN("warn message");

        spdlog::drop("video_pipeline_test");
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "spdlog test failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
