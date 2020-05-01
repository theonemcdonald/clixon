/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * XML support functions.
 * @see https://www.w3.org/TR/2008/REC-xml-20081126
 *      https://www.w3.org/TR/2009/REC-xml-names-20091208
 * The function can do yang validation, process xml and json, etc.
 * On success, nothing is printed and exitcode 0
 * On failure, an error is printed on stderr and exitcode != 0
 * Failure error prints are different, it would be nice to make them more
 * uniform. (see clixon_netconf_error)
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
#include <sys/time.h>
#include <sys/stat.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/* Command line options passed to getopt(3) */
#define UTIL_XML_OPTS "hD:f:Jjl:pvoy:Y:t:T:"

static int
validate_tree(clicon_handle h,
	      cxobj        *xt,
	      yang_stmt    *yspec)
{
    int    retval = -1;
    int    ret;
    cxobj *xerr = NULL; /* malloced must be freed */
    cbuf         *cbret = NULL;

    /* should already be populated */
    /* Add default values */
    if (xml_default_recurse(xt) < 0)
	goto done;
    if (xml_apply(xt, -1, xml_sort_verify, h) < 0)
	clicon_log(LOG_NOTICE, "%s: sort verify failed", __FUNCTION__);
    if ((ret = xml_yang_validate_all_top(h, xt, &xerr)) < 0) 
	goto done;
    if (ret > 0 && (ret = xml_yang_validate_add(h, xt, &xerr)) < 0)
	goto done;
    if (ret == 0){
	if ((cbret = cbuf_new()) ==NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}
	if (netconf_err2cb(xerr, cbret) < 0)
	    goto done;
	fprintf(stderr, "xml validation error: %s\n", cbuf_get(cbret));
	goto done;
    }
    retval = 0;
 done:
    if (cbret)
	cbuf_free(cbret);
    if (xerr)
	xml_free(xerr);
    return retval;
}

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options] with xml on stdin (unless -f)\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug\n"
	    "\t-f <file>\tXML input file (overrides stdin)\n"
	    "\t-J \t\tInput as JSON\n"
	    "\t-j \t\tOutput as JSON\n"
	    "\t-l <s|e|o> \tLog on (s)yslog, std(e)rr, std(o)ut (stderr is default)\n"
	    "\t-o \t\tOutput the file\n"
	    "\t-v \t\tValidate the result in terms of Yang model (requires -y)\n"
	    "\t-p \t\tPretty-print output\n"
	    "\t-y <filename> \tYang filename or dir (load all files)\n"
    	    "\t-Y <dir> \tYang dirs (can be several)\n"
   	    "\t-t <file>\tXML top input file (where base tree is pasted to)\n"
	    "\t-T <path>\tXPath to where in top input file base should be pasted\n"
	    ,
	    argv0);
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int           retval = -1;
    int           ret;
    cxobj        *xt = NULL;   /* Base cxobj tree parsed from xml or json */
    cxobj        *xc;
    cbuf         *cb = cbuf_new();
    int           c;
    int           logdst = CLICON_LOG_STDERR;
    int           jsonin = 0;
    int           jsonout = 0;
    char         *input_filename = NULL;
    char         *top_input_filename = NULL;
    char         *yang_file_dir = NULL;
    yang_stmt    *yspec = NULL;
    cxobj        *xerr = NULL; /* malloced must be freed */
    int           pretty = 0;
    int           validate = 0;
    int           output = 0;
    clicon_handle h;
    struct stat   st;
    int           fd = 0; /* base file, stdin */
    int           tfd = -1; /* top file */
    cxobj        *xcfg = NULL;
    cbuf         *cbret = NULL;
    cxobj        *xtop = NULL; /* Top tree if any */
    char         *top_path = NULL;
    cxobj        *xbot;        /* Place in xtop where base cxobj is parsed */
    cvec         *nsc = NULL; 
    yang_bind     yb;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__FILE__, LOG_INFO, CLICON_LOG_STDERR); 

    /* Initialize clixon handle */
    if ((h = clicon_handle_init()) == NULL)
	goto done;
    if ((xcfg = xml_new("clixon-config", NULL, CX_ELMNT)) == NULL)
	goto done;
    if (clicon_conf_xml_set(h, xcfg) < 0)
	goto done;
    xcfg = xml_new("clixon-config", NULL, CX_ELMNT);
    clicon_conf_xml_set(h, xcfg);
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, UTIL_XML_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(argv[0]);
	    break;
    	case 'D':
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(argv[0]);
	    break;
	case 'f':
	    input_filename = optarg;
	    break;
	case 'J':
	    jsonin++;
	    break;
	case 'j':
	    jsonout++;
	    break;
	case 'l': /* Log destination: s|e|o|f */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(argv[0]);
	    break;
	case 'o':
	    output++;
	    break;
	case 'v':
	    validate++;
	    break;
	case 'p':
	    pretty++;
	    break;
	case 'y':
	    yang_file_dir = optarg;
	    break;
	case 'Y':
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 't':
	    top_input_filename = optarg;
	    break;
	case 'T': /* top file xpath */
	    top_path = optarg;
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    if (validate && !yang_file_dir){
	fprintf(stderr, "-v requires -y\n");
	usage(argv[0]);
    }
    if (top_input_filename && top_path == NULL){
	fprintf(stderr, "-t requires -T\n");
	usage(argv[0]);
    }
    clicon_log_init(__FILE__, debug?LOG_DEBUG:LOG_INFO, logdst);
    /* 1. Parse yang */
    if (yang_file_dir){
	if ((yspec = yspec_new()) == NULL)
	    goto done;
	if (stat(yang_file_dir, &st) < 0){
	    clicon_err(OE_YANG, errno, "%s not found", yang_file_dir);
	    goto done;
	}
	if (S_ISDIR(st.st_mode)){
	    if (yang_spec_load_dir(h, yang_file_dir, yspec) < 0)
		goto done;
	}
	else{
	    if (yang_spec_parse_file(h, yang_file_dir, yspec) < 0)
		goto done;
	}
    }
    /* If top file is declared, the base XML/JSON is pasted as child to the top-file.
     * This is to emulate sub-tress, not just top-level parsing.
     * Always validated
     */
    if (top_input_filename){
	if ((tfd = open(top_input_filename, O_RDONLY)) < 0){
	    clicon_err(OE_YANG, errno, "open(%s)", top_input_filename);	
	    goto done;
	}
	if ((ret = clixon_xml_parse_file(tfd, YB_MODULE, yspec, NULL, &xtop, &xerr)) < 0){
	    fprintf(stderr, "xml parse error: %s\n", clicon_err_reason);
	    goto done;
	}
	if (ret == 0){
	    clixon_netconf_error(xerr, "Parse top file", NULL);
	    goto done;
	}
	if (validate_tree(h, xtop, yspec) < 0)
	    goto done;

	/* Compute canonical namespace context */
	if (xml_nsctx_yangspec(yspec, &nsc) < 0)
	    goto done;
	if ((xbot = xpath_first(xtop, nsc, "%s", top_path)) == NULL){
	    fprintf(stderr, "Path not found in top tree: %s\n", top_path);
	    goto done;
	}
	xt = xbot;
    }
    if (input_filename){
	if ((fd = open(input_filename, O_RDONLY)) < 0){
	    clicon_err(OE_YANG, errno, "open(%s)", input_filename);	
	    goto done;
	}
    }
    /* 2. Parse data (xml/json) */
    if (jsonin){
	if ((ret = clixon_json_parse_file(fd, top_input_filename?YB_PARENT:YB_MODULE, yspec, &xt, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    clixon_netconf_error(xerr, "util_xml", NULL);
	    goto done;
	}
    }
    else{ /* XML */
	if (!yang_file_dir)
	    yb = YB_NONE;
	else if (xt == NULL)
	    yb = YB_MODULE;
	else
	    yb = YB_PARENT;
	if ((ret = clixon_xml_parse_file(fd, yb, yspec, NULL, &xt, &xerr)) < 0){
	    fprintf(stderr, "xml parse error: %s\n", clicon_err_reason);
	    goto done;
	}
	if (ret == 0){
	    clixon_netconf_error(xerr, "util_xml", NULL);
	    goto done;
	}
    }

    /* Dump data structures (for debug) */
    if (debug){
	cbuf_reset(cb);
	xmltree2cbuf(cb, xt, 0);       
	fprintf(stderr, "%s\n", cbuf_get(cb));
	cbuf_reset(cb);
    }

    /* 3. Validate data (if yspec) */
    if (validate){
	if (validate_tree(h, xt, yspec) < 0)
	    goto done;
    }
    /* 4. Output data (xml/json) */
    if (output){
	xc = NULL;
	while ((xc = xml_child_each(xt, xc, -1)) != NULL) 
	    if (jsonout)
		xml2json_cbuf(cb, xc, pretty); /* print xml */
	    else
		clicon_xml2cbuf(cb, xc, 0, pretty, -1); /* print xml */
	fprintf(stdout, "%s", cbuf_get(cb));
	fflush(stdout);
    }
    retval = 0;
 done:
    if (nsc)
	cvec_free(nsc);
    if (cbret)
	cbuf_free(cbret);
    if (xcfg)
	xml_free(xcfg);
    if (xerr)
	xml_free(xerr);
    if (xtop)
	xml_free(xtop);
    else if (xt)
	xml_free(xt);
    if (cb)
	cbuf_free(cb);
    return retval;
}


