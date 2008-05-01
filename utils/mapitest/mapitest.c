/*
   Stand-alone MAPI testsuite

   OpenChange Project

   Copyright (C) Julien Kerihuel 2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libmapi/libmapi.h>
#include <samba/popt.h>
#include <param.h>

#include <utils/openchange-tools.h>
#include "utils/mapitest/mapitest.h"

/**
   Initialize mapitest structure
 */
static void mapitest_init(TALLOC_CTX *mem_ctx, struct mapitest *mt)
{
	mt->mem_ctx = mem_ctx;
	mt->stream = NULL;
	memset(&mt->info, 0, sizeof (mt->info));

	mt->mapi_all = true;
	mt->confidential = false;
	mt->no_server = false;
	mt->online = false;
	mt->mapi_suite = false;
	mt->cmdline_calls = NULL;
	mt->cmdline_suite = NULL;
}


static void mapitest_init_stream(struct mapitest *mt, const char *filename)
{
	if (filename == NULL) {
		mt->stream = fdopen(STDOUT_FILENO, "r+");
	} else {
		mt->stream = fopen(filename, "w+");
	}

	if (mt->stream == NULL) {
		err(errno, "fdopen/fopen");
	}
}

static void mapitest_cleanup_stream(struct mapitest *mt)
{
	fclose(mt->stream);
}


static bool mapitest_get_testnames(TALLOC_CTX *mem_ctx, struct mapitest *mt,
				   const char *parameter)
{
	struct mapitest_unit	*el = NULL;
	char			*tmp = NULL;

	if ((tmp = strtok((char *)parameter, ";")) == NULL) {
		fprintf(stderr, "Invalid testname list [;]\n");
		return false;
	}

	el = talloc_zero(mem_ctx, struct mapitest_unit);
	el->name = talloc_strdup(mem_ctx, tmp);
	DLIST_ADD(mt->cmdline_calls, el);

	while ((tmp = strtok(NULL, ";")) != NULL) {
		el = talloc_zero(mem_ctx, struct mapitest_unit);
		el->name = talloc_strdup(mem_ctx, tmp);
		DLIST_ADD_END(mt->cmdline_calls, el, struct mapitest_unit *);
	}

	return true;
}


static void mapitest_list(struct mapitest *mt, const char *name)
{
	struct mapitest_suite		*sel;
	struct mapitest_test		*el;

	/* List all tests */
	if (!name) {
		for (sel = mt->mapi_suite; sel; sel = sel->next) {
			printf("[*] Suite %s\n", sel->name);
			printf("===================================\n");
			printf("    * %-15s %s\n", "Name:", sel->name);
			printf("    * %-15s %5s\n", "Description:", sel->description);
			printf("    * Running Tests:\n");
			for (el = sel->tests; el; el = el->next) {
				printf("\t    - %-35s: %-10s\n", el->name, el->description);
			}
			printf("\n\n");
		}
	}
}


/**
 * Retrieve server specific information
 */
static bool mapitest_get_server_info(struct mapitest *mt,
				     const char *profdb,
				     const char *profname,
				     const char *password,
				     bool opt_dumpdata,
				     const char *opt_debug)
{
	enum MAPISTATUS		retval;
	struct emsmdb_info	*info = NULL;
	struct mapi_session	*session = NULL;

	if (mt->no_server == true) return 0;

	if (!profdb) {
		profdb = talloc_asprintf(mt->mem_ctx, DEFAULT_PROFDB, getenv("HOME"));
	}
	retval = MAPIInitialize(profdb);
	if (retval != MAPI_E_SUCCESS) {
		mapi_errstr("MAPIInitialize", retval);
		return false;
	}

	if (!profname) {
		retval = GetDefaultProfile(&profname);
		if (retval != MAPI_E_SUCCESS) {
			mapi_errstr("GetDefaultProfile", retval);
			return false;
		}
	}

	if (opt_debug) {
		lp_set_cmdline(global_mapi_ctx->lp_ctx, "log level", opt_debug);
	}

	if (opt_dumpdata == true) {
		global_mapi_ctx->dumpdata = true;
	}	

	retval = MapiLogonEx(&session, profname, password);
	if (retval != MAPI_E_SUCCESS) {
		mapi_errstr("MapiLogonEx", retval);
		return false;
	}

	info = emsmdb_get_info();
	memcpy(&mt->info, info, sizeof (struct emsmdb_info));

	/* extract org and org_unit from info.mailbox */
	mt->org = x500_get_dn_element(mt->mem_ctx, info->mailbox, "/o=");
	mt->org_unit = x500_get_dn_element(mt->mem_ctx, info->mailbox, "/ou=");
	
	return true;
}



/**
 *  main program
 */
