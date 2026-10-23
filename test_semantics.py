"""Content-level export regression. Uses Python standard library and local FFmpeg."""
import argparse
import array
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build" / "semantic-tests"
parser = argparse.ArgumentParser()
parser.add_argument("--tools-directory", type=Path)
parser.add_argument("--verify-only", action="store_true")
args = parser.parse_args()
ffmpeg = str(args.tools_directory / "ffmpeg.exe") if args.tools_directory else shutil.which("ffmpeg.exe")
ffprobe = str(args.tools_directory / "ffprobe.exe") if args.tools_directory else shutil.which("ffprobe.exe")
if not ffmpeg or not ffprobe:
    parser.error("Supply --tools-directory containing ffmpeg.exe and ffprobe.exe")
OUT.mkdir(parents=True, exist_ok=True)

def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, **kwargs).stdout

if not args.verify_only:
    pictures = []
    for second in range(6):
        intensity = 100 + second * 25
        frame = bytearray()
        for y in range(180):
            for x in range(320):
                rgb = (intensity, 0, 0) if x < 160 and y < 90 else (
                    (0, 0, intensity) if y < 90 else ((0, intensity, 0) if x < 160 else (intensity, intensity, 0)))
                frame.extend(rgb)
        pictures.append(bytes(frame) * 30)
    tone = lambda hz: f"aevalsrc=0.12*sin(2*PI*{hz}*t)*(1+lt(mod(t\\,1)\\,0.04)):s=48000:d=6"
    run([ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pixel_format", "rgb24",
         "-video_size", "320x180", "-framerate", "30", "-i", "pipe:0",
         "-f", "lavfi", "-i", tone(440), "-f", "lavfi", "-i", tone(880),
         "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "libx264",
         "-preset", "ultrafast", "-crf", "0", "-pix_fmt", "yuv420p", "-c:a", "aac",
         str(OUT / "source.mp4")], input=b"".join(pictures))
    import os
    env = os.environ.copy()
    if args.tools_directory:
        env["PATH"] = str(args.tools_directory) + os.pathsep + env["PATH"]
    result = subprocess.run([str(ROOT / "build" / "semantic_render_probe.exe"),
                             str(OUT / "source.mp4"), str(OUT)], env=env, check=True)

failures = []
checks = []
def check(ok, label, details=""):
    checks.append({"test": label, "passed": bool(ok), "details": details})
    print(("PASS " if ok else "FAIL ") + label + (": " + details if details else ""))
    if not ok:
        failures.append(label)

def metadata(name):
    return json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(OUT / (name + ".mp4"))]))

cache = {}
def frames(name):
    if name not in cache:
        meta = metadata(name)
        stream = next(s for s in meta["streams"] if s["codec_type"] == "video")
        w, h = stream["width"], stream["height"]
        data = run([ffmpeg, "-v", "error", "-i", str(OUT / (name + ".mp4")), "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"])
        cache[name] = (w, h, data)
    return cache[name]

