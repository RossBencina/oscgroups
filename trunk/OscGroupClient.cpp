/*
OSCgroups -- open sound control groupcasting infrastructure
Copyright (C) 2005  Ross Bencina

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/


#include "OscGroupClient.h"

#include <cstring>
#include <iostream>
#include <vector>
#include <ctime>
#include <string>
#include <cassert>
#include <cstdlib>

#include "osc/OscReceivedElements.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscPacketListener.h"

#include "ip/UdpSocket.h"
#include "ip/IpEndpointName.h"
#include "ip/PacketListener.h"
#include "ip/TimerListener.h"

#include "md5.h"


/*
	There are three sockets:
		externalSocket is used to send and receive data from the network
        including the server and other peers
        
		localRxSocket is used to forward data to the local (client)
        application from other peers

		localTxSocket is used to recieve data from the local (client)
        application to be forwarded to other peers


    Some behavioral rules:

        the choice of which peer endpoint to forward traffic to is made based
        on which endpoints we have received pings from. if pings have been
        received from multiple endpoints preference is given to the private
        endpoint. as soon as a ping has been received we start forwarding data
        
        during the establishment phase we ping all endpoints repeatedly
        even if we have already received pings from an endpoint.

        during the establishment phase we initially send pings faster, and then
        gradually return to a slower rate.

        the establishment phase ends when:
            - we receive a ping from the private endpoint

            - we receive a ping from any endpoint, and
                ESTABLISHMENT_PING_PERIOD_COUNT ping periods have elapsed

        if the server indicates that any of the port or address information of
        the peer has changed, we restart the establishment process.

        if a ping is received from an endpoint which we haven't received from
        before we restart the establishment phase.

        pings are sent less frequently if the channel is being kept open
        by forwarded traffic. We ensure that some traffic is sent accross
        the link every IDLE_PING_PERIOD_SECONDS and that a ping is sent
        accross the link at least every ACTIVE_PING_PERIOD_SECONDS


        if the last time at which the server has heard from a peer
        AND the last time from which we received a ping from the peer
        exceeds PURGE_PEER_TIMEOUT_SECONDS then the peer is removed from
        the peers list.

        
        if we havn't received a ping from a peer in
        PEER_ESTABLISHMENT_RETRY_TIME_SECONDS then we attempt to re-establish
        the connection.



    TODO:

        o- report ping times (requires higher resolution timer i think)


    -----------

        
    
*/




#define PURGE_PEER_TIMEOUT_SECONDS          180              // time before removing peer from active list

#define PEER_ESTABLISHMENT_RETRY_TIME_SECONDS    60

#define IDLE_PING_PERIOD_SECONDS            6              // seconds between pings when the link is idle

#define ACTIVE_PING_PERIOD_SECONDS          30              // seconds between pings when the link is active

#define ESTABLISHMENT_PING_PERIOD_COUNT     5               // 5 pings are always sent to all peer endpoints



struct PeerEndpoint{
    PeerEndpoint()
        : pingReceived( false )
        , sentPingsCount( 0 )
        , forwardedPacketsCount( 0 ) {}

    IpEndpointName endpointName;
    bool pingReceived;
    std::time_t lastPingReceiveTime;

    int sentPingsCount;
    std::time_t lastPingSendTime;

    int forwardedPacketsCount;
    std::time_t lastPacketForwardTime;
};


struct Peer{
    Peer( const char *userName )
        : name( userName )
        , pingPeriodCount( 0 ) {}
        
    std::string name;

    std::time_t lastUserInfoReceiveTime;
    int secondsSinceLastAliveReceivedByServer;
    
    /*
        we maintain three addresses for each peer. the private address is the
        address the peer thinks it is, the public is the address that the
        peer appears as to the server, and the ping address is the address
        that the client appears as to us when we receive a ping from it. this
        last address is necessary for half cone (symmetric) NATs which assign
        the peer a different port to talk to us than the one they assigned to
        talk to the server.
    */

	PeerEndpoint privateEndpoint;
    PeerEndpoint publicEndpoint;
    PeerEndpoint pingEndpoint;

    int pingPeriodCount;
    std::time_t lastPingPeriodTime;

