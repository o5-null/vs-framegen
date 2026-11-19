r"""Probe: can CUDA import the FSR bridge's shared Vulkan handles?

The bridge (smv_fsrfg_bridge.dll) creates three textures and a shared fence on a
vkd3d-proton device and hands out the raw Vulkan Win32 export handles.  D3D12
cannot open those, CUDA can.  This script walks the intended host path end to
end without the VapourSynth plugin:

  1. pick the CUDA device that matches the DXGI adapter LUID,
  2. create a bridge context (1920x1080, RGBA16F),
  3. import in/mv/out as external memory and map them as CUDA arrays,
  4. import the shared fence as an external semaphore (tries several types),
  5. feed two synthetic frames, generate one midpoint, read it back and print
     the statistics of the result.

Run:  E:\code\VFI\.venv\Scripts\python.exe test\fsr4_cuda_probe.py [w h fmt]
"""

import ctypes
import os
import sys
from ctypes import (POINTER, Structure, byref, c_char_p, c_double, c_int,
                    c_size_t, c_uint, c_ubyte, c_ulong, c_ulonglong, c_void_p)

import numpy as np

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(PKG, "engine_fsrfg", "smv_fsrfg_bridge.dll")
CUDA = ctypes.WinDLL(r"C:\Windows\System32\nvcuda.dll")


def cfn(name):
    """Look up a CUDA driver entry point, newest version suffix first."""
    for cand in (name + "_v3", name + "_v2", name):
        try:
            return getattr(CUDA, cand)
        except AttributeError:
            continue
    raise RuntimeError("nvcuda.dll has no " + name)


def check(res, what):
    if res == 0:
        return
    err = c_char_p()
    try:
        CUDA.cuGetErrorName.argtypes = [c_int, POINTER(c_char_p)]
        CUDA.cuGetErrorName(c_int(res), byref(err))
        text = err.value.decode() if err.value else "?"
    except Exception:
        text = "?"
    raise RuntimeError("%s failed: %d (%s)" % (what, res, text))


# ---------------------------------------------------------------- CUDA types
class ExtHandle(Structure):
    # CUexternalMemoryHandle / CUexternalSemaphoreHandle: int fd; win32{handle,name}; win32Kmt{handle}
    _fields_ = [("fd", c_int), ("pad", c_uint),
                ("h", c_void_p), ("name", c_void_p), ("kmt", c_void_p)]


class MemDesc(Structure):
    _fields_ = [("type", c_int), ("pad", c_uint), ("handle", ExtHandle),
                ("size", c_ulonglong), ("flags", c_uint), ("reserved", c_uint * 16)]


class SemDesc(Structure):
    _fields_ = [("type", c_int), ("pad", c_uint), ("handle", ExtHandle),
                ("flags", c_uint), ("reserved", c_uint * 16)]


class Array3D(Structure):
    _fields_ = [("Width", c_size_t), ("Height", c_size_t), ("Depth", c_size_t),
                ("Format", c_int), ("NumChannels", c_uint), ("Flags", c_uint)]


class SemParams(Structure):
    _fields_ = [("fence_value", c_ulonglong), ("key", c_ulonglong),
                ("params_reserved", c_uint * 12),
                ("flags", c_uint), ("reserved", c_uint * 16)]


class Memcpy2D(Structure):
    _fields_ = [("srcXInBytes", c_size_t), ("srcY", c_size_t),
                ("srcType", c_int), ("srcPad", c_uint),
                ("srcHost", c_void_p), ("srcDevice", c_ulonglong), ("srcArray", c_void_p),
                ("srcPitch", c_size_t),
                ("dstXInBytes", c_size_t), ("dstY", c_size_t),
                ("dstType", c_int), ("dstPad", c_uint),
                ("dstHost", c_void_p), ("dstDevice", c_ulonglong), ("dstArray", c_void_p),
                ("dstPitch", c_size_t),
                ("WidthInBytes", c_size_t), ("Height", c_size_t)]


class FsrfgShared(Structure):
    _fields_ = [("hin", c_void_p), ("hmv", c_void_p), ("hout", c_void_p), ("hfence", c_void_p),
                ("inBytes", c_ulonglong), ("mvBytes", c_ulonglong), ("outBytes", c_ulonglong),
                ("rowPitch", c_uint), ("mvRowPitch", c_uint)]


