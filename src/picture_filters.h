#pragma once
#include "render.h"
#include <cmath>
#include <sstream>
#include <iomanip>

namespace vp {
inline std::string filterNumber(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

inline std::string shiftPicture(double x, double y) {
    if (std::abs(x) < 1e-6 && std::abs(y) < 1e-6) return {};
    return ",pad=w=3*iw:h=3*ih:x=iw:y=ih:color=black,crop=w=iw/3:h=ih/3:x=iw/3*(1-"
        + filterNumber(x) + "):y=ih/3*(1-" + filterNumber(y) + ")";
}

// Pan is a fraction of the fixed canvas, independent of zoom. Both operations
// happen on the enlarged surface before clipping: cropping first lost pixels
// that panning should have been able to bring back into view.
inline std::string framePicture(double zoom, double x, double y, int width, int height) {
    zoom = std::clamp(zoom, .25, 4.0);
    if (std::abs(zoom - 1) < 1e-6) return shiftPicture(x, y);
    width = std::max(2, width / 2 * 2); height = std::max(2, height / 2 * 2);
    const int scaledW = std::max(2, static_cast<int>(std::lround(width*zoom/2))*2);
    const int scaledH = std::max(2, static_cast<int>(std::lround(height*zoom/2))*2);
    return ",scale=" + std::to_string(scaledW) + ":" + std::to_string(scaledH)
        + ",pad=" + std::to_string(std::max(scaledW, width*3)) + ":"
        + std::to_string(std::max(scaledH, height*3)) + ":(ow-iw)/2:(oh-ih)/2:color=black"
        + ",crop=" + std::to_string(width) + ":" + std::to_string(height)
        + ":(iw-ow)/2-ow*" + filterNumber(x) + ":(ih-oh)/2-oh*" + filterNumber(y);
}

// Shared by mpv's lavfi preview and export. Timestamps are source timestamps.
// Global rotation/zoom/pan follow this chain on the GPU in the preview.
inline std::string pictureFilters(const RenderJob& job, const ClipVideo& clip) {
    std::string chain = "null";
    int canvasW = job.sourceWidth, canvasH = job.sourceHeight;
    if (job.portrait) {
        const double t = std::clamp(.5 - job.panX * .5, 0.0, 1.0);
        chain += ",crop=w='min(iw,ih*9/16)':h=ih:x=(iw-ow)*" + filterNumber(t) + ":y=0";
        canvasW = std::min(canvasW, canvasH * 9 / 16);
    }
    double x = 0, y = 0, w = 1, h = 1;
    if (clipWindow(clip, &x, &y, &w, &h)) {
        chain += ",crop=w=iw*" + filterNumber(w) + ":h=ih*" + filterNumber(h)
            + ":x=iw*" + filterNumber(x) + ":y=ih*" + filterNumber(y);
        if (canvasW > 1 && canvasH > 1) {
            canvasW = std::max(2, canvasW / 2 * 2);
            canvasH = std::max(2, canvasH / 2 * 2);
            const std::string size = std::to_string(canvasW) + ":" + std::to_string(canvasH);
            chain += ",scale=" + size + ":force_original_aspect_ratio=decrease:force_divisible_by=2"
                + ",pad=" + size + ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1";
        }
    }
    double sx = 0, sy = 0;
    clipShift(clip, &sx, &sy);
    chain += shiftPicture(sx, sy);
    const bool local = clip.set;
    const int contrast = std::clamp(job.contrast + (local ? clip.contrast : 0), -100, 100);
    const int brightness = std::clamp(job.brightness + (local ? clip.brightness : 0), -100, 100);
    const int saturation = std::clamp(job.saturation + (local ? clip.saturation : 0), -100, 100);
    const int gamma = std::clamp(job.gamma + (local ? clip.gamma : 0), -100, 100);
    const int hue = std::clamp(job.hue + (local ? clip.hue : 0), -100, 100);
    if (contrast || brightness || saturation || gamma)
        chain += ",eq=contrast=" + filterNumber(1 + contrast / 100.0)
            + ":brightness=" + filterNumber(brightness / 100.0)
            + ":saturation=" + filterNumber(1 + saturation / 100.0)
            + ":gamma=" + filterNumber(std::max(.01, 1 + gamma / 100.0));
    if (hue) chain += ",hue=h=" + filterNumber(hue * 1.8);
    const int denoise = std::clamp(job.denoise + (local ? clip.denoise : 0), 0, 100);
    if (denoise) {
        const double d = denoise / 100.0;
        chain += ",hqdn3d=" + filterNumber(2*d) + ":" + filterNumber(1.5*d)
            + ":" + filterNumber(4*d) + ":" + filterNumber(4*d);
    }
    const int sharpen = std::clamp(job.sharpen + (local ? clip.sharpen : 0), 0, 200);
    if (sharpen) chain += ",unsharp=3:3:" + filterNumber(sharpen / 100.0 * .95) + ":3:3:0";
    if (local && clip.opacity < .999f) {
        const std::string opacity = filterNumber(std::clamp<double>(clip.opacity, 0, 1));
        chain += ",colorchannelmixer=rr=" + opacity + ":gg=" + opacity + ":bb=" + opacity;
    }
    if (job.flipH) chain += ",hflip";
    if (job.flipV) chain += ",vflip";
    const RowFades fade = job.fades.empty() ? RowFades{} : job.fades[0];
    if (fade.curve == FadeCurve::Smooth && (fade.in > .001 || fade.out > .001)) {
        std::string gain = "1";
        if (fade.in > .001) gain += "*clip((T-"+filterNumber(job.fadeStart)+")/"+filterNumber(fade.in)+",0,1)";
        if (fade.out > .001) gain += "*clip(("+filterNumber(job.fadeEnd >= 0 ? job.fadeEnd : job.duration)
            +"-T)/"+filterNumber(fade.out)+",0,1)";
        gain = "(0.5-0.5*cos(PI*("+gain+")))";
        chain += ",format=gbrp,geq=r='r(X,Y)*"+gain+"':g='g(X,Y)*"+gain+"':b='b(X,Y)*"+gain+"'";
        return chain;
    }
    if (fade.in > .001) chain += ",fade=t=in:st=" + filterNumber(job.fadeStart) + ":d=" + filterNumber(fade.in);
    if (fade.out > .001) chain += ",fade=t=out:st="
        + filterNumber(std::max(job.fadeStart, (job.fadeEnd >= 0 ? job.fadeEnd : job.duration) - fade.out))
        + ":d=" + filterNumber(fade.out);
    return chain;
}
} // namespace vp
