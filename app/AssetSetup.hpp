#pragma once

// 把"资产在哪"这套产品相关的知识集中在一处（ADOCAO 是唯一调用者）。
// 库那边（adofai::assetOptions）默认只有"相对当前目录"，产品名/布局都从这里进。
void configureAssetPaths();
