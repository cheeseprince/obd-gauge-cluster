# ClusterFuzzLite build image. Base is OSS-Fuzz's builder, which supplies the
# clang/libFuzzer toolchain and the $CXX / $CXXFLAGS / $LIB_FUZZING_ENGINE /
# $OUT contract that build.sh relies on.
#
# PINNED BY DIGEST, and the pin is only safe because of the Dependabot entry
# that goes with it. This comment previously argued the opposite -- that pinning
# would let the fuzzing toolchain "silently rot instead of tracking the platform
# it has to run on". That reasoning was sound while nothing could bump the
# digest; .github/dependabot.yml now has a `docker` ecosystem entry pointed at
# this directory, so a stale base image becomes a PR instead of silent drift.
#
# The tag is kept alongside the digest deliberately: Dependabot needs the
# `:tag@sha256:` form to know what it is tracking, and Scorecard's
# Pinned-Dependencies check only accepts the digest half.
#
# If the Dependabot entry is ever removed, put the unpinned FROM back -- a
# rotting fuzzing toolchain is worse than an unpinned one, exactly as the
# original comment argued. This image never touches OTA_SIGNING_KEY or any
# secret; it builds a fuzz target in an isolated job.
FROM gcr.io/oss-fuzz-base/base-builder:latest@sha256:8b4a73d83374b298a0a771eeec9a1d44ceff3416a678b7d7946303389d022aca

COPY . $SRC/obd-gauge-cluster
COPY .clusterfuzzlite/build.sh $SRC/build.sh
WORKDIR $SRC/obd-gauge-cluster
