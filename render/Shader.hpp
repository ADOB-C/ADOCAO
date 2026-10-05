#pragma once

#include "glad/gl_core.hpp"
#include <string>


namespace adofai {

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    bool compile(const char* vertSrc, const char* fragSrc);
    bool compileFile(const char* vertPath, const char* fragPath);
    void use() const;
    void destroy();

    GLuint id() const { return m_program; }

    // Uniform setters
    void setMat4(const char* name, const float* value) const;
    void setVec3(const char* name, float x, float y, float z) const;
    void setVec4(const char* name, float x, float y, float z, float w) const;
    void setFloat(const char* name, float v) const;

private:
    GLuint m_program = 0;

    static GLuint compileShader(GLenum type, const char* src);
};

}  // namespace adofai
