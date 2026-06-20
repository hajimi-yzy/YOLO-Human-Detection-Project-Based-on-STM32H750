from pathlib import Path
import math
import re


ROOT = Path(__file__).resolve().parents[1]
REPORT_PATH = ROOT / "X-CUBE-AI" / "App" / "network_generate_report.txt"
DECODER_PATH = ROOT / "Core" / "User" / "Src" / "nanov4_postprocess.c"
HEADER_PATH = ROOT / "Core" / "User" / "Inc" / "nanov4_postprocess.h"
COMMENT_PATTERN = re.compile(r"/\*.*?\*/|//.*?$", re.DOTALL | re.MULTILINE)
GRID_WIDTH = 24
GRID_HEIGHT = 24
CHANNELS = 5
OUTPUT_SCALE = 0.071815751
OUTPUT_ZERO = -59
STRIDE = 4.0
MAX_CANDIDATES = 32
MAX_DETECTIONS = 5


def read_required_text(path: Path) -> str:
    assert path.exists(), f"Missing required source: {path}"
    return path.read_text(encoding="utf-8", errors="replace")


def strip_c_comments(source: str) -> str:
    return COMMENT_PATTERN.sub("", source)


def assert_regex(pattern: str, source: str) -> None:
    assert re.search(pattern, source), f"Missing pattern: {pattern}"


