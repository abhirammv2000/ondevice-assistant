FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build clang lld python3 python3-venv python3-numpy git curl ca-certificates valgrind \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