    std::time_t MostRecentActivityTime() const
    {
        // the most recent activity time is the most recent time either the
        // server heard from the peer, or we received a ping from the peer
    
        std::time_t lastUserInfoReceivedByTheServerTime =
                lastUserInfoReceiveTime - secondsSinceLastAliveReceivedByServer; // FIXME: assumes time_t is in seconds

        std::time_t result = lastUserInfoReceivedByTheServerTime;
        if( privateEndpoint.pingReceived )
            result = std::max( result, privateEndpoint.lastPingReceiveTime );
        if( publicEndpoint.pingReceived )
            result = std::max( result, publicEndpoint.lastPingReceiveTime );
        if( pingEndpoint.pingReceived )
            result = std::max( result, pingEndpoint.lastPingReceiveTime );

        return result;
    }

};


static std::vector<Peer> peers_;


class ExternalCommunicationsSender : public TimerListener {
    #define IP_MTU_SIZE 1536
    char aliveBuffer_[IP_MTU_SIZE];
    std::size_t aliveSize_;
    std::time_t lastAliveSentTime_;
    
    char pingBuffer_[IP_MTU_SIZE];
    std::size_t pingSize_;

    UdpSocket& externalSocket_;
    IpEndpointName remoteServerEndpoint_;
    IpEndpointName localToServerEndpoint_;

    std::string userName_;
    std::string userPassword_;
    std::string groupName_;
    std::string groupPassword_;

    void PrepareAliveBuffer()
    {
        osc::OutboundPacketStream p( aliveBuffer_, IP_MTU_SIZE );

        p << osc::BeginBundle();

        p << osc::BeginMessage( "/groupserver/user_alive" )
                    << userName_.c_str()
                    << userPassword_.c_str()
                    << ~((osc::int32)localToServerEndpoint_.address)
                    << (osc::int32)localToServerEndpoint_.port
                    << groupName_.c_str()
                    << groupPassword_.c_str()
                    << osc::EndMessage;

        p << osc::BeginMessage( "/groupserver/get_group_users_info" )
                    << groupName_.c_str()
                    << groupPassword_.c_str()
                    << osc::EndMessage;

        p << osc::EndBundle;

        aliveSize_ = p.Size();
    }

    void SendAlive( std::time_t currentTime )
    {
        int secondsSinceLastAliveSent = (int)difftime(currentTime, lastAliveSentTime_);
        if( secondsSinceLastAliveSent >= IDLE_PING_PERIOD_SECONDS ){
           
            externalSocket_.SendTo( remoteServerEndpoint_, aliveBuffer_, aliveSize_ );

            lastAliveSentTime_ = currentTime;
        }   
    }

    void PreparePingBuffer()
    {
        osc::OutboundPacketStream p( pingBuffer_, IP_MTU_SIZE );

        p << osc::BeginBundle();

        p << osc::BeginMessage( "/groupclient/ping" )
                    << userName_.c_str()
                    << osc::EndMessage;

        p << osc::EndBundle;

        pingSize_ = p.Size();
    }

    void SendPing( PeerEndpoint& to, std::time_t currentTime )
	{
		char addressString[ IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH ];
		to.endpointName.AddressAndPortAsString( addressString );
         
        std::cout << "sending ping to " << addressString << "\n";

        externalSocket_.SendTo( to.endpointName, pingBuffer_, pingSize_ );
        ++to.sentPingsCount;
        to.lastPingSendTime = currentTime;
	}

    
    PeerEndpoint* SelectEndpoint( Peer& peer )
    {
        if( peer.privateEndpoint.pingReceived ){

            return &peer.privateEndpoint;

        }else if( peer.publicEndpoint.pingReceived ){

            return &peer.publicEndpoint;

        }else if( peer.pingEndpoint.pingReceived ){

            return &peer.pingEndpoint;
        }

        return 0;
    }


