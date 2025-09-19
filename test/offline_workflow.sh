#!/bin/sh
set -eu

chunk_store_bin=$1
zckdir_bin=$2
sample=$3

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT INT TERM

chunk_dir="$workdir/cache"
mkdir -p "$chunk_dir"

"$chunk_store_bin" "$chunk_dir" "$sample"

header_path="$chunk_dir/headers/$(basename "$sample").header"
out_path="$workdir/output.zck"

"$zckdir_bin" --chunk-dir "$chunk_dir" --header "$header_path" "$out_path"

cmp "$sample" "$out_path"
