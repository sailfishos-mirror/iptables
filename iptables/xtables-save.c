/* Code to save the xtables state, in human readable-form. */
/* (C) 1999 by Paul 'Rusty' Russell <rusty@rustcorp.com.au> and
 * (C) 2000-2002 by Harald Welte <laforge@gnumonks.org>
 * (C) 2012 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This code is distributed under the terms of GNU GPL v2
 *
 */
#include "config.h"
#include <getopt.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>
#include "libiptc/libiptc.h"
#include "iptables.h"
#include "xtables-multi.h"
#include "nft.h"
#include "nft-cache.h"

#include <libnftnl/chain.h>

#ifndef NO_SHARED_LIBS
#include <dlfcn.h>
#endif

#define prog_name xtables_globals.program_name
#define prog_vers xtables_globals.program_version

static const char *ipt_save_optstring = "bcdt:M:f:V";
static const struct option ipt_save_options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "dump",     .has_arg = false, .val = 'd'},
	{.name = "table",    .has_arg = true,  .val = 't'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{.name = "file",     .has_arg = true,  .val = 'f'},
	{NULL},
};

static const char *arp_save_optstring = "cM:V";
static const struct option arp_save_options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{NULL},
};

static const char *ebt_save_optstring = "ct:M:V";
static const struct option ebt_save_options[] = {
	{.name = "counters", .has_arg = false, .val = 'c'},
	{.name = "version",  .has_arg = false, .val = 'V'},
	{.name = "table",    .has_arg = true,  .val = 't'},
	{.name = "modprobe", .has_arg = true,  .val = 'M'},
	{NULL},
};

struct do_output_data {
	unsigned int format;
	bool commit;
};

static int
__do_output(struct nft_handle *h, const char *tablename, void *data)
{
	struct do_output_data *d = data;
	time_t now;

	if (!nft_table_builtin_find(h, tablename))
		return 0;

	if (!nft_is_table_compatible(h, tablename, NULL)) {
		printf("# Table `%s' is incompatible, use 'nft' tool.\n",
		       tablename);
		return 0;
	} else if (nft_is_table_tainted(h, tablename)) {
		printf("# Table `%s' contains incompatible base-chains, use 'nft' tool to list them.\n",
		       tablename);
	}

	now = time(NULL);
	printf("# Generated by %s v%s on %s", prog_name,
	       prog_vers, ctime(&now));

	printf("*%s\n", tablename);
	/* Dump out chain names first,
	 * thereby preventing dependency conflicts */
	nft_cache_sort_chains(h, tablename);
	nft_chain_foreach(h, tablename, nft_chain_save, h);
	nft_rule_save(h, tablename, d->format);
	if (d->commit)
		printf("COMMIT\n");

	now = time(NULL);
	printf("# Completed on %s", ctime(&now));
	return 0;
}

static int
do_output(struct nft_handle *h, const char *tablename, struct do_output_data *d)
{
	int ret;

	if (!tablename) {
		ret = nft_for_each_table(h, __do_output, d);
		nft_check_xt_legacy(h->family, true);
		return !!ret;
	}

	if (!nft_table_find(h, tablename) &&
	    !nft_table_builtin_find(h, tablename)) {
		fprintf(stderr, "Table `%s' does not exist\n", tablename);
		return 1;
	}

	ret = __do_output(h, tablename, d);
	nft_check_xt_legacy(h->family, true);
	return ret;
}

/* Format:
 * :Chain name POLICY packets bytes
 * rule
 */
static int
xtables_save_main(int family, int argc, char *argv[],
		  const char *optstring, const struct option *longopts)
{
	const struct builtin_table *tables;
	const char *tablename = NULL;
	struct do_output_data d = {
		.format = FMT_NOCOUNTS,
	};
	struct nft_handle h;
	bool dump = false;
	FILE *file = NULL;
	int ret, c;

	xtables_globals.program_name = basename(*argv);;
	c = xtables_init_all(&xtables_globals, family);
	if (c < 0) {
		fprintf(stderr, "%s/%s Failed to initialize xtables\n",
				xtables_globals.program_name,
				xtables_globals.program_version);
		exit(1);
	}

	while ((c = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
		switch (c) {
		case 'b':
			fprintf(stderr, "-b/--binary option is not implemented\n");
			break;
		case 'c':
			d.format &= ~FMT_NOCOUNTS;
			break;

		case 't':
			/* Select specific table. */
			tablename = optarg;
			break;
		case 'M':
			xtables_modprobe_program = optarg;
			break;
		case 'f':
			file = fopen(optarg, "w");
			if (file == NULL) {
				fprintf(stderr, "Failed to open file, error: %s\n",
					strerror(errno));
				exit(1);
			}
			ret = dup2(fileno(file), STDOUT_FILENO);
			if (ret == -1) {
				fprintf(stderr, "Failed to redirect stdout, error: %s\n",
					strerror(errno));
				exit(1);
			}
			fclose(file);
			break;
		case 'd':
			dump = true;
			break;
		case 'V':
			printf("%s v%s (nf_tables)\n", prog_name, prog_vers);
			exit(0);
		default:
			fprintf(stderr,
				"Look at manual page `%s.8' for more information.\n",
				prog_name);
			exit(1);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unknown arguments found on commandline\n");
		exit(1);
	}

	switch (family) {
	case NFPROTO_IPV4:
	case NFPROTO_IPV6: /* fallthough, same table */
#if defined(ALL_INCLUSIVE) || defined(NO_SHARED_LIBS)
		init_extensions();
		init_extensions4();
		init_extensions6();
#endif
		tables = xtables_ipv4;
		d.commit = true;
		break;
	case NFPROTO_ARP:
		tables = xtables_arp;
		break;
	case NFPROTO_BRIDGE: {
		const char *ctr = getenv("EBTABLES_SAVE_COUNTER");

		if (!(d.format & FMT_NOCOUNTS)) {
			d.format |= FMT_EBT_SAVE;
		} else if (ctr && !strcmp(ctr, "yes")) {
			d.format &= ~FMT_NOCOUNTS;
			d.format |= FMT_C_COUNTS | FMT_EBT_SAVE;
		}
		tables = xtables_bridge;
		break;
	}
	default:
		fprintf(stderr, "Unknown family %d\n", family);
		return 1;
	}

	if (nft_init(&h, family, tables) < 0) {
		fprintf(stderr, "%s/%s Failed to initialize nft: %s\n",
				xtables_globals.program_name,
				xtables_globals.program_version,
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	nft_cache_level_set(&h, NFT_CL_RULES, NULL);
	nft_cache_build(&h);
	nft_xt_fake_builtin_chains(&h, tablename, NULL);

	ret = do_output(&h, tablename, &d);
	nft_fini(&h);
	xtables_fini();
	if (dump)
		exit(0);

	return ret;
}

int xtables_ip4_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_IPV4, argc, argv,
				 ipt_save_optstring, ipt_save_options);
}

int xtables_ip6_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_IPV6, argc, argv,
				 ipt_save_optstring, ipt_save_options);
}

int xtables_eb_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_BRIDGE, argc, argv,
				 ebt_save_optstring, ebt_save_options);
}

int xtables_arp_save_main(int argc, char *argv[])
{
	return xtables_save_main(NFPROTO_ARP, argc, argv,
				 arp_save_optstring, arp_save_options);
}
