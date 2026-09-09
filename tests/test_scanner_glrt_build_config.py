"""Real CMake configuration gates; placeholders are NOT executable SDK tests.

Requires the desktop C compiler, cmake, flex, bison and libxml2 headers. No hardware, firmware
build, external source tree or optional network-discovery dependencies.
"""

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def configure(tmp_path, *options):
    # CMake only checks existence here. Actual SDK/worker/provider execution is
    # a separate native integration suite, not implied by these placeholders.
    sdk = tmp_path / "candidate-sdk.so"
    sdk.write_bytes(b"configure-only placeholder")
    headers = tmp_path / "headers"
    headers.mkdir()
    (headers / "scanner_glrt.h").write_text("/* configure only */\n")
    build = tmp_path / "build"
    command = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(build),
        "-DWITH_TESTS=OFF",
        "-DWITH_IIOD=ON",
        "-DWITH_XML_BACKEND=ON",
        "-DWITH_LOCAL_BACKEND=ON",
        "-DWITH_USB_BACKEND=OFF",
        "-DWITH_SERIAL_BACKEND=OFF",
        "-DHAVE_DNS_SD=OFF",
        "-DWITH_AIO=OFF",
        "-DWITH_ZSTD=OFF",
        "-DWITH_DOC=OFF",
        "-DWITH_MAN=OFF",
        f"-DIIOD_BUFFER_METADATA_PROVIDER={ROOT}/iiod/spf-buffer-metadata.c",
        f"-DIIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER={ROOT}/iiod/spf-hop-device-userspace.c",
        f"-DIIOD_SCANNER_GLRT_LIBRARY={sdk}",
        f"-DIIOD_SCANNER_GLRT_INCLUDE_DIR={headers}",
        f"-DIIOD_SCANNER_GLRT_WORKER_PATH={tmp_path}/unused-worker",
        f"-DIIOD_SCANNER_GLRT_TEMPLATES_2500000={tmp_path}/unused-2m",
        f"-DIIOD_SCANNER_GLRT_TEMPLATES_5000000={tmp_path}/unused-5m",
        "-DIIOD_SCANNER_GLRT_ALGORITHM_SHA256=" + "12" * 32,
        "-DIIOD_SCANNER_GLRT_CONFIGURATION_SHA256=" + "34" * 32,
        *options,
    ]
    return (
        subprocess.run(
            command, text=True, capture_output=True, timeout=30, check=False
        ),
        build,
        sdk,
    )


def test_default_remains_unqualified_and_links_exact_sdk_path(tmp_path):
    result, build, sdk = configure(tmp_path)
    assert result.returncode == 0, result.stderr
    flags = (build / "iiod/CMakeFiles/iiod.dir/flags.make").read_text()
    assert "unqualified-evidence" in flags and "POSITIVE_ONLY" not in flags
    assert "IIOD_HAS_SCANNER_ADAPTIVE_HOP" not in flags
    assert "IIOD_SCANNER_GLRT_CAPTURE_PROTECTION" not in flags
    link = (build / "iiod/CMakeFiles/iiod.dir/link.txt").read_text()
    assert str(sdk) in link and "-lcandidate-sdk" not in link


def test_capture_protection_requires_explicit_sdk(tmp_path):
    result, _, _ = configure(tmp_path, "-DIIOD_SCANNER_GLRT_LIBRARY=",
                             "-DIIOD_SCANNER_GLRT_CAPTURE_PROTECTION=ON")
    assert result.returncode != 0
    assert "capture protection requires an explicit GLRT SDK" in result.stderr


