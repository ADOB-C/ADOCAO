#pragma once

// Minimal OpenGL 3.3 Core loader using MinGW/mesa glcorearb.h types.
// On Linux, GL/glcorearb.h is provided by mesa.
// On Windows (MinGW), it comes with the MinGW-w64 distribution.
// On macOS, the SDK provides OpenGL/gl3.h (covers core profile up to 4.1).
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
// glcorearb.h 把 GL 1.0/1.1 的**函数原型**藏在 GL_GLEXT_PROTOTYPES 后面（typedef 不受影响）。
// 苹果的 gl3.h 是无条件声明的，所以在 macOS 上写 glFinish/glReadPixels 能编过、到 Linux/Windows
// 就"未声明"。这里显式打开，凡是绕过 glad_ 指针直接用系统符号的地方都能编。
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/glcorearb.h>
#endif

// Shader
extern PFNGLCREATESHADERPROC  glad_CreateShader;
extern PFNGLSHADERSOURCEPROC  glad_ShaderSource;
extern PFNGLCOMPILESHADERPROC glad_CompileShader;
extern PFNGLGETSHADERIVPROC   glad_GetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glad_GetShaderInfoLog;
extern PFNGLDELETESHADERPROC  glad_DeleteShader;
extern PFNGLCREATEPROGRAMPROC glad_CreateProgram;
extern PFNGLATTACHSHADERPROC  glad_AttachShader;
extern PFNGLLINKPROGRAMPROC   glad_LinkProgram;
extern PFNGLGETPROGRAMIVPROC  glad_GetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glad_GetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC    glad_UseProgram;
extern PFNGLDELETEPROGRAMPROC glad_DeleteProgram;

// Uniforms
extern PFNGLGETUNIFORMLOCATIONPROC glad_GetUniformLocation;
extern PFNGLUNIFORM1IPROC     glad_Uniform1i;
extern PFNGLUNIFORM1FPROC     glad_Uniform1f;
extern PFNGLUNIFORM2FPROC     glad_Uniform2f;
extern PFNGLUNIFORM3FPROC     glad_Uniform3f;
extern PFNGLUNIFORM4FPROC     glad_Uniform4f;
extern PFNGLUNIFORMMATRIX4FVPROC glad_UniformMatrix4fv;

// VAO / VBO
extern PFNGLGENVERTEXARRAYSPROC    glad_GenVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC    glad_BindVertexArray;
extern PFNGLDELETEVERTEXARRAYSPROC glad_DeleteVertexArrays;
extern PFNGLGENBUFFERSPROC         glad_GenBuffers;
extern PFNGLBINDBUFFERPROC         glad_BindBuffer;
extern PFNGLBUFFERDATAPROC         glad_BufferData;
extern PFNGLBUFFERSUBDATAPROC      glad_BufferSubData;
extern PFNGLDELETEBUFFERSPROC      glad_DeleteBuffers;
extern PFNGLVERTEXATTRIBPOINTERPROC glad_VertexAttribPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC  glad_EnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC glad_DisableVertexAttribArray;
extern PFNGLVERTEXATTRIBDIVISORPROC     glad_VertexAttribDivisor;

// Textures
extern PFNGLGENTEXTURESPROC    glad_GenTextures;
extern PFNGLBINDTEXTUREPROC    glad_BindTexture;
extern PFNGLTEXIMAGE2DPROC     glad_TexImage2D;
extern PFNGLTEXPARAMETERIPROC  glad_TexParameteri;
extern PFNGLGENERATEMIPMAPPROC glad_GenerateMipmap;
extern PFNGLDELETETEXTURESPROC glad_DeleteTextures;
extern PFNGLACTIVETEXTUREPROC  glad_ActiveTexture;

// Framebuffers
extern PFNGLGENFRAMEBUFFERSPROC        glad_GenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC        glad_BindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC   glad_FramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glad_CheckFramebufferStatus;
extern PFNGLDELETEFRAMEBUFFERSPROC     glad_DeleteFramebuffers;

