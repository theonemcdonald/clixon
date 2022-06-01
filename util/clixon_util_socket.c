/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC (Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  * Unit test for testting the backend socket, ie simulating a client by 
  * directly sending XML to the backend.
  * Precondition:
  * The backend must have been started using socket path goven as -s
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options] with xml on stdin (unless -f)\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug\n"
	    "\t-a <family>\tSocket address family (default UNIX)\n"
	    "\t-s <sockpath> \tPath to unix domain socket (or IP addr)\n"
	    "\t-f <file>\tXML input file (overrides stdin)\n"
	    "\t-J \t\tInput as JSON (instead of XML)\n"
	    ,
	    argv0);
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int                retval = -1;
    int                c;
    int                logdst = CLICON_LOG_STDERR;
    struct clicon_msg *msg = NULL;
    char              *sockpath = NULL;
    char              *retdata = NULL;
    int                jsonin = 0;
    char              *input_filename = NULL;
    FILE              *fp = stdin;
    cxobj             *xt = NULL;
    cxobj             *xc;
    cxobj             *xerr = NULL;
    char              *family = "UNIX";
    int                ret;
    cbuf              *cb = cbuf_new();
    clicon_handle      h;
    int                dbg = 0;
    int                s;
    int                eof = 0;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__FILE__, LOG_INFO, CLICON_LOG_STDERR); 

    if ((h = clicon_handle_init()) == NULL)
	goto done;

    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, "hD:s:f:Ja:")) != -1)
	switch (c) {
	case 'h':
	    usage(argv[0]);
	    break;
    	case 'D':
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(argv[0]);
	    break;
	case 's':
	    sockpath = optarg;
	    break;
	case 'f':
	    input_filename = optarg;
	    break;
	case 'J':
	    jsonin++;
	    break;
	case 'a':
	    family = optarg;
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    clicon_log_init(__FILE__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clicon_debug_init(dbg, NULL);

    if (sockpath == NULL){
	fprintf(stderr, "Mandatory option missing: -s <sockpath>\n");
	usage(argv[0]);
    }
    if (input_filename){
	if ((fp = fopen(input_filename, "r")) == NULL){
	    clicon_err(OE_YANG, errno, "open(%s)", input_filename);	
	    goto done;
	}
    }
    /* 2. Parse data (xml/json) */
    if (jsonin){
	if ((ret = clixon_json_parse_file(fp, 0, YB_NONE, NULL, &xt, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    fprintf(stderr, "Invalid JSON\n");
	    goto done;
	}
    }
    else{
	if (clixon_xml_parse_file(fp, YB_NONE, NULL, &xt, NULL) < 0){
	    fprintf(stderr, "xml parse error: %s\n", clicon_err_reason);
	    goto done;
	}
    }
    if ((xc = xml_child_i(xt, 0)) == NULL){
	fprintf(stderr, "No xml\n");
	goto done;
    }
    if (clicon_xml2cbuf(cb, xc, 0, 0, -1, 0) < 0)
	goto done;
    if ((msg = clicon_msg_encode(getpid(), "%s", cbuf_get(cb))) < 0)
	goto done;
    if (strcmp(family, "UNIX")==0){
	if (clicon_rpc_connect_unix(h, sockpath, &s) < 0)
	    goto done;
    }
    else
	if (clicon_rpc_connect_inet(h, sockpath, 4535, &s) < 0)
	    goto done;
    if (clicon_rpc(s, msg, &retdata, &eof) < 0)
	goto done;
    close(s);
    fprintf(stdout, "%s\n", retdata);
    retval = 0;
 done:
    if (fp)
	fclose(fp);
    if (xerr)
	xml_free(xerr);
    if (xt)
	xml_free(xt);
    if (msg)
	free(msg);
    if (cb)
	cbuf_free(cb);
    return retval;
}


