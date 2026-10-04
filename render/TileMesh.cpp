#include "TileMesh.hpp"
#include "TileGeometry.hpp"
#include "glad/gl_core.hpp"
#include "render/CullSIMD.hpp"
#include "core/util/Logger.hpp"
#include "core/util/ThreadPool.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <cstring>

struct CachedGeo {
    std::vector<float> interleaved; std::vector<unsigned> indices;
    unsigned idxCount=0, strokeVertCount=0, strokeIdxCount=0;
    double localMinX=0,localMinY=0,localMaxX=0,localMaxY=0;
};
using GeoKey = std::tuple<int,int,bool>;
namespace std { template<> struct hash<GeoKey> {
    size_t operator()(const GeoKey& k) const { return (size_t)get<0>(k)*31+(size_t)get<1>(k)*17+(size_t)get<2>(k); }
};}
static std::unordered_map<GeoKey,CachedGeo,std::hash<GeoKey>> s_geoCache;
static constexpr size_t MAX_INSTANCES_PER_GROUP = 1000000;

void TileMesh::freeSoA(ShapeGroup& sg) {
    std::free(sg.cullMinX); sg.cullMinX = nullptr;
    sg.cullMaxX = sg.cullMinY = sg.cullMaxY = nullptr;
    std::free(sg.posX); sg.posX = nullptr;
    sg.posY = sg.posZ = nullptr;
    sg.instanceCount = 0;
}
void TileMesh::allocSoA(ShapeGroup& sg, size_t n) {
    sg.instanceCount = n;
    size_t cullBytes = n*sizeof(double)*4;
    size_t posBytes = n*sizeof(float)*3;
    LOG_D("allocSoA: n=%zu, cull=%.1fMB pos=%.1fMB", n, cullBytes/1048576.0, posBytes/1048576.0);
    sg.cullMinX = (double*)std::malloc(cullBytes);
    if (!sg.cullMinX) {
        LOG_E("allocSoA: FAILED to allocate cullMinX (%.1fMB), n=%zu", cullBytes/1048576.0, n);
        sg.instanceCount = 0;
        return;
    }
    sg.cullMaxX = sg.cullMinX + n;
    sg.cullMinY = sg.cullMaxX + n;
    sg.cullMaxY = sg.cullMinY + n;
    sg.posX = (float*)std::malloc(posBytes);
    if (!sg.posX) {
        LOG_E("allocSoA: FAILED to allocate posX (%.1fMB), n=%zu", posBytes/1048576.0, n);
        std::free(sg.cullMinX);
        sg.cullMinX = nullptr; sg.cullMaxX = sg.cullMinY = sg.cullMaxY = nullptr;
        sg.instanceCount = 0;
        return;
    }
    sg.posY = sg.posX + n;
    sg.posZ = sg.posY + n;
}

TileMesh::~TileMesh() { destroy(); }
TileMesh::TileMesh(TileMesh&& o) noexcept
    : m_shapes(std::move(o.m_shapes)), m_iconGroups(std::move(o.m_iconGroups))
    , m_visCaches(std::move(o.m_visCaches)), m_iconVisCaches(std::move(o.m_iconVisCaches))
    , m_tileToShape(std::move(o.m_tileToShape)), m_tileToInstance(std::move(o.m_tileToInstance)) {}
TileMesh& TileMesh::operator=(TileMesh&& o) noexcept {
    if(this!=&o){destroy();m_shapes=std::move(o.m_shapes);m_iconGroups=std::move(o.m_iconGroups);
    m_visCaches=std::move(o.m_visCaches);m_iconVisCaches=std::move(o.m_iconVisCaches);
    m_tileToShape=std::move(o.m_tileToShape);m_tileToInstance=std::move(o.m_tileToInstance);} return *this;
}

void TileMesh::destroy() {
    for(auto& s:m_shapes){if(s.instVbo)glDeleteBuffers(1,&s.instVbo);if(s.colorVbo)glDeleteBuffers(1,&s.colorVbo);
    if(s.ebo)glDeleteBuffers(1,&s.ebo);if(s.vbo)glDeleteBuffers(1,&s.vbo);if(s.vao)glDeleteVertexArrays(1,&s.vao);freeSoA(s);}
    m_shapes.clear(); m_sgTileIndices.clear();
    for(auto& s:m_iconGroups){if(s.instVbo)glDeleteBuffers(1,&s.instVbo);if(s.colorVbo)glDeleteBuffers(1,&s.colorVbo);
    if(s.ebo)glDeleteBuffers(1,&s.ebo);if(s.vbo)glDeleteBuffers(1,&s.vbo);if(s.vao)glDeleteVertexArrays(1,&s.vao);freeSoA(s);}
    m_iconGroups.clear(); m_sgIconTileIndices.clear();
    m_iconEntryFirst.clear(); m_iconEntries.clear();
}
bool TileMesh::empty() const { return m_shapes.empty(); }

