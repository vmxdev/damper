#ifndef damper_h_included
#define damper_h_included

#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <unistd.h>

/* IP header */
struct damper_ip_header
{
	uint8_t  ip_vhl;                /* version << 4 | header length >> 2 */
	uint8_t  ip_tos;                /* type of service */
	uint16_t ip_len;                /* total length */
	uint16_t ip_id;                 /* identification */
	uint16_t ip_off;                /* fragment offset field */
	#define IP_RF 0x8000            /* reserved fragment flag */
	#define IP_DF 0x4000            /* dont fragment flag */
	#define IP_MF 0x2000            /* more fragments flag */
	#define IP_OFFMASK 0x1fff       /* mask for fragmenting bits */
	uint8_t  ip_ttl;                /* time to live */
	uint8_t  ip_p;                  /* protocol */
	uint16_t ip_sum;                /* checksum */
	struct  in_addr ip_src,ip_dst;  /* source and dest address */
};

#define DAMPER_MAX_PACKET_SIZE 0xffff

struct mpacket
{
	int size;
	char packet[DAMPER_MAX_PACKET_SIZE];
};

struct userdata
{
	int socket;
	struct sockaddr_in daddr;

	int queue; /* nfqueue queue id */

	struct mpacket *packets;
	double *prioarray;
	size_t qlen;

	int stat; /* enable statistics */
	uint64_t limit;

	int mark; /* reinjected packets mark */
	int dscp; /* and we can set DSCP (Differenciated Services Code Point) */

	char interface[IFNAMSIZ];

	pthread_t sender_tid;
	pthread_mutex_t lock;
};

/* modules */

typedef void * (*module_init_func)    (struct userdata *);
typedef void   (*module_conf_func)    (void *, char *param1, char *param2);
typedef int    (*module_postconf_func)(void *);
typedef double (*module_metric_func)  (void *, char *packet, int packetlen, int mark);
typedef void   (*module_done_func)    (void *);

struct module_info
{
	char *name;
	module_init_func init;
	module_conf_func conf;
	module_postconf_func postconf;
	module_metric_func metric;
	module_done_func done;

	void *mptr;
	int enabled;
};

extern struct module_info modules[];

#endif
