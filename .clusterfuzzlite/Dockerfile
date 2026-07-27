# ClusterFuzzLite build image. Base is OSS-Fuzz's builder, which supplies the
# clang/libFuzzer toolchain and the $CXX / $CXXFLAGS / $LIB_FUZZING_ENGINE /
# $OUT contract that build.sh relies on.
#
# Deliberately unpinned: gcr.io/oss-fuzz-base/base-builder is Google's
# infrastructure image, and ClusterFuzzLite expects the current one — pinning a
# digest here means the fuzzing toolchain silently rots instead of tracking the
# platform it has to run on. That is the opposite of the reasoning applied to
# GitHub Actions elsewhere in this repo, and the difference is deliberate: this
# image never touches OTA_SIGNING_KEY or any secret. It builds a fuzz target in
# an isolated job.
FROM gcr.io/oss-fuzz-base/base-builder

COPY . $SRC/obd-gauge-cluster
COPY .clusterfuzzlite/build.sh $SRC/build.sh
WORKDIR $SRC/obd-gauge-cluster
