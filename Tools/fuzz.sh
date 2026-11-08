#!/bin/zsh
# Builds the fuzz targets with Homebrew LLVM's libFuzzer and runs each for a while.
#
# Usage: Tools/fuzz.sh [seconds per target, default 300]
set -euo pipefail
root=${0:A:h:h}
seconds=${1:-300}
llvm=$(brew --prefix llvm)
out=$root/build/fuzz
mkdir -p $out
flags=(-std=c++23 -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
       -I$root/Engine -I$root/Import -isysroot $(xcrun --show-sdk-path))
base=($root/Engine/base/*.cpp)
build() {
    local name=$1; shift
    $llvm/bin/clang++ $flags $root/Tests/fuzz/$name.cpp $@ $base -o $out/$name
}
build fuzz_part21 $root/Import/step/part21.cpp
build fuzz_stl $root/Import/stl/stl_reader.cpp
build fuzz_blob $root/Import/format/blob.cpp
build fuzz_stream $root/Engine/scene/segment.cpp $root/Import/format/blob.cpp
for name in fuzz_part21 fuzz_stl fuzz_blob fuzz_stream; do
    mkdir -p $out/corpus/$name
    echo "== $name, $seconds s"
    $out/$name -max_total_time=$seconds -max_len=65536 -print_final_stats=1 $out/corpus/$name \
        2>&1 | grep -E "Done|stat::number_of_executed_units|ERROR|SUMMARY|crash" || true
done
