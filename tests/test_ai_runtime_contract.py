from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
AI_SOURCE = ROOT / "X-CUBE-AI" / "App" / "app_x-cube-ai.c"
CAMERA_SOURCE = ROOT / "Core" / "User" / "Src" / "app_camera_ai.c"
UVOPTX = ROOT / "MDK-ARM" / "shiyan002.uvoptx"
MAIN_SOURCE = ROOT / "Core" / "Src" / "main.c"
IOC_SOURCE = ROOT / "shiyan002.ioc"
COMMENT_PATTERN = re.compile(r"/\*.*?\*/|//.*?$", re.DOTALL | re.MULTILINE)


def source(path: Path) -> str:
    return COMMENT_PATTERN.sub("", path.read_text(encoding="utf-8", errors="replace"))


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text)
    assert match, f"Missing function: {name}"
    start = match.end()
    depth = 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
    raise AssertionError(f"Unterminated function: {name}")


def test_init_executes_real_decoder_selftest() -> None:
    text = source(AI_SOURCE)
    body = function_body(text, "MX_X_CUBE_AI_Init")
    assert "NanoV4_SelfTest" in body
    assert "g_ai_selftest_status" in text


def test_ai_debug_state_is_flushed_for_jlink_visibility() -> None:
    text = source(AI_SOURCE)
    assert "AI_DebugFlush" in function_body(text, "ai_log_err")
    assert "AI_DebugFlush" in function_body(text, "AI_ProcessPendingRequest")
    assert "AI_DebugFlush" in function_body(text, "MX_X_CUBE_AI_Init")


def test_keil_jlink_configuration_uses_swd() -> None:
    text = UVOPTX.read_text(encoding="utf-8", errors="replace")
    assert "<Key>JL2CM3</Key>" in text
    assert "ARM CoreSight SW-DP" in text


def test_pllq_matches_the_verified_board_clock() -> None:
    main = source(MAIN_SOURCE)
    ioc = IOC_SOURCE.read_text(encoding="utf-8", errors="replace")
    assert "RCC_OscInitStruct.PLL.PLLQ = 20;" in main
    assert "RCC_OscInitStruct.HSEState = RCC_HSE_ON;" in main
    assert "RCC.DIVQ1=20" in ioc
    assert "RCC.DIVQ1Freq_Value=48000000" in ioc
    assert "PH0-OSC_IN\\ (PH0).Mode=HSE-External-Oscillator" in ioc
    assert "PH1-OSC_OUT\\ (PH1).Mode=HSE-External-Oscillator" in ioc


def test_ai_errors_return_instead_of_deadlocking() -> None:
    text = source(AI_SOURCE)
    body = function_body(text, "ai_log_err")
    assert "while (1)" not in body
    assert "g_ai_error_type" in body
    assert "g_ai_error_code" in body


def test_pending_request_reports_the_real_processing_result() -> None:
    text = source(AI_SOURCE)
    pending = function_body(text, "AI_ProcessPendingRequest")
    process = function_body(text, "MX_X_CUBE_AI_Process")
    assert "s_ai_last_result" in pending
    assert "s_ai_last_result" in process
    assert "s_ai_process_requested = 0U" in process


def test_frame_counter_advances_only_after_successful_ai_run() -> None:
    text = source(CAMERA_SOURCE)
    body = function_body(text, "App_RunFrame")
    result_assignment = re.search(r"\bai_result\s*=\s*AI_ProcessPendingRequest\s*\(\s*\)", body)
    success_guard = re.search(r"if\s*\(\s*ai_result\s*>\s*0\s*\)", body)
    assert result_assignment
    assert success_guard
    assert body.find("g_dbg_ai_count++") > success_guard.start()


def test_overlay_reports_ai_ok_and_uses_runtime_thresholds() -> None:
    text = source(AI_SOURCE)
    post = function_body(text, "PostProcessNanoV4")
    assert re.search(r"NanoV4_Decode\s*\([^;]*0\.35f\s*,\s*0\.40f\s*\)", post)
