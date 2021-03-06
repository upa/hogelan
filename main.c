#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <string.h>
#include <net/if.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <syslog.h>


#include "common.h"
#include "sockaddrmacro.h"
#include "error.h"
#include "net.h"
#include "fdb.h"
#include "iftap.h"
#include "vxlan.h"
#include "control.h"


struct vxlan vxlan;

void process_vxlan (void);

void debug_print_vhdr (struct vxlan_hdr * vhdr);
void debug_print_ether (struct ether_header * ether);


void
usage (void)
{
	printf ("\n"
		" Usage\n"
		"\n"
		"   vxland -m [MCASTADDR] -i [INTERFACE]"
		"\n"
		"\t -m : Multicast Address(v4/v6)\n"
		"\t -i : Multicast Interface\n"
		"\t -e : Print Error Massage to STDOUT\n"
		"\t -d : Daemon Mode\n"
		"\t -h : Print Usage (this message)\n"
		"\n"
		"   vxland is forwarding daemon. Please configure using vxlanctl.\n"
		"\n"
		);

	return;
}

void
cleanup (void)
{
	int n, vins_num;
	struct vxlan_instance ** vins_list;
	
	/* stop control thread */
	pthread_cancel (vxlan.control_tid);

	/* stop all vxlan instance */
	vins_list = (struct vxlan_instance **)
		create_list_from_hash (&vxlan.vins_tuple, &vins_num);

	for (n = 0; n < vins_num; n++) {
		destroy_vxlan_instance (vins_list[n]);
	}
	
	destroy_hash (&vxlan.vins_tuple);

	/* close sockets */
	close (vxlan.udp_sock);
	close (vxlan.control_sock);
	
	return;
}

void 
sig_cleanup (int signal)
{
	cleanup ();
}


int
main (int argc, char * argv[])
{
	int ch;
	int d_flag = 0, err_flag = 0;

	extern int opterr;
	extern char * optarg;
	struct addrinfo hints, *res;

	char mcast_caddr[40] = "";
	char vxlan_if_name[IFNAMSIZ] = "";

	memset (&vxlan, 0, sizeof (vxlan));
	init_hash (&vxlan.vins_tuple, VXLAN_VNISIZE);

	while ((ch = getopt (argc, argv, "ehm:di:")) != -1) {
		switch (ch) {
		case 'e' :
			err_flag = 1;
			break;
			
		case 'm' :
			strncpy (mcast_caddr, optarg, sizeof (mcast_caddr));

			break;

		case 'i' :
			strcpy (vxlan_if_name, optarg);
			
			break;

		case 'd' :
			d_flag = 1;
			break;

		case 'h' :
			usage ();
			return 0;
			
		default :
			usage ();
			return -1;
		}
	}

	if (d_flag > 0) {
		if (daemon (1, err_flag) < 0)
			err (EXIT_FAILURE, "failed to run as a daemon");
	}

	/* Create UDP Mulciast Socket */

        vxlan.port = VXLAN_PORT_BASE;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
			
	if (getaddrinfo (mcast_caddr, VXLAN_CPORT, &hints, &res) != 0) {
		error_quit ("Invalid Multicast Address \"%s\"", mcast_caddr);
	}

	if ((vxlan.udp_sock = socket (res->ai_family, 
				      res->ai_socktype,
				      res->ai_protocol)) < 0)
		err (EXIT_FAILURE, "can not create socket");

	memcpy (&vxlan.mcast_addr, res->ai_addr, res->ai_addrlen);
	
	freeaddrinfo (res);

	switch (((struct sockaddr *)&vxlan.mcast_addr)->sa_family) {
	case AF_INET :
		bind_ipv4_inaddrany (vxlan.udp_sock, vxlan.port);
		set_ipv4_multicast_join_and_iface (vxlan.udp_sock, 
						   ((struct sockaddr_in *)
						    &vxlan.mcast_addr)->sin_addr,
						   vxlan_if_name);
		set_ipv4_multicast_loop (vxlan.udp_sock, 0);
		set_ipv4_multicast_ttl (vxlan.udp_sock, VXLAN_MCAST_TTL);
		break;

	case AF_INET6 :
		bind_ipv6_inaddrany (vxlan.udp_sock, vxlan.port);
		set_ipv6_multicast_join_and_iface (vxlan.udp_sock,
						   ((struct sockaddr_in6 *)
						    &vxlan.mcast_addr)->sin6_addr,
						   vxlan_if_name);
		set_ipv6_multicast_loop (vxlan.udp_sock, 0);
		set_ipv6_multicast_ttl (vxlan.udp_sock, VXLAN_MCAST_TTL);
		break;

	default :
		error_quit ("unkown protocol family");
	}


	((struct sockaddr_in *)&vxlan.mcast_addr)->sin_port = htons (vxlan.port);
	

	/* Start Control Thread */
	init_vxlan_control ();

        /* Enable syslog */
	if (err_flag == 0) {
		openlog (VXLAN_LOGNAME, 
			 LOG_CONS | LOG_PERROR, 
			 VXLAN_LOGFACILITY);
		error_enable_syslog();
		syslog (LOG_INFO, "vxlan start");
	}


	/* set signal handler */
	if (atexit (cleanup) < 0)
		err (EXIT_FAILURE, "failed to register exit hook");

	/*
	if (signal (SIGINT, sig_cleanup) == SIG_ERR)
		err (EXIT_FAILURE, "failed to register SIGINT hook");
	*/

	process_vxlan ();


	return 0;
}