void TileMesh::setVisibleThreshold(int lastVisible) {
    if (lastVisible != m_visibleThreshold) {
        m_visibleThreshold = lastVisible;
        for (auto& c : m_visCaches) c.valid = false;
        for (auto& c : m_iconVisCaches) c.valid = false;
    }
}

void TileMesh::updateVisibleRange(int startTile, int endTile, bool visible) {
    if (m_tileToShape.empty()) return;
    // Lazy init per-instance visibility arrays to all-visible
    if (m_sgVisible.size() != m_shapes.size()) {
        m_sgVisible.resize(m_shapes.size());
        for (size_t si = 0; si < m_shapes.size(); si++)
            m_sgVisible[si].assign(m_shapes[si].instanceCount, 1);
        m_sgIconVisible.resize(m_iconGroups.size());
        for (size_t si = 0; si < m_iconGroups.size(); si++)
            m_sgIconVisible[si].assign(m_iconGroups[si].instanceCount, 1);
    }
    int n = (int)m_tileToShape.size();
    if (startTile < 0) startTile = 0;
    if (endTile >= n) endTile = n - 1;
    uint8_t v = visible ? 1 : 0;
    for (int i = startTile; i <= endTile; i++) {
        int sg = m_tileToShape[i], inst = m_tileToInstance[i];
        if (sg < 0 || sg >= (int)m_sgVisible.size()) continue;
        if (inst < 0 || inst >= (int)m_sgVisible[sg].size()) continue;
        m_sgVisible[sg][inst] = v;
    }
    // Icons via the per-tile CSR index: touch only icons on the affected tiles
    // (was a full scan of every icon group per changed tile).
    if (!m_iconEntries.empty()) {
        const size_t cap = m_iconEntryFirst.size();
        for (int i = startTile; i <= endTile; i++) {
            size_t t = (size_t)i;
            if (t + 1 >= cap) break;
            uint32_t lo = m_iconEntryFirst[t], hi = m_iconEntryFirst[t + 1];
            for (uint32_t e = lo; e < hi; e++) {
                uint32_t h = m_iconEntries[e];
                uint32_t g = h >> 20, inst = h & 0xFFFFFu;
                if (g < m_sgIconVisible.size() && inst < m_sgIconVisible[g].size())
                    m_sgIconVisible[g][inst] = v;
            }
        }
    }
    // Note: caller should call setVisibleThreshold(lastHidden) once after all
    // updateVisibleRange calls to batch-invalidate caches.
}