    void PollPeerPingTimer( Peer& peer, std::time_t currentTime, bool executeNowIgnoringTimeouts=false )
    {
        bool noPingsReceivedYet =
                !peer.privateEndpoint.pingReceived
                && !peer.publicEndpoint.pingReceived
                && !peer.pingEndpoint.pingReceived;

        if( !noPingsReceivedYet ){
            // check whether we should attempt to re-establish the link
            // due to no traffic arriving for PEER_ESTABLISHMENT_RETRY_TIME_SECONDS

            std::time_t mostRecentPingTime = 0;
            if( peer.privateEndpoint.pingReceived )
                mostRecentPingTime = std::max( mostRecentPingTime, peer.privateEndpoint.lastPingReceiveTime );
            if( peer.publicEndpoint.pingReceived )
                mostRecentPingTime = std::max( mostRecentPingTime, peer.publicEndpoint.lastPingReceiveTime );
            if( peer.pingEndpoint.pingReceived )
                mostRecentPingTime = std::max( mostRecentPingTime, peer.pingEndpoint.lastPingReceiveTime );
            
            if( (int)std::difftime(currentTime, mostRecentPingTime) > PEER_ESTABLISHMENT_RETRY_TIME_SECONDS ){

                peer.pingPeriodCount = 0;
                executeNowIgnoringTimeouts = true;
            }
        }


        bool inEstablishmentPhase =
                ( ( peer.pingPeriodCount < ESTABLISHMENT_PING_PERIOD_COUNT )
                &&  ( !peer.privateEndpoint.pingReceived ) )
                || noPingsReceivedYet;

        if( inEstablishmentPhase ){
            int pingPeriod;
            if( peer.pingPeriodCount < ESTABLISHMENT_PING_PERIOD_COUNT ){
            
                pingPeriod = (int) (IDLE_PING_PERIOD_SECONDS *
                        ((double)(peer.pingPeriodCount + 1) / (double) ESTABLISHMENT_PING_PERIOD_COUNT));

            }else{
                pingPeriod = IDLE_PING_PERIOD_SECONDS;
            }

            if( currentTime >= (peer.lastPingPeriodTime + pingPeriod)
                    || executeNowIgnoringTimeouts ){
                SendPing( peer.privateEndpoint, currentTime );
                SendPing( peer.publicEndpoint, currentTime );
                if( peer.pingEndpoint.pingReceived )
                    SendPing( peer.pingEndpoint, currentTime );

                peer.lastPingPeriodTime = currentTime;
                ++peer.pingPeriodCount;
            }
        
        }else{

            PeerEndpoint *peerEndpointToUse = SelectEndpoint( peer );
            assert( peerEndpointToUse != 0 );

            bool sendPing = false;

            if( executeNowIgnoringTimeouts ){

                sendPing = true;

            }else{
            
                if( peerEndpointToUse->sentPingsCount == 0 ){

                    sendPing = true;

                }else{

                    int secondsSinceLastPing = (int)std::difftime(currentTime, peerEndpointToUse->lastPingSendTime);

                    if( peerEndpointToUse->forwardedPacketsCount == 0 ){

                        if( secondsSinceLastPing >= IDLE_PING_PERIOD_SECONDS ){
                        
                            sendPing = true;
                        }

                    }else{

                        int secondsSinceLastForwardedTraffic =
                                (int)std::difftime(currentTime, peerEndpointToUse->lastPacketForwardTime);

                        if( secondsSinceLastForwardedTraffic >= IDLE_PING_PERIOD_SECONDS ){

                            if( secondsSinceLastPing >= IDLE_PING_PERIOD_SECONDS ){
                                sendPing = true;
                            }

                        }else if( secondsSinceLastPing >= ACTIVE_PING_PERIOD_SECONDS ){
                            sendPing = true;
                        }
                    }
                }
            }

            if( sendPing ){
                SendPing( *peerEndpointToUse, currentTime );
                peer.lastPingPeriodTime = currentTime;
                ++peer.pingPeriodCount;
            }
        }
    }

    ExternalCommunicationsSender(); // no default ctor
    ExternalCommunicationsSender( const ExternalCommunicationsSender& ); // no copy ctor
    ExternalCommunicationsSender& operator=( const ExternalCommunicationsSender& ); // no assignment operator
    
public:
    ExternalCommunicationsSender( UdpSocket& externalSocket,
			IpEndpointName remoteServerEndpoint,
			int localToRemotePort,
            const char *userName, const char *userPassword,
            const char *groupName, const char *groupPassword )
        : lastAliveSentTime_( 0 )
        , externalSocket_( externalSocket )
		, remoteServerEndpoint_( remoteServerEndpoint )
		, localToServerEndpoint_( 
				externalSocket.LocalEndpointFor( remoteServerEndpoint ).address, 
				localToRemotePort )
        , userName_( userName )
        , userPassword_( userPassword )
        , groupName_( groupName )
        , groupPassword_( groupPassword )
    {
        PrepareAliveBuffer();
        PreparePingBuffer();
    }


    void RestartPeerCommunicationEstablishment( Peer& peer, std::time_t currentTime )
    {
        peer.pingPeriodCount = 0;
        PollPeerPingTimer( peer, currentTime, true );
    }


