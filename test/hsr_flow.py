r"""Dense backward motion vectors for vdlssg, computed with OpenCV DIS.

The plugin expects one field per source frame, laid out on that frame's grid, pointing at the
previous frame, in pixels: plane 0 = x, plane 1 = y, plane 2 unused.

VapourSynth frames handed to ModifyFrame are read-only from Python, so the field travels through
16-bit PNGs: code = round((px / kFlowScale + 0.5) * 65535), decoded with std.Expr `x 256 * 128 -`
(kFlowScale = 256, i.e. +-128 px, which covers any 2-frame step this content has). Flow runs at half
resolution -- DIS is slow at 4K and a 2x2 average is plenty for a motion field -- then is scaled back
to full-resolution pixels.
"""

import os

import cv2
import numpy as np
import vapoursynth as vs

K_FLOW_SCALE = 512.0          # pixel value range of the 16-bit carrier: +-256 px
MV_DIR = os.path.join(r"E:\code\dlssg-vfi", "work", "hsr_mv")


def _gray(frame):
    """RGBS frame -> 8-bit luma at half resolution (DIS works on uint8 and is far cheaper there)."""
    rgb = np.stack([np.asarray(frame[p], dtype=np.float32) for p in range(3)], axis=-1)
    luma = rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722
    small = cv2.resize(luma, (luma.shape[1] // 2, luma.shape[0] // 2), interpolation=cv2.INTER_AREA)
    return np.ascontiguousarray((np.clip(small, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))


def _encode(flow):
    code = np.clip(flow / K_FLOW_SCALE + 0.5, 0.0, 1.0) * 65535.0
    return code.astype(np.uint16)


def backward_flows(clip, tag):
    """flows[k][y, x] = where pixel (x, y) of frame k came from in frame k-1, in frame pixels.

    The field is written as 16-bit PNG pairs under work/hsr_mv/<tag>/{u,v}; the files are reused when
    the frame count and size match, so repeated runs on the same segment skip the DIS pass.
    """
    out_u = os.path.join(MV_DIR, tag, "u")
    out_v = os.path.join(MV_DIR, tag, "v")
    paths_u = [os.path.join(out_u, "%04d.png" % (k + 1)) for k in range(clip.num_frames)]
    paths_v = [os.path.join(out_v, "%04d.png" % (k + 1)) for k in range(clip.num_frames)]
    if all(os.path.exists(p) for p in paths_u + paths_v):
        print("  reusing motion vectors in %s" % os.path.join(MV_DIR, tag))
        return paths_u, paths_v

    os.makedirs(out_u, exist_ok=True)
    os.makedirs(out_v, exist_ok=True)
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    dis.setUseSpatialPropagation(True)
    prev = None
    for n in range(clip.num_frames):
        cur = _gray(clip.get_frame(n))
        if prev is None:
            flow = np.zeros((clip.height, clip.width, 2), dtype=np.float32)
        else:
            # DIS(first, second) gives, for each pixel of `first`, the offset of its match in
            # `second`; asking cur -> prev is exactly the backward field the runtime wants.
            flow = dis.calc(prev, cur, None)
            flow = cv2.resize(flow, (clip.width, clip.height),
                              interpolation=cv2.INTER_LINEAR) * 2.0
        enc = _encode(flow)
        cv2.imwrite(paths_u[n], enc[..., 0])
        cv2.imwrite(paths_v[n], enc[..., 1])
        prev = cur
        print("  flow %d/%d" % (n + 1, clip.num_frames), end="\r", flush=True)
    print()
    return paths_u, paths_v


def mv_clip(core, clip, paths_u, paths_v):
    """Two 16-bit fields -> one RGBS clip the plugin accepts as `mv`."""
    zero = core.std.BlankClip(width=clip.width, height=clip.height, format=vs.GRAYS,
                              length=clip.num_frames, fpsnum=clip.fps_num, fpsden=clip.fps_den,
                              color=[0.0])
    fields = []
    for paths in (paths_u, paths_v):
        field = core.imwri.Read(paths, firstnum=1)
        field = core.resize.Point(field, format=vs.GRAYS)
        fields.append(core.std.Expr([field], "x %g * %g -" % (K_FLOW_SCALE, K_FLOW_SCALE / 2)))
    mv = core.std.ShufflePlanes(fields + [zero], planes=[0, 0, 0], colorfamily=vs.RGB)
    return core.std.AssumeFPS(mv, fpsnum=clip.fps_num, fpsden=clip.fps_den)
