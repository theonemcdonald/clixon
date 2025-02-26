/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
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

 * JSON utility command
 * @see http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-404.pdf
 *  and RFC 7951 JSON Encoding of Data Modeled with YANG
 *  and RFC 8259 The JavaScript Object Notation (JSON) Data Interchange Format
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <signal.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/*
 * JSON parse and pretty print test program
 * Usage: xpath
 * read json from input
 * Example compile:
 gcc -g -o json -I. -I../clixon ./clixon_json.c -lclixon -lcligen
 * Example run:
    echo '{"foo": -23}' | ./json
*/
static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options] JSON as input on stdin\n"
            "where options are\n"
            "\t-h \t\tHelp\n"
            "\t-D <level> \tDebug\n"
            "\t-j \t\tOutput as JSON (default is as XML)\n"
            "\t-l <s|e|o> \tLog on (s)yslog, std(e)rr, std(o)ut (stderr is default)\n"
            "\t-p \t\tPretty-print output\n"
            "\t-y <filename> \tyang filename to parse (must be stand-alone)\n"      ,
            argv0);
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int        retval = -1;
    cxobj     *xt = NULL;
    cbuf      *cb = cbuf_new();
    int        c;
    int        logdst = CLICON_LOG_STDERR;
    int        json = 0;
    char      *yang_filename = NULL;
    yang_stmt *yspec = NULL;
    cxobj     *xerr = NULL; /* malloced must be freed */
    int        ret;
    int        pretty = 0;
    int        dbg = 0;
    
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, "hD:jl:py:")) != -1)
        switch (c) {
        case 'h':
            usage(argv[0]);
            break;
        case 'D':
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(argv[0]);
            break;
        case 'j':
            json++;
            break;
        case 'l': /* Log destination: s|e|o|f */
            if ((logdst = clicon_log_opt(optarg[0])) < 0)
                usage(argv[0]);
            break;
        case 'p':
            pretty++;
            break;
        case 'y':
            yang_filename = optarg;
            break;
        default:
            usage(argv[0]);
            break;
        }
    clicon_log_init(__FILE__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clicon_debug_init(dbg, NULL);

    if (yang_filename){
        if ((yspec = yspec_new()) == NULL)
            goto done;
        if (yang_parse_filename(NULL, yang_filename, yspec) == NULL){
            fprintf(stderr, "yang parse error %s\n", clicon_err_reason);
            return -1;
        }
    }
    if ((ret = clixon_json_parse_file(stdin, yspec?1:0, yspec?YB_MODULE:YB_NONE, yspec, &xt, &xerr)) < 0)
        goto done;
    if (ret == 0){
        xml_print(stderr, xerr);
        goto done;
    }
    if (json){
        if (clixon_json2cbuf(cb, xt, pretty, 1, 0) < 0)
            goto done;
    }
    else if (clixon_xml2cbuf(cb, xt, 0, pretty, NULL, -1, 1) < 0)
        goto done;
    fprintf(stdout, "%s", cbuf_get(cb));
    fflush(stdout);
    retval = 0;
 done:
    if (yspec)
        ys_free(yspec);
    if (xt)
        xml_free(xt);
    if (cb)
        cbuf_free(cb);
    return retval;
}
