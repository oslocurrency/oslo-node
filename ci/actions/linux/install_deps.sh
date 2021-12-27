#!/bin/bash

sudo mkdir -p /etc/docker && echo '{"ipv6":true,"fixed-cidr-v6":"2001:db8:1::/64"}' | sudo tee /etc/docker/daemon.json && sudo service docker restart;

ci/build-docker-image.sh docker/ci/Dockerfile-base oslocurrency/oslo-env:base
ci/build-docker-image.sh docker/ci/Dockerfile-gcc oslocurrency/oslo-env:gcc
ci/build-docker-image.sh docker/ci/Dockerfile-clang oslocurrency/oslo-env:clang
