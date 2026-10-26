#pragma once
#include <epoxy/gl.h>
#include <string>
namespace gpu::linuxgl {
GLuint CompileShader(const std::string& hlsl, GLenum stage);
GLuint LinkProgram(GLuint vertex, GLuint fragment);
}
