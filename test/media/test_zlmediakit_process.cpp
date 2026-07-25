#include "common/process/process.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kDefaultMediaServerPath =
    R"(E:\share\project\video-pipeline\third_apps\win32\zlmediakit\MediaServer.exe)";

struct TestOptions {
    std::filesystem::path executable{kDefaultMediaServerPath};
    std::filesystem::path config_template;
    unsigned short http_port{8888};
    unsigned short rtc_signaling_port{13000};
    unsigned short rtc_signaling_ssl_port{13001};
    int startup_timeout_ms{8000};
    int stable_ms{2000};
    bool keep_running{false};
    bool visible{false};
};

void PrintUsage(const char* program) {
    std::cout
        << "Usage:\n"
        << "  " << program << " [MediaServer.exe]\n"
        << "  " << program << " --exe <MediaServer.exe> [--http-port 8888] "
        << "[--config config.ini] [--rtc-signaling-port 13000] "
        << "[--timeout-ms 8000] [--stable-ms 2000] [--visible] [--keep-running]\n";
}

bool ParseUnsignedShort(const std::string& value, unsigned short& out) {
    try {
        const auto parsed = std::stoul(value);
        if (parsed > 65535) {
            return false;
        }
        out = static_cast<unsigned short>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseInt(const std::string& value, int& out) {
    try {
        out = std::stoi(value);
        return out > 0;
    } catch (...) {
        return false;
    }
}

bool ParseArgs(int argc, char** argv, TestOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return false;
        }

        if (arg == "--exe") {
            if (++i >= argc) {
                std::cerr << "--exe requires a path\n";
                return false;
            }
            options.executable = argv[i];
            continue;
        }

        if (arg == "--http-port") {
            if (++i >= argc || !ParseUnsignedShort(argv[i], options.http_port)) {
                std::cerr << "--http-port requires a port in [0, 65535]\n";
                return false;
            }
            continue;
        }

        if (arg == "--config") {
            if (++i >= argc) {
                std::cerr << "--config requires a path\n";
                return false;
            }
            options.config_template = argv[i];
            continue;
        }

        if (arg == "--rtc-signaling-port") {
            if (++i >= argc || !ParseUnsignedShort(argv[i], options.rtc_signaling_port)) {
                std::cerr << "--rtc-signaling-port requires a port in [0, 65535]\n";
                return false;
            }
            continue;
        }

        if (arg == "--rtc-signaling-ssl-port") {
            if (++i >= argc ||
                !ParseUnsignedShort(argv[i], options.rtc_signaling_ssl_port)) {
                std::cerr << "--rtc-signaling-ssl-port requires a port in [0, 65535]\n";
                return false;
            }
            continue;
        }

        if (arg == "--timeout-ms") {
            if (++i >= argc || !ParseInt(argv[i], options.startup_timeout_ms)) {
                std::cerr << "--timeout-ms requires a positive integer\n";
                return false;
            }
            continue;
        }

        if (arg == "--stable-ms") {
            if (++i >= argc || !ParseInt(argv[i], options.stable_ms)) {
                std::cerr << "--stable-ms requires a positive integer\n";
                return false;
            }
            continue;
        }

        if (arg == "--visible") {
            options.visible = true;
            continue;
        }

        if (arg == "--keep-running") {
            options.keep_running = true;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }

        options.executable = arg;
    }

    return true;
}

std::string TrimLeft(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n';
                }));
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

std::filesystem::path MakeRuntimeConfig(const TestOptions& options) {
    auto source = options.config_template;
    if (source.empty()) {
        source = options.executable.parent_path() / "config.ini";
    }

    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("config template not found: " + source.string());
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto output = std::filesystem::temp_directory_path() /
                  ("video_pipeline_zlmediakit_" + std::to_string(stamp) + ".ini");

    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open config template: " + source.string());
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create runtime config: " + output.string());
    }

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = TrimLeft(line);
        if (!trimmed.empty() && trimmed.front() == '[') {
            auto end = trimmed.find(']');
            section = end == std::string::npos ? std::string{} : trimmed.substr(0, end + 1);
        }

        if (section == "[rtc]" && StartsWith(trimmed, "signalingPort=")) {
            out << "signalingPort=" << options.rtc_signaling_port << "\n";
            continue;
        }

        if (section == "[rtc]" && StartsWith(trimmed, "signalingSslPort=")) {
            out << "signalingSslPort=" << options.rtc_signaling_ssl_port << "\n";
            continue;
        }

        out << line << "\n";
    }

    return output;
}

