# Stage 1: Build ION from source
FROM ubuntu:22.04@sha256:ce4a593b4e323dcc3dd728e397e0a866a1bf516a1b7c31d6aa06991baec4f2e0 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    automake autoconf libtool gcc make git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Pin to ion-open-source-4.1.4 by commit SHA for reproducible builds
# --filter=blob:none avoids downloading full history (~100MB savings)
RUN git clone --filter=blob:none --no-checkout https://github.com/nasa-jpl/ION-DTN.git /ion-src \
    && cd /ion-src && git checkout 67bd88ed12fbe0e308c34061fad8570e6beb2e6e

WORKDIR /ion-src
RUN autoreconf -fi \
    && ./configure --quiet \
    && make -j"$(nproc)" \
    && make install DESTDIR=/ion-install

# Stage 2: Slim runtime image
FROM ubuntu:22.04@sha256:ce4a593b4e323dcc3dd728e397e0a866a1bf516a1b7c31d6aa06991baec4f2e0

# Copy ION installation
COPY --from=builder /ion-install/usr/local /usr/local
RUN ldconfig

RUN apt-get update && apt-get install -y --no-install-recommends \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

COPY scripts/ionstart.sh /usr/local/bin/ionstart.sh
RUN chmod +x /usr/local/bin/ionstart.sh

WORKDIR /ion

ENTRYPOINT ["/usr/local/bin/ionstart.sh"]