void TileMesh::build(const LevelData& level, const std::string& fillColorHex, const std::string& strokeColorHex, bool legacyCulling) {
    destroy();
    m_legacyCulling = legacyCulling;
    const auto& tiles = level.tiles;
    if(tiles.size()<2) return;
    int n = (int)tiles.size()-1;
    LOG_D("TileMesh::build start: %d tiles, legacyCulling=%d", n, legacyCulling);

    auto hexToColor=[](const std::string& hex)->std::tuple<float,float,float>{
        unsigned v=hexToUInt(hex); return {((v>>16)&0xFF)/255.0f,((v>>8)&0xFF)/255.0f,(v&0xFF)/255.0f};
    };
    auto[fillR,fillG,fillB]=hexToColor(fillColorHex);
    auto[outR,outG,outB]=hexToColor(strokeColorHex);

    std::unordered_map<GeoKey,std::vector<int>,std::hash<GeoKey>> shapeGroups;
    for(int i=0;i<n;i++){
        float sa=(i==0)?-180.0f:tiles[i-1].direction-180.0f, ea=tiles[i].direction;
        bool mid=(i<(int)level.angleData.size()&&level.angleData[i]==999.0);
        shapeGroups[GeoKey((int)std::round(sa*100),(int)std::round(ea*100),mid)].push_back(i);
    }
    LOG_D("TileMesh::build: %zu unique shapes", shapeGroups.size());

    // Split large groups to avoid huge contiguous allocations (e.g. 7M tiles)
    // Local scratch: geometry generation is single-threaded per build.
    Scratch sc;
    // Count total groups after splitting
    size_t totalGroups = 0;
    for (auto& [key, tileIndices] : shapeGroups) {
        totalGroups += (tileIndices.size() + MAX_INSTANCES_PER_GROUP - 1) / MAX_INSTANCES_PER_GROUP;
    }
    m_shapes.resize(totalGroups);
    m_tileToShape.assign(n,-1); m_tileToInstance.assign(n,-1);
    size_t si=0;

    for(auto&[key,tileIndices]:shapeGroups){
        std::sort(tileIndices.begin(),tileIndices.end(),std::greater<int>());
        auto[sa,ea,mid]=key; float sA=sa/100.0f, eA=ea/100.0f;

        bool cached=false; unsigned csv=0,csi=0;
        double lmx=0,lmy=0,lMx=0,lMy=0;
        auto cit=s_geoCache.find(key);
        if(cit!=s_geoCache.end()){const auto& cg=cit->second;csv=cg.strokeVertCount;csi=cg.strokeIdxCount;
        lmx=cg.localMinX;lmy=cg.localMinY;lMx=cg.localMaxX;lMy=cg.localMaxY;cached=true;}

        CachedGeo ng;
        if(!cached){
            sc.clear(); mid?createMidSpinMesh(sA,sc):createTileMesh(sA,eA,sc);
            size_t vc=sc.verts.size()/3; ng.interleaved.reserve(vc*4);
            for(size_t vi=0;vi<vc;vi++){ng.interleaved.push_back(sc.verts[vi*3]);ng.interleaved.push_back(sc.verts[vi*3+1]);
            ng.interleaved.push_back(sc.verts[vi*3+2]);ng.interleaved.push_back(sc.types[vi]);}
            ng.indices.assign(sc.indices.begin(),sc.indices.end()); ng.idxCount=(unsigned)sc.indices.size();
            {unsigned tv=(unsigned)ng.interleaved.size()/4;
            for(unsigned vi=0;vi<tv;vi++){if(ng.interleaved[vi*4+3]<0.5f)ng.strokeVertCount++;else break;}
            for(unsigned ii=0;ii<ng.idxCount;ii++){if(ng.indices[ii]>=ng.strokeVertCount){ng.strokeIdxCount=ii;break;}}
            if(ng.strokeIdxCount==0&&ng.idxCount>0)ng.strokeIdxCount=ng.idxCount;}
            {double mnX=1e99,mnY=1e99,mxX=-1e99,mxY=-1e99;
            for(size_t vi=0;vi<vc;vi++){double lx=ng.interleaved[vi*4],ly=ng.interleaved[vi*4+1];
            if(lx<mnX)mnX=lx;if(lx>mxX)mxX=lx;if(ly<mnY)mnY=ly;if(ly>mxY)mxY=ly;}
            ng.localMinX=mnX;ng.localMinY=mnY;ng.localMaxX=mxX;ng.localMaxY=mxY;}
            csv=ng.strokeVertCount;csi=ng.strokeIdxCount;
            lmx=ng.localMinX;lmy=ng.localMinY;lMx=ng.localMaxX;lMy=ng.localMaxY;
            s_geoCache[key]=std::move(ng);
        }

        unsigned idc=cached?cit->second.idxCount:s_geoCache[key].idxCount;

        // Split into subgroups to keep allocations manageable
        size_t total=tileIndices.size();
        if (total > MAX_INSTANCES_PER_GROUP) {
            LOG_D("TileMesh::build: splitting large group total=%zu into %zu subgroups", total,
                  (total + MAX_INSTANCES_PER_GROUP - 1) / MAX_INSTANCES_PER_GROUP);
        }
        for(size_t subStart=0;subStart<total;subStart+=MAX_INSTANCES_PER_GROUP){
            size_t subEnd=std::min(subStart+MAX_INSTANCES_PER_GROUP,total);
            size_t cnt=subEnd-subStart;
            ShapeGroup& sg=m_shapes[si];
            sg.indexCount=idc; sg.strokeIndexCount=csi; sg.fillIndexCount=idc-csi;
            sg.fillIndexByteOffset=csi*(unsigned)sizeof(unsigned);

            if (legacyCulling) {
                sg.instances.reserve(cnt);
            } else {
                allocSoA(sg,cnt);
            }
            std::vector<float> po; po.reserve(cnt*3);
            std::vector<float> co; co.reserve(cnt*7);
            double gmx=1e99,gmy=1e99,gMx=-1e99,gMy=-1e99;

            for(size_t k=subStart;k<subEnd;k++){int i=tileIndices[k]; size_t local=k-subStart;
                double wx=tiles[i].position[0],wy=tiles[i].position[1]; float wz=tileZForIndex(i,n);
                po.push_back((float)wx);po.push_back((float)wy);po.push_back(wz);
                co.push_back(fillR);co.push_back(fillG);co.push_back(fillB);
                co.push_back(outR);co.push_back(outG);co.push_back(outB);co.push_back(1.0f);
                double mx=lmx+wx,my=lmy+wy,Mx=lMx+wx,My=lMy+wy;
                if (legacyCulling) {
                    sg.instances.push_back({wx,wy,wz,fillR,fillG,fillB,outR,outG,outB,1.0f,mx,my,Mx,My});
                } else {
                    sg.cullMinX[local]=mx;sg.cullMaxX[local]=Mx;sg.cullMinY[local]=my;sg.cullMaxY[local]=My;
                    sg.posX[local]=(float)wx;sg.posY[local]=(float)wy;sg.posZ[local]=wz;
                }
                if(mx<gmx)gmx=mx;if(Mx>gMx)gMx=Mx;if(my<gmy)gmy=my;if(My>gMy)gMy=My;
                m_tileToShape[i]=(int)si;m_tileToInstance[i]=(int)local;
            }
            sg.groupMinX=gmx;sg.groupMinY=gmy;sg.groupMaxX=gMx;sg.groupMaxY=gMy;

            glGenVertexArrays(1,&sg.vao);glBindVertexArray(sg.vao);
            glGenBuffers(1,&sg.vbo);glBindBuffer(GL_ARRAY_BUFFER,sg.vbo);
            if(cached)glBufferData(GL_ARRAY_BUFFER,cit->second.interleaved.size()*sizeof(float),cit->second.interleaved.data(),GL_STATIC_DRAW);
            else glBufferData(GL_ARRAY_BUFFER,s_geoCache[key].interleaved.size()*sizeof(float),s_geoCache[key].interleaved.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
            glEnableVertexAttribArray(1);glVertexAttribPointer(1,1,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(3*sizeof(float)));
            glGenBuffers(1,&sg.ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,sg.ebo);
            if(cached)glBufferData(GL_ELEMENT_ARRAY_BUFFER,cit->second.indices.size()*sizeof(unsigned),cit->second.indices.data(),GL_STATIC_DRAW);
            else glBufferData(GL_ELEMENT_ARRAY_BUFFER,s_geoCache[key].indices.size()*sizeof(unsigned),s_geoCache[key].indices.data(),GL_STATIC_DRAW);
            glGenBuffers(1,&sg.instVbo);glBindBuffer(GL_ARRAY_BUFFER,sg.instVbo);
            glBufferData(GL_ARRAY_BUFFER,po.size()*sizeof(float),po.data(),GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(2);glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);glVertexAttribDivisor(2,1);
            GLsizei cs=7*sizeof(float);glGenBuffers(1,&sg.colorVbo);glBindBuffer(GL_ARRAY_BUFFER,sg.colorVbo);
            glBufferData(GL_ARRAY_BUFFER,co.size()*sizeof(float),co.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,cs,(void*)0);glVertexAttribDivisor(3,1);
            glEnableVertexAttribArray(4);glVertexAttribPointer(4,3,GL_FLOAT,GL_FALSE,cs,(void*)(3*sizeof(float)));glVertexAttribDivisor(4,1);
            glEnableVertexAttribArray(5);glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,cs,(void*)(6*sizeof(float)));glVertexAttribDivisor(5,1);
            { std::vector<int> ti; ti.reserve(cnt); for(size_t k=subStart;k<subEnd;k++) ti.push_back(tileIndices[k]); m_sgTileIndices.push_back(std::move(ti)); }
            glBindVertexArray(0);si++;
        }
    }
    LOG_D("Built track: %d tiles -> %zu shape groups",n,m_shapes.size());
    m_visCaches.resize(m_shapes.size()); buildIcons(level);
    LOG_D("buildIcons done, %zu icon groups", m_iconGroups.size());
}


