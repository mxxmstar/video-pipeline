#include "render/opengl_video_renderer.h"

#include <array>
#include <iostream>
#include <utility>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace render {
namespace {

// 顶点 shader 只负责把全屏 quad 的位置和纹理坐标传给片元阶段。
constexpr const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

// 片元 shader 从 RGBA 纹理采样并直接输出。
// 颜色空间转换已经在 CPU 侧 FrameConverter 完成。
constexpr const char* kFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    FragColor = texture(uTex, vUV);
}
)";

// 编译单个 shader，并把编译日志输出到 stderr，便于无 UI 调试。
std::uint32_t CompileShader(unsigned int type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "render shader compile failed: " << log << '\n';
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

OpenGLVideoRenderer::~OpenGLVideoRenderer() {
    Shutdown();
}

bool OpenGLVideoRenderer::Init(const RenderConfig& config) {
    config_ = config;
    if (initialized_) {
        return true;
    }
    return CreateWindow() && CreateProgram() && CreateQuad();
}

bool OpenGLVideoRenderer::Render(const MediaFrame& frame) {
    if (!initialized_ && !Init(config_)) {
        return false;
    }
    if (!window_ || ShouldClose()) {
        return false;
    }

    // 先把业务侧 MediaFrame 统一转换成 RGBA8，OpenGL 上传路径只处理这一种格式。
    std::string convert_error;
    if (!converter_.ConvertToRgba(frame, rgba_frame_, &convert_error)) {
        SetError(std::move(convert_error));
        return false;
    }

    // GLFW context 是线程相关状态；每次渲染前显式绑定当前窗口更稳妥。
    glfwMakeContextCurrent(window_);
    if (!UploadTexture(rgba_frame_)) {
        return false;
    }

    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);
    glViewport(0, 0, fb_width, fb_height);
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 绘制覆盖整个窗口的两个三角形，片元 shader 从视频纹理采样。
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glfwSwapBuffers(window_);
    return true;
}

void OpenGLVideoRenderer::PollEvents() {
    if (!window_) {
        return;
    }

    // GLFW 需要在事件循环中轮询，否则窗口关闭和键盘事件不会被处理。
    glfwPollEvents();
    if (config_.close_on_escape &&
        glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

bool OpenGLVideoRenderer::ShouldClose() const {
    return window_ && glfwWindowShouldClose(window_);
}

void OpenGLVideoRenderer::Shutdown() {
    if (window_) {
        // 删除 GL 对象前绑定 context，避免在没有当前 context 的线程上调用 glDelete*。
        glfwMakeContextCurrent(window_);
        DestroyGlResources();
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    if (glfw_initialized_) {
        glfwTerminate();
        glfw_initialized_ = false;
    }

    initialized_ = false;
    texture_width_ = 0;
    texture_height_ = 0;
}

bool OpenGLVideoRenderer::CreateWindow() {
    if (!glfwInit()) {
        SetError("failed to initialize GLFW");
        return false;
    }
    glfw_initialized_ = true;

    // 使用 OpenGL 3.3 core profile，覆盖 Windows/Linux/macOS 上较常见的能力基线。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, config_.visible ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(config_.window_width,
                               config_.window_height,
                               config_.title.c_str(),
                               nullptr,
                               nullptr);
    if (!window_) {
        SetError("failed to create GLFW window");
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(config_.vsync ? 1 : 0);

    // glad 必须在拥有当前 context 之后加载，否则无法解析 OpenGL 函数指针。
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        SetError("failed to initialize GLAD");
        return false;
    }

    initialized_ = true;
    return true;
}

bool OpenGLVideoRenderer::CreateProgram() {
    const auto vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    const auto fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex_shader == 0 || fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);
    glLinkProgram(program_);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    int ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        std::cerr << "render program link failed: " << log << '\n';
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    // uTex 固定绑定到 texture unit 0，Render() 中每帧只需要绑定纹理对象。
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
    return true;
}

bool OpenGLVideoRenderer::CreateQuad() {
    // 顶点格式为 position.xy + uv.xy。
    // UV 的 y 方向按图像内存从上到下的布局翻转，避免画面上下颠倒。
    const std::array<float, 16> vertices{
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };
    const std::array<unsigned int, 6> indices{0, 1, 2, 0, 2, 3};

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
                 indices.data(),
                 GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return true;
}

bool OpenGLVideoRenderer::UploadTexture(const RgbaFrame& frame) {
    if (frame.Empty()) {
        SetError("empty RGBA frame");
        return false;
    }

    if (texture_ == 0) {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture_);
    }

    // RGBA8 每行可能不是 4 字节对齐宽度的倍数，使用 1 字节对齐避免上传错行。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (frame.width != texture_width_ || frame.height != texture_height_) {
        // 尺寸变化时重新分配纹理存储。
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     frame.width,
                     frame.height,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     frame.pixels.data());
        texture_width_ = frame.width;
        texture_height_ = frame.height;
    } else {
        // 尺寸不变时只更新已有纹理内容，避免每帧重新分配 GPU 资源。
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        frame.width,
                        frame.height,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        frame.pixels.data());
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return true;
}

void OpenGLVideoRenderer::DestroyGlResources() {
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (ebo_ != 0) {
        glDeleteBuffers(1, &ebo_);
        ebo_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void OpenGLVideoRenderer::SetError(std::string message) {
    last_error_ = std::move(message);
    if (!last_error_.empty()) {
        std::cerr << "OpenGLVideoRenderer: " << last_error_ << '\n';
    }
}

} // namespace render
