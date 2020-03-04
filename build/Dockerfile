### Setup ###

FROM ubuntu:18.04
SHELL ["/bin/bash", "-c"]
### Install packages for Debian-based OS ###

RUN apt-get update && apt-get install -yq autoconf-archive bison build-essential cmake curl flex git gperf joe libboost-all-dev libcap-dev libdouble-conversion-dev libevent-dev libgflags-dev libgoogle-glog-dev libkrb5-dev libpcre3-dev libpthread-stubs0-dev libnuma-dev libsasl2-dev libsnappy-dev libsqlite3-dev libssl-dev libtool netcat-openbsd pkg-config sudo unzip wget python3-venv
RUN apt-get install -yq gcc-'7' g++-'7'
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-'7' 40 --slave /usr/bin/g++ g++ /usr/bin/g++-'7'
RUN update-alternatives --config gcc

USER 'nobody'
USER root
RUN mkdir -p '/home/install' && chown 'nobody' '/home/install'
USER 'nobody'
WORKDIR '/home/install'

### Diagnostics ###

# Builder DockerFBCodeBuilder(google/googletest:cmake_defines={u'BUILD_GTEST': u'ON', u'BUILD_SHARED_LIBS': u'OFF'}, google/googletest:git_hash=u'release-1.8.1', gcc_version='7', facebook/zstd:git_hash=ShellQuoted(u'$(git describe --abbrev=0 --tags origin/master)'), hyperic/sigar:autoconf_options={u'CFLAGS': u'-fgnu89-inline'}, openr/build:cmake_defines={u'ADD_ROOT_TESTS': u'OFF'}, thom311/libnl:git_hash=u'libnl3_2_25', docker_context_dir=u'/tmp/docker-context-F_Z8Yo', fmtlib/fmt:git_hash=u'5.3.0', wangle/wangle/build:cmake_defines={u'BUILD_TESTS': u'OFF'}, prefix=u'/home/install', os_image='ubuntu:18.04', user=u'nobody', fizz/fizz/build:cmake_defines={u'BUILD_TESTS': u'ON'}, projects_dir=u'/home', zeromq/libzmq:git_hash=u'v4.2.2', make_parallelism=12, jedisct1/libsodium:git_hash=u'stable')
RUN hostname
RUN cat /etc/issue || echo no /etc/issue
RUN g++ --version || echo g++ not installed
RUN cmake --version || echo cmake not installed

### Check out fmtlib/fmt, workdir build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'fmtlib/fmt'
USER root
RUN mkdir -p '/home'/'fmt'/'build' && chown 'nobody' '/home'/'fmt'/'build'
USER 'nobody'
WORKDIR '/home'/'fmt'/'build'
RUN git checkout '5.3.0'

### Build and install fmtlib/fmt ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/folly, workdir _build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/folly'
USER root
RUN mkdir -p '/home'/'folly'/'_build' && chown 'nobody' '/home'/'folly'/'_build'
USER 'nobody'
WORKDIR '/home'/'folly'/'_build'
RUN git checkout '134472ee45780ca2afa6f64cb7baac318c60a7c3'

### Build and install facebook/folly ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out jedisct1/libsodium, workdir . ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'jedisct1/libsodium'
USER root
RUN mkdir -p '/home'/'libsodium'/'.' && chown 'nobody' '/home'/'libsodium'/'.'
USER 'nobody'
WORKDIR '/home'/'libsodium'/'.'
RUN git checkout 'stable'

### Build and install jedisct1/libsodium ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebookincubator/fizz, workdir fizz/build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebookincubator/fizz'
USER root
RUN mkdir -p '/home'/'fizz'/'fizz/build' && chown 'nobody' '/home'/'fizz'/'fizz/build'
USER 'nobody'
WORKDIR '/home'/'fizz'/'fizz/build'

### Build and install fizz/fizz/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'BUILD_TESTS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out rsocket/rsocket-cpp, workdir rsocket ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'rsocket/rsocket-cpp'
USER root
RUN mkdir -p '/home'/'rsocket-cpp'/'rsocket' && chown 'nobody' '/home'/'rsocket-cpp'/'rsocket'
USER 'nobody'
WORKDIR '/home'/'rsocket-cpp'/'rsocket'

### Build and install rsocket-cpp/rsocket ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/wangle, workdir wangle/build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/wangle'
USER root
RUN mkdir -p '/home'/'wangle'/'wangle/build' && chown 'nobody' '/home'/'wangle'/'wangle/build'
USER 'nobody'
WORKDIR '/home'/'wangle'/'wangle/build'
RUN git checkout 'eab7a0e4e307de4910c75917d744e5a6002d694f'

### Build and install wangle/wangle/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'BUILD_TESTS'='OFF' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/zstd, workdir . ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/zstd'
USER root
RUN mkdir -p '/home'/'zstd'/'.' && chown 'nobody' '/home'/'zstd'/'.'
USER 'nobody'
WORKDIR '/home'/'zstd'/'.'
RUN git checkout $(git describe --abbrev=0 --tags origin/master)

### Build and install zstd ###

RUN make -j '16' VERBOSE=1 'PREFIX'='/home/install'
RUN make install VERBOSE=1 'PREFIX'='/home/install'

### Check out facebook/fbthrift, workdir thrift ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/fbthrift'
USER root
RUN mkdir -p '/home'/'fbthrift'/'thrift' && chown 'nobody' '/home'/'fbthrift'/'thrift'
USER 'nobody'
WORKDIR '/home'/'fbthrift'/'thrift'
RUN git checkout 'ea8bae1f0f2a57618e8316bcdb2ecc4d34d9f538'

