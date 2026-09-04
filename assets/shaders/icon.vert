#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aType;
layout(location = 2) in vec3 aInstOffset;
layout(location = 3) in vec3 iColor;
layout(location = 4) in vec3 iBgColor;
layout(location = 5) in float iOpacity;
uniform mat4 uVP;
out vec3 vColor;
out float vOpacity;
void main() {
    vColor = mix(iBgColor, iColor, aType);
    vOpacity = iOpacity;
    gl_Position = uVP * vec4(aPos + aInstOffset, 1.0);
}
