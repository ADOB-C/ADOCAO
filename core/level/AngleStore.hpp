#pragma once

// ③ 切片 4 的另一半：`angleData` 的**位精确紧凑存储**（字典 + 位打包下标）。
//
// 为什么要有它：`Timeline::totalAngleAt()`/`angleDirBefore()` 每帧都要读 `angleData`（行星/拖尾的
// 转动），所以它**不能**像别的一次性数组那样直接释放 —— 但 8 B/层（MYC 54 MB）太贵。
// 实测（`adocao pack --codec-report`）真实谱面的角度**取值集极小**：MYC n=6,770,912 只有 **45** 个
// 不同取值，所以"唯一值字典 + ceil(log2(k)) 位下标"= 6 bit/层 = **0.75 B/层 = 4.8 MB**（省 49 MB）。
//
// 三条要求：
//   1. **逐位精确**：字典里存的是 double 的**位模式**（`memcmp` 相等即同一个值），解码出来与原值
//      逐位相同；哨兵 999.0 也只是一个普通取值，不需要特判。
//   2. 取值集超上限（`kMaxUnique`）就**不启用**（调用方保留原数组）—— 自定义角度极多的谱面
//      不该因为这里而退化。
//   3. 只服务**运行期读者**；加载期（解析、`Timeline::build`、`computeCheckpoints`、
//      `TileMesh::build`）仍然读原 `std::vector<double>`（那时它还在）。

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace adofai {

class AngleStore {
public:
    static constexpr int64_t kMaxUnique = 65536;   // 超过就放弃（下标的位宽上限 16）

    // 从已解析的角度数组编码。失败（含取值集过大）时不改变本对象、返回 false。
    bool build(const std::vector<double>& angles, std::string& err) {
        err.clear();
        m_dict.clear(); m_data.clear(); m_bits = 0; m_n = 0; m_valid = false;
        const int64_t n = (int64_t)angles.size();
        std::unordered_map<uint64_t, uint32_t> seen;
        seen.reserve(64);
        std::vector<uint32_t> idx((size_t)n);
        for (int64_t i = 0; i < n; ++i) {
            const double v = angles[(size_t)i];
            uint64_t bits = 0;
            std::memcpy(&bits, &v, 8);
            auto it = seen.find(bits);
            if (it == seen.end()) {
                if ((int64_t)m_dict.size() >= kMaxUnique) {
                    err = "角度取值集超过 " + std::to_string(kMaxUnique) + "（本谱不适合紧凑存储）";
                    return false;
                }
                const uint32_t id = (uint32_t)m_dict.size();
                m_dict.push_back(v);
                it = seen.emplace(bits, id).first;
            }
            idx[(size_t)i] = it->second;
        }
        m_bits = 1;
        while ((int64_t(1) << m_bits) < (int64_t)m_dict.size()) ++m_bits;   // ceil(log2(k))，k>=1 → >=1 位
        if (m_bits > 16) { err = "下标位宽超过 16"; m_dict.clear(); return false; }
        const size_t totalBits = (size_t)n * (size_t)m_bits;
        m_data.assign((totalBits + 7) / 8, 0);
        for (int64_t i = 0; i < n; ++i) putBits((size_t)i * (size_t)m_bits, idx[(size_t)i]);
        m_n = n;
        m_valid = true;
        return true;
    }

    bool valid() const { return m_valid; }
    int64_t size() const { return m_n; }
    int64_t uniqueCount() const { return (int64_t)m_dict.size(); }
    int bitsPerIndex() const { return m_bits; }

    double at(int64_t i) const {
        if (i < 0 || i >= m_n) return 180.0;
        const uint32_t id = getBits((size_t)i * (size_t)m_bits);
        return id < m_dict.size() ? m_dict[id] : 180.0;
    }

    // 账：字典（8 B/唯一值）+ 位打包数据
    size_t bytes() const { return m_dict.size() * sizeof(double) + m_data.size(); }

    // **仅测试用**：改一层，用于"逐位对拍"的反面对照
    bool setForTest(int64_t i, double v) {
        if (!m_valid || i < 0 || i >= m_n) return false;
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        for (size_t id = 0; id < m_dict.size(); ++id) {
            uint64_t db = 0;
            std::memcpy(&db, &m_dict[id], 8);
            if (db == bits) { putBits((size_t)i * (size_t)m_bits, (uint32_t)id); return true; }
        }
        return false;   // 只允许改成字典里已有的取值（否则就不是"位打包下标"了）
    }

private:
    void putBits(size_t bitPos, uint32_t v) {
        for (int b = 0; b < m_bits; ++b) {
            const size_t p = bitPos + (size_t)b;
            if (p / 8 >= m_data.size()) return;
            const uint8_t mask = (uint8_t)(1u << (p % 8));
            // ⚠️ 必须**清位**再置位：只置位的话，`setForTest` 换成"1 更少"的下标会留下残留位
            // （反面对照当场抓到这个 —— 生产路径 build() 是在全零缓冲上写的，所以只有它没事）
            if ((v >> b) & 1u) m_data[p / 8] |= mask;
            else               m_data[p / 8] &= (uint8_t)~mask;
        }
    }
    uint32_t getBits(size_t bitPos) const {
        uint32_t v = 0;
        for (int b = 0; b < m_bits; ++b) {
            const size_t p = bitPos + (size_t)b;
            if (p / 8 >= m_data.size()) return 0;
            if (m_data[p / 8] & (uint8_t)(1u << (p % 8))) v |= (1u << b);
        }
        return v;
    }

    std::vector<double> m_dict;      // 唯一取值（位模式去重，值本身逐位保留）
    std::vector<uint8_t> m_data;     // 位打包的下标
    int m_bits = 0;
    int64_t m_n = 0;
    bool m_valid = false;
};

}  // namespace adofai
