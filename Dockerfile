# syntax=docker/dockerfile:1

# ---- Build stage: compile, run the full test suite, keep only the binaries ----
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build libgtest-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# libgtest-dev is found by find_package(GTest), so the build needs no network access.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel \
 && ctest --test-dir build --output-on-failure

# ---- Runtime stage: small image with the exchange CLI and the benchmark ----
FROM ubuntu:24.04

RUN useradd --create-home --uid 10001 trader
COPY --from=build /src/build/bin/miniexchange /src/build/bin/miniexchange_bench /usr/local/bin/
COPY scripts/demo.txt /opt/miniexchange/demo.txt

USER trader
WORKDIR /home/trader

# Interactive by default:   docker run -it miniexchange
# Scripted demo:            docker run --rm miniexchange --script /opt/miniexchange/demo.txt
# Benchmarks:               docker run --rm --entrypoint miniexchange_bench miniexchange
ENTRYPOINT ["miniexchange"]