// Rendering
extern PFNGLSCISSORPROC      glad_Scissor;
extern PFNGLVIEWPORTPROC     glad_Viewport;
extern PFNGLCLEARPROC        glad_Clear;
extern PFNGLCLEARCOLORPROC   glad_ClearColor;
extern PFNGLDRAWARRAYSPROC   glad_DrawArrays;
extern PFNGLDRAWELEMENTSPROC glad_DrawElements;
extern PFNGLDRAWELEMENTSINSTANCEDPROC glad_DrawElementsInstanced;
extern PFNGLENABLEPROC       glad_Enable;
extern PFNGLDISABLEPROC      glad_Disable;
extern PFNGLBLENDFUNCPROC    glad_BlendFunc;
extern PFNGLLINEWIDTHPROC    glad_LineWidth;
extern PFNGLDEPTHFUNCPROC    glad_DepthFunc;

// Misc
extern PFNGLGETSTRINGPROC glad_GetString;
extern PFNGLGETERRORPROC  glad_GetError;

// 同步 / 像素回读（GL 1.0/1.1：Windows 的 wglGetProcAddress 拿不到 1.1，GLFW 会兜住）
extern PFNGLFINISHPROC      glad_Finish;
extern PFNGLPIXELSTOREIPROC glad_PixelStorei;
extern PFNGLREADPIXELSPROC  glad_ReadPixels;

bool loadGLCore();

// Map standard GL names to glad_ prefixed pointers
#define glCreateShader          glad_CreateShader
#define glShaderSource          glad_ShaderSource
#define glCompileShader         glad_CompileShader
#define glGetShaderiv           glad_GetShaderiv
#define glGetShaderInfoLog      glad_GetShaderInfoLog
#define glDeleteShader          glad_DeleteShader
#define glCreateProgram         glad_CreateProgram
#define glAttachShader          glad_AttachShader
#define glLinkProgram           glad_LinkProgram
#define glGetProgramiv          glad_GetProgramiv
#define glGetProgramInfoLog     glad_GetProgramInfoLog
#define glUseProgram            glad_UseProgram
#define glDeleteProgram         glad_DeleteProgram

#define glGetUniformLocation    glad_GetUniformLocation
#define glUniform1i             glad_Uniform1i
#define glUniform1f             glad_Uniform1f
#define glUniform2f             glad_Uniform2f
#define glUniform3f             glad_Uniform3f
#define glUniform4f             glad_Uniform4f
#define glUniformMatrix4fv      glad_UniformMatrix4fv

#define glGenVertexArrays       glad_GenVertexArrays
#define glBindVertexArray       glad_BindVertexArray
#define glDeleteVertexArrays    glad_DeleteVertexArrays
#define glGenBuffers            glad_GenBuffers
#define glBindBuffer            glad_BindBuffer
#define glBufferData            glad_BufferData
#define glBufferSubData         glad_BufferSubData
#define glDeleteBuffers         glad_DeleteBuffers
#define glVertexAttribPointer   glad_VertexAttribPointer
#define glEnableVertexAttribArray    glad_EnableVertexAttribArray
#define glDisableVertexAttribArray   glad_DisableVertexAttribArray
#define glVertexAttribDivisor        glad_VertexAttribDivisor

#define glGenTextures           glad_GenTextures
#define glBindTexture           glad_BindTexture
#define glTexImage2D            glad_TexImage2D
#define glTexParameteri         glad_TexParameteri
#define glGenerateMipmap        glad_GenerateMipmap
#define glDeleteTextures        glad_DeleteTextures
#define glActiveTexture         glad_ActiveTexture

#define glGenFramebuffers       glad_GenFramebuffers
#define glBindFramebuffer       glad_BindFramebuffer
#define glFramebufferTexture2D  glad_FramebufferTexture2D
#define glCheckFramebufferStatus glad_CheckFramebufferStatus
#define glDeleteFramebuffers    glad_DeleteFramebuffers

#define glScissor               glad_Scissor
#define glViewport              glad_Viewport
#define glClear                 glad_Clear
#define glClearColor            glad_ClearColor
#define glDrawArrays            glad_DrawArrays
#define glDrawElements          glad_DrawElements
#define glDrawElementsInstanced glad_DrawElementsInstanced
#define glEnable                glad_Enable
#define glDisable               glad_Disable
#define glBlendFunc             glad_BlendFunc
#define glLineWidth             glad_LineWidth
#define glDepthFunc             glad_DepthFunc

#define glGetString             glad_GetString
#define glGetError              glad_GetError

#define glFinish                glad_Finish
#define glPixelStorei           glad_PixelStorei
#define glReadPixels            glad_ReadPixels
