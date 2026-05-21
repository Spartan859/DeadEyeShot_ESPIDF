# UPDATES.md - 算法与架构变更日志

## 2026-05-21: 连通域靶纸检测算法 (分支: feature/connected-component-detection)

### 问题
原算法计算所有暗像素的质心作为靶心，当画面中存在其他暗色物体（阴影、设备、背景）时会失效。

### 新算法：两趟连通域标记

1. **自适应阈值**: 计算整帧灰度均值，取均值/2 作为二值化阈值（钳位至 [30, 180]）。

2. **两趟连通域标记**（4-连通）:
   - 第一趟：从左到右、从上到下扫描。对每个暗像素检查左、上邻居，使用 union-find 进行等价标签合并。
   - 第二趟：将所有标签解析为 union-find 根节点。

3. **连通域统计**（单趟遍历标签缓冲区）:
   - 面积（像素计数）
   - 质心（sum_x/area, sum_y/area）
   - 周长（边界像素计数：任一 4-邻居标签不同的像素）

4. **过滤条件**:
   - 最小面积：200 像素
   - 最小圆度：0.4（圆度 = 4*pi*面积 / 周长^2，完美圆 = 1.0）

5. **选择策略**: 取 quality = 圆度 * sqrt(面积) 最高的连通域，兼顾大小和形状。

6. **输出**:
   - 靶心：选中连通域的质心
   - 黑圆半径：sqrt(面积 / pi)（等效圆半径）

### Mask 可视化
- `target_detect()` 接受可选的 `mask_out` 缓冲区（width*height 字节）
- 填充 255（暗像素）/ 0（亮像素），即原始二值 mask
- Web 服务器通过 `/mask.bin` 端点提供 mask 数据
- 前端将 mask 渲染为绿底黑底的 canvas，用于阈值调参

### 变更文件
- `main/target_detector.h` - 新增 `mask_out` 参数
- `main/target_detector.c` - 全部重写为连通域标记算法
- `main/web_server.h` - 新增 `web_server_update_mask()`
- `main/web_server.c` - mask 存储、`/mask.bin` 端点、前端 canvas
- `main/main.c` - 在 proc_task 中传递 mask 缓冲区