def c_function_body(source: str, function_name: str) -> str:
    match = re.search(rf"\b{function_name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    assert match, f"Missing function definition: {function_name}"
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    raise AssertionError(f"Unterminated function definition: {function_name}")


def make_output() -> list[int]:
    output = [-128] * (GRID_WIDTH * GRID_HEIGHT * CHANNELS)
    for cell in range(GRID_WIDTH * GRID_HEIGHT):
        base = cell * CHANNELS
        output[base + 1 : base + 5] = [-45, -45, -45, -45]
    return output


def set_cell(output: list[int], x: int, y: int, quality: int, distances=None) -> None:
    base = (y * GRID_WIDTH + x) * CHANNELS
    output[base] = quality
    if distances is not None:
        output[base + 1 : base + 5] = distances


def dequantize(raw: int) -> float:
    return (raw - OUTPUT_ZERO) * OUTPUT_SCALE


def sigmoid(value: float) -> float:
    return 1.0 / (1.0 + math.exp(-value))


def golden_decode(output, capacity=MAX_DETECTIONS, score_threshold=0.5, nms_iou=0.5):
    if output is None or capacity <= 0:
        return []
    if not 0.0 <= score_threshold <= 1.0 or not 0.0 <= nms_iou <= 1.0:
        return []

    cell_count = GRID_WIDTH * GRID_HEIGHT
    visited = [False] * cell_count
    representatives = []
    for start in range(cell_count):
        if visited[start]:
            continue
        raw = output[start * CHANNELS]
        queue = [start]
        visited[start] = True
        lowest = start
        is_maximum = True
        head = 0
        while head < len(queue):
            cell = queue[head]
            head += 1
            x = cell % GRID_WIDTH
            y = cell // GRID_WIDTH
            for ny in range(max(0, y - 1), min(GRID_HEIGHT - 1, y + 1) + 1):
                for nx in range(max(0, x - 1), min(GRID_WIDTH - 1, x + 1) + 1):
                    neighbor = ny * GRID_WIDTH + nx
                    neighbor_raw = output[neighbor * CHANNELS]
                    if neighbor_raw > raw:
                        is_maximum = False
                    if neighbor_raw == raw and not visited[neighbor]:
                        visited[neighbor] = True
                        queue.append(neighbor)
                        lowest = min(lowest, neighbor)
        if is_maximum:
            representatives.append(lowest)

    candidates = []
    for cell in representatives:
        base = cell * CHANNELS
        score = sigmoid(dequantize(output[base]))
        if score < score_threshold:
            continue
        x = cell % GRID_WIDTH
        y = cell // GRID_WIDTH
        left, top, right, bottom = (
            max(-2.0, min(24.0, dequantize(output[base + channel])))
            for channel in range(1, 5)
        )
        center_x = (x + 0.5) * STRIDE
        center_y = (y + 0.5) * STRIDE
        candidate = (
            max(0.0, min(95.0, center_x - left * STRIDE)),
            max(0.0, min(95.0, center_y - top * STRIDE)),
            max(0.0, min(95.0, center_x + right * STRIDE)),
            max(0.0, min(95.0, center_y + bottom * STRIDE)),
            score,
            cell,
        )
        if candidate[2] - candidate[0] > 1.0 and candidate[3] - candidate[1] > 1.0:
            candidates.append(candidate)

    candidates.sort(key=lambda candidate: (-candidate[4], candidate[5]))
    candidates = candidates[:MAX_CANDIDATES]
    selected = []
    for candidate in candidates:
        if len(selected) >= min(capacity, MAX_DETECTIONS):
            break
        if all(golden_iou(candidate, kept) <= nms_iou for kept in selected):
            selected.append(candidate)
    return [
        (
            int(candidate[0] + 0.5),
            int(candidate[1] + 0.5),
            int(candidate[2] + 0.5),
            int(candidate[3] + 0.5),
            max(0, min(100, int(candidate[4] * 100.0 + 0.5))),
        )
        for candidate in selected
    ]


def golden_iou(a, b) -> float:
    width = min(a[2], b[2]) - max(a[0], b[0])
    height = min(a[3], b[3]) - max(a[1], b[1])
    if width <= 0.0 or height <= 0.0:
        return 0.0
    intersection = width * height
    area_a = (a[2] - a[0]) * (a[3] - a[1])
    area_b = (b[2] - b[0]) * (b[3] - b[1])
    return intersection / (area_a + area_b - intersection)


def test_generated_network_matches_nanov4_contract() -> None:
    report = read_required_text(REPORT_PATH)
    assert "int8(1x96x96x3)" in report
    assert "QLinear(0.003921569,-128,int8)" in report
    assert "int8(1x24x24x5)" in report
    assert "QLinear(0.071815751,-59,int8)" in report


def test_public_api_declares_output_length_and_self_test() -> None:
    header = read_required_text(HEADER_PATH)
    assert_regex(
        r"(?m)^\s*#\s*define\s+NANOV4_OUTPUT_ELEMENTS\s+"
        r"\(24U\s*\*\s*24U\s*\*\s*5U\)\s*$",
        header,
    )
    assert "at least NANOV4_OUTPUT_ELEMENTS readable int8_t elements" in header
    assert_regex(
        r"\bint\s+NanoV4_SelfTest\s*\(\s*int8_t\s*\*\s*workspace\s*,"
        r"\s*uint32_t\s+element_count\s*\)\s*;",
        header,
    )


def test_large_scratch_is_one_aligned_module_workspace() -> None:
    source = strip_c_comments(read_required_text(DECODER_PATH))
    assert_regex(r"(?s)\btypedef\s+struct\s*\{.*?\}\s*NanoV4_Workspace\s*;", source)
    assert_regex(
        r"__align\s*\(\s*32\s*\)\s+static\s+NanoV4_Workspace\s+"
        r"s_nanov4_workspace\s*;",
        source,
    )
    for member in (
        r"NanoV4_Candidate\s+candidates\s*\[\s*NANOV4_MAX_CANDIDATES\s*\]",
        r"uint8_t\s+visited\s*\[\s*NANOV4_GRID_CELLS\s*\]",
        r"uint8_t\s+maxima\s*\[\s*NANOV4_GRID_CELLS\s*\]",
        r"uint16_t\s+queue\s*\[\s*NANOV4_GRID_CELLS\s*\]",
        r"NanoV4_Box\s+selftest_boxes\s*\[\s*NANOV4_MAX_DETECTIONS\s*\]",
    ):
        assert_regex(member, source)

    large_array = (
        r"\[[^\]]*(?:NANOV4_GRID_CELLS|NANOV4_MAX_CANDIDATES)[^\]]*\]"
    )
    for function_name in ("NanoV4_LocalMaximum", "NanoV4_Decode", "NanoV4_SelfTest"):
        assert not re.search(large_array, c_function_body(source, function_name))


def test_self_test_initializes_caller_buffer_and_calls_real_decoder() -> None:
    source = strip_c_comments(read_required_text(DECODER_PATH))
    body = c_function_body(source, "NanoV4_SelfTest")
    assert_regex(r"element_count\s*<\s*NANOV4_OUTPUT_ELEMENTS", body)
    assert_regex(r"\bworkspace\s*\[\s*(?:index|i)\s*\]\s*=", body)
    calls = re.findall(r"\bNanoV4_Decode\s*\(\s*workspace\s*,", body)
    assert len(calls) >= 2
    assert_regex(r"selftest_boxes\s*\[\s*0U?\s*\]\s*\.\s*x1", body)
    assert_regex(r"selftest_boxes\s*\[\s*0U?\s*\]\s*\.\s*score_x100", body)
    assert_regex(r"\bcapacity_count\s*=\s*NanoV4_Decode", body)
    assert_regex(r"\bnms_count\s*=\s*NanoV4_Decode", body)
    for expected_check in (
        r"nms_count\s*!=\s*1U",
        r"selftest_boxes\s*\[\s*0U\s*\]\.x2\s*!=\s*55",
        r"capacity_count\s*!=\s*1U",
        r"nms_count\s*!=\s*2U",
        r"selftest_boxes\s*\[\s*1U\s*\]\.x1\s*!=\s*78",
        r"selftest_boxes\s*\[\s*1U\s*\]\.score_x100\s*!=\s*99U",
    ):
        assert_regex(expected_check, body)


def test_self_test_documents_destruction_and_validates_before_fill() -> None:
    header = read_required_text(HEADER_PATH)
    assert "destructively overwrites workspace[0..NANOV4_OUTPUT_ELEMENTS-1]" in header
    assert "before first inference" in header
    assert "must not use the synthetic tensor after any return" in header

    source = strip_c_comments(read_required_text(DECODER_PATH))
    body = c_function_body(source, "NanoV4_SelfTest")
    validation = re.search(
        r"if\s*\(\s*\(workspace\s*==\s*0\)\s*\|\|\s*"
        r"\(element_count\s*<\s*NANOV4_OUTPUT_ELEMENTS\)\s*\)",
        body,
    )
    first_write = re.search(r"\bworkspace\s*\[[^\]]+\]\s*=", body)
    assert validation, "NanoV4_SelfTest must validate pointer and length together"
    assert first_write, "NanoV4_SelfTest must fill the caller workspace"
    assert validation.end() < first_write.start(), "Validation must precede every fill"


def test_decoder_source_declares_fcos_contract() -> None:
    source = strip_c_comments(read_required_text(DECODER_PATH))
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_STRIDE\s+\(4\.0f\)\s*$", source)
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_OUTPUT_SCALE\s+\(0\.071815751f\)\s*$", source)
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_OUTPUT_ZERO\s+\(-59\)\s*$", source)
    assert_regex(
        r"\(\s*raw\s*-\s*NANOV4_OUTPUT_ZERO\s*\)\s*\*\s*NANOV4_OUTPUT_SCALE",
        source,
    )
    assert_regex(r"\(\(float\)x\s*\+\s*0\.5f\)", source)
    assert_regex(r"\(\(float\)y\s*\+\s*0\.5f\)", source)
    assert_regex(r"(?ms)^\s*(?:static\s+)?[A-Za-z_][\w\s\*]*?\s+NanoV4_Sigmoid\s*\([^)]*\)\s*\{", source)
    assert_regex(r"(?ms)^\s*(?:static\s+)?[A-Za-z_][\w\s\*]*?\s+NanoV4_LocalMaximum\s*\([^)]*\)\s*\{", source)
    assert_regex(r"(?ms)^\s*(?:static\s+)?[A-Za-z_][\w\s\*]*?\s+NanoV4_Nms\s*\([^)]*\)\s*\{", source)
    assert_regex(
        r"(?m)^\s*#\s*define\s+NANOV4_GRID_CELLS\s+"
        r"\(NANOV4_GRID_WIDTH\s*\*\s*NANOV4_GRID_HEIGHT\)\s*$",
        source,
    )
    assert_regex(r"\buint8_t\s+visited\s*\[\s*NANOV4_GRID_CELLS\s*\]", source)
    assert_regex(r"\buint16_t\s+queue\s*\[\s*NANOV4_GRID_CELLS\s*\]", source)
    assert_regex(r"\bwhile\s*\(\s*head\s*<\s*tail\s*\)", source)
    assert_regex(r"\bmaxima\s*\[\s*lowest\s*\]\s*=\s*1U", source)


def test_decoder_reads_24x24x5_output_in_nhwc_order() -> None:
    source = strip_c_comments(read_required_text(DECODER_PATH))
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_GRID_WIDTH\s+\(24U\)\s*$", source)
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_GRID_HEIGHT\s+\(24U\)\s*$", source)
    assert_regex(r"(?m)^\s*#\s*define\s+NANOV4_CHANNELS\s+\(5U\)\s*$", source)
    assert_regex(
        r"(?:y\s*\*\s*NANOV4_GRID_WIDTH\s*\+\s*x|x\s*\+\s*y\s*\*\s*NANOV4_GRID_WIDTH)"
        r"\s*\)\s*\*\s*NANOV4_CHANNELS",
        source,
    )
    for channel, name in enumerate(("quality", "left", "top", "right", "bottom")):
        assert_regex(
            rf"\b{name}\s*=\s*NanoV4_Dequantize\s*\(\s*output\s*\[\s*base\s*\+\s*{channel}U?\s*\]",
            source,
        )


def test_irregular_connected_plateau_chooses_lowest_index() -> None:
    output = make_output()
    for x, y in ((1, 1), (1, 2), (2, 3), (3, 2), (3, 1)):
        set_cell(output, x, y, 10)
    assert golden_decode(output) == [(2, 2, 10, 10, 99)]


def test_two_separate_equal_maxima_both_survive() -> None:
    output = make_output()
    set_cell(output, 1, 1, 10)
    set_cell(output, 5, 1, 10)
    assert golden_decode(output) == [(2, 2, 10, 10, 99), (18, 2, 26, 10, 99)]


def test_candidates_are_sorted_by_descending_score_then_cell_index() -> None:
    output = make_output()
    set_cell(output, 9, 1, -50)
    set_cell(output, 5, 1, -40)
    set_cell(output, 1, 1, -40)
    assert golden_decode(output) == [
        (2, 2, 10, 10, 80),
        (18, 2, 26, 10, 80),
        (34, 2, 42, 10, 66),
    ]


def test_box_coordinates_are_clipped_to_image() -> None:
    output = make_output()
    set_cell(output, 0, 0, 10, [127, 127, 127, 127])
    set_cell(output, 23, 23, 9, [127, 127, 127, 127])
    assert golden_decode(output, nms_iou=1.0) == [
        (0, 0, 55, 55, 99),
        (41, 41, 95, 95, 99),
    ]


def test_invalid_boxes_are_rejected() -> None:
    output = make_output()
    set_cell(output, 10, 10, 10, [-128, -128, -128, -128])
    assert golden_decode(output) == []


def test_nms_suppresses_overlapping_lower_priority_box() -> None:
    output = make_output()
    set_cell(output, 5, 5, 10, [0, 0, 0, 0])
    set_cell(output, 7, 5, 9, [0, 0, 0, 0])
    assert len(golden_decode(output, nms_iou=0.5)) == 1


def test_capacity_and_public_maximum_limit_output_count() -> None:
    output = make_output()
    for x, y in ((1, 1), (4, 1), (7, 1), (10, 1), (13, 1), (16, 1), (19, 1)):
        set_cell(output, x, y, 10)
    assert len(golden_decode(output, capacity=3)) == 3
    assert len(golden_decode(output, capacity=99)) == MAX_DETECTIONS


def test_score_is_rounded_to_nearest_percent() -> None:
    output = make_output()
    set_cell(output, 1, 1, -58)
    assert golden_decode(output)[0][4] == 52