    void ForwardPacketToAllPeers( const char *data, int size )
    {
        std::time_t currentTime = std::time(0);
        
        for( std::vector<Peer>::iterator i = peers_.begin(); i != peers_.end(); ++i ){

            PeerEndpoint *peerEndpointToUse = SelectEndpoint( *i );
            if( peerEndpointToUse ){
                externalSocket_.SendTo( peerEndpointToUse->endpointName, data, size );
                ++peerEndpointToUse->forwardedPacketsCount;
                peerEndpointToUse->lastPacketForwardTime = currentTime;
            }
        }       
    }

    
    virtual void TimerExpired()
    {        
        std::time_t currentTime = std::time(0);

        SendAlive( currentTime );

        // check for peers to purge, 
        std::vector<Peer>::iterator i = peers_.begin();
        while( i != peers_.end() ){

            if( difftime(currentTime,i->MostRecentActivityTime()) > PURGE_PEER_TIMEOUT_SECONDS ){

                i = peers_.erase( i );

            }else{
                PollPeerPingTimer( *i, currentTime );
                ++i;
            }       
        }
    }
};


class ExternalSocketListener : public osc::OscPacketListener {

    void user_alive_status( const osc::ReceivedMessage& m, const IpEndpointName& remoteEndpoint )
    {
        // only accept user_alive_status from the server
        if( remoteEndpoint != remoteServerEndpoint_ )
            return;

        // /groupclient/user_alive_status userName userPassword status

        osc::ReceivedMessageArgumentStream args = m.ArgumentStream();

        const char *userName, *userPassword, *status;

        args >> userName >> userPassword >> status;

        if( std::strcmp( userName, userName_ ) == 0
                &&  std::strcmp( userPassword, userPassword_ ) == 0 ){
            // message really is for us

            if( std::strcmp( status, "ok" ) == 0 ){

                std::cout << "ok: user '" << userName << "' is registered with server\n";

            }else{
                std::cout << "user registration error: server returned status of '" << status
                        << "' for user '" << userName << "'\n";
            }
        }
    }

    void user_group_status( const osc::ReceivedMessage& m, const IpEndpointName& remoteEndpoint )
    {
        // only accept user_alive_status from the server
        if( remoteEndpoint != remoteServerEndpoint_ )
            return;

        // /groupclient/user_group_status userName userPassword groupName groupPassword status

        osc::ReceivedMessageArgumentStream args = m.ArgumentStream();

        const char *userName, *userPassword, *groupName, *groupPassword, *status;

        args >> userName >> userPassword >> groupName >> groupPassword >> status;

        if( std::strcmp( userName, userName_ ) == 0
                && std::strcmp( userPassword, userPassword_ ) == 0
                && std::strcmp( groupName, groupName_ ) == 0
                && std::strcmp( groupPassword, groupPassword_ ) == 0 ){
            // message really is for us

            if( std::strcmp( status, "ok" ) == 0 ){

                std::cout << "ok: user '" << userName << "' is a member of group '" << groupName << "'\n";

            }else{
                std::cout << "group membership error: server returned status of '" << status
                    << "' for user '" << userName
                    << "' membership of group '" << groupName << "'\n";
            }
        }
    }