### Build and install fbthrift/thrift ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out google/googletest, workdir build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'google/googletest'
USER root
RUN mkdir -p '/home'/'googletest'/'build' && chown 'nobody' '/home'/'googletest'/'build'
USER 'nobody'
WORKDIR '/home'/'googletest'/'build'
RUN git checkout 'release-1.8.1'

### Build and install google/googletest ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_GTEST'='ON' -D'BUILD_SHARED_LIBS'='OFF' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out hyperic/sigar, workdir . ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'hyperic/sigar'
USER root
RUN mkdir -p '/home'/'sigar'/'.' && chown 'nobody' '/home'/'sigar'/'.'
USER 'nobody'
WORKDIR '/home'/'sigar'/'.'

### Build and install sigar ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 'CFLAGS'='-fgnu89-inline'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

# XXX Begin Added code
USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
RUN apt-get -yq remove libboost-all-dev 
USER 'nobody'
WORKDIR '/home'
RUN git clone https://github.com/arshanh/openr openr-xr
USER root
RUN mkdir -p '/home'/'openr-xr'/'build' && chown 'nobody' '/home'/'openr-xr'/'build'
WORKDIR '/home'/'openr-xr'/'build'

# Build Boost
RUN python3 fbcode_builder/getdeps.py fetch --no-tests boost --install-dir '/usr'
RUN python3 fbcode_builder/getdeps.py build --no-tests boost --install-dir '/usr'
# XXX End Added code

### Check out zeromq/libzmq, workdir . ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'zeromq/libzmq'
USER root
RUN mkdir -p '/home'/'libzmq'/'.' && chown 'nobody' '/home'/'libzmq'/'.'
USER 'nobody'
WORKDIR '/home'/'libzmq'/'.'
RUN git checkout 'v4.2.2'

### Build and install zeromq/libzmq ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/fbzmq, workdir _build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/fbzmq'
USER root
RUN mkdir -p '/home'/'fbzmq'/'_build' && chown 'nobody' '/home'/'fbzmq'/'_build'
USER 'nobody'
WORKDIR '/home'/'fbzmq'/'_build'
RUN git checkout '0ffb23f1fd3bbe62b2a271395da25e1ef5ee754f'

### Build and install fbzmq/ ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN PYTHONPATH="$PYTHONPATH:"'/home/install'/lib/python2.7/site-packages make -j '16'
RUN make install

### Check out google/re2, workdir build ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'google/re2'
USER root
RUN mkdir -p '/home'/'re2'/'build' && chown 'nobody' '/home'/'re2'/'build'
USER 'nobody'
WORKDIR '/home'/'re2'/'build'

### Build and install google/re2 ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out thom311/libnl, workdir . ###

USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
USER 'nobody'
WORKDIR '/home'
RUN git clone  https://github.com/'thom311/libnl'
USER root
RUN mkdir -p '/home'/'libnl'/'.' && chown 'nobody' '/home'/'libnl'/'.'
USER 'nobody'
WORKDIR '/home'/'libnl'/'.'
RUN git checkout 'libnl3_2_25'

### Build and install thom311/libnl ###

RUN curl -O https://raw.githubusercontent.com/facebook/openr/master/build/fix-route-obj-attr-list.patch
RUN git apply 'fix-route-obj-attr-list.patch'
RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

# Check out iosxrsl
USER root
RUN mkdir -p '/home' && chown 'nobody' '/home'
WORKDIR '/home'
RUN git clone https://wwwin-github.cisco.com/ios-xr/service-layer-objmodel.git -b add-jobs-to-make && \
		cd service-layer-objmodel/grpc/cpp && \
		./build_libiosxrsl.sh -g 1.9.1 -p 3.5.0 -j '16'

# RUN apt-get update && apt-get install -y git iproute2 vim && git clone https://wwwin-github.cisco.com/akshshar/service-layer-objmodel ~/service-layer-objmodel && \
#            cd ~/service-layer-objmodel/grpc/cpp && \
#            ./build_libiosxrsl.sh -g 1.9.1 -p 3.5.0 &&  \
#            cd ~/ && rm -r ~/service-layer-objmodel

### Check out facebook/openr, workdir build ###

# Comment the following as we have already done this above
# USER root
# RUN mkdir -p '/home' && chown 'nobody' '/home'
# USER 'nobody'
# WORKDIR '/home'
# RUN git clone  https://github.com/'facebook/openr'
# USER root
# RUN mkdir -p '/home'/'openr'/'build' && chown 'nobody' '/home'/'openr'/'build'
USER 'nobody'
WORKDIR '/home'/'openr-xr'/'build'

### Build and install openr/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'ADD_ROOT_TESTS'='OFF' '..'
RUN PYTHONPATH="$PYTHONPATH:"'/home/install'/lib/python2.7/site-packages make -j '16'
RUN make install

#####################################################################################################################################3

FROM ubuntu:18.04
COPY --from=0 /usr/local/lib /usr/local/lib
COPY --from=0 /usr/local/share /usr/local/share
COPY --from=0 /usr/local/include /usr/local/include
COPY --from=0 /usr/local/sbin /usr/local/sbin
COPY --from=0 /usr/local/bin /usr/local/bin

COPY --from=0 /usr/lib /usr/lib
COPY --from=0 /lib /lib
COPY --from=0 /usr/include /usr/include

COPY --from=0 /usr/sbin /usr/sbin
COPY --from=0 /usr/bin /usr/bin

COPY --from=0 '/home/install' '/home/install'

RUN mkdir /var/log/openr && apt-get update && apt-get install -y iproute2 vim
COPY run_openr_xr.sh /usr/sbin/run_openr_xr.sh
COPY run_openr_linux.sh /usr/sbin/run_openr_linux.sh
COPY start_openr.sh /usr/bin/start_openr