CU_AD_FORMAT_UNSIGNED_INT8 = 0x01
CU_AD_FORMAT_HALF = 0x10
CU_MEMORYTYPE_HOST = 1
CU_MEMORYTYPE_ARRAY = 3
CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32 = 1
SEM_TYPES = [
    (8, "TIMELINE_SEMAPHORE_WIN32"),
    (3, "D3D12_FENCE"),
    (1, "OPAQUE_WIN32"),
]


def main():
    w = int(sys.argv[1]) if len(sys.argv) > 1 else 1920
    h = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
    fmt = int(sys.argv[3]) if len(sys.argv) > 3 else 1  # 1 = RGBA16F
    print("bridge: %s" % BRIDGE)

    # ---------------------------------------------------------- CUDA device
    cuInit = cfn("cuInit"); cuInit.argtypes = [c_uint]; cuInit.restype = c_int
    check(cuInit(0), "cuInit")

    count = c_int()
    cuDeviceGetCount = cfn("cuDeviceGetCount"); cuDeviceGetCount.argtypes = [POINTER(c_int)]
    check(cuDeviceGetCount(byref(count)), "cuDeviceGetCount")

    cuDeviceGet = cfn("cuDeviceGet"); cuDeviceGet.argtypes = [POINTER(c_int), c_int]
    cuDeviceGetName = cfn("cuDeviceGetName"); cuDeviceGetName.argtypes = [c_char_p, c_int, c_int]
    cuDeviceGetLuid = cfn("cuDeviceGetLuid")
    cuDeviceGetLuid.argtypes = [POINTER(c_ubyte), POINTER(c_uint), c_int]

    dev = c_int()
    luid_bytes = None
    for i in range(count.value):
        d = c_int()
        check(cuDeviceGet(byref(d), i), "cuDeviceGet")
        name = ctypes.create_string_buffer(256)
        check(cuDeviceGetName(name, 256, d.value), "cuDeviceGetName")
        luid = (c_ubyte * 8)(); node = c_uint()
        res = cuDeviceGetLuid(luid, byref(node), d.value)
        desc = bytes(luid).hex() if res == 0 else "?"
        print("cuda device %d: %s luid %s" % (i, name.value.decode(errors="replace"), desc))
        if res == 0 and luid_bytes is None:
            dev = d
            luid_bytes = bytes(luid)
    if luid_bytes is None:
        raise RuntimeError("no CUDA device with a LUID")

    luid_low = int.from_bytes(luid_bytes[0:4], "little")
    luid_high = int.from_bytes(luid_bytes[4:8], "little", signed=True)
    print("using CUDA device %d, LUID %08x:%08x" % (dev.value, luid_low, luid_high))

    ctx = c_void_p()
    cuDevicePrimaryCtxRetain = cfn("cuDevicePrimaryCtxRetain")
    cuDevicePrimaryCtxRetain.argtypes = [POINTER(c_void_p), c_int]
    check(cuDevicePrimaryCtxRetain(byref(ctx), dev.value), "cuDevicePrimaryCtxRetain")
    cuCtxSetCurrent = cfn("cuCtxSetCurrent"); cuCtxSetCurrent.argtypes = [c_void_p]
    check(cuCtxSetCurrent(ctx), "cuCtxSetCurrent")

    # ---------------------------------------------------------- bridge context
    bridge = ctypes.WinDLL(BRIDGE)
    bridge.fsrfg_create_i.argtypes = [c_int, c_uint, c_uint, c_uint, c_int, c_int,
                                      POINTER(FsrfgShared)]
    bridge.fsrfg_create_i.restype = c_int
    bridge.fsrfg_frame_i.argtypes = [c_int, c_ulonglong, c_ulonglong, c_int, c_double]
    bridge.fsrfg_frame_i.restype = c_int
    bridge.fsrfg_last_error.restype = c_char_p
    bridge.fsrfg_destroy_i.argtypes = [c_int]
    bridge.fsrfg_provider.restype = c_char_p

    sh = FsrfgShared()
    res = bridge.fsrfg_create_i(0, w, h, luid_low, luid_high, fmt, byref(sh))
    print("create -> %d  %s" % (res, bridge.fsrfg_last_error()))
    if res != 0:
        return 1
    print("provider: %s" % bridge.fsrfg_provider())
    print("handles in=%#x mv=%#x out=%#x fence=%#x" % (sh.hin or 0, sh.hmv or 0, sh.hout or 0, sh.hfence or 0))
    print("bytes in=%d mv=%d out=%d rowPitch=%d mvRowPitch=%d"
          % (sh.inBytes, sh.mvBytes, sh.outBytes, sh.rowPitch, sh.mvRowPitch))

    try:
        # what kind of kernel object is the bridge handing out?
        def handle_type_name(h):
            k32 = ctypes.WinDLL("kernel32.dll")
            flags = c_int()
            ok = k32.GetHandleInformation(c_void_p(h), byref(flags))
            buf = ctypes.create_string_buffer(1024)
            ntdll = ctypes.WinDLL("ntdll.dll")
            ret_len = c_uint(0)
            st = ntdll.NtQueryObject(c_void_p(h), c_int(2), buf, c_ulong(1024), byref(ret_len))
            name = "?"
            if st == 0 and ret_len.value >= 16:
                ptr = ctypes.cast(ctypes.byref(buf, 8), POINTER(c_void_p))[0]
                try:
                    name = ctypes.wstring_at(ptr)
                except Exception:
                    name = "?"
            return ok, flags.value, st, name

        for label, h in (("in", sh.hin), ("mv", sh.hmv), ("out", sh.hout), ("fence", sh.hfence)):
            print("handle %-5s %#x -> GetHandleInformation %d flags %#x NtQueryObject %d type %s"
                  % ((label, h or 0) + handle_type_name(h)))

        half = fmt == 1

        cuImportExternalMemory = cfn("cuImportExternalMemory")
        cuImportExternalMemory.argtypes = [POINTER(c_void_p), POINTER(MemDesc)]
        print("sizeof(shared)=%d sizeof(desc)=%d" % (ctypes.sizeof(FsrfgShared), ctypes.sizeof(MemDesc)))

        def try_import(handle, size):
            ext = c_void_p()
            d = MemDesc()
            d.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32
            d.size, d.flags = size, 0
            d.handle.h = handle
            return cuImportExternalMemory(byref(ext), byref(d)), ext

        print("primary context import -> %d" % try_import(sh.hin, sh.inBytes)[0])

        # a duplicated handle with the widest rights, in case the export's
        # GENERIC_READ|0x1 limits what CUDA can map
        k32 = ctypes.WinDLL("kernel32.dll")
        dup = c_void_p()
        k32.DuplicateHandle(k32.GetCurrentProcess(), c_void_p(sh.hin),
                            k32.GetCurrentProcess(), byref(dup), 0, False, 0x2)
        print("duplicated %#x -> %#x, import -> %d"
              % (sh.hin, dup.value or 0, try_import(dup.value or 0, sh.inBytes)[0]))

        # a private context instead of the primary one
        cuCtxCreate = cfn("cuCtxCreate")
        cuCtxCreate.argtypes = [POINTER(c_void_p), c_uint, c_int]
        ctx2 = c_void_p()
        res = cuCtxCreate(byref(ctx2), 0, dev.value)
        print("cuCtxCreate -> %d ctx %#x" % (res, ctx2.value or 0))
        if res == 0:
            cuCtxSetCurrent(ctx2)
            print("private context import -> %d" % try_import(sh.hin, sh.inBytes)[0])
            cuCtxSetCurrent(ctx)

        for label, mtype, size, flags in (
                ("opaque,size,0", 1, sh.inBytes, 0),
                ("opaque,size,dedicated", 1, sh.inBytes, 1),
                ("opaque,0,0", 1, 0, 0),
                ("opaque,0,dedicated", 1, 0, 1),
                ("kmt,size,0", 2, sh.inBytes, 0),
                ("kmt,0,0", 2, 0, 0),
                ("kmt,0,dedicated", 2, 0, 1),
                ("kmt,size,dedicated", 2, sh.inBytes, 1),
                ("opaque,w*h*8", 1, w * h * 8, 0)):
            trial = c_void_p()
            td = MemDesc()
            td.type, td.size, td.flags = mtype, size, flags
            td.handle.h = sh.hin
            print("  trial %-22s -> %d" % (label, cuImportExternalMemory(byref(trial), byref(td))))

        cuExternalMemoryGetMappedMipmappedArray = cfn("cuExternalMemoryGetMappedMipmappedArray")
        cuExternalMemoryGetMappedMipmappedArray.argtypes = [POINTER(c_void_p), c_void_p,
                                                            POINTER(Array3D)]
        cuMipmappedArrayGetLevel = cfn("cuMipmappedArrayGetLevel")
        cuMipmappedArrayGetLevel.argtypes = [POINTER(c_void_p), c_void_p, c_uint]

        half = fmt == 1
        arrays = {}
        for label, handle, size, channels in (
                ("in", sh.hin, sh.inBytes, 4),
                ("mv", sh.hmv, sh.mvBytes, 2),
                ("out", sh.hout, sh.outBytes, 4)):
            ext = c_void_p()
            desc = MemDesc()
            desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32
            desc.handle.h = handle
            desc.size = size
            res = cuImportExternalMemory(byref(ext), byref(desc))
            if res != 0:
                print("IMPORT FAILED %s -> %d" % (label, res))
                break
            mip = c_void_p()
            a3 = Array3D()
            a3.Width, a3.Height, a3.Depth = w, h, 0
            a3.Format = CU_AD_FORMAT_HALF if half else CU_AD_FORMAT_UNSIGNED_INT8
            a3.NumChannels, a3.Flags = channels, 0
            res = cuExternalMemoryGetMappedMipmappedArray(byref(mip), ext, byref(a3))
            if res != 0:
                print("MAP FAILED %s (channels %d) -> %d" % (label, channels, res))
                break
            arr = c_void_p()
            check(cuMipmappedArrayGetLevel(byref(arr), mip, 0), "cuMipmappedArrayGetLevel")
            arrays[label] = arr
            print("imported %s -> array %#x" % (label, arr.value or 0))

        # ------------------------------------------------------ import fence
        cuImportExternalSemaphore = cfn("cuImportExternalSemaphore")
        cuImportExternalSemaphore.argtypes = [POINTER(c_void_p), POINTER(SemDesc)]
        sem = None
        for sem_type, sem_name in SEM_TYPES:
            handle = c_void_p()
            sd = SemDesc()
            sd.type = sem_type
            sd.handle.h = sh.hfence
            res = cuImportExternalSemaphore(byref(handle), byref(sd))
            print("import fence as type %d (%s) -> %d" % (sem_type, sem_name, res))
            if res == 0:
                sem = handle
                print("fence imported as %s" % sem_name)
                break
        if sem is None:
            raise RuntimeError("no CUDA semaphore handle type accepted the fence")
        if len(arrays) != 3:
            raise RuntimeError("texture import failed; fence came through")

        cuSignalExternalSemaphoresAsync = cfn("cuSignalExternalSemaphoresAsync")
        cuSignalExternalSemaphoresAsync.argtypes = [POINTER(c_void_p), POINTER(SemParams),
                                                    c_uint, c_void_p]
        cuWaitExternalSemaphoresAsync = cfn("cuWaitExternalSemaphoresAsync")
        cuWaitExternalSemaphoresAsync.argtypes = [POINTER(c_void_p), POINTER(SemParams),
                                                  c_uint, c_void_p]
        cuStreamCreate = cfn("cuStreamCreate"); cuStreamCreate.argtypes = [POINTER(c_void_p), c_uint]
        cuStreamSynchronize = cfn("cuStreamSynchronize"); cuStreamSynchronize.argtypes = [c_void_p]
        cuMemcpy2DAsync = cfn("cuMemcpy2DAsync"); cuMemcpy2DAsync.argtypes = [POINTER(Memcpy2D), c_void_p]
        cuMemHostAlloc = cfn("cuMemHostAlloc")
        cuMemHostAlloc.argtypes = [POINTER(c_void_p), c_size_t, c_uint]
        cuMemFreeHost = cfn("cuMemFreeHost"); cuMemFreeHost.argtypes = [c_void_p]

        stream = c_void_p()
        check(cuStreamCreate(byref(stream), 0), "cuStreamCreate")

        def signal(value):
            p = SemParams()
            p.fence_value = value
            one = c_void_p(sem.value)
            check(cuSignalExternalSemaphoresAsync(byref(one), byref(p), 1, stream),
                  "cuSignalExternalSemaphoresAsync")

        def wait(value):
            p = SemParams()
            p.fence_value = value
            one = c_void_p(sem.value)
            check(cuWaitExternalSemaphoresAsync(byref(one), byref(p), 1, stream),
                  "cuWaitExternalSemaphoresAsync")

        # ------------------------------------------------------ test frames
        bpp = 8 if half else 4  # RGBA
        host = c_void_p()
        bytes_per_frame = w * bpp
        check(cuMemHostAlloc(byref(host), c_size_t(bytes_per_frame * h), 1), "cuMemHostAlloc")
        out_host = c_void_p()
        check(cuMemHostAlloc(byref(out_host), c_size_t(bytes_per_frame * h), 1), "cuMemHostAlloc")

        # a texture every `period` columns makes the midpoint test visible
        yy, xx = np.mgrid[0:h, 0:w]

        def put_image(dst, phase):
            if half:
                img = np.zeros((h, w, 4), np.float16)
            else:
                img = np.zeros((h, w, 4), np.uint8)
            img[..., 0] = ((xx + phase) % 64 < 32)
            img[..., 1] = yy * (1.0 / h)
            img[..., 2] = 0.25
            img[..., 3] = 1
            ctypes.memmove(dst, img.ctypes.data, bytes_per_frame * h)

        def copy_to(array, src):
            cp = Memcpy2D()
            cp.srcType, cp.srcHost, cp.srcPitch = CU_MEMORYTYPE_HOST, src, c_size_t(w * bpp)
            cp.dstType, cp.dstArray = CU_MEMORYTYPE_ARRAY, array
            cp.WidthInBytes, cp.Height = c_size_t(w * bpp), c_size_t(h)
            check(cuMemcpy2DAsync(byref(cp), stream), "cuMemcpy2DAsync(to array)")

        def copy_from(array, dst):
            cp = Memcpy2D()
            cp.srcType, cp.srcArray = CU_MEMORYTYPE_ARRAY, array
            cp.dstType, cp.dstHost, cp.dstPitch = CU_MEMORYTYPE_HOST, dst, c_size_t(w * bpp)
            cp.WidthInBytes, cp.Height = c_size_t(w * bpp), c_size_t(h)
            check(cuMemcpy2DAsync(byref(cp), stream), "cuMemcpy2DAsync(from array)")

        # motion: zero vectors
        mv_bytes = w * 4
        mv_host = c_void_p()
        check(cuMemHostAlloc(byref(mv_host), c_size_t(mv_bytes * h), 1), "cuMemHostAlloc")
        ctypes.memset(mv_host, 0, mv_bytes * h)

        value = 0

        def feed(frame_phase, reset):
            nonlocal value
            put_image(host, frame_phase)
            copy_to(arrays["in"], host)
            copy_to(arrays["mv"], mv_host)
            value += 1
            signal(value)
            check(cuStreamSynchronize(stream), "cuStreamSynchronize")
            wait_for = value
            value += 1
            res = bridge.fsrfg_frame_i(0, wait_for, value, reset, 16.7)
            if res != 0:
                raise RuntimeError("frame_i -> %d %s" % (res, bridge.fsrfg_last_error()))
            wait(value)
            check(cuStreamSynchronize(stream), "cuStreamSynchronize(fence)")

        feed(0, 1)
        feed(16, 0)
        copy_from(arrays["out"], out_host)
        check(cuStreamSynchronize(stream), "cuStreamSynchronize(readback)")

        raw = ctypes.string_at(out_host, bytes_per_frame * h)
        img = np.frombuffer(raw, np.float16 if half else np.uint8).reshape(h, w, 4)
        data = img.astype(np.float32) / (1.0 if half else 255.0)
        print("out: mean %.4f %.4f %.4f %.4f  min %.3f max %.3f  nonzero %.3f"
              % (data[..., 0].mean(), data[..., 1].mean(), data[..., 2].mean(),
                 data[..., 3].mean(), data.min(), data.max(),
                 float((data[..., 0] > 0.02).mean())))
        # the generated midpoint of a 16 px shift sits between the two frames:
        # expect roughly half the period bright in red
        print("row 540 first 8 red: " + " ".join("%.3f" % v for v in data[540, :8, 0]))
        return 0
    finally:
        bridge.fsrfg_destroy_i(0)
        print("destroyed")


if __name__ == "__main__":
    sys.exit(main())
