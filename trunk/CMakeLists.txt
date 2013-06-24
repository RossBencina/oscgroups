cmake_minimum_required(VERSION 2.6)
PROJECT(Oscgroups)

set(OscpackDir ../oscpack)

# separate versions of NetworkingUtils.cpp and UdpSocket.cpp are provided for Win32 and POSIX
# the IpSystemTypePath selects the correct ones based on the current platform

IF(WIN32)
 set(IpSystemTypePath ${OscpackDir}/ip/win32)
 set(LIBS ${LIBS} Ws2_32 winmm)
ELSE(WIN32)
 set(IpSystemTypePath ${OscpackDir}/ip/posix)
ENDIF(WIN32)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR} ${OscpackDir} ${OscpackDir}/osc ${OscpackDir}/ip )

ADD_LIBRARY(oscpack 

${OscpackDir}/ip/IpEndpointName.h
${OscpackDir}/ip/IpEndpointName.cpp

${OscpackDir}/ip/NetworkingUtils.h
${IpSystemTypePath}/NetworkingUtils.cpp

${OscpackDir}/ip/UdpSocket.h
${IpSystemTypePath}/UdpSocket.cpp

${OscpackDir}/ip/PacketListener.h
${OscpackDir}/ip/TimerListener.h

${OscpackDir}/osc/OscTypes.h
${OscpackDir}/osc/OscTypes.cpp 
${OscpackDir}/osc/OscHostEndianness.h
${OscpackDir}/osc/OscException.h
${OscpackDir}/osc/OscPacketListener.h
${OscpackDir}/osc/MessageMappingOscPacketListener.h
${OscpackDir}/osc/OscReceivedElements.h
${OscpackDir}/osc/OscReceivedElements.cpp
${OscpackDir}/osc/OscPrintReceivedElements.h
${OscpackDir}/osc/OscPrintReceivedElements.cpp
${OscpackDir}/osc/OscOutboundPacketStream.h
${OscpackDir}/osc/OscOutboundPacketStream.cpp

)

ADD_EXECUTABLE(OscGroupClient OscGroupClient.cpp md5.cpp)
TARGET_LINK_LIBRARIES(OscGroupClient oscpack ${LIBS})

ADD_EXECUTABLE(OscGroupServer OscGroupServer.cpp GroupServer.cpp md5.cpp)
TARGET_LINK_LIBRARIES(OscGroupServer oscpack ${LIBS})


if(MSVC)
  # Force to always compile with W4
  if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
  endif()
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  # Update if necessary
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-long-long -pedantic")
endif()