    void user_info( const osc::ReceivedMessage& m, const IpEndpointName& remoteEndpoint )
    {
        // only accept user_info from the server
        if( remoteEndpoint != remoteServerEndpoint_ )
            return;

        // /groupclient/user_info userName privateIpAddress privatePort
        //      publicIpAddress publicPort secondsSinceLastAlive group0 group1 ...

        osc::ReceivedMessageArgumentStream args = m.ArgumentStream();

        const char *userName;
        osc::int32 privateAddress;
        osc::int32 privatePort;
        osc::int32 publicAddress;
        osc::int32 publicPort;
        osc::int32 secondsSinceLastAlive;

        args >> userName >> privateAddress >> privatePort >>
                publicAddress >> publicPort >> secondsSinceLastAlive;

        // addresses are transmitted as ones complement (bit inverse)
        // to avoid problems with buggy NATs trying to re-write addresses
        privateAddress = ~privateAddress;
        publicAddress = ~publicAddress;

		IpEndpointName privateEndpoint( privateAddress, privatePort );
		IpEndpointName publicEndpoint( publicAddress, publicPort );

		char privateAddressString[ IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH ];
		privateEndpoint.AddressAndPortAsString( privateAddressString );
        char publicAddressString[ IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH ];
		publicEndpoint.AddressAndPortAsString( publicAddressString );
        
        std::cout << "user info received for '" << userName << "', "
            << "private: " << privateAddressString
            << " public: " << publicAddressString
            << "\n";
                
        if( std::strcmp( userName, userName_ ) == 0 )
            return; // discard info referring to ourselves


        bool userIsInGroup = false;
        while( !args.Eos() ){
            const char *groupName;
            args >> groupName;
            if( std::strcmp( groupName, groupName_ ) == 0 ){
                userIsInGroup = true;
                break;
            }
        }
        

        if( userIsInGroup ){
            bool restartPeerCommunicationEstablishment = false;
            
            bool found = false;
            std::vector<Peer>::iterator peer;
            for( std::vector<Peer>::iterator i = peers_.begin(); i != peers_.end(); ++i ){

                if( i->name.compare( userName ) == 0 ){
                    peer = i;
                    found = true;
                    break;
                }
            }

            if( !found ){
                peers_.push_back( Peer( userName ) );
                peer = peers_.end() - 1;
                restartPeerCommunicationEstablishment = true;
            }

            if( peer->privateEndpoint.endpointName != privateEndpoint ){
                peer->privateEndpoint.endpointName = privateEndpoint;
                peer->privateEndpoint.pingReceived = false;
                peer->privateEndpoint.forwardedPacketsCount = 0;
                peer->pingEndpoint.pingReceived = false;
                peer->pingEndpoint.forwardedPacketsCount = 0;
                restartPeerCommunicationEstablishment = true;
            }

            if( peer->publicEndpoint.endpointName != publicEndpoint ){
                peer->publicEndpoint.endpointName = publicEndpoint;
                peer->publicEndpoint.pingReceived = false;
                peer->publicEndpoint.forwardedPacketsCount = 0;
                peer->pingEndpoint.pingReceived = false;
                peer->pingEndpoint.forwardedPacketsCount = 0;
                restartPeerCommunicationEstablishment = true;
            }


            peer->secondsSinceLastAliveReceivedByServer = secondsSinceLastAlive;

            std::time_t currentTime = std::time(0);
            peer->lastUserInfoReceiveTime = currentTime;

            if( restartPeerCommunicationEstablishment )
                externalCommunicationsSender_.RestartPeerCommunicationEstablishment( *peer, currentTime );
            
        }else{
            // fixme should remove user from peer list if it is present
        }
    }

    void ping( const osc::ReceivedMessage& m, const IpEndpointName& remoteEndpoint )
    {
        osc::ReceivedMessageArgumentStream args = m.ArgumentStream();

        const char *userName;
        // osc::TimeTag timeSent;

        // TODO:
        // support 3 variants of the ping message:
        // /ping userName (basic version, only one needed for compatibility)
        // /ping userName timeSent
        //        response -> /ping userName timeSent inResponseToUserName inResponseToTimeSent

        args >> userName >> osc::EndMessage;

		char sourceAddressString[ IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH ];
		remoteEndpoint.AddressAndPortAsString( sourceAddressString );
       
        std::cout << "ping recieved from '" << userName << "' at "
                << sourceAddressString  << "\n";

        for( std::vector<Peer>::iterator i = peers_.begin(); i != peers_.end(); ++i ){

            if( i->name.compare( userName ) == 0 ){
                bool restartPeerCommunicationEstablishment = false;

                std::time_t currentTime = std::time(0);

				if( remoteEndpoint == i->privateEndpoint.endpointName ){

                    restartPeerCommunicationEstablishment = !i->privateEndpoint.pingReceived;
                    i->privateEndpoint.pingReceived = true;
                    i->privateEndpoint.lastPingReceiveTime = currentTime;

                }else if( remoteEndpoint == i->publicEndpoint.endpointName ){

                    restartPeerCommunicationEstablishment = !i->publicEndpoint.pingReceived;
                    i->publicEndpoint.pingReceived = true;
                    i->publicEndpoint.lastPingReceiveTime = currentTime;
                    
                }else{
					// otherwise assume the messages is coming from the ping endpoint

                    restartPeerCommunicationEstablishment = ( !i->pingEndpoint.pingReceived
                            || i->pingEndpoint.endpointName != remoteEndpoint );
                            
                    i->pingEndpoint.endpointName = remoteEndpoint;
                    i->pingEndpoint.pingReceived = true;
                    i->pingEndpoint.lastPingReceiveTime = currentTime;
                }

                if( restartPeerCommunicationEstablishment )
                    externalCommunicationsSender_.RestartPeerCommunicationEstablishment( *i, currentTime );

                break;
            }
        }
    }
    
protected:

    virtual void ProcessMessage( const osc::ReceivedMessage& m, 
			const IpEndpointName& remoteEndpoint )
    {
        try{
    
            if( std::strcmp( m.AddressPattern(), "/groupclient/user_info" ) == 0 ){
                user_info( m, remoteEndpoint );
            }else if( std::strcmp( m.AddressPattern(), "/groupclient/ping" ) == 0 ){
                ping( m, remoteEndpoint );
            }else if( std::strcmp( m.AddressPattern(), "/groupclient/user_alive_status" ) == 0 ){
                user_alive_status( m, remoteEndpoint );
            }else if( std::strcmp( m.AddressPattern(), "/groupclient/user_group_status" ) == 0 ){
                user_group_status( m, remoteEndpoint );
            }

        }catch( osc::Exception& e ){
            std::cout << "error while parsing message: " << e.what() << "\n";
        }
    }

    IpEndpointName remoteServerEndpoint_;

	const char *userName_;
    const char *userPassword_;
    const char *groupName_;
    const char *groupPassword_;

    UdpTransmitSocket localRxSocket_;

    ExternalCommunicationsSender& externalCommunicationsSender_;

    ExternalSocketListener(); // no default ctor
    ExternalSocketListener( const ExternalSocketListener& ); // no copy ctor
    ExternalSocketListener& operator=( const ExternalSocketListener& ); // no assignment operator

public:
    ExternalSocketListener( const IpEndpointName& remoteServerEndpoint, 
			int localRxPort, const char *userName, const char *userPassword,
            const char *groupName, const char *groupPassword,
            ExternalCommunicationsSender& externalCommunicationsSender )
        : remoteServerEndpoint_( remoteServerEndpoint )
        , userName_( userName )
        , userPassword_( userPassword )
        , groupName_( groupName )
        , groupPassword_( groupPassword )
        , localRxSocket_( IpEndpointName( "localhost", localRxPort ) )
        , externalCommunicationsSender_( externalCommunicationsSender )
    {
    }

    virtual void ProcessPacket( const char *data, int size, 
			const IpEndpointName& remoteEndpoint )
    {
        // for now we parse _all_ packets, and pass all those on to clients
        // except those which come from the server. ideally we should avoid
        // parsing most packets except the ones containing pings, or perhaps
        // only process non-bundled pings.

        // in the future it could be useful to register which peer a packet
        // is coming from so that we can keep track of channel activity
        // not just by receiving pings but also by recieving other traffic
        // this would also allow us to reject packets from unknown sources

        
        osc::OscPacketListener::ProcessPacket( data, size, remoteEndpoint );
                    
        if( remoteEndpoint != remoteServerEndpoint_ ){
         
            // forward packet to local receive socket

            localRxSocket_.Send( data, size );
        }
    }
};


class LocalTxSocketListener : public PacketListener {

    ExternalCommunicationsSender& externalCommunicationsSender_;
    
    LocalTxSocketListener(); // no default ctor
    LocalTxSocketListener( const LocalTxSocketListener& ); // no copy ctor
    LocalTxSocketListener& operator=( const LocalTxSocketListener& ); // no assignment operator

public:
    LocalTxSocketListener( ExternalCommunicationsSender& externalCommunicationsSender )
        : externalCommunicationsSender_( externalCommunicationsSender )
    {
    }

	virtual void ProcessPacket( const char *data, int size, 
			const IpEndpointName& remoteEndpoint )
    {
        (void) remoteEndpoint; // suppress unused parameter warning
        externalCommunicationsSender_.ForwardPacketToAllPeers( data, size );
    }
};


char IntToHexDigit( int n )
{
    if( n < 10 )
        return (char)('0' + n);
    else
        return (char)('a' + (n-10));
}

void MakeHashString( char *dest, const char *src )
{
    MD5_CTX md5Context;
    MD5Init( &md5Context );
    MD5Update( &md5Context, (unsigned char*)src, (unsigned int)std::strlen(src) );
    unsigned char numericHash[16];
    MD5Final( numericHash, &md5Context );
    char *p = dest;
    for( int i=0; i < 16; ++i ){

        *p++ = IntToHexDigit(((unsigned char)numericHash[i] >> 4) & 0x0F);
        *p++ = IntToHexDigit((unsigned char)numericHash[i] & 0x0F);
    }
    *p = '\0';

    //printf( "src: %s dest: %s\n", src, dest );
}

