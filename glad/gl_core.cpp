#include "gl_core.hpp"
#include <GLFW/glfw3.h>
#include <cstdio>

// Define all GL function pointers
PFNGLCREATESHADERPROC        glad_CreateShader;
PFNGLSHADERSOURCEPROC        glad_ShaderSource;
PFNGLCOMPILESHADERPROC       glad_CompileShader;
PFNGLGETSHADERIVPROC         glad_GetShaderiv;
PFNGLGETSHADERINFOLOGPROC    glad_GetShaderInfoLog;
PFNGLDELETESHADERPROC        glad_DeleteShader;
PFNGLCREATEPROGRAMPROC       glad_CreateProgram;
PFNGLATTACHSHADERPROC        glad_AttachShader;
PFNGLLINKPROGRAMPROC         glad_LinkProgram;
PFNGLGETPROGRAMIVPROC        glad_GetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC   glad_GetProgramInfoLog;
PFNGLUSEPROGRAMPROC          glad_UseProgram;
PFNGLDELETEPROGRAMPROC       glad_DeleteProgram;

PFNGLGETUNIFORMLOCATIONPROC  glad_GetUniformLocation;
PFNGLUNIFORM1IPROC           glad_Uniform1i;
PFNGLUNIFORM1FPROC           glad_Uniform1f;
PFNGLUNIFORM2FPROC           glad_Uniform2f;
PFNGLUNIFORM3FPROC           glad_Uniform3f;
PFNGLUNIFORM4FPROC           glad_Uniform4f;
PFNGLUNIFORMMATRIX4FVPROC    glad_UniformMatrix4fv;

PFNGLGENVERTEXARRAYSPROC     glad_GenVertexArrays;
PFNGLBINDVERTEXARRAYPROC     glad_BindVertexArray;
PFNGLDELETEVERTEXARRAYSPROC  glad_DeleteVertexArrays;
PFNGLGENBUFFERSPROC          glad_GenBuffers;
PFNGLBINDBUFFERPROC          glad_BindBuffer;
PFNGLBUFFERDATAPROC          glad_BufferData;
PFNGLBUFFERSUBDATAPROC       glad_BufferSubData;
PFNGLDELETEBUFFERSPROC       glad_DeleteBuffers;
PFNGLVERTEXATTRIBPOINTERPROC glad_VertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC  glad_EnableVertexAttribArray;
PFNGLDISABLEVERTEXATTRIBARRAYPROC glad_DisableVertexAttribArray;
PFNGLVERTEXATTRIBDIVISORPROC     glad_VertexAttribDivisor;

PFNGLGENTEXTURESPROC    glad_GenTextures;
PFNGLBINDTEXTUREPROC    glad_BindTexture;
PFNGLTEXIMAGE2DPROC     glad_TexImage2D;
PFNGLTEXPARAMETERIPROC  glad_TexParameteri;
PFNGLGENERATEMIPMAPPROC glad_GenerateMipmap;
PFNGLDELETETEXTURESPROC glad_DeleteTextures;
PFNGLACTIVETEXTUREPROC  glad_ActiveTexture;

PFNGLGENFRAMEBUFFERSPROC        glad_GenFramebuffers;
PFNGLBINDFRAMEBUFFERPROC        glad_BindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC   glad_FramebufferTexture2D;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glad_CheckFramebufferStatus;
PFNGLDELETEFRAMEBUFFERSPROC     glad_DeleteFramebuffers;

PFNGLSCISSORPROC      glad_Scissor;
PFNGLVIEWPORTPROC     glad_Viewport;
PFNGLCLEARPROC        glad_Clear;
PFNGLCLEARCOLORPROC   glad_ClearColor;
PFNGLDRAWARRAYSPROC   glad_DrawArrays;
PFNGLDRAWELEMENTSPROC glad_DrawElements;
PFNGLDRAWELEMENTSINSTANCEDPROC glad_DrawElementsInstanced;
PFNGLENABLEPROC       glad_Enable;
PFNGLDISABLEPROC      glad_Disable;
PFNGLBLENDFUNCPROC    glad_BlendFunc;
PFNGLLINEWIDTHPROC    glad_LineWidth;
PFNGLDEPTHFUNCPROC    glad_DepthFunc;

PFNGLGETSTRINGPROC glad_GetString;
PFNGLGETERRORPROC  glad_GetError;

PFNGLFINISHPROC      glad_Finish;
PFNGLPIXELSTOREIPROC glad_PixelStorei;
PFNGLREADPIXELSPROC  glad_ReadPixels;

namespace {

bool failed = false;

template<typename T>
void loadGL(T*& ptr, const char* name) {
    ptr = reinterpret_cast<T*>(glfwGetProcAddress(name));
    if (!ptr) { fprintf(stderr, "[GL] Failed to load: %s\n", name); failed = true; }
}

} // namespace

