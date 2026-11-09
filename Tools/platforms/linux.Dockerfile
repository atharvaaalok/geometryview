# Linux for the viewer's checks, without a GPU: Debian's Chromium, drawing WebGPU on the CPU through
# Mesa's lavapipe or Chromium's SwiftShader, and Node for the launcher. The repository is mounted at
# /geometry, so nothing is copied into the image.
#
#   docker build -t geometry-linux -f Tools/platforms/linux.Dockerfile Tools/platforms
#   docker run --rm -v "$PWD":/geometry -w /geometry geometry-linux <command>
FROM node:22-bookworm
RUN apt-get update \
 && apt-get install -y --no-install-recommends chromium mesa-vulkan-drivers libvulkan1 vulkan-tools xvfb xauth \
      fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*
ENV GEOMETRYVIEW_CHROME=/usr/bin/chromium
