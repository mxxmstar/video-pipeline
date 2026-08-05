#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @file executor.h
 * @brief MediaFlow 的执行器抽象和 Boost.Asio 线程池实现。
 *
 * Executor 只负责接受任务并安排线程执行，不了解 Graph 或媒体消息。
 * AsioExecutor 是可重复启动的：Stop 后调用 io_context::restart()，因此
 * 一个 Graph 可以在断流恢复或配置切换时复用同一个执行器对象。
 */
namespace mediaflow {

/// 节点所依赖的最小任务调度接口。
class IExecutor {
public:
    using Task = std::function<void()>;

    virtual ~IExecutor() = default;
    virtual bool Start() = 0;       ///< 启动工作线程，重复启动返回 false。
    virtual void Stop() = 0;        ///< 停止接收任务并等待工作线程退出。
    virtual bool Post(Task task) = 0;  ///< 投递任务，未运行或已停止时返回 false。
    virtual bool IsRunning() const = 0;
};

/**
 * @brief 基于一个 boost::asio::io_context 的可重启线程池。
 *
 * Start/Stop 使用 mutex 保护接受任务的边界。Stop 先把 accepting_ 设为
 * false，再停止 io_context，避免管理线程认为任务已成功投递，而任务实际
 * 在关闭流程中被丢弃。工作线程在 Stop 中 join，保证对象析构后不会再访问
 * this。
 */
class AsioExecutor final : public IExecutor {
public:
    /// @param name 便于日志和调试定位的执行器名称。
    /// @param thread_count 工作线程数量，0 会被规范化为 1。
    explicit AsioExecutor(std::string name = "mediaflow",
                          std::size_t thread_count = 1)
        : name_(std::move(name)),
          thread_count_(thread_count == 0 ? 1 : thread_count) {}

    /// 析构时执行 Stop，确保工作线程不泄漏。
    ~AsioExecutor() override {
        Stop();
    }

    AsioExecutor(const AsioExecutor&) = delete;
    AsioExecutor& operator=(const AsioExecutor&) = delete;

    /// 启动执行器，并为 io_context 创建 work guard 防止 run 提前返回。
    bool Start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return false;
        }

        // 上一次 Stop 会让 io_context 进入 stopped 状态，重启前必须清除该标志。
        io_.restart();
        work_ = std::make_unique<WorkGuard>(boost::asio::make_work_guard(io_));
        accepting_ = true;
        running_ = true;

        workers_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back([this]() {
                io_.run();
            });
        }
        return true;
    }

    /// 停止接受新任务，停止 io_context，并等待所有工作线程退出。
    void Stop() override {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && workers_.empty()) {
                return;
            }

            // 先关闭入口，再停止上下文；Post 会在同一把锁下观察这个状态。
            accepting_ = false;
            work_.reset();
            io_.stop();
            workers.swap(workers_);
            running_ = false;
        }

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    /// 将任务异步投递到 io_context，任务不会在调用线程内直接执行。
    bool Post(Task task) override {
        if (!task) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return false;
        }

        boost::asio::post(io_, std::move(task));
        return true;
    }

    /// 查询执行器是否处于可接收任务的运行状态。
    bool IsRunning() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    /// 返回构造时设置的名称。
    const std::string& Name() const {
        return name_;
    }

    /// 返回实际创建的工作线程数量。
    std::size_t ThreadCount() const {
        return thread_count_;
    }

private:
    using WorkGuard = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>;

    std::string name_;                   ///< 执行器名称。
    std::size_t thread_count_;           ///< 工作线程数量。
    mutable std::mutex mutex_;           ///< 保护状态和 Post/Stop 的并发边界。
    boost::asio::io_context io_;         ///< Asio 任务队列和事件循环。
    std::unique_ptr<WorkGuard> work_;    ///< 保证没有任务时线程仍保持运行。
    std::vector<std::thread> workers_;   ///< 当前执行器拥有的工作线程。
    bool accepting_{false};              ///< 是否允许新任务进入 io_context。
    bool running_{false};                ///< 是否已经完成 Start。
};

} // namespace mediaflow