def pixel(name, time, x=.25, y=.25):
    w, h, data = frames(name)
    frame = min(int(time*30), len(data)//(w*h*3)-1)
    at = (frame*w*h + int(y*h)*w + int(x*w))*3
    return tuple(data[at:at+3])

audio_cache = {}
def audio(name, start, length=.2):
    if name not in audio_cache:
        data = run([ffmpeg, "-v", "error", "-i", str(OUT / (name + ".mp4")),
                    "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "pipe:1"])
        samples = array.array("f"); samples.frombytes(data)
        audio_cache[name] = samples
    return audio_cache[name][int(start*48000):int((start+length)*48000)]

def power(samples, hz):
    # Goertzel avoids external dependencies. A Hann window reduces leakage.
    if not samples: return 0
    coeff = 2*math.cos(2*math.pi*hz/48000)
    a = b = 0
    for i, sample in enumerate(samples):
        value = sample*(.5-.5*math.cos(2*math.pi*i/(len(samples)-1)))
        a, b = value+coeff*a-b, a
    return max(0, a*a+b*b-coeff*a*b)**.5 / len(samples)

for name, expected in [("baseline",2.5),("local_opacity",2.5),("local_speed",1.75),
                       ("combined_speed",1.75/1.5),("local_tape_speed",1.75),
                       ("rotation",2.5),("square_padding",2.5)]:
    meta=metadata(name)
    duration=float(meta["format"]["duration"])
    check(abs(duration-expected)<.06, name+" duration", f"{duration:.4f}s expected {expected:.4f}s")
    av=[float(s.get("duration",duration)) for s in meta["streams"] if s["codec_type"] in ("video","audio")]
    check(max(av)-min(av)<.06, name+" audio/video duration agreement", str(av))

check(abs(pixel("baseline",.2)[0]-100)<8, "cut starts at source 0.5s", str(pixel("baseline",.2)))
check(abs(pixel("baseline",.7)[0]-125)<8, "first kept clip contains correct second")
check(abs(pixel("baseline",1.0)[0]-175)<8, "cut joins to source 3s without deleted frames", str(pixel("baseline",1.0)))
check(abs(pixel("baseline",2.2)[0]-200)<8, "last kept clip contains correct source second")
check(max(pixel("local_opacity",1.3))<6 and pixel("local_opacity",.2)[0]>90,
      "local opacity affects only its clip")
check(pixel("local_brightness",1.3)[1]>55 and pixel("local_brightness",.2)[1]<8,
      "local brightness affects only its clip")
check(power(audio("local_mute",.4),440)>.01 and power(audio("local_mute",1.2),440)<.001,
      "per-cut mute removes correct audio track")
check(power(audio("local_mute",1.2),880)>.01, "other track survives local mute")
check(power(audio("track_two",.3),440)<.001 and power(audio("track_two",.3),880)>.01,
      "included audio stream matches selected track")
check(power(audio("local_speed",1.15),440)>.008, "local speed preserves pitch when enabled")
check(power(audio("local_tape_speed",1.15),880)>.008 and power(audio("local_tape_speed",1.15),440)<.002,
      "local speed changes pitch when pitch preservation is off")
check(power(audio("combined_pitch",.3),220)>.008 and power(audio("combined_pitch",1.3),440)>.008,
      "global and clip pitch combine as semitone offsets")
check(max(pixel("zoom_out",.2,.1,.1))<6 and pixel("zoom_out",.2,.4,.4)[0]>90,
      "zoom out adds centered black padding")
check(max(pixel("zoom_pan",.2,.1,.25))>60, "zoom then pan recovers pixels instead of adding false black border")
check(max(pixel("local_crop",1.3,.1,.25))<6 and pixel("local_crop",1.3,.5,.25)[0]>150,
      "local nonuniform crop preserves aspect and adds padding")
baseline = pixel("baseline",.2)[0]
faded = pixel("source_fades",.2)[0]
check(.2<faded/baseline<.5, "fades use source time across cuts", f"ratio {faded/baseline:.3f}")
smooth=pixel("smooth_fades",.2)[0]/baseline
check(.15<smooth<.35 and smooth<faded/baseline,"smooth video fade follows cosine curve",f"ratio {smooth:.3f}")
check(max(pixel("small_zoom_pan",.2,.9,.5))<6,"zoom out with large pan clears whole canvas")
check(not any(s["codec_type"]=="audio" for s in metadata("no_audio")["streams"]),"video-only export has no unwanted audio stream")
ratio=power(audio("normalization",.3),440)/power(audio("baseline",.3),440)
check(abs(ratio-10**(6/20))<.08,"export normalization uses expected bounded gain",f"ratio {ratio:.3f}")
ratio=power(audio("local_gain",1.3),440)/power(audio("baseline",1.3),440)
check(abs(ratio-.25)<.025,"local volume affects only intended track and clip",f"ratio {ratio:.3f}")
def white_count(name,time):
    w,h,data=frames(name); at=int(time*30)*w*h*3
    frame=data[at:at+w*h*3]
    return sum(min(frame[i:i+3])>220 for i in range(0,len(frame),3))
check(white_count("caption_timing",1.3)>30 and white_count("caption_timing",.8)<5 and white_count("caption_timing",1.9)<5,
      "caption timing follows source clips after cutting")
check(frames("rotation")[:2]==(180,320), "rotation retains expected output canvas")
check(frames("square_padding")[:2]==(320,320) and max(pixel("square_padding",.2,.5,.05))<6,
      "explicit output size fits picture with black padding")
# The boosted audio pulse at source 3s must be at the picture cut at output 1s.
samples=audio("baseline",.9,.3)
bins=[sum(v*v for v in samples[i:i+480])/480 for i in range(0,len(samples)-480,480)]
peak=.9+bins.index(max(bins))*.01
check(abs(peak-1.02)<.05, "audio marker stays synchronized at cut", f"peak {peak:.3f}s")
check(pixel("many_fast_cuts",.9)[0]>168,
      "many short speed-adjusted clips do not accumulate video timing drift",str(pixel("many_fast_cuts",.9)))
for name,time in [("baseline",.2),("local_opacity",1.3),("local_brightness",1.3),
                  ("local_crop",1.3),("zoom_out",.2),("zoom_pan",.2),("rotation",.2),("small_zoom_pan",.2)]:
    png=OUT/("preview_"+name+".png")
    if not png.exists():
        continue
    w,h,_=frames(name)
    data=run([ffmpeg,"-v","error","-i",str(png),"-vf",f"scale={w}:{h}","-pix_fmt","rgb24","-f","rawvideo","pipe:1"])
    errors=[]
    for x,y in [(.1,.1),(.25,.25),(.4,.4),(.6,.3),(.75,.75),(.9,.9)]:
        at=(int(y*h)*w+int(x*w))*3
        expected=pixel(name,time,x,y)
        errors.extend(abs(a-b) for a,b in zip(data[at:at+3],expected))
    check(max(errors)<16,"native mpv preview matches export: "+name,"max channel difference "+str(max(errors)))
(OUT/"results.json").write_text(json.dumps(checks,indent=2),encoding="utf-8")
print(f"{len(checks)-len(failures)}/{len(checks)} content checks passed")
sys.exit(bool(failures))
