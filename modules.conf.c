#include "damper.h"

#include "inhibit_big_flows.c"
#include "bymark.c"
#include "entropy.c"
#include "random.c"

struct module_info modules[] = {
	{
		"inhibit_big_flows",		/* module name */
		&inhibit_big_flows_init,	/* constructor */
		&inhibit_big_flows_conf,	/* configuration parameters */
		&inhibit_big_flows_postconf,	/* when configuration done */
		&inhibit_big_flows_weight,	/* weight calculation */
		&inhibit_big_flows_free		/* destructor */
	},
#if 0
	{
		"random",
		&random_init,
		&random_conf,
		&random_postconf,
		&random_weight,
		&random_free
	},
	{
		"bymark",
		&bymark_init,
		&bymark_conf,
		&bymark_postconf,
		&bymark_weight,
		&bymark_free
	},
	{
		"entropy",
		&entropy_init,
		&entropy_conf,
		&entropy_postconf,
		&entropy_weight,
		&entropy_free
	},
#endif
	{NULL}
};