int main(int argc, const char *argv[])
{
	TALLOC_CTX		*mem_ctx;
	struct mapitest		mt;
	poptContext		pc;
	int			opt;
	bool			ret;
	bool			opt_dumpdata = false;
	const char     		*opt_debug = NULL;
	const char		*opt_profdb = NULL;
	const char		*opt_profname = NULL;
	const char		*opt_username = NULL;
	const char		*opt_password = NULL;
	const char		*opt_outfile = NULL;

	enum { OPT_PROFILE_DB=1000, OPT_PROFILE, OPT_USERNAME, OPT_PASSWORD,
	       OPT_CONFIDENTIAL, OPT_OUTFILE, OPT_MAPI_ALL, OPT_MAPI_CALLS,
	       OPT_MAPIADMIN_ALL, OPT_NO_SERVER, OPT_LIST_ALL, OPT_DUMP_DATA,
	       OPT_DEBUG };

	struct poptOption long_options[] = {
		POPT_AUTOHELP
		{ "database",     'f', POPT_ARG_STRING, NULL, OPT_PROFILE_DB,    "set the profile database" },
		{ "profile",      'p', POPT_ARG_STRING, NULL, OPT_PROFILE,       "set the profile name" },
		{ "username",     'u', POPT_ARG_STRING, NULL, OPT_USERNAME,      "set the account username" },
		{ "password",     'p', POPT_ARG_STRING, NULL, OPT_PASSWORD,      "set the profile or account password" },
		{ "confidential",  0,  POPT_ARG_NONE,   NULL, OPT_CONFIDENTIAL,  "remove any sensitive data from the report" },
		{ "outfile",      'o', POPT_ARG_STRING, NULL, OPT_OUTFILE,       "set the report output file" },
		{ "mapi-calls",    0,  POPT_ARG_STRING, NULL, OPT_MAPI_CALLS,    "test custom ExchangeRPC tests" },
		{ "list-all",      0,  POPT_ARG_NONE,   NULL, OPT_LIST_ALL,      "list suite and tests - names and description" },
		{ "no-server",     0,  POPT_ARG_NONE,   NULL, OPT_NO_SERVER,     "only run tests that do not require server connection" },
		{ "dump-data",     0,  POPT_ARG_NONE,   NULL, OPT_DUMP_DATA,     "dump the hex data" },
		{ "debuglevel",   'd', POPT_ARG_STRING, NULL, OPT_DEBUG,         "set debug level" },
		{ NULL }
	};

	mem_ctx = talloc_init("mapitest");
	mapitest_init(mem_ctx, &mt);
	mapitest_register_modules(&mt);

	pc = poptGetContext("mapitest", argc, argv, long_options, 0);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		case OPT_DUMP_DATA:
			opt_dumpdata = true;
			break;
		case OPT_DEBUG:
			opt_debug = poptGetOptArg(pc);
			break;
		case OPT_PROFILE_DB:
			opt_profdb = poptGetOptArg(pc);
			break;
		case OPT_PROFILE:
			opt_profname = poptGetOptArg(pc);
			break;
		case OPT_USERNAME:
			opt_username = poptGetOptArg(pc);
			break;
		case OPT_PASSWORD:
			opt_password = poptGetOptArg(pc);
			break;
		case OPT_CONFIDENTIAL:
			mt.confidential = true;
			break;
		case OPT_OUTFILE:
			opt_outfile = poptGetOptArg(pc);
			break;
		case OPT_MAPI_CALLS:
			ret = mapitest_get_testnames(mem_ctx, &mt, poptGetOptArg(pc));
			if (ret == false) exit (-1);
			mt.mapi_all = false;
			break;
		case OPT_NO_SERVER:
			mt.no_server = true;
			break;
		case OPT_LIST_ALL:
			mapitest_list(&mt, NULL);
			return 0;
			break;
		}
	}

	poptFreeContext(pc);

	/* Sanity check */
	if (mt.cmdline_calls && (mt.mapi_all == true)) {
		fprintf(stderr, "mapi-calls and mapi-all can't be set at the same time\n");
		return -1;
	}

	mapitest_init_stream(&mt, opt_outfile);
	
	if (mt.no_server == false) {
	  mt.online = mapitest_get_server_info(&mt, opt_profdb, opt_profname, opt_password,
					       opt_dumpdata, opt_debug);
	}

	mapitest_print_headers(&mt);

	/* Run custom tests */
	if (mt.cmdline_calls) {
		struct mapitest_unit	*el;
		bool			ret;
		
		for (el = mt.cmdline_calls; el; el = el->next) {
			printf("[*] %s\n", el->name);
			ret = mapitest_run_test(&mt, el->name);
		}
	} else {
		mapitest_run_all(&mt);
	}

	mapitest_cleanup_stream(&mt);

	/* Uninitialize and free memory */
	if (mt.no_server == false) {
		MAPIUninitialize();
	}

	talloc_free(mt.mem_ctx);

	return 0;
}