// Culling loop: tests AABBs in batches of CullSIMD::WIDTH.
// When C++26 std::simd lands, update CullSIMD::test4 — no changes needed here.
static void simdCullGroup(const ShapeGroup& sg, double vl, double vr, double vb, double vt, std::vector<int>& out) {
    size_t n = sg.instanceCount; if (!n) return; out.reserve(n);
    size_t i = 0;
    constexpr size_t W = CullSIMD::WIDTH;
    for (; i + (W-1) < n; i += W) {
        int mask = CullSIMD::test4(sg.cullMinX + i, sg.cullMaxX + i,
                                    sg.cullMinY + i, sg.cullMaxY + i,
                                    vl, vr, vb, vt);
        for (size_t b = 0; b < W; b++)
            if (mask & (1 << (int)b)) out.push_back((int)(i + b));
    }
    for (; i < n; i++) {
        if (sg.cullMaxX[i] < vl || sg.cullMinX[i] > vr || sg.cullMaxY[i] < vb || sg.cullMinY[i] > vt) continue;
        out.push_back((int)i);
    }
}


// Legacy culling: brute-force AoS path (used when legacyCulling=true)
static void cullAndOffsetGroupsLegacy(const std::vector<ShapeGroup>& groups,
    std::vector<TileMesh::VisibilityCache>& caches, size_t start, size_t end,
    double vl, double vr, double vb, double vt, double camX, double camY,
    const std::vector<std::vector<uint8_t>>* visible = nullptr) {
    for (size_t si = start; si < end; si++) {
        const auto& sg = groups[si]; auto& ca = caches[si];
        const uint8_t* vis = (visible && si < visible->size()) ? (*visible)[si].data() : nullptr;
        if (!ca.valid || TileMesh::frustumCheck(ca, (float)vl, (float)vr, (float)vb, (float)vt)) {
            ca.indices.clear(); ca.indices.reserve(sg.instances.size());
            for (int ii = (int)sg.instances.size()-1; ii >= 0; ii--) {
                const auto& inst = sg.instances[ii];
                if (vis && !vis[ii]) continue;
                if (inst.maxX < vl || inst.minX > vr || inst.maxY < vb || inst.minY > vt) continue;
                ca.indices.push_back(ii);
            }
            ca.vl = vl; ca.vr = vr; ca.vb = vb; ca.vt = vt; ca.valid = true; ca.offsetsValid = false;
        }
        if (ca.indices.empty()) continue;
        size_t vc = ca.indices.size();
        ca.offsets.resize(vc * 3);
        for (size_t i = 0; i < vc; i++) {
            const auto& inst = sg.instances[ca.indices[i]];
            ca.offsets[i*3] = (float)(inst.offX - camX);
            ca.offsets[i*3+1] = (float)(inst.offY - camY);
            ca.offsets[i*3+2] = inst.offZ;
        }
    }
}

