### Setup ###

FROM ubuntu:18.04
SHELL ["/bin/bash", "-c"]
### Install packages for Debian-based OS ###

USER root

RUN apt-get update && apt-get install -yq autoconf-archive bison build-essential cmake curl flex git gperf joe libboost-all-dev libcap-dev libdouble-conversion-dev libevent-dev libgflags-dev libgoogle-glog-dev libkrb5-dev libpcre3-dev libpthread-stubs0-dev libnuma-dev libsasl2-dev libsnappy-dev libsqlite3-dev libssl-dev libtool netcat-openbsd pkg-config sudo unzip wget python3-venv
RUN apt-get install -yq gcc-'7' g++-'7'
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-'7' 40 --slave /usr/bin/g++ g++ /usr/bin/g++-'7'
RUN update-alternatives --config gcc

RUN mkdir -p '/home/install' && chown 'root' '/home/install'
WORKDIR '/home/install'

### Diagnostics ###

# Builder DockerFBCodeBuilder(google/googletest:cmake_defines={u'BUILD_GTEST': u'ON', u'BUILD_SHARED_LIBS': u'OFF'}, google/googletest:git_hash=u'release-1.8.1', gcc_version='7', facebook/zstd:git_hash=ShellQuoted(u'$(git describe --abbrev=0 --tags origin/master)'), hyperic/sigar:autoconf_options={u'CFLAGS': u'-fgnu89-inline'}, openr/build:cmake_defines={u'ADD_ROOT_TESTS': u'OFF'}, thom311/libnl:git_hash=u'libnl3_2_25', docker_context_dir=u'/tmp/docker-context-F_Z8Yo', fmtlib/fmt:git_hash=u'5.3.0', wangle/wangle/build:cmake_defines={u'BUILD_TESTS': u'OFF'}, prefix=u'/home/install', os_image='ubuntu:18.04', user=u'root', fizz/fizz/build:cmake_defines={u'BUILD_TESTS': u'ON'}, projects_dir=u'/home', zeromq/libzmq:git_hash=u'v4.2.2', make_parallelism=12, jedisct1/libsodium:git_hash=u'stable')
RUN hostname
RUN cat /etc/issue || echo no /etc/issue
RUN g++ --version || echo g++ not installed
RUN cmake --version || echo cmake not installed

# Check out fmtlib/fmt, workdir build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'fmtlib/fmt'
RUN mkdir -p '/home'/'fmt'/'build' && chown 'root' '/home'/'fmt'/'build'
WORKDIR '/home'/'fmt'/'build'
RUN git checkout '5.3.0'

### Build and install fmtlib/fmt ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/folly, workdir _build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/folly'
RUN mkdir -p '/home'/'folly'/'_build' && chown 'root' '/home'/'folly'/'_build'
WORKDIR '/home'/'folly'/'_build'
RUN git checkout '134472ee45780ca2afa6f64cb7baac318c60a7c3'

### Build and install facebook/folly ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out jedisct1/libsodium, workdir . ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'jedisct1/libsodium'
RUN mkdir -p '/home'/'libsodium'/'.' && chown 'root' '/home'/'libsodium'/'.'
WORKDIR '/home'/'libsodium'/'.'
RUN git checkout 'stable'

### Build and install jedisct1/libsodium ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebookincubator/fizz, workdir fizz/build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebookincubator/fizz'
RUN mkdir -p '/home'/'fizz'/'fizz/build' && chown 'root' '/home'/'fizz'/'fizz/build'
WORKDIR '/home'/'fizz'/'fizz/build'

### Build and install fizz/fizz/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'BUILD_TESTS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out rsocket/rsocket-cpp, workdir rsocket ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'rsocket/rsocket-cpp'
RUN mkdir -p '/home'/'rsocket-cpp'/'rsocket' && chown 'root' '/home'/'rsocket-cpp'/'rsocket'
WORKDIR '/home'/'rsocket-cpp'/'rsocket'

### Build and install rsocket-cpp/rsocket ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/wangle, workdir wangle/build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/wangle'
RUN mkdir -p '/home'/'wangle'/'wangle/build' && chown 'root' '/home'/'wangle'/'wangle/build'
WORKDIR '/home'/'wangle'/'wangle/build'
RUN git checkout 'eab7a0e4e307de4910c75917d744e5a6002d694f'

### Build and install wangle/wangle/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'BUILD_TESTS'='OFF' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/zstd, workdir . ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/zstd'
RUN mkdir -p '/home'/'zstd'/'.' && chown 'root' '/home'/'zstd'/'.'
WORKDIR '/home'/'zstd'/'.'
RUN git checkout $(git describe --abbrev=0 --tags origin/master)

### Build and install zstd ###

RUN make -j '16' VERBOSE=1 'PREFIX'='/home/install'
RUN make install VERBOSE=1 'PREFIX'='/home/install'

### Check out facebook/fbthrift, workdir thrift ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/fbthrift'
RUN mkdir -p '/home'/'fbthrift'/'thrift' && chown 'root' '/home'/'fbthrift'/'thrift'
WORKDIR '/home'/'fbthrift'/'thrift'
RUN git checkout 'ea8bae1f0f2a57618e8316bcdb2ecc4d34d9f538'