@pytest.mark.parametrize("mode", ["unqualified-evidence", "positive-only-v1"])
def test_capture_protection_is_an_explicit_additive_opt_in(tmp_path, mode):
    options = ["-DIIOD_SCANNER_GLRT_CAPTURE_PROTECTION=ON", f"-DIIOD_SCANNER_GLRT_MODE={mode}"]
    if mode == "positive-only-v1":
        options += ["-DIIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE=0.175",
                    "-DIIOD_SCANNER_GLRT_MINIMUM_MARGIN=0.025"]
    result, build, _ = configure(tmp_path, *options)
    assert result.returncode == 0, result.stderr
    flags = (build / "iiod/CMakeFiles/iiod.dir/flags.make").read_text()
    assert "IIOD_SCANNER_GLRT_CAPTURE_PROTECTION=1" in flags


@pytest.mark.parametrize(
    "score,margin", [("0.175", "0.025"), ("0", ".025"), ("10", "1")]
)
def test_positive_mode_requires_explicit_decimal_thresholds(tmp_path, score, margin):
    result, build, _ = configure(
        tmp_path,
        "-DIIOD_SCANNER_GLRT_MODE=positive-only-v1",
        f"-DIIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE={score}",
        f"-DIIOD_SCANNER_GLRT_MINIMUM_MARGIN={margin}",
    )
    assert result.returncode == 0, result.stderr
    flags = (build / "iiod/CMakeFiles/iiod.dir/flags.make").read_text()
    assert "IIOD_SCANNER_GLRT_POSITIVE_ONLY=1" in flags
    expected = score if "." in score else score + ".0"
    assert f"IIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE={expected}" in flags


@pytest.mark.parametrize(
    "score,margin",
    [
        ("", "0.025"),
        ("nan", "0.025"),
        ("inf", "0.025"),
        ("-1", "0.025"),
        ("08", "0.025"),
        ("0.175", "0"),
        ("0.175", ".000"),
        ("0.175", ""),
        ("0.175;ignored", "0.025"),
        ("9" * 25, "0.025"),
    ],
)
def test_invalid_or_ambiguous_thresholds_fail_configuration(tmp_path, score, margin):
    result, _, _ = configure(
        tmp_path,
        "-DIIOD_SCANNER_GLRT_MODE=positive-only-v1",
        f"-DIIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE={score}",
        f"-DIIOD_SCANNER_GLRT_MINIMUM_MARGIN={margin}",
    )
    assert result.returncode != 0
    assert "Positive-only GLRT requires" in result.stderr


@pytest.mark.parametrize(
    "options,expected",
    [
        (
            ("-DIIOD_SCANNER_GLRT_MODE=arbitrary",),
            "Unsupported scanner GLRT decision profile",
        ),
        (
            ("-DIIOD_SCANNER_GLRT_MINIMUM_MARGIN=0.025",),
            "thresholds require explicit positive-only-v1",
        ),
    ],
)
def test_mode_and_threshold_opt_in_cannot_be_implicit(tmp_path, options, expected):
    result, _, _ = configure(tmp_path, *options)
    assert result.returncode != 0 and expected in result.stderr


@pytest.mark.parametrize("mode", ["unqualified-evidence", "positive-only-v1"])
@pytest.mark.parametrize("provider", ["userspace", "local"])
def test_adaptive_mode_requires_positive_userspace_build(tmp_path, mode, provider):
    options = [
        "-DIIOD_SCANNER_ADAPTIVE_HOP=ON",
        f"-DIIOD_SCANNER_GLRT_MODE={mode}",
        f"-DIIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER={ROOT}/iiod/spf-hop-device-{provider}.c",
    ]
    if mode == "positive-only-v1":
        options += [
            "-DIIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE=0.175",
            "-DIIOD_SCANNER_GLRT_MINIMUM_MARGIN=0.025",
        ]
    result, build, _ = configure(tmp_path, *options)
    if mode == "positive-only-v1" and provider == "userspace":
        assert result.returncode == 0, result.stderr
        assert (
            "IIOD_HAS_SCANNER_ADAPTIVE_HOP=1"
            in (build / "iiod/CMakeFiles/iiod.dir/flags.make").read_text()
        )
    else:
        assert result.returncode != 0
        assert "Adaptive hop V2 requires" in result.stderr
