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

---

## 2026-05-21: 提高圆度阈值 + 过曝动态阈值搜索 (分支: feature/connected-component-detection)

### 问题
1. 圆度阈值 0.4 太低，5:1 长条都能通过，导致不圆的连通块被判成靶子。
2. 自动曝光过曝时画面泛白，mask 几乎为空，找不到靶子。

### 改动
1. **MIN_CIRCULARITY 0.4 → 0.8**：正方形约 0.785，只有明显比正方形更圆的形状才通过。

2. **过曝动态阈值搜索**：
   - 首次尝试初始阈值（mean/2）后，若暗像素占比 < 2%，判定为过曝。
   - 以 mean * 5% 为步长逐步提高阈值，上限 mean * 90%。
   - 每步重新执行 CC 标记 + 圆度筛选，找到圆度达标的靶子即停止。
   - 暗像素占比超过 50% 仍未找到，判定靶子在视野外，放弃。
   - 所有缓冲区只分配一次，跨重试复用。

### 变更文件
- `main/target_detector.c` - 重构为阈值搜索循环

---

## 2026-06-26: 恢复摄像头自动曝光 (分支: codex/image-upload-only-phone-detect)

### 问题
当前环境下摄像头画面欠曝光。此前初始化时关闭自动曝光和自动增益，并固定 `aec_value=300`、`agc_gain=8`，在光线不足时无法自动补亮。

### 改动
1. **恢复自动曝光**：启用 `set_exposure_ctrl(s, 1)`，由 OV5640 根据现场光照自动调整曝光。
2. **恢复自动增益**：启用 `set_gain_ctrl(s, 1)`，低光环境下允许传感器自动提高增益。
3. **启用 AEC2**：启用 `set_aec2(s, 1)`，提升低光场景下的自动曝光适应能力。
4. **移除固定曝光/增益值**：不再设置 `set_aec_value(s, 300)` 和 `set_agc_gain(s, 8)`，避免手动值继续压暗画面。

### 变更文件
- `main/camera_driver.c` - 摄像头初始化改为自动曝光/自动增益

---

## 2026-06-18: 板端只上传图片，检测迁移到手机端 (分支: codex/image-upload-only-phone-detect)

### 背景
ESP32-S3 板端内存和算力有限，后续计划由手机上位机使用 uni-app x + zj-opencv 执行靶心检测。

### 改动
1. **板端不再执行 target detect**：
   - 触发后只取最新 JPEG 帧并缓存到 Web 服务。
   - 移除灰度转换、连通域检测、mask 生成和分数计算流程。

2. **HTTP 接口精简**：
   - 保留 `/shot.jpg`，用于手机端拉取最新 JPEG。
   - `/api/shot` 仅返回图片状态和元信息：`has_shot`、`w`、`h`、`jpeg_len`。
   - 移除 `/mask.bin` 调试端点和网页上的检测标注。

3. **BLE 只保留状态通知**：
   - 触发后发送 `0x01`。
   - 图片缓存成功发送 `0x02`。
   - 无帧或缓存失败发送 `0xFF`。
   - 不再发送板端分数。

### 变更文件
- `main/main.c` - 简化触发处理为 JPEG 缓存和状态通知
- `main/web_server.c` - 精简页面、JSON 和端点
- `main/web_server.h` - 简化 `web_server_update_shot()` 参数
- `main/ble_service.h` - 移除评分和靶型接口
- `main/ble_service.cpp` - 只保留状态 characteristic
- `main/CMakeLists.txt` - 不再编译 `target_detector.c` 和 `scorer.c`
