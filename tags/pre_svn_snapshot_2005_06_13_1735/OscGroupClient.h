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

#ifndef INCLUDED_OSCGROUPCLIENT_H
#define INCLUDED_OSCGROUPCLIENT_H


class IpEndpointName;

void RunOscGroupClientUntilSigInt( 
		const IpEndpointName& serverRemoteEndpoint, 
		int localToRemotePort, int localTxPort, int localRxPort, 
		const char *userName, const char *userPassword, 
		const char *groupName, const char *groupPassword );

int oscgroupclient_main( int argc, char* argv[] );


#endif /* INCLUDED_OSCGROUPCLIENT_H */
