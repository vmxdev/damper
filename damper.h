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
#include <math.h>

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
} __attribute__((packed));

#define DAMPER_MAX_PACKET_SIZE 0xffff

struct mpacket
{
	int id; /* ID assigned to packet by netfilter */
	int size;
	unsigned char packet[DAMPER_MAX_PACKET_SIZE];
};


struct stat_info
{
	uint32_t packets_pass, octets_pass;
	uint32_t packets_drop, octets_drop;
} __attribute__((packed));

typedef struct stat_info day_stat[60 * 60 * 24];

struct userdata
{
	int queue;               /* nfqueue queue id */
	struct nfq_q_handle *qh; /* queue handle */
	int nfqlen;              /* internal queue length */

	struct mpacket *packets;
	double *prioarray;
	size_t qlen;

	uint64_t limit;

	pthread_t sender_tid, stat_tid;
	pthread_mutex_t lock;

	int stat;                /* enable statistics */
	int keep_stat;           /* how many days keep statistics */
	char statdir[PATH_MAX];
	FILE *statf;             /* statistics file handle */
	struct stat_info stat_info;
	time_t stat_start, curr_timestamp, old_timestamp;

	int wchart; /* enable weights chart */
};


/* modules */

typedef void * (*module_init_func)    (struct userdata *, size_t n);
typedef void   (*module_conf_func)    (void *, char *param1, char *param2);
typedef int    (*module_postconf_func)(void *);
typedef double (*module_weight_func)  (void *, char *packet, int packetlen, int mark);
typedef void   (*module_done_func)    (void *);

struct module_info
{
	char *name;
	module_init_func init;
	module_conf_func conf;
	module_postconf_func postconf;
	module_weight_func weight;
	module_done_func done;

	double k; /* multiplicator */

	void *mptr;
	int enabled;

	FILE *st;       /* weight chart statistics file */
	double stw;     /* sum of weights per second */
	double nw;      /* number of weight samples per second */
};

extern struct module_info modules[];

#endif