static void cullAndOffsetGroups(const std::vector<ShapeGroup>& groups,
    std::vector<TileMesh::VisibilityCache>& caches, size_t start, size_t end,
    double vl, double vr, double vb, double vt, double camX, double camY,
    const std::vector<std::vector<uint8_t>>* visible = nullptr) {
    for(size_t si=start;si<end;si++){
        const auto& sg=groups[si]; auto& ca=caches[si];
        if(sg.groupMaxX<vl||sg.groupMinX>vr||sg.groupMaxY<vb||sg.groupMinY>vt){ca.indices.clear();ca.offsets.clear();ca.valid=false;continue;}
        bool re=!ca.valid||TileMesh::frustumCheck(ca,(float)vl,(float)vr,(float)vb,(float)vt);
        if(re){ca.indices.clear();simdCullGroup(sg,vl,vr,vb,vt,ca.indices);ca.vl=vl;ca.vr=vr;ca.vb=vb;ca.vt=vt;ca.valid=true;ca.offsetsValid=false;}
        if (visible && si < visible->size()) {
            const auto& vis = (*visible)[si];
            if (!vis.empty()) {
                ca.indices.erase(std::remove_if(ca.indices.begin(), ca.indices.end(),
                    [&vis](int idx) { return !vis[idx]; }), ca.indices.end());
            }
        }
        if(ca.indices.empty())continue;
        size_t vc=ca.indices.size();
        if(ca.offsetsValid){float dx=(float)(ca.prevCamX-camX),dy=(float)(ca.prevCamY-camY);
            for(size_t i=0;i<vc;i++){ca.offsets[i*3]+=dx;ca.offsets[i*3+1]+=dy;}}
        else{ca.offsets.resize(vc*3);
            for(size_t i=0;i<vc;i++){int idx=ca.indices[i];ca.offsets[i*3]=sg.posX[idx]-(float)camX;
            ca.offsets[i*3+1]=sg.posY[idx]-(float)camY;ca.offsets[i*3+2]=sg.posZ[idx];}ca.offsetsValid=true;}
        ca.prevCamX=camX;ca.prevCamY=camY;
    }
}

static ThreadPool& getPool() { static ThreadPool pool; return pool; }

