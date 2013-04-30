CLIENT := OscGroupClient
SERVER := OscGroupServer


# should be either OSC_HOST_BIG_ENDIAN or OSC_HOST_LITTLE_ENDIAN
# Apple Intel: OSC_HOST_LITTLE_ENDIAN
# Apple PowerPC: OSC_HOST_BIG_ENDIAN
# Win32: OSC_HOST_LITTLE_ENDIAN
# i386 Linux: OSC_HOST_LITTLE_ENDIAN

ENDIANESS=OSC_DETECT_ENDIANESS #source code will detect using preprocessor
#ENDIANESS=OSC_HOST_LITTLE_ENDIAN

INCLUDES := -I../oscpack
COPTS  := -Wall -Wextra -O3
CDEBUG := -Wall -Wextra -g 
CXXFLAGS := $(COPTS) $(INCLUDES) -D$(ENDIANESS)
LIBS := -lpthread

BINDIR := bin

#Name definitions
OSCGROUPSERVER := $(BINDIR)/$(SERVER)
OSCGROUPCLIENT := $(BINDIR)/$(CLIENT)

COMMONSOURCES := \
	../oscpack/osc/OscTypes.cpp  \
	../oscpack/osc/OscOutboundPacketStream.cpp \
	../oscpack/osc/OscReceivedElements.cpp \
	../oscpack/ip/posix/NetworkingUtils.cpp  \
	../oscpack/ip/IpEndpointName.cpp \
	../oscpack/ip/posix/UdpSocket.cpp

SERVERSOURCES := ./GroupServer.cpp ./OscGroupServer.cpp
CLIENTSOURCES := ./OscGroupClient.cpp ./md5.cpp 

COMMONOBJECTS := $(COMMONSOURCES:.cpp=.o)
SERVEROBJECTS := $(SERVERSOURCES:.cpp=.o)
CLIENTOBJECTS := $(CLIENTSOURCES:.cpp=.o)

SCRIPTS := \
    ./OscGroupServerStartStop.sh \
    ./run_client.sh \
    ./run_server.sh

.PHONY: all server client

all:	server client

server : $(OSCGROUPSERVER)
client : $(OSCGROUPCLIENT)

$(OSCGROUPSERVER) $(OSCGROUPCLIENT) : $(COMMONOBJECTS) | $(BINDIR)
	$(CXX) -o $@ $^

$(OSCGROUPSERVER) : $(SERVEROBJECTS)
$(OSCGROUPCLIENT) : $(CLIENTOBJECTS)

$(BINDIR):
	mkdir $@

# set executable bit on scripts
scripts:
	chmod +x $(SCRIPTS) 

# install the daemon on linux. make sure you
# edit the script with the right path information first
install_daemon : OscGroupServerStartStop.sh
	ln -s ./OscGroupServerStartStop.sh /etc/init.d/OscGroupServer
	update-rc.d OscGroupServer defaults

clean:
	rm -rf $(BINDIR) $(SERVEROBJECTS) $(CLIENTOBJECTS)


