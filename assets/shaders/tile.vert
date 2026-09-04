#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aType;        // 0.0=stroke, 1.0=fill
layout(location = 2) in vec3 aInstOffset;   // per-instance camera-relative offset
layout(location = 3) in vec3 iColor;         // fill color (per-instance)
layout(location = 4) in vec3 iBgColor;      // stroke color (per-instance)
layout(location = 5) in float iOpacity;     // opacity (per-instance)
uniform mat4 uVP;
out vec3 vColor;
out float vOpacity;
void main() {
    vColor = mix(iBgColor, iColor, aType);
    vOpacity = iOpacity;
    gl_Position = uVP * vec4(aPos + aInstOffset, 1.0);
}