void
process_vxlan (void)
{
	int len;
	char buf[VXLAN_PACKET_BUF_LEN];
	fd_set fds;

	struct vxlan_hdr 	* vhdr;
	struct ether_header 	* ether;
	struct sockaddr_storage sa_str;
	struct vxlan_instance 	* vins;
	socklen_t s_t = sizeof (sa_str);

	memset (buf, 0, sizeof (buf));

	/* From Internet */
	while (1) {

		FD_ZERO (&fds);
		FD_SET (vxlan.udp_sock, &fds);
		
		pselect (vxlan.udp_sock + 1, &fds, NULL, NULL, NULL, NULL);
		
		if (!FD_ISSET (vxlan.udp_sock, &fds))
			break;

		memset (&sa_str, 0, sizeof (sa_str));
		if ((len = recvfrom (vxlan.udp_sock, buf, sizeof (buf), 0,	
				     (struct sockaddr *)&sa_str, &s_t)) < 0) 
			continue;

		vhdr = (struct vxlan_hdr *) buf;
		if ((vins = search_hash (&vxlan.vins_tuple, vhdr->vxlan_vni)) == NULL) {
			error_warn ("invalid VNI %02x%02x%02x", 
				    vhdr->vxlan_vni[0], 
				    vhdr->vxlan_vni[1], 
				    vhdr->vxlan_vni[2]);
			continue;
		}
			
		ether = (struct ether_header *) (buf + sizeof (struct vxlan_hdr));
		process_fdb_etherflame_from_vxlan (vins, ether, &sa_str);
		send_etherflame_from_vxlan_to_local (vins, ether, 
						     len - sizeof (struct vxlan_hdr));

	}

	return;
}


void 
debug_print_vhdr (struct vxlan_hdr * vhdr)
{
	printf ("vxlan header\n");
	printf ("Flag : %u\n", vhdr->vxlan_flags);
	printf ("VNI  : %u%u%u\n", 
		vhdr->vxlan_vni[0], 
		vhdr->vxlan_vni[1], 
		vhdr->vxlan_vni[2]);
	printf ("\n");

	return;
}

void
debug_print_ether (struct ether_header * ether) 
{
	printf ("Mac\n");
	printf ("DST : %02x:%02x:%02x:%02x:%02x:%02x\n",
		ether->ether_dhost[0], ether->ether_dhost[1], 
		ether->ether_dhost[2], ether->ether_dhost[3], ether->ether_dhost[4], 
		ether->ether_dhost[5]);
	printf ("SRC : %02x:%02x:%02x:%02x:%02x:%02x\n",
		ether->ether_shost[0], ether->ether_shost[1], 
		ether->ether_shost[2], ether->ether_shost[3], ether->ether_shost[4],
		ether->ether_shost[5]);
	return;
}