bool loadGLCore() {
    failed = false;

    // Shader
    loadGL(glad_CreateShader,      "glCreateShader");
    loadGL(glad_ShaderSource,      "glShaderSource");
    loadGL(glad_CompileShader,     "glCompileShader");
    loadGL(glad_GetShaderiv,       "glGetShaderiv");
    loadGL(glad_GetShaderInfoLog,  "glGetShaderInfoLog");
    loadGL(glad_DeleteShader,      "glDeleteShader");
    loadGL(glad_CreateProgram,     "glCreateProgram");
    loadGL(glad_AttachShader,      "glAttachShader");
    loadGL(glad_LinkProgram,       "glLinkProgram");
    loadGL(glad_GetProgramiv,      "glGetProgramiv");
    loadGL(glad_GetProgramInfoLog, "glGetProgramInfoLog");
    loadGL(glad_UseProgram,        "glUseProgram");
    loadGL(glad_DeleteProgram,     "glDeleteProgram");

    // Uniforms
    loadGL(glad_GetUniformLocation, "glGetUniformLocation");
    loadGL(glad_Uniform1i,          "glUniform1i");
    loadGL(glad_Uniform1f,          "glUniform1f");
    loadGL(glad_Uniform2f,          "glUniform2f");
    loadGL(glad_Uniform3f,          "glUniform3f");
    loadGL(glad_Uniform4f,          "glUniform4f");
    loadGL(glad_UniformMatrix4fv,   "glUniformMatrix4fv");

    // VAO / VBO
    loadGL(glad_GenVertexArrays,      "glGenVertexArrays");
    loadGL(glad_BindVertexArray,      "glBindVertexArray");
    loadGL(glad_DeleteVertexArrays,   "glDeleteVertexArrays");
    loadGL(glad_GenBuffers,           "glGenBuffers");
    loadGL(glad_BindBuffer,           "glBindBuffer");
    loadGL(glad_BufferData,           "glBufferData");
    loadGL(glad_BufferSubData,        "glBufferSubData");
    loadGL(glad_DeleteBuffers,        "glDeleteBuffers");
    loadGL(glad_VertexAttribPointer,  "glVertexAttribPointer");
    loadGL(glad_EnableVertexAttribArray,  "glEnableVertexAttribArray");
    loadGL(glad_DisableVertexAttribArray, "glDisableVertexAttribArray");
    loadGL(glad_VertexAttribDivisor,     "glVertexAttribDivisor");

    // Textures
    loadGL(glad_GenTextures,     "glGenTextures");
    loadGL(glad_BindTexture,     "glBindTexture");
    loadGL(glad_TexImage2D,      "glTexImage2D");
    loadGL(glad_TexParameteri,   "glTexParameteri");
    loadGL(glad_GenerateMipmap,  "glGenerateMipmap");
    loadGL(glad_DeleteTextures,  "glDeleteTextures");
    loadGL(glad_ActiveTexture,   "glActiveTexture");

    // Framebuffers
    loadGL(glad_GenFramebuffers,        "glGenFramebuffers");
    loadGL(glad_BindFramebuffer,        "glBindFramebuffer");
    loadGL(glad_FramebufferTexture2D,   "glFramebufferTexture2D");
    loadGL(glad_CheckFramebufferStatus, "glCheckFramebufferStatus");
    loadGL(glad_DeleteFramebuffers,     "glDeleteFramebuffers");

    // Rendering
    loadGL(glad_Scissor,      "glScissor");
    loadGL(glad_Viewport,     "glViewport");
    loadGL(glad_Clear,        "glClear");
    loadGL(glad_ClearColor,   "glClearColor");
    loadGL(glad_DrawArrays,   "glDrawArrays");
    loadGL(glad_DrawElements, "glDrawElements");
    loadGL(glad_DrawElementsInstanced, "glDrawElementsInstanced");
    loadGL(glad_Enable,       "glEnable");
    loadGL(glad_Disable,      "glDisable");
    loadGL(glad_BlendFunc,    "glBlendFunc");
    loadGL(glad_LineWidth,    "glLineWidth");
    loadGL(glad_DepthFunc,   "glDepthFunc");

    // Misc
    loadGL(glad_GetString, "glGetString");
    loadGL(glad_GetError,  "glGetError");

    // 同步 / 像素回读（--capture 用；GL 1.0/1.1，GLFW 的 getProcAddress 在三个平台都能取到）
    loadGL(glad_Finish,      "glFinish");
    loadGL(glad_PixelStorei, "glPixelStorei");
    loadGL(glad_ReadPixels,  "glReadPixels");

    return !failed;
}
