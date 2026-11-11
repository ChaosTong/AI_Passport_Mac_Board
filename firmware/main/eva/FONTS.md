# EVA 音乐页 UI 位图字体

顶栏、侧栏与底部 `PREV/PLAY/…` 标签由 `firmware/tools/prepare_eva_*.py` 预渲染为 A8/ARGB 位图。

默认字体：**[Eva-Ming-SC](https://github.com/kaonashi-tyc/Eva-Ming-Simplified)**（`assets/fonts/Eva-Ming-SC-v0.1.otf`），基于 Matisse EB 风格的简体补字（CC BY-NC 4.0，非商用）。

首次缺字体：

```bash
./firmware/tools/fetch_eva_ming_font.sh
```

重新生成并烧录：

```bash
python3 firmware/tools/prepare_eva_text_assets.py
python3 firmware/tools/prepare_eva_text_buttons.py
./build-flash.sh flash
```

覆盖字体：`EVA_FONT_PATH=/path/to/font.otf`（例如官方 `FOT-MatissePro-EB.otf`）。
