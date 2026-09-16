#!/usr/bin/env bash
# Build the ABI-3 iiOD provider against an explicit Buildroot SDK and UAPI.
set -euo pipefail

if [[ $# != 4 ]]; then
    echo "Usage: $0 TOOLCHAIN_CMAKE METADATA_SOURCE KERNEL_UAPI_INCLUDE BUILD_DIR" >&2
    exit 2
fi
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
toolchain="$1"
metadata="$2"
uapi="$3"
build_dir="$4"
[[ -f "$toolchain" && -f "$uapi/linux/adi_tandem_agc.h" ]]
[[ "$(git -C "$metadata" rev-parse HEAD)" == 3294365ff44da26b261be4a2ccb241b7896d23ad ]]
sdk="$(cd "$(dirname "$toolchain")/../.." && pwd)"
extra="$repo_dir/iiod/spf-tandem-session.c;$repo_dir/iiod/spf-tandem-metadata.c"
for source in spf_radio_frame_v3 spf_gain_read spf_gain_sampler spf_rssi_read spf_thread_join spf_time_anchor; do
    extra+=";$metadata/$source.c"
done
cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SKIP_RPATH=ON -DWITH_IIOD=ON -DWITH_IIOD_USBD=ON \
    -DWITH_AIO=ON -DWITH_ZSTD=OFF -DHAVE_DNS_SD=OFF \
    -DWITH_DOC=OFF -DWITH_EXAMPLES=OFF -DWITH_TESTS=ON \
    -DWITH_SERIAL_BACKEND=OFF -DWITH_IIOD_SERIAL=OFF \
    -DWITH_USB_BACKEND=ON -DPYTHON_BINDINGS=OFF \
    -DIIOD_BUFFER_METADATA_PROVIDER="$repo_dir/iiod/spf-buffer-metadata.c" \
    -DIIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES="$extra" \
    -DIIOD_BUFFER_METADATA_INCLUDE_DIRS="$metadata;$uapi" \
    -DBISON_EXECUTABLE="$sdk/bin/bison" -DFLEX_EXECUTABLE="$sdk/bin/flex"
cmake --build "$build_dir" --parallel 8 --target iiod