void TileMesh::draw(float vL, float vR, float vB, float vT, double cX, double cY) const {
    double m=20.0, vl=vL-m, vr=vR+m, vb=vB-m, vt=vT+m;
    size_t n=m_shapes.size(); if(!n)return;
    if (m_legacyCulling) {
        constexpr size_t PT=64;
        if(n>=PT){auto& p=getPool();p.parallelFor(0,n,[&](size_t s,size_t e){cullAndOffsetGroupsLegacy(m_shapes,m_visCaches,s,e,vl,vr,vb,vt,cX,cY,&m_sgVisible);},1);}
        else cullAndOffsetGroupsLegacy(m_shapes,m_visCaches,0,n,vl,vr,vb,vt,cX,cY,&m_sgVisible);
    } else {
        constexpr size_t PT=64;
        if(n>=PT){auto& p=getPool();p.parallelFor(0,n,[&](size_t s,size_t e){cullAndOffsetGroups(m_shapes,m_visCaches,s,e,vl,vr,vb,vt,cX,cY,&m_sgVisible);},1);}
        else cullAndOffsetGroups(m_shapes,m_visCaches,0,n,vl,vr,vb,vt,cX,cY,&m_sgVisible);
    }
    for(size_t si=0;si<n;si++){const auto& sg=m_shapes[si];auto& ca=m_visCaches[si];if(ca.indices.empty())continue;
        glBindVertexArray(sg.vao);glBindBuffer(GL_ARRAY_BUFFER,sg.instVbo);
        glBufferSubData(GL_ARRAY_BUFFER,0,ca.offsets.size()*sizeof(float),ca.offsets.data());
        glDrawElementsInstanced(GL_TRIANGLES,sg.indexCount,GL_UNSIGNED_INT,nullptr,(GLsizei)(ca.indices.size()));glBindVertexArray(0);}
}

static constexpr float IR=0.11f; static constexpr int IS=16;
static const float TC[3]={0.502f,0,0.502f},SUC[3]={1,0,0},SDC[3]={0,0,1};

