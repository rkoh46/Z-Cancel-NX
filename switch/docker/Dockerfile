# SPDX-License-Identifier: GPL-3.0-or-later
# The toolchain Z-Cancel NX is built with: devkitA64 (GCC, libnx) and the Switch portlibs it links
# (Mesa's EGL/OpenGL for the picture, FreeType for the menus, curl + mbedTLS for patch downloads,
# zlib, libpng). builder/docker-image.sh builds it as z-cancel-nx-build:latest.
FROM devkitpro/devkita64:latest

RUN dkp-pacman -Sy --noconfirm --needed \
        switch-mesa switch-glad switch-libdrm_nouveau switch-freetype switch-libpng \
        switch-curl switch-mbedtls switch-zlib switch-bzip2 \
    && dkp-pacman -Scc --noconfirm

RUN apt-get update && apt-get install -y --no-install-recommends rsync patch python3 \
    && rm -rf /var/lib/apt/lists/*

ENV DEVKITPRO=/opt/devkitpro
