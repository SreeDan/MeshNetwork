FROM --platform=$BUILDPLATFORM debian:bookworm-slim as builder
WORKDIR /app

RUN apt-get update && \
    apt-get install -y \
        build-essential \
        git \
        curl \
        zip \
        unzip \
        tar \
        cmake \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT /opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT

# Change to the vcpkg directory and run the bootstrap script
WORKDIR $VCPKG_ROOT
RUN ./bootstrap-vcpkg.sh -disableMetrics

WORKDIR /app

COPY . .

RUN rm -rf build

RUN make build VERBOSE=1

FROM --platform=$BUILDPLATFORM debian:bookworm-slim AS runtime

COPY --from=builder /app/build/MeshNetworking /usr/local/bin/MeshNetworking


ENTRYPOINT ["/usr/local/bin/MeshNetworking", "/etc/MeshNetworking/config.yaml"]