### Build and install fbthrift/thrift ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out google/googletest, workdir build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'google/googletest'
RUN mkdir -p '/home'/'googletest'/'build' && chown 'root' '/home'/'googletest'/'build'
WORKDIR '/home'/'googletest'/'build'
RUN git checkout 'release-1.8.1'

### Build and install google/googletest ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_GTEST'='ON' -D'BUILD_SHARED_LIBS'='OFF' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out hyperic/sigar, workdir . ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'hyperic/sigar'
RUN mkdir -p '/home'/'sigar'/'.' && chown 'root' '/home'/'sigar'/'.'
WORKDIR '/home'/'sigar'/'.'

### Build and install sigar ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 'CFLAGS'='-fgnu89-inline'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

# XXX Begin Added code
RUN mkdir -p '/home' && chown 'root' '/home'
RUN apt-get -yq remove libboost-all-dev 
WORKDIR '/home'
RUN git clone https://github.com/arshanh/openr openr-xr
RUN mkdir -p '/home'/'openr-xr'/'build' && chown 'root' '/home'/'openr-xr'/'build'
WORKDIR '/home'/'openr-xr'/'build'

# Build Boost
RUN python3 fbcode_builder/getdeps.py fetch --no-tests boost --install-dir '/usr'
RUN python3 fbcode_builder/getdeps.py build --no-tests boost --install-dir '/usr'
# XXX End Added code

### Check out zeromq/libzmq, workdir . ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'zeromq/libzmq'
RUN mkdir -p '/home'/'libzmq'/'.' && chown 'root' '/home'/'libzmq'/'.'
WORKDIR '/home'/'libzmq'/'.'
RUN git checkout 'v4.2.2'

### Build and install zeromq/libzmq ###

RUN ./autogen.sh
RUN LDFLAGS="$LDFLAGS -L"'/home/install'"/lib -Wl,-rpath="'/home/install'"/lib" CFLAGS="$CFLAGS -I"'/home/install'"/include" CPPFLAGS="$CPPFLAGS -I"'/home/install'"/include" PY_PREFIX='/home/install' ./configure --prefix='/home/install' 
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out facebook/fbzmq, workdir _build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'facebook/fbzmq'
RUN mkdir -p '/home'/'fbzmq'/'_build' && chown 'root' '/home'/'fbzmq'/'_build'
WORKDIR '/home'/'fbzmq'/'_build'
RUN git checkout '0ffb23f1fd3bbe62b2a271395da25e1ef5ee754f'

### Build and install fbzmq/ ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN PYTHONPATH="$PYTHONPATH:"'/home/install'/lib/python2.7/site-packages make -j '16'
RUN make install

### Check out google/re2, workdir build ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'google/re2'
RUN mkdir -p '/home'/'re2'/'build' && chown 'root' '/home'/'re2'/'build'
WORKDIR '/home'/'re2'/'build'

### Build and install google/re2 ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' '..'
RUN make -j '16' VERBOSE=1 
RUN make install VERBOSE=1 

### Check out thom311/libnl, workdir . ###

RUN mkdir -p '/home' && chown 'root' '/home'
WORKDIR '/home'
RUN git clone  https://github.com/'thom311/libnl'
RUN mkdir -p '/home'/'libnl'/'.' && chown 'root' '/home'/'libnl'/'.'
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
RUN mkdir -p '/home' && chown 'root' '/home'
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
# # RUN mkdir -p '/home' && chown 'root' '/home'
# # WORKDIR '/home'
# RUN git clone  https://github.com/'facebook/openr'
# # RUN mkdir -p '/home'/'openr'/'build' && chown 'root' '/home'/'openr'/'build'
WORKDIR '/home'/'openr-xr'/'build'

### Build and install openr/build ###

RUN CXXFLAGS="$CXXFLAGS -fPIC -isystem "'/home/install'"/include" CFLAGS="$CFLAGS -fPIC -isystem "'/home/install'"/include" cmake -D'CMAKE_INSTALL_PREFIX'='/home/install' -D'BUILD_SHARED_LIBS'='ON' -D'ADD_ROOT_TESTS'='OFF' '..'
RUN PYTHONPATH="$PYTHONPATH:"'/home/install'/lib/python2.7/site-packages make -j '16'
RUN make install

# The above installs to /home/install, so move it to usr/
# I tried generating the docker file to install to /usr, but it doesn't work
# seems like it is meant to be installed then copied over if necessary

RUN cp -r /home/install/bin/* /usr/bin/
RUN cp -r /home/install/etc/* /usr/etc/
RUN cp -r /home/install/include/* /usr/include/
RUN cp -r /home/install/lib/* /usr/lib/
RUN cp -r /home/install/sbin/* /usr/sbin/
RUN cp -r /home/install/share/* /usr/share/

#######################################################################################################################

# Create a new much smaller image that only contains what is necessary

FROM ubuntu:18.04

WORKDIR '/home'/'openr-xr'/'build'

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

# RUN mkdir /var/log/openr && apt-get update && apt-get install -y iproute2 vim
RUN mkdir /var/log/openr

