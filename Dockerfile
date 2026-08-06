# Build environment for gem5-CIMD (gem5 v25.x).
#
# The source tree is NOT copied into the image: mount it at /gem5 instead so
# builds land in the host working copy (see docker-run.sh).
#
#   docker build -t gem5-cimd --build-arg UID=$(id -u) --build-arg GID=$(id -g) .
#   docker run -it --rm -v "$PWD":/gem5 -w /gem5 gem5-cimd
#   scons build/X86/gem5.opt -j$(nproc)

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# gem5 v25 requires GCC 11-14 (24.04 ships 13) and Python >= 3.8 (24.04: 3.12).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-13 g++-13 \
        git \
        m4 \
        scons \
        python3 \
        python3-dev \
        python3-venv \
        python3-pip \
        pkg-config \
        zlib1g-dev \
        libprotobuf-dev protobuf-compiler libprotoc-dev \
        libgoogle-perftools-dev \
        libboost-all-dev \
        libhdf5-serial-dev \
        libpng-dev \
        libelf-dev \
        libcapstone-dev \
        libssl-dev \
        doxygen \
        ca-certificates \
        curl \
        wget \
        vim \
        less \
        gdb \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

# gem5's Python requirements. 24.04 marks the system Python as externally
# managed, so use a venv that is on PATH for every shell.
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv "$VIRTUAL_ENV"
ENV PATH="$VIRTUAL_ENV/bin:$PATH"
RUN pip install --no-cache-dir mypy==1.16.0 pre-commit==4.2.0 tqdm==4.67.1

# Run as a user matching the host so build/ artifacts stay writable outside.
ARG UID=1000
ARG GID=1000
ARG USER=gem5
RUN if getent group "$GID" >/dev/null; then \
        groupmod -n "$USER" "$(getent group "$GID" | cut -d: -f1)"; \
    else \
        groupadd -g "$GID" "$USER"; \
    fi \
    && if getent passwd "$UID" >/dev/null; then \
        usermod -l "$USER" -d /home/"$USER" -m -g "$GID" \
            "$(getent passwd "$UID" | cut -d: -f1)"; \
    else \
        useradd -u "$UID" -g "$GID" -m -s /bin/bash "$USER"; \
    fi
USER $UID:$GID
ENV HOME=/home/gem5

# git refuses to operate on a tree owned by another uid; the mount is ours,
# but be permissive so builds work even under --user overrides.
RUN git config --global --add safe.directory '*'

WORKDIR /gem5

CMD ["/bin/bash"]
