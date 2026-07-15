#!/bin/sh
set -e

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT_DIR=${1:-"$ROOT_DIR/dist"}

if [ -z "${IDF_PATH:-}" ]; then
	IDF_ACTIVATE="$HOME/.espressif/tools/activate_idf_v5.5.4.sh"
	if [ ! -f "$IDF_ACTIVATE" ]; then
		echo "未找到 ESP-IDF 5.5.4。请先激活 ESP-IDF 环境。" >&2
		exit 1
	fi
	set -a
	eval "$("$IDF_ACTIVATE" -e)"
	set +a
fi
set -u

if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
	IDF_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
elif command -v python3 >/dev/null 2>&1; then
	IDF_PYTHON=$(command -v python3)
else
	echo "未找到 Python 3。" >&2
	exit 1
fi

IDF_PY="$IDF_PATH/tools/idf.py"
ESPTOOL_PY="$IDF_PATH/components/esptool_py/esptool/esptool.py"
if [ ! -f "$IDF_PY" ] || [ ! -f "$ESPTOOL_PY" ]; then
	echo "ESP-IDF 路径无效：$IDF_PATH" >&2
	exit 1
fi
if ! command -v zip >/dev/null 2>&1; then
	echo "未找到 zip 命令。" >&2
	exit 1
fi
if ! command -v shasum >/dev/null 2>&1; then
	echo "未找到 shasum 命令。" >&2
	exit 1
fi

echo "正在构建固件..."
"$IDF_PYTHON" "$IDF_PY" -C "$ROOT_DIR" build

BOOTLOADER="$ROOT_DIR/build/bootloader/bootloader.bin"
PARTITION_TABLE="$ROOT_DIR/build/partition_table/partition-table.bin"
APPLICATION="$ROOT_DIR/build/DeadEyeShot_ESPIDF.bin"
for file in "$BOOTLOADER" "$PARTITION_TABLE" "$APPLICATION"; do
	if [ ! -f "$file" ]; then
		echo "缺少构建产物：$file" >&2
		exit 1
	fi
done

VERSION=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || date +%Y%m%d-%H%M%S)
if [ -n "$(git -C "$ROOT_DIR" status --porcelain --untracked-files=no 2>/dev/null || true)" ]; then
	VERSION="$VERSION-dirty"
fi

PACKAGE_NAME="DeadEyeShot-ESP32S3-$VERSION"
TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/deadeye-firmware.XXXXXX")
PACKAGE_DIR="$TEMP_DIR/$PACKAGE_NAME"
trap 'rm -rf "$TEMP_DIR"' EXIT INT TERM

mkdir -p "$PACKAGE_DIR" "$OUTPUT_DIR"
cp "$BOOTLOADER" "$PACKAGE_DIR/bootloader.bin"
cp "$PARTITION_TABLE" "$PACKAGE_DIR/partition-table.bin"
cp "$APPLICATION" "$PACKAGE_DIR/DeadEyeShot_ESPIDF.bin"

"$IDF_PYTHON" "$ESPTOOL_PY" --chip esp32s3 merge_bin \
	--flash_mode dio --flash_freq 80m --flash_size 16MB \
	-o "$PACKAGE_DIR/DeadEyeShot-ESP32S3-merged.bin" \
	0x0 "$PACKAGE_DIR/bootloader.bin" \
	0x8000 "$PACKAGE_DIR/partition-table.bin" \
	0x10000 "$PACKAGE_DIR/DeadEyeShot_ESPIDF.bin"

printf '# DeadEyeShot ESP32-S3 固件\n\n固件版本：`%s`\n\n' "$VERSION" > "$PACKAGE_DIR/README.md"
cat >> "$PACKAGE_DIR/README.md" <<'EOF'
## 准备工作

电脑需要安装 Python 3 和 esptool：

```sh
python3 -m pip install --upgrade esptool
```

Windows 如果无法使用 `python3`，请改用 `py`。

## 首次安装或完全重装

合并固件适合首次安装或完全重装。该方式会清除设备中保存的 Wi-Fi 配置：

```sh
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 DeadEyeShot-ESP32S3-merged.bin
```

请将串口名称替换为电脑上实际显示的端口：

- macOS：`/dev/cu.usbmodem*`
- Linux：`/dev/ttyACM0` 或 `/dev/ttyUSB0`
- Windows：`COM3`、`COM4` 等

也可以直接使用压缩包内的一键烧录脚本：

```sh
./flash-macos-linux.sh /dev/cu.usbmodem101
```

```bat
flash-windows.bat COM3
```

## 升级固件并保留 Wi-Fi

如果需要保留设备中已经保存的 Wi-Fi 配置，请使用三文件烧录方式：

```sh
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 DeadEyeShot_ESPIDF.bin
```

Windows 请把 `python3` 改为 `py`，并把串口改成实际的 `COM` 端口。

## 连接问题

- 如果烧录连接不稳定，请把 `-b 460800` 改为 `-b 115200`。
- 如果无法自动进入下载模式，可在开始连接时按住开发板的 BOOT 键，连接成功后松开。
- 如果串口被占用，请先关闭串口监视器或其他烧录工具。

## 固件参数

- 芯片：ESP32-S3
- Flash 模式：DIO
- Flash 频率：80 MHz
- Flash 容量：16 MB
- 合并固件从 `0x0` 开始写入，会覆盖 NVS 并清除已保存的 Wi-Fi。
- 三文件命令不会写入 NVS 分区，可以保留已保存的 Wi-Fi。

## 文件校验

macOS 或 Linux 可在解压目录执行：

```sh
shasum -a 256 -c SHA256SUMS
```
EOF

cat > "$PACKAGE_DIR/flash-macos-linux.sh" <<'EOF'
#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
	echo "用法：$0 /dev/cu.usbmodem101"
	exit 1
fi

cd "$(dirname "$0")"
python3 -m esptool --chip esp32s3 -p "$1" -b 460800 \
	--before default_reset --after hard_reset write_flash \
	--flash_mode dio --flash_freq 80m --flash_size 16MB \
	0x0 DeadEyeShot-ESP32S3-merged.bin
EOF
chmod +x "$PACKAGE_DIR/flash-macos-linux.sh"

cat > "$PACKAGE_DIR/flash-windows.bat" <<'EOF'
@echo off
if "%~1"=="" (
  echo 用法：flash-windows.bat COM3
  exit /b 1
)

cd /d "%~dp0"
py -m esptool --chip esp32s3 -p "%~1" -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 DeadEyeShot-ESP32S3-merged.bin
EOF

(
	cd "$PACKAGE_DIR"
	shasum -a 256 bootloader.bin partition-table.bin DeadEyeShot_ESPIDF.bin DeadEyeShot-ESP32S3-merged.bin > SHA256SUMS
)

ZIP_PATH="$OUTPUT_DIR/$PACKAGE_NAME.zip"
rm -f "$ZIP_PATH"
(
	cd "$TEMP_DIR"
	zip -qr "$ZIP_PATH" "$PACKAGE_NAME"
)

unzip -tq "$ZIP_PATH"
echo "打包完成：$ZIP_PATH"
