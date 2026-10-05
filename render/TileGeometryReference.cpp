#include "TileGeometry.hpp"


namespace adofai {

void pushType(std::vector<float>& c, float type, int n) {
    for (int i = 0; i < n; i++) c.push_back(type);
}

void createCircle(float cx, float cy, float radius, float type,
                  Scratch& sc, int res)
{
    unsigned ci = (unsigned)sc.verts.size() / 3;
    sc.verts.insert(sc.verts.end(), {cx, cy, 0.0f});
    sc.types.push_back(type);
    for (int i = 0; i < res; i++) {
        float a = (2.0f * 3.14159265f * i) / res;
        sc.verts.insert(sc.verts.end(), {std::cos(a)*radius + cx, std::sin(a)*radius + cy, 0.0f});
        sc.types.push_back(type);
    }
    for (int i = 1; i < res; i++)
        sc.indices.insert(sc.indices.end(), {ci, ci+(unsigned)i, ci+(unsigned)(i+1)});
    sc.indices.insert(sc.indices.end(), {ci, ci+(unsigned)res, ci+1});
}

// 中旋砖（angleData=999）的网格：**五边形**。
//
// 参考实现三处一致（同一支代码的传承，不是三方独立验证）：
//   StArray/A4GDX      Tile.java          CreateMidSpinMesh(a1)
//   adofaiex/AdoCpp    Tile.cpp           createMidSpinMesh(width, a1, …)  — 注释写明 "Thanks for StArray's code"
//   adofaiex/Re_ADOJAS mesh_reserve.ts    createMidSpinMesh(angle)         — 同上，TS 版
// 形状：以 a1（入砖方向 = 上一砖 direction-180）为轴，0..length 是宽 2*width 的方块，
// 再向来路伸出一个 depth = width 的尖角，整体沿 -a1 平移 0.04。
// 注意 length 的**起点等于 width**（半长 0.275，不是普通砖的 0.5）：三家参考里中旋砖都比普通砖短。
// 7 顶点 / 3 三角形的顺序三家一致；描边层是整体各加 OUTLINE 的同形状。
void createMidSpinMesh(float a1, Scratch& sc) {
    float width = TILE_WIDTH, length = TILE_WIDTH;
    const float rad = a1 * 3.14159265f / 180.0f;
    const float m1 = std::cos(rad), m2 = std::sin(rad);
    const float mx = -m1 * 0.04f, my = -m2 * 0.04f;

    auto emit = [&](float w, float l, float type) {
        unsigned c = (unsigned)sc.verts.size() / 3;
        const float px[7] = { l*m1 + w*m2, l*m1 - w*m2, -w*m2,  w*m2, -w*m1,  w*m2, -w*m2 };
        const float py[7] = { l*m2 - w*m1, l*m2 + w*m1,  w*m1, -w*m1, -w*m2, -w*m1,  w*m1 };
        for (int i = 0; i < 7; i++)
            sc.verts.insert(sc.verts.end(), {mx + px[i], my + py[i], 0.0f});
        pushType(sc.types, type, 7);
        sc.indices.insert(sc.indices.end(), {c, c+1, c+2, c+2, c+3, c, c+4, c+5, c+6});
    };

    width += OUTLINE; length += OUTLINE;
    emit(width, length, 0.0f);          // 描边（黑）
    width -= OUTLINE * 2; length -= OUTLINE * 2;
    emit(width, length, 1.0f);          // 填充（白）
}

void createTileMesh(float startAngle, float endAngle, Scratch& sc) {
    float width = TILE_WIDTH, length = TILE_LENGTH;
    float m11=std::cos(startAngle*3.14159265f/180), m12=std::sin(startAngle*3.14159265f/180);
    float m21=std::cos(endAngle*3.14159265f/180),   m22=std::sin(endAngle*3.14159265f/180);

    float a0,a1;
    if (fmodWrap(startAngle-endAngle,360) >= fmodWrap(endAngle-startAngle,360))
        {a0=fmodWrap(startAngle,360)*3.14159265f/180;a1=a0+fmodWrap(endAngle-startAngle,360)*3.14159265f/180;}
    else
        {a0=fmodWrap(endAngle,360)*3.14159265f/180;a1=a0+fmodWrap(startAngle-endAngle,360)*3.14159265f/180;}
    float ang=a1-a0,mid=a0+ang/2;

    if (ang < 2.0943952f && ang > 0) {
        float x;
        if (ang<0.08726646f) x=1;
        else if (ang<0.5235988f) x=lerp(1,0.83f,std::pow((ang-0.08726646f)/0.43633235f,0.5f));
        else if (ang<0.7853982f) x=lerp(0.83f,0.77f,std::pow((ang-0.5235988f)/0.2617994f,1));
        else if (ang<1.5707964f) x=lerp(0.77f,0.15f,std::pow((ang-0.7853982f)/0.7853982f,0.7f));
        else x=lerp(0.15f,0,std::pow((ang-1.5707964f)/0.5235988f,0.5f));

        float dist,rad;
        if (x==1){dist=0;rad=width;}else{rad=lerp(0,width,x);dist=(width-rad)/std::sin(ang/2);}
        float cx=-dist*std::cos(mid),cy=-dist*std::sin(mid);

        // Outer (black) - accumulate outline
        width += OUTLINE; length += OUTLINE; rad += OUTLINE;

        createCircle(cx,cy,rad, 0.0f, sc);
        unsigned cnt;

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            -rad*std::sin(a1)+cx,rad*std::cos(a1)+cy,0,cx,cy,0,
            rad*std::sin(a0)+cx,-rad*std::cos(a0)+cy,0,width*std::sin(a0),-width*std::cos(a0),0,
            0,0,0,-width*std::sin(a1),width*std::cos(a1),0,
        });
        pushType(sc.types, 0.0f,6);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+5,cnt+4,cnt+1,cnt+5,cnt+2,cnt+3,cnt+4,cnt+1,cnt+3,cnt+4});

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            length*m11+width*m12,length*m12-width*m11,0,length*m11-width*m12,length*m12+width*m11,0,
            -width*m12,width*m11,0,width*m12,-width*m11,0,
            length*m21+width*m22,length*m22-width*m21,0,length*m21-width*m22,length*m22+width*m21,0,
            -width*m22,width*m21,0,width*m22,-width*m21,0,
        });
        pushType(sc.types, 0.0f,8);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt,cnt+4,cnt+5,cnt+6,cnt+6,cnt+7,cnt+4});

        // Inner (white) - shrink from accumulated by 2*OUTLINE
        width -= OUTLINE*2; length -= OUTLINE*2; rad -= OUTLINE*2;
        if (rad<0){rad=0;cx=(-width/std::sin(ang/2))*std::cos(mid);cy=(-width/std::sin(ang/2))*std::sin(mid);}
        createCircle(cx,cy,rad, 1.0f, sc);

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            -rad*std::sin(a1)+cx,rad*std::cos(a1)+cy,0,cx,cy,0,
            rad*std::sin(a0)+cx,-rad*std::cos(a0)+cy,0,width*std::sin(a0),-width*std::cos(a0),0,
            0,0,0,-width*std::sin(a1),width*std::cos(a1),0,
        });
        pushType(sc.types, 1.0f,6);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+5,cnt+4,cnt+1,cnt+5,cnt+2,cnt+3,cnt+4,cnt+1,cnt+3,cnt+4});

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            length*m11+width*m12,length*m12-width*m11,0,length*m11-width*m12,length*m12+width*m11,0,
            -width*m12,width*m11,0,width*m12,-width*m11,0,
            length*m21+width*m22,length*m22-width*m21,0,length*m21-width*m22,length*m22+width*m21,0,
            -width*m22,width*m21,0,width*m22,-width*m21,0,
        });
        pushType(sc.types, 1.0f,8);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt,cnt+4,cnt+5,cnt+6,cnt+6,cnt+7,cnt+4});

    } else if (ang > 0) {
        // Outer (black) - accumulate outline
        width += OUTLINE; length += OUTLINE;
        float cx=(-width/std::sin(ang/2))*std::cos(mid),cy=(-width/std::sin(ang/2))*std::sin(mid);
        unsigned cnt;

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{cx,cy,0,width*std::sin(a0),-width*std::cos(a0),0,0,0,0,-width*std::sin(a1),width*std::cos(a1),0});
        pushType(sc.types, 0.0f,4);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt});

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            length*m11+width*m12,length*m12-width*m11,0,length*m11-width*m12,length*m12+width*m11,0,
            -width*m12,width*m11,0,width*m12,-width*m11,0,
            length*m21+width*m22,length*m22-width*m21,0,length*m21-width*m22,length*m22+width*m21,0,
            -width*m22,width*m21,0,width*m22,-width*m21,0,
        });
        pushType(sc.types, 0.0f,8);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt,cnt+4,cnt+5,cnt+6,cnt+6,cnt+7,cnt+4});

        // Inner (white) - shrink from accumulated by 2*OUTLINE
        width -= OUTLINE*2; length -= OUTLINE*2;
        cx=(-width/std::sin(ang/2))*std::cos(mid);cy=(-width/std::sin(ang/2))*std::sin(mid);

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{cx,cy,0,width*std::sin(a0),-width*std::cos(a0),0,0,0,0,-width*std::sin(a1),width*std::cos(a1),0});
        pushType(sc.types, 1.0f,4);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt});

        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            length*m11+width*m12,length*m12-width*m11,0,length*m11-width*m12,length*m12+width*m11,0,
            -width*m12,width*m11,0,width*m12,-width*m11,0,
            length*m21+width*m22,length*m22-width*m21,0,length*m21-width*m22,length*m22+width*m21,0,
            -width*m22,width*m21,0,width*m22,-width*m21,0,
        });
        pushType(sc.types, 1.0f,8);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt,cnt+4,cnt+5,cnt+6,cnt+6,cnt+7,cnt+4});

    } else {
        length=width; width+=OUTLINE; length+=OUTLINE;
        float m1=m11,m2=m12,mx=-m1*0.04f,my=-m2*0.04f;
        createCircle(mx,my,width, 0.0f, sc);
        unsigned cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            mx+length*m1+width*m2,my+length*m2-width*m1,0,mx+length*m1-width*m2,my+length*m2+width*m1,0,
            mx-width*m2,my+width*m1,0,mx+width*m2,my-width*m1,0,
        });
        pushType(sc.types, 0.0f,4);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt});

        width-=OUTLINE*2; length-=OUTLINE*2;
        createCircle(mx,my,width, 1.0f, sc);
        cnt=(unsigned)sc.verts.size()/3;
        sc.verts.insert(sc.verts.end(),{
            mx+length*m1+width*m2,my+length*m2-width*m1,0,mx+length*m1-width*m2,my+length*m2+width*m1,0,
            mx-width*m2,my+width*m1,0,mx+width*m2,my-width*m1,0,
        });
        pushType(sc.types, 1.0f,4);
        sc.indices.insert(sc.indices.end(),{cnt,cnt+1,cnt+2,cnt+2,cnt+3,cnt});
    }
}

}  // namespace adofai
