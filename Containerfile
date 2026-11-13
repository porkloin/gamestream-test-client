# gamestream-test-client build and runtime, on Arch: its FFmpeg has the H.264, HEVC and AV1 decoders and its Mesa
# the VAAPI H.264 and HEVC decoders (Fedora leaves those out, Debian ships Mesa late for new GPUs), with
# intel-media-driver for Intel GPUs (--decode hw), libnl for the Wi-Fi link and hwdata's PCI database to name GPUs
# their driver doesn't (`probe`). The same stack as moonshine-test-headless's image, so the client decodes the same way
# on the host and on another machine.
FROM docker.io/library/archlinux:latest AS build
RUN pacman -Syu --noconfirm --needed base-devel cmake pkgconf openssl curl expat util-linux-libs opus ffmpeg libva libnl \
    && pacman -Scc --noconfirm
COPY . /src
# The commit --version and the results name (CI passes it).
ARG GTC_COMMIT=dev
RUN cmake -S /src/client -B /build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTC_COMMIT="$GTC_COMMIT" \
    && cmake --build /build -j"$(nproc)" \
    && cmake --install /build --prefix /usr/local

FROM docker.io/library/archlinux:latest
LABEL org.opencontainers.image.source=https://github.com/porkloin/gamestream-test-client
RUN pacman -Syu --noconfirm --needed openssl curl expat util-linux-libs opus ffmpeg libva libnl mesa intel-media-driver \
      libva-utils hwdata ca-certificates \
    && pacman -Scc --noconfirm
COPY --from=build /usr/local/bin/gamestream-test-client /usr/local/bin/
ENTRYPOINT ["gamestream-test-client"]
