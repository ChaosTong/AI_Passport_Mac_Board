#!/usr/bin/env python3
"""MacBoard 编译音效转换：源音频 -> 16 kHz 单声道 IMA-ADPCM（4 bit）C 数组。

用法（需要 ffmpeg）：
    python3 convert_sounds.py
源文件（YouTube 下载，仅个人设备使用）：
    src/build_success.flac  <- https://www.youtube.com/watch?v=L2EhcPBuT4k（瑪莉歐過關音效）
    src/build_failure.flac  <- https://www.youtube.com/watch?v=-PfvRcCWSw8
    下载：yt-dlp -x --audio-format flac --js-runtimes node -o "src/%(id)s.%(ext)s" <url>
输出：
    build_sounds.c         —— 固件内嵌的 ADPCM 数组（buddy_sound.c 解码播放）
    preview/*.wav          —— 解码后的试听文件（与板子播放的数据一致）
"""
import os
import struct
import subprocess
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 16000
# name, source, trim start (s), trim end (s)
CLIPS = [
    ("build_success", "src/build_success.flac", 0.57, 6.05),
    ("build_failure", "src/build_failure.flac", 0.03, 2.75),
]
TARGET_RMS_DB = -17.0   # 两段响度对齐
PEAK_LIMIT_DB = -1.0    # 不削波
FADE_MS = 15

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
        60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def load(path, start, end):
    # 高通 120 Hz（小喇叭放不出低频，省得浪费动态范围），单声道 16 kHz
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(start), "-to", str(end), "-i", path,
         "-af", "highpass=f=120", "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
        check=True, capture_output=True).stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def normalize(samples):
    rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
    peak = max(abs(s) for s in samples)
    gain = min(10 ** (TARGET_RMS_DB / 20) * 32767 / rms, 10 ** (PEAK_LIMIT_DB / 20) * 32767 / peak)
    out = [s * gain for s in samples]
    fade = RATE * FADE_MS // 1000
    for i in range(fade):
        out[i] *= i / fade
        out[-1 - i] *= i / fade
    return [max(-32768, min(32767, int(round(s)))) for s in out], gain


def adpcm_encode(samples):
    pred, idx, nibbles, decoded = 0, 0, [], []
    for s in samples:
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 1; delta += step
        pred = pred - delta if code & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        nibbles.append(code)
        decoded.append(pred)
    if len(nibbles) % 2:
        nibbles.append(0)
    data = bytes(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, len(nibbles), 2))
    return data, decoded


def main():
    os.makedirs(os.path.join(HERE, "preview"), exist_ok=True)
    out = ["/* 由 convert_sounds.py 生成，勿手改。16 kHz 单声道 IMA-ADPCM，低 nibble 在前，"
           "初始 predictor=0 index=0。 */", "#include \"build_sounds.h\"", ""]
    for name, src, start, end in CLIPS:
        samples, gain = normalize(load(os.path.join(HERE, src), start, end))
        data, decoded = adpcm_encode(samples)
        with wave.open(os.path.join(HERE, "preview", name + ".wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(struct.pack("<%dh" % len(decoded), *decoded))
        print("%s: %.2fs, %d samples, %d bytes ADPCM, gain %.2f" %
              (name, len(samples) / RATE, len(samples), len(data), gain))
        out.append("const uint32_t %s_samples = %dU;" % (name, len(samples)))
        out.append("const uint8_t %s_adpcm[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 16):
            out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
        out.append("};")
        out.append("")
    with open(os.path.join(HERE, "build_sounds.c"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
