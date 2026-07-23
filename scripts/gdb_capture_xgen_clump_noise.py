"""Capture one Autodesk Classic ClumpingFX noisy guide under GDB.

Load this script with ``gdb -x`` while running an Autodesk calibration tool.
Set NANOXGEN_XGEN_GUIDE_INDEX to the vector index to capture.  The script
stops immediately after the first matching serial or parallel
computeNoiseAxis call and prints both the input and output vectors as doubles.

This is deliberately a calibration-only helper.  NanoXGen runtime code does
not load or call Autodesk private symbols.
"""

import os
import struct

import gdb


_guide_index = int(os.environ["NANOXGEN_XGEN_GUIDE_INDEX"], 0)
_capture_frames = os.environ.get(
    "NANOXGEN_XGEN_CAPTURE_FRAMES", "0") not in ("", "0", "false", "False")
_capture_kind = os.environ.get("NANOXGEN_XGEN_CAPTURE_KIND", "either")
if _capture_kind not in ("either", "serial", "parallel"):
    raise ValueError(
        "NANOXGEN_XGEN_CAPTURE_KIND must be either, serial, or parallel")
_expected_root = None
if "NANOXGEN_XGEN_EXPECT_ROOT" in os.environ:
    _expected_root = tuple(
        float(value)
        for value in os.environ["NANOXGEN_XGEN_EXPECT_ROOT"].split(","))
    if len(_expected_root) != 3:
        raise ValueError("NANOXGEN_XGEN_EXPECT_ROOT must contain x,y,z")
_captured = False
_capture_thread = None


def _register_double(name):
    return float(gdb.parse_and_eval("${}.v2_double[0]".format(name)))


class _NoiseFrameBreakpoint(gdb.Breakpoint):
    def __init__(self, address, thread):
        super().__init__("*{:#x}".format(address), internal=True)
        self.silent = True
        self._thread = thread

    def stop(self):
        if gdb.selected_thread() != self._thread:
            return False
        stack = int(gdb.parse_and_eval("$rsp"))
        memory = gdb.selected_inferior().read_memory(stack, 0x70)
        raw = bytes(memory)
        first = (
            struct.unpack_from("<d", raw, 0x00)[0],
            struct.unpack_from("<d", raw, 0x18)[0],
            struct.unpack_from("<d", raw, 0x10)[0])
        second = (
            _register_double("xmm3"),
            _register_double("xmm13"),
            _register_double("xmm14"))
        sample = (
            _register_double("xmm0"),
            _register_double("xmm1"),
            _register_double("xmm2"))
        magnitude = struct.unpack_from("<d", raw, 0x68)[0]
        cv = int(gdb.parse_and_eval("$ebp"))
        gdb.write(
            "noise_frame {} first {:.17g} {:.17g} {:.17g} "
            "second {:.17g} {:.17g} {:.17g} "
            "sample {:.17g} {:.17g} {:.17g} magnitude {:.17g}\n".format(
                cv, *first, *second, *sample, magnitude))
        return False


def _read_vector(vector_address):
    inferior = gdb.selected_inferior()
    header = bytes(inferior.read_memory(vector_address, 24))
    begin, end, capacity = struct.unpack("<QQQ", header)
    if end < begin or capacity < end or (end - begin) % 24 != 0:
        raise gdb.GdbError(
            "computeNoiseAxis argument is not a valid vector<SgVec3d>")
    count = (end - begin) // 24
    if count > 1_000_000:
        raise gdb.GdbError("computeNoiseAxis vector is unreasonably large")
    values = struct.unpack(
        "<" + "d" * (count * 3),
        bytes(inferior.read_memory(begin, end - begin)))
    return [values[index:index + 3] for index in range(0, len(values), 3)]


def _print_vector(label, values):
    gdb.write("{} {}".format(label, len(values)))
    for point in values:
        gdb.write(" {:.17g} {:.17g} {:.17g}".format(*point))
    gdb.write("\n")


class _NoiseFinishBreakpoint(gdb.FinishBreakpoint):
    def __init__(self, frame, vector_address, input_values, kind):
        super().__init__(frame, internal=True)
        self.silent = True
        self._vector_address = vector_address
        self._input_values = input_values
        self._kind = kind

    def stop(self):
        global _captured
        output_values = _read_vector(self._vector_address)
        gdb.write(
            "nanoxgen_clump_noise_capture {} guide {}\n".format(
                self._kind, _guide_index))
        _print_vector("input_axis", self._input_values)
        _print_vector("output_axis", output_values)
        _captured = True
        return True


class _NoiseBreakpoint(gdb.Breakpoint):
    def __init__(self, function, kind):
        super().__init__(function, internal=True)
        self.silent = True
        self._kind = kind

    def stop(self):
        if _captured:
            return False
        if _capture_kind != "either" and self._kind != _capture_kind:
            return False
        if int(gdb.parse_and_eval("$esi")) != _guide_index:
            return False
        vector_address = int(gdb.parse_and_eval("$rdx"))
        input_values = _read_vector(vector_address)
        if (_expected_root is not None and
                (not input_values or any(
                    abs(actual - expected) > 1.0e-3
                    for actual, expected in zip(
                        input_values[0], _expected_root)))):
            return False
        self.enabled = False
        if _capture_frames:
            # First xgutil::noise call in this Maya 2027.1 function. At this
            # point the two transported frame vectors and scale are live.
            _NoiseFrameBreakpoint(
                int(gdb.parse_and_eval("$pc")) + 0x51a,
                gdb.selected_thread())
        _NoiseFinishBreakpoint(
            gdb.newest_frame(), vector_address, input_values, self._kind)
        return False


gdb.execute("set breakpoint pending on")
_NoiseBreakpoint(
    "XgClumpingFXModule::computeNoiseAxis(double, unsigned int, "
    "std::vector<SgVec3T<double>, std::allocator<SgVec3T<double> > >&)",
    "serial")
_NoiseBreakpoint(
    "XgClumpingFXModule::computeNoiseAxisParallel(double, unsigned int, "
    "std::vector<SgVec3T<double>, std::allocator<SgVec3T<double> > >&, "
    "XgSplinePrimitiveContext&)",
    "parallel")
