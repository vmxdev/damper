#include "damper.h"

#include "inhibit_big_flows.c"

struct module_info modules[] = {
	{
		"inhibit_big_flows",		/* module name */
		&inhibit_big_flows_init,	/* constructor */
		&inhibit_big_flows_conf,	/* configuration parameters */
		&inhibit_big_flows_postconf,	/* when configuration done */
		&inhibit_big_flows_weight,	/* weight calculation */
		&inhibit_big_flows_free		/* destructor */
	},
	{NULL}
};
