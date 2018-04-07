FROM alpine:3.7 as builder
MAINTAINER Rashad Kamsheh gusalkara@student.gu.se
RUN apk update && \
    apk --no-cache add \
        ca-certificates \
        cmake \
        g++ \
        make && \
    apk add libcluon --no-cache --repository https://chrberger.github.io/libcluon/alpine/v3.7 --allow-untrusted
ADD . /opt/sources
WORKDIR /opt/sources
RUN cd /opt/sources && \
    cp ip.txt /tmp && \
    mkdir build && \
    cd build && \
    cmake -D CMAKE_BUILD_TYPE=Release .. && \
    make && \
    cp CarServices-V2VService /tmp
    
# Deploy.
FROM alpine:3.7
MAINTAINER Mans Thornvik gusthomaa@student.gu.se
RUN apk update && \
    apk add libcluon --no-cache --repository https://chrberger.github.io/libcluon/alpine/v3.7 --allow-untrusted && \
    mkdir /opt
WORKDIR /opt
COPY --from=builder /tmp/CarServices-V2VService .
COPY --from=builder /tmp/ip.txt .
CMD ["./CarServices-V2VService"]