bool CanConnectToHttpPort(unsigned short port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::system::error_code ec;
    socket.connect(
        boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port),
        ec);
    return !ec;
}

bool WaitForStartup(common::process::Process& process,
                    unsigned short http_port,
                    int timeout_ms,
                    int stable_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const bool running = process.IsRunning(ec);
        if (ec) {
            std::cerr << "Failed to query MediaServer state: " << ec.message() << "\n";
            return false;
        }

        if (!running) {
            std::cerr << "MediaServer exited during startup, exit_code="
                      << process.ExitCode() << "\n";
            return false;
        }

        if (CanConnectToHttpPort(http_port)) {
            const auto stable_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(stable_ms);
            while (std::chrono::steady_clock::now() < stable_deadline) {
                boost::system::error_code stable_ec;
                if (!process.IsRunning(stable_ec)) {
                    std::cerr << "MediaServer exited after HTTP became reachable, exit_code="
                              << process.ExitCode() << "\n";
                    return false;
                }

                if (!CanConnectToHttpPort(http_port)) {
                    std::cerr << "MediaServer HTTP port became unreachable during stability check\n";
                    return false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cerr << "Timed out waiting for MediaServer HTTP port " << http_port << "\n";
    return false;
}

bool StopProcess(common::process::Process& process) {
    boost::system::error_code ec;
    if (!process.IsRunning(ec)) {
        return true;
    }

    process.RequestExit(ec);
    if (ec) {
        std::cout << "RequestExit failed, will terminate: " << ec.message() << "\n";
    }

    for (int i = 0; i < 20; ++i) {
        boost::system::error_code running_ec;
        if (!process.IsRunning(running_ec)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ec.clear();
    if (!process.Terminate(ec)) {
        std::cerr << "Terminate failed: " << ec.message() << "\n";
        return false;
    }

    for (int i = 0; i < 30; ++i) {
        boost::system::error_code running_ec;
        if (!process.IsRunning(running_ec)) {
            boost::system::error_code wait_ec;
            process.Wait(wait_ec);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cerr << "MediaServer did not exit after terminate\n";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    TestOptions options;
    if (!ParseArgs(argc, argv, options)) {
        return 2;
    }

    if (!std::filesystem::exists(options.executable)) {
        std::cerr << "MediaServer executable not found: "
                  << options.executable.string() << "\n";
        return 2;
    }

    std::filesystem::path runtime_config;
    try {
        runtime_config = MakeRuntimeConfig(options);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    boost::asio::io_context io;
    common::process::Process media_server(io.get_executor());

    common::process::ProcessOptions process_options;
    process_options.executable = options.executable;
    process_options.working_directory = options.executable.parent_path();
    process_options.arguments = {"-c", runtime_config.string()};
    options.visible = true;
    process_options.console_mode = common::process::ConsoleMode::NewConsole;
    process_options.window_mode = options.visible
                                      ? common::process::WindowMode::Default
                                      : common::process::WindowMode::Hidden;

    boost::system::error_code ec;
    if (!media_server.Start(process_options, ec)) {
        std::cerr << "Failed to start MediaServer: " << ec.message()
                  << " (" << media_server.LastError() << ")\n";
        return 1;
    }

    std::cout << "Started MediaServer, pid=" << media_server.Pid()
              << ", exe=" << options.executable.string()
              << ", config=" << runtime_config.string() << "\n";

    if (!WaitForStartup(media_server,
                        options.http_port,
                        options.startup_timeout_ms,
                        options.stable_ms)) {
        StopProcess(media_server);
        std::filesystem::remove(runtime_config, ec);
        return 1;
    }

    std::cout << "MediaServer is running; HTTP port " << options.http_port
              << " is reachable.\n";

    if (options.keep_running) {
        std::cout << "Keep-running mode is enabled. Waiting for MediaServer to exit.\n";
        const int exit_code = media_server.Wait(ec);
        if (ec) {
            std::cerr << "Wait failed: " << ec.message() << "\n";
            return 1;
        }
        std::cout << "MediaServer exited, exit_code=" << exit_code << "\n";
        std::filesystem::remove(runtime_config, ec);
        return exit_code;
    }

    if (!StopProcess(media_server)) {
        std::filesystem::remove(runtime_config, ec);
        return 1;
    }

    std::filesystem::remove(runtime_config, ec);
    std::cout << "MediaServer smoke test passed.\n";
    return 0;
}
