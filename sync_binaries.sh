#!/bin/sh
set -e
for target in aarch64 armv6 i686 x86_64; do
  cp /usr/local/$target-unknown-cloudabi*/libexec/cloudabi-reexec bin/$target/
done