void TileMesh::buildIcons(const LevelData& level) {
    LOG_D("buildIcons: start");
    for(auto& s:m_iconGroups){if(s.instVbo)glDeleteBuffers(1,&s.instVbo);if(s.colorVbo)glDeleteBuffers(1,&s.colorVbo);
    if(s.ebo)glDeleteBuffers(1,&s.ebo);if(s.vbo)glDeleteBuffers(1,&s.vbo);if(s.vao)glDeleteVertexArrays(1,&s.vao);freeSoA(s);}
    m_iconGroups.clear();
    const auto& tiles=level.tiles; int n=(int)tiles.size()-1; if(n<=0)return;
    struct II{int ti;float zo;}; std::vector<II> cg[3];
    for(int i=0;i<n;i++){bool ht=i<(int)level.tileHasTwirl.size()&&level.tileHasTwirl[i];
        bool hs=i<(int)level.tileHasSetSpeed.size()&&level.tileHasSetSpeed[i]; float tz=tileZForIndex(i,n);
        if(ht)cg[0].push_back({i,tz+kIconZBase});
        if(hs&&i>0&&i<(int)level.tileBPMs.size()){float r=level.tileBPMs[i]/level.tileBPMs[i-1];
            if(r>1.05f||r<0.95f){int ci=(r>1.05f)?1:2;float zo=kIconZBase+(ht?kIconZExtra:(kIconZBase*0.5f));cg[ci].push_back({i,tz+zo});}}
    }
    const float* cs[3]={TC,SUC,SDC}; Scratch sc; sc.clear();
    createCircle(0,0,IR,1.0f,sc,IS); size_t vc=sc.verts.size()/3;
    std::vector<float> sv;sv.reserve(vc*4);
    for(size_t vi=0;vi<vc;vi++){sv.push_back(sc.verts[vi*3]);sv.push_back(sc.verts[vi*3+1]);sv.push_back(sc.verts[vi*3+2]);sv.push_back(sc.types[vi]);}
    unsigned sic=(unsigned)sc.indices.size(); double il=-(double)IR,iL=(double)IR;
    std::vector<uint64_t> iconTmp;
    iconTmp.reserve((size_t)cg[0].size() + cg[1].size() + cg[2].size());
    for(int ci=0;ci<3;ci++){if(cg[ci].empty())continue; auto& gr=cg[ci];
        float cr=cs[ci][0],cgv=cs[ci][1],cb=cs[ci][2]; size_t total=gr.size();
        // Split icon groups like tile groups
        for(size_t subStart=0;subStart<total;subStart+=MAX_INSTANCES_PER_GROUP){
            size_t subEnd=std::min(subStart+MAX_INSTANCES_PER_GROUP,total);
            size_t cnt=subEnd-subStart;
            uint32_t g = (uint32_t)m_iconGroups.size();   // final index of this subgroup
            ShapeGroup sg; sg.indexCount=sic; sg.fillIndexCount=sic;
            if (!m_legacyCulling) allocSoA(sg,cnt);
            if (m_legacyCulling) sg.instances.reserve(cnt);
            std::vector<float> ip;ip.reserve(cnt*3);std::vector<float> ic;ic.reserve(cnt*7);
            double gmx=1e99,gmy=1e99,gMx=-1e99,gMy=-1e99;
            for(size_t k=subStart;k<subEnd;k++){double wx=tiles[gr[k].ti].position[0],wy=tiles[gr[k].ti].position[1];float wz=gr[k].zo;
                size_t local=k-subStart;
                iconTmp.push_back(((uint64_t)(uint32_t)gr[k].ti << 32) |
                                  ((uint64_t)g << 20) | (uint32_t)local);
                ip.push_back((float)wx);ip.push_back((float)wy);ip.push_back(wz);
                ic.push_back(cr);ic.push_back(cgv);ic.push_back(cb);ic.push_back(cr);ic.push_back(cgv);ic.push_back(cb);ic.push_back(1);
                double mx=wx+il,my=wy+il,Mx=wx+iL,My=wy+iL;
                if (m_legacyCulling) {
                    sg.instances.push_back({wx,wy,wz,cr,cgv,cb,cr,cgv,cb,1.0f,mx,my,Mx,My});
                } else {
                    sg.cullMinX[local]=mx;sg.cullMaxX[local]=Mx;sg.cullMinY[local]=my;sg.cullMaxY[local]=My;
                    sg.posX[local]=(float)wx;sg.posY[local]=(float)wy;sg.posZ[local]=wz;
                }
                if(mx<gmx)gmx=mx;if(Mx>gMx)gMx=Mx;if(my<gmy)gmy=my;if(My>gMy)gMy=My;
            }
            sg.groupMinX=gmx;sg.groupMinY=gmy;sg.groupMaxX=gMx;sg.groupMaxY=gMy;
            glGenVertexArrays(1,&sg.vao);glBindVertexArray(sg.vao);
            glGenBuffers(1,&sg.vbo);glBindBuffer(GL_ARRAY_BUFFER,sg.vbo);glBufferData(GL_ARRAY_BUFFER,sv.size()*sizeof(float),sv.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
            glEnableVertexAttribArray(1);glVertexAttribPointer(1,1,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(3*sizeof(float)));
            glGenBuffers(1,&sg.ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,sg.ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sc.indices.size()*sizeof(unsigned),sc.indices.data(),GL_STATIC_DRAW);
            glGenBuffers(1,&sg.instVbo);glBindBuffer(GL_ARRAY_BUFFER,sg.instVbo);glBufferData(GL_ARRAY_BUFFER,ip.size()*sizeof(float),ip.data(),GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(2);glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);glVertexAttribDivisor(2,1);
            GLsizei cst=7*sizeof(float);glGenBuffers(1,&sg.colorVbo);glBindBuffer(GL_ARRAY_BUFFER,sg.colorVbo);glBufferData(GL_ARRAY_BUFFER,ic.size()*sizeof(float),ic.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,cst,(void*)0);glVertexAttribDivisor(3,1);
            glEnableVertexAttribArray(4);glVertexAttribPointer(4,3,GL_FLOAT,GL_FALSE,cst,(void*)(3*sizeof(float)));glVertexAttribDivisor(4,1);
            glEnableVertexAttribArray(5);glVertexAttribPointer(5,1,GL_FLOAT,GL_FALSE,cst,(void*)(6*sizeof(float)));glVertexAttribDivisor(5,1);
            { std::vector<int> ti; ti.reserve(cnt); for(size_t k=subStart;k<subEnd;k++) ti.push_back(gr[k].ti); m_sgIconTileIndices.push_back(std::move(ti)); }
            glBindVertexArray(0);m_iconGroups.push_back(std::move(sg));
        }
    }
    // CSR bucket icon instances by tile (counting sort on the tile id).
    if (iconTmp.empty()) {
        m_iconEntryFirst.clear();
        m_iconEntries.clear();
    } else {
        m_iconEntryFirst.assign((size_t)n + 1, 0);
        for (uint64_t e : iconTmp)
            m_iconEntryFirst[(uint32_t)(e >> 32)]++;
        uint32_t acc = 0;
        for (uint32_t i = 0; i < (uint32_t)n; i++) {
            uint32_t c = m_iconEntryFirst[i];
            m_iconEntryFirst[i] = acc;
            acc += c;
        }
        m_iconEntryFirst[(size_t)n] = acc;
        std::vector<uint32_t> cur = m_iconEntryFirst;   // cursor per tile
        m_iconEntries.assign(acc, 0);
        for (uint64_t e : iconTmp) {
            uint32_t t = (uint32_t)(e >> 32);
            m_iconEntries[cur[t]++] = (uint32_t)(e & 0xFFFFFFFFu);
        }
    }
    m_iconVisCaches.resize(m_iconGroups.size());
    LOG_D("Built event icons: %zu icon groups",m_iconGroups.size());
}

void TileMesh::drawIcons(float vL,float vR,float vB,float vT,double cX,double cY) const {
    double m=20.0,vl=vL-m,vr=vR+m,vb=vB-m,vt=vT+m;
    for(size_t si=0;si<m_iconGroups.size();si++){const auto& sg=m_iconGroups[si];auto& ca=m_iconVisCaches[si];
        if(sg.groupMaxX<vl||sg.groupMinX>vr||sg.groupMaxY<vb||sg.groupMinY>vt){ca.indices.clear();ca.offsets.clear();ca.valid=false;continue;}
        bool re=!ca.valid||frustumCheck(ca,(float)vl,(float)vr,(float)vb,(float)vt);
        if(re){ca.indices.clear();simdCullGroup(sg,vl,vr,vb,vt,ca.indices);ca.vl=vl;ca.vr=vr;ca.vb=vb;ca.vt=vt;ca.valid=true;ca.offsetsValid=false;}
        if (si < m_sgIconVisible.size()) {
            const auto& vis = m_sgIconVisible[si];
            if (!vis.empty()) {
                ca.indices.erase(std::remove_if(ca.indices.begin(), ca.indices.end(),
                    [&vis](int idx) { return !vis[idx]; }), ca.indices.end());
            }
        }
        if(ca.indices.empty())continue;
        size_t vc=ca.indices.size();
        if(ca.offsetsValid){float dx=(float)(ca.prevCamX-cX),dy=(float)(ca.prevCamY-cY);
            for(size_t i=0;i<vc;i++){ca.offsets[i*3]+=dx;ca.offsets[i*3+1]+=dy;}}
        else{ca.offsets.resize(vc*3);for(size_t i=0;i<vc;i++){int idx=ca.indices[i];
            ca.offsets[i*3]=sg.posX[idx]-(float)cX;ca.offsets[i*3+1]=sg.posY[idx]-(float)cY;ca.offsets[i*3+2]=sg.posZ[idx];}ca.offsetsValid=true;}
        ca.prevCamX=cX;ca.prevCamY=cY;
        glBindVertexArray(sg.vao);glBindBuffer(GL_ARRAY_BUFFER,sg.instVbo);
        glBufferSubData(GL_ARRAY_BUFFER,0,ca.offsets.size()*sizeof(float),ca.offsets.data());
        glDrawElementsInstanced(GL_TRIANGLES,sg.indexCount,GL_UNSIGNED_INT,nullptr,(GLsizei)vc);glBindVertexArray(0);}
}

void TileMesh::drawHighlightedTile(int ti,double cX,double cY) const {
    if(ti<0||ti>=(int)m_tileToShape.size())return;int sI=m_tileToShape[ti],iI=m_tileToInstance[ti];if(sI<0||iI<0)return;
    const auto& sg=m_shapes[sI];
    float off[3];
    if (m_legacyCulling) {
        if(iI>=(int)sg.instances.size())return;
        off[0]=(float)(sg.instances[iI].offX-cX);off[1]=(float)(sg.instances[iI].offY-cY);off[2]=sg.instances[iI].offZ;
    } else {
        if(iI>=(int)sg.instanceCount)return;
        off[0]=sg.posX[iI]-(float)cX;off[1]=sg.posY[iI]-(float)cY;off[2]=sg.posZ[iI];
    }
    glBindVertexArray(sg.vao);glBindBuffer(GL_ARRAY_BUFFER,sg.instVbo);glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(off),off);
    glDrawElementsInstanced(GL_TRIANGLES,sg.indexCount,GL_UNSIGNED_INT,nullptr,1);glBindVertexArray(0);
}

unsigned int TileMesh::hexToUInt(const std::string& hex) {unsigned v=0;for(char c:hex){v<<=4;
    if(c>='0'&&c<='9')v|=c-'0';else if(c>='a'&&c<='f')v|=c-'a'+10;else if(c>='A'&&c<='F')v|=c-'A'+10;else break;}return v;}

float TileMesh::tileZForIndex(int i,int n){if(n<=1)return kMaxTileZ*0.5f;return kMaxTileZ*(1.0f-(float)i/(float)(n-1));}
