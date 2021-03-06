FROM nanocurrency/nano-env:gcc

ARG NETWORK=live
ARG TRAVIS_TAG=DEV_BUILD
ARG CI_BUILD=OFF
ADD ./ /tmp/src

RUN mkdir /tmp/build && \
    cd /tmp/build && \
    cmake /tmp/src -DCI_BUILD=${CI_BUILD} -DBOOST_ROOT=${BOOST_ROOT} -DACTIVE_NETWORK=oslo_${NETWORK}_network \
    -DOSLO_ROCKSDB=ON -DOSLO_POW_SERVER=ON -DROCKSDB_LIBRARIES=/tmp/rocksdb/lib/librocksdb.a \
    -DROCKSDB_INCLUDE_DIRS=/tmp/rocksdb/include -DOSLO_SHARED_BOOST=ON && \
    make oslo_node -j $(nproc) && \
    make oslo_rpc -j $(nproc) && \
    make oslo_pow_server -j $(nproc) && \
    cd .. && \
    echo ${NETWORK} > /etc/oslo-network

FROM ubuntu:16.04

RUN groupadd --gid 1000 oslocurrency && \
    useradd --uid 1000 --gid oslocurrency --shell /bin/bash --create-home oslocurrency

COPY --from=0 /tmp/build/oslo_node /usr/bin
COPY --from=0 /tmp/build/oslo_rpc /usr/bin
COPY --from=0 /tmp/build/oslo_pow_server /usr/bin
COPY --from=0 /tmp/src/api/ /usr/bin/api/
COPY --from=0 /etc/oslo-network /etc
COPY docker/node/entry.sh /usr/bin/entry.sh
COPY docker/node/config /usr/share/oslo/config
COPY --from=0 /tmp/boost/lib/* /usr/local/lib/
RUN chmod +x /usr/bin/entry.sh
RUN ln -s /usr/bin/oslo_node /usr/bin/rai_node
RUN ldconfig

WORKDIR /root
USER root

ENV PATH="${PATH}:/usr/bin"
ENTRYPOINT ["/bin/bash",  "/usr/bin/entry.sh"]
CMD ["oslo_node daemon -l"]
