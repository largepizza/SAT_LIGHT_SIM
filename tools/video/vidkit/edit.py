"""ffmpeg side: normalise clips, burn captions, and join a timeline with crossfades.

Every segment is first normalised to the project's size / frame rate / pixel format (letterboxed, not
stretched), so the final join is a plain filter graph over identical inputs.
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from . import env, theme


def run(cmd: list[str]):
    print("  $", " ".join(str(c) for c in cmd[:6]), "..." if len(cmd) > 6 else "")
    subprocess.run([str(c) for c in cmd], check=True)


def probe(path) -> dict:
    out = subprocess.run([env.ffprobe(), "-v", "error", "-show_entries",
                          "stream=codec_type,width,height,r_frame_rate:format=duration", "-of", "json", str(path)],
                         check=True, capture_output=True, text=True).stdout
    d = json.loads(out)
    v = next((s for s in d.get("streams", []) if s.get("codec_type") == "video"), {})
    return {"duration": float(d.get("format", {}).get("duration", 0.0)), "width": v.get("width"),
            "height": v.get("height"), "has_audio": any(s.get("codec_type") == "audio" for s in d.get("streams", []))}


def caption_png(text: str, width: int, height: int, path: Path, size: int | None = None, mark: str | None = None):
    """A transparent full-frame PNG with a caption on a soft dark band near the bottom."""
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    size = size or max(12, int(height * 0.034))

    def load(sz):
        for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
            try:
                return ImageFont.truetype(name, sz)
            except OSError:
                continue
        return ImageFont.load_default()

    while True:  # shrink until the caption fits 88% of the width
        font = load(size)
        box = d.textbbox((0, 0), text, font=font)
        tw, th = box[2] - box[0], box[3] - box[1]
        if tw <= 0.88 * width or size <= 10:
            break
        size -= 1
    x, y = (width - tw) // 2, height - th - int(height * 0.075)
    pad = int(size * 0.55)
    d.rounded_rectangle((x - pad, y - pad * 0.7, x + tw + pad, y + th + pad), radius=pad // 2, fill=(11, 13, 18, 170))
    d.text((x - box[0], y - box[1]), text, font=font, fill=(232, 230, 225, 255))
    if mark is not None:  # a ring at the frame centre (a tracked object), labelled above it
        r = int(height * 0.045)
        cx, cy = width // 2, height // 2
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(224, 69, 58, 230), width=max(2, height // 400))
        if mark:
            mb = d.textbbox((0, 0), mark, font=font)
            d.text((cx - (mb[2] - mb[0]) // 2, cy - r - (mb[3] - mb[1]) - int(height * 0.015)), mark, font=font,
                   fill=(224, 69, 58, 255))
    img.save(path)


def normalize(src, dst, width: int, height: int, fps: int, start: float | None = None, end: float | None = None,
              caption: str | None = None, speed: float = 1.0, fade: float = 0.0, mark: str | None = None):
    """Re-encode `src` to the project format, optionally trimmed (seconds in the source), sped up and
    captioned. Audio is dropped: the soundtrack is added once at assembly."""
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [env.ffmpeg(), "-y", "-loglevel", "error"]
    if start is not None:
        cmd += ["-ss", f"{start:.3f}"]
    if end is not None:
        cmd += ["-to", f"{end:.3f}"]
    cmd += ["-i", str(src)]
    vf = (f"setpts=PTS/{speed},fps={fps},scale={width}:{height}:force_original_aspect_ratio=decrease:flags=lanczos,"
          f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:color={theme.BG.lstrip('#')},setsar=1,format=yuv420p")
    if caption or mark is not None:
        png = dst.with_suffix(".caption.png")
        caption_png(caption or "", width, height, png, mark=mark)
        dur = probe(src)["duration"] if end is None else end - (start or 0.0)
        dur /= speed
        a, b = 0.4, max(0.5, dur - 0.6)
        cmd += ["-loop", "1", "-i", str(png)]
        fc = (f"[0:v]{vf}[v];[1:v]format=rgba,fade=in:st={a}:d=0.4:alpha=1,fade=out:st={b}:d=0.4:alpha=1[c];"
              f"[v][c]overlay=0:0:shortest=1,format=yuv420p[o]")
        cmd += ["-filter_complex", fc, "-map", "[o]"]
    else:
        cmd += ["-vf", vf]
    cmd += ["-an", "-c:v", "libx264", "-preset", "medium", "-crf", "17", "-pix_fmt", "yuv420p", str(dst)]
    run(cmd)


def join(segments: list[Path], dst, xfade: float = 0.5, music: str | None = None, music_gain_db: float = -6.0,
         fade_out: float = 1.5):
    """Concatenate normalised segments with a crossfade between each pair; lay `music` under the whole."""
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    durs = [probe(s)["duration"] for s in segments]
    cmd = [env.ffmpeg(), "-y", "-loglevel", "error"]
    for s in segments:
        cmd += ["-i", str(s)]
    parts, last, t = [], "[0:v]", durs[0]
    for i in range(1, len(segments)):
        x = min(xfade, durs[i - 1] / 2, durs[i] / 2) if xfade > 0 else 0
        out = f"[x{i}]"
        if x > 0:
            parts.append(f"{last}[{i}:v]xfade=transition=fade:duration={x:.3f}:offset={t - x:.3f}{out}")
            t += durs[i] - x
        else:
            parts.append(f"{last}[{i}:v]concat=n=2:v=1:a=0{out}")
            t += durs[i]
        last = out
    total = t
    vchain = (";".join(parts) + ";" if parts else "") + f"{last}fade=out:st={max(0, total - fade_out):.3f}:d={fade_out}[vo]"
    if music:
        cmd += ["-i", music]
        mi = len(segments)
        vchain += (f";[{mi}:a]atrim=0:{total:.3f},volume={music_gain_db}dB,"
                   f"afade=in:st=0:d=1.5,afade=out:st={max(0, total - 3):.3f}:d=3[ao]")
        cmd += ["-filter_complex", vchain, "-map", "[vo]", "-map", "[ao]", "-c:a", "aac", "-b:a", "192k"]
    else:
        cmd += ["-filter_complex", vchain, "-map", "[vo]"]
    cmd += ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
            str(dst)]
    run(cmd)
    return total