void SanityCheckMd5()
{
    // if anything in this function fails there's a problem with your build configuration
    
    // check that the size of types declared in md5.h are correct
    assert( sizeof(UINT2) == 2 );
    assert( sizeof(UINT4) == 4 );

    // sanity check that the hash is working by comparing to a known good hash:
    char testHash[33];
    MakeHashString( testHash, "0123456789" );
    assert( std::strcmp( testHash, "781e5e245d69b566979b86e28d23f2c7" ) == 0 );
}

void RunOscGroupClientUntilSigInt( 
		const IpEndpointName& serverRemoteEndpoint, 
		int localToRemotePort, int localTxPort, int localRxPort, 
		const char *userName, const char *userPassword, 
		const char *groupName, const char *groupPassword )
{
    // used hashed passwords instead of the user supplied ones
    
    char userPasswordHash[33];
    MakeHashString( userPasswordHash, userPassword );

    char groupPasswordHash[33];
    MakeHashString( groupPasswordHash, groupPassword );

	UdpReceiveSocket externalSocket( 
			IpEndpointName( IpEndpointName::ANY_ADDRESS, localToRemotePort ) );

	UdpReceiveSocket localTxSocket( localTxPort );

    ExternalCommunicationsSender externalCommunicationsSender( externalSocket, 
			serverRemoteEndpoint, localToRemotePort, 
			userName, userPasswordHash, groupName, groupPasswordHash );

    ExternalSocketListener externalSocketListener( 
			serverRemoteEndpoint, localRxPort,
            userName, userPasswordHash, groupName, groupPasswordHash,
            externalCommunicationsSender );

    LocalTxSocketListener localTxSocketListener( externalCommunicationsSender );

	SocketReceiveMultiplexer mux;
    mux.AttachPeriodicTimerListener( 0, (IDLE_PING_PERIOD_SECONDS * 1000) / 10, &externalCommunicationsSender );
	mux.AttachSocketListener( &externalSocket, &externalSocketListener );
	mux.AttachSocketListener( &localTxSocket, &localTxSocketListener );    

	std::cout << "running...\n";
	std::cout << "press ctrl-c to end\n";

	mux.RunUntilSigInt();

	std::cout << "finishing.\n";	
}


int oscgroupclient_main(int argc, char* argv[])
{
    SanityCheckMd5();
    
    try{
        if( argc != 10 ){
            std::cout << "usage: oscgroupclient serveraddress serverport localtoremoteport localtxport localrxport username password groupname grouppassword\n";
            std::cout << "users should send data to localhost:localtxport and listen on localhost:localrxport\n";
            return 0;
        }

		IpEndpointName serverRemoteEndpoint( argv[1], atoi( argv[2] ) );
        int localToRemotePort = std::atoi( argv[3] );
        int localTxPort = std::atoi( argv[4] );
        int localRxPort = std::atoi( argv[5] );
        const char *userName = argv[6];
        const char *userPassword = argv[7];
        const char *groupName = argv[8];
        const char *groupPassword = argv[9];

		char serverAddressString[ IpEndpointName::ADDRESS_AND_PORT_STRING_LENGTH ];
		serverRemoteEndpoint.AddressAndPortAsString( serverAddressString );

        std::cout << "oscgroupclient\n";
        std::cout << "connecting to group '" << groupName << "' as user '" << userName << "'.\n";
        std::cout << "using server at " << serverAddressString
                        << " with external traffic on local port " << localToRemotePort << "\n";
        std::cout << "--> send outbound traffic to localhost port " << localTxPort << "\n";
        std::cout << "<-- listen for inbound traffic on localhost port " << localRxPort << "\n";

        RunOscGroupClientUntilSigInt( serverRemoteEndpoint, localToRemotePort,
                localTxPort, localRxPort, userName, userPassword, groupName, groupPassword );

    }catch( std::exception& e ){
        std::cout << e.what() << std::endl;
    }
    
    return 0;
}


#ifndef NO_MAIN

int main(int argc, char* argv[])
{
    return oscgroupclient_main( argc, argv );
}

#endif /* NO_MAIN */

