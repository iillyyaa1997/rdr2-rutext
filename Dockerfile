FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
