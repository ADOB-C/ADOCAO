#version 330 core
in vec3  vColor;
in float vOpacity;
in vec2  vIconLocal;   // 砖心局部坐标（单位：砖）
in float vIconBits;    // bit0 twirl / bit1 ssUp / bit2 ssDn
out vec4 fragColor;

// 图标画在砖的**片元**里（Re_ADOJAS instanced.frag 的同一条路）：
// 同一个片元、同一个 draw，没有独立几何、没有 z、透明度天然跟砖走。
const vec3 kTwirlColor = vec3(0.502, 0.0, 0.502);
const vec3 kSSUpColor  = vec3(1.0, 0.0, 0.0);
const vec3 kSSDownColor= vec3(0.0, 0.0, 1.0);
const float kIconRadius = 0.11;   // 参考的 uIconSize=0.44 是**贴图**尺寸，字形只占中间一块；我们画实心圆，取 0.11

void main() {
    vec3 color = vColor;
    int bits = int(vIconBits + 0.5);
    if (bits != 0 && length(vIconLocal) <= kIconRadius) {
        if      ((bits & 1) != 0) color = kTwirlColor;
        else if ((bits & 2) != 0) color = kSSUpColor;
        else                      color = kSSDownColor;
    }
    fragColor = vec4(color, vOpacity);
}
