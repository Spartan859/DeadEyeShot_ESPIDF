# UPDATES.md - Algorithm & Architecture Changelog

## 2026-05-21: Connected Component Target Detection (branch: feature/connected-component-detection)

### Problem
Original algorithm computed centroid of ALL dark pixels to find target center. This fails when other dark objects (shadows, equipment, background) are present in the frame.

### New Algorithm: Two-Pass Connected Component Labeling

1. **Adaptive threshold**: Compute mean grayscale of the frame, use mean/2 as binary threshold (clamped [30, 180]).

2. **Two-pass connected component labeling** (4-connectivity):
   - Pass 1: Scan left-to-right, top-to-bottom. For each dark pixel, check left and top neighbors. Assign provisional labels using union-find for equivalence resolution.
   - Pass 2: Resolve all labels to their union-find roots.

3. **Component statistics** (single pass over label buffer):
   - Area (pixel count)
   - Centroid (sum_x/area, sum_y/area)
   - Perimeter (count of boundary pixels where any 4-neighbor has a different label)

4. **Filtering**:
   - Minimum area: 200 pixels
   - Minimum circularity: 0.4 (where circularity = 4*pi*area / perimeter^2; perfect circle = 1.0)

5. **Selection**: Choose component with highest quality score = circularity * sqrt(area). This balances size and shape.

6. **Output**:
   - Target center: centroid of selected component
   - Black radius: sqrt(area / pi) (equivalent radius of a circle with same area)

### Mask Visualization
- `target_detect()` accepts optional `mask_out` buffer (width*height bytes).
- Fills it with 255 for dark pixels, 0 for light pixels (raw binary mask).
- Web server serves mask via `/mask.bin` endpoint.
- Frontend renders mask as green-on-black canvas for threshold tuning.

### Files Changed
- `main/target_detector.h` - Added `mask_out` parameter
- `main/target_detector.c` - Full rewrite with CC labeling
- `main/web_server.h` - Added `web_server_update_mask()`
- `main/web_server.c` - Mask storage, `/mask.bin` endpoint, frontend canvas
- `main/main.c` - Wire mask buffer through proc_task
