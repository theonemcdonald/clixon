/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "clixon_backend_plugin.h"
#include "clixon_backend_commit.h"
#include "backend_client.h"
#include "backend_handle.h"
#include "backend_get.h"

/*!
 * Maybe should be in the restconf client instead of backend?
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   Xpath selection, not used but may be to filter early
 * @param[out]    xrs     XML restconf-state node
 * @see netconf_hello_server
 * @see rfc8040 Sections 9.1
 */
static int
client_get_capabilities(clicon_handle h,
			yang_stmt    *yspec,
			char         *xpath,
			cxobj       **xret)
{
    int     retval = -1;
    cxobj  *xrstate = NULL; /* xml restconf-state node */
    cbuf   *cb = NULL;
    
    if ((xrstate = xpath_first(*xret, NULL, "restconf-state")) == NULL){
	clicon_err(OE_YANG, ENOENT, "restconf-state not found in config node");
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "<capabilities>");
    cprintf(cb, "<capability>urn:ietf:params:restconf:capability:defaults:1.0?basic-mode=explicit</capability>");
    cprintf(cb, "<capability>urn:ietf:params:restconf:capability:depth:1.0</capability>");
    cprintf(cb, "</capabilities>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_PARENT, NULL, &xrstate, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Get streams state according to RFC 8040 or RFC5277 common function
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   Xpath selection, not used but may be to filter early
 * @param[in]     module  Name of yang module
 * @param[in]     top     Top symbol, ie netconf or restconf-state
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       Statedata callback failed
 * @retval        1       OK
 */
static int
client_get_streams(clicon_handle   h,
		   yang_stmt      *yspec,
		   char           *xpath,
		   yang_stmt      *ymod,
		   char           *top,
		   cxobj         **xret)
{
    int            retval = -1;
    yang_stmt     *yns = NULL;  /* yang namespace */
    cxobj         *x = NULL;
    cbuf          *cb = NULL;
    int            ret;

    if ((yns = yang_find(ymod, Y_NAMESPACE, NULL)) == NULL){
	clicon_err(OE_YANG, 0, "%s yang namespace not found", yang_argument_get(ymod));
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "<%s xmlns=\"%s\">", top, yang_argument_get(yns));
    /* Second argument is a hack to have the same function for the
     * RFC5277 and 8040 stream cases
     */
    if (stream_get_xml(h, strcmp(top,"restconf-state")==0, cb) < 0)
	goto done;
    cprintf(cb,"</%s>", top);

    if (clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x, NULL) < 0){
	if (xret && netconf_operation_failed_xml(xret, "protocol", clicon_err_reason)< 0)
	    goto done;
	goto fail;
    }
    if ((ret = netconf_trymerge(x, yspec, xret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    retval = 1;
 done:
    if (cb)
	cbuf_free(cb);
    if (x)
	xml_free(x);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get system state-data, including streams and plugins
 * @param[in]     h       Clicon handle
 * @param[in]     xpath   XPath selection, may be used to filter early
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       Statedata callback failed (clicon_err called)
 * @retval        1       OK
 */
static int
get_client_statedata(clicon_handle h,
		     char         *xpath,
		     cvec         *nsc,
		     cxobj       **xret)
{
    int        retval = -1;
    yang_stmt *yspec;
    yang_stmt *ymod;
    int        ret;
    char      *namespace;
    cbuf      *cb = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277")){
	if ((ymod = yang_find_module_by_name(yspec, "clixon-rfc5277")) == NULL){
	    clicon_err(OE_YANG, ENOENT, "yang module clixon-rfc5277 not found");
	    goto done;
	}
	if ((namespace = yang_find_mynamespace(ymod)) == NULL){
	    clicon_err(OE_YANG, ENOENT, "clixon-rfc5277 namespace not found");
	    goto done;
	}
	cprintf(cb, "<netconf xmlns=\"%s\"/>", namespace);
	if (clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, xret, NULL) < 0)
	    goto done;
	if ((ret = client_get_streams(h, yspec, xpath, ymod, "netconf", xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040")){
	if ((ymod = yang_find_module_by_name(yspec, "ietf-restconf-monitoring")) == NULL){
	    clicon_err(OE_YANG, ENOENT, "yang module ietf-restconf-monitoring not found");
	    goto done;
	}
	if ((namespace = yang_find_mynamespace(ymod)) == NULL){
	    clicon_err(OE_YANG, ENOENT, "ietf-restconf-monitoring namespace not found");
	    goto done;
	}
	cbuf_reset(cb);
	/* XXX This code does not filter state data with  xpath */
	cprintf(cb, "<restconf-state xmlns=\"%s\"/>", namespace);
	if (clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, xret, NULL) < 0)
	    goto done;
	if ((ret = client_get_streams(h, yspec, xpath, ymod, "restconf-state", xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
	if ((ret = client_get_capabilities(h, yspec, xpath, xret)) < 0)
	    goto done;
    }
    if (clicon_option_bool(h, "CLICON_YANG_LIBRARY")){
	if ((ret = yang_modules_state_get(h, yspec, xpath, nsc, 0, xret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    /* Use plugin state callbacks */
    if ((ret = clixon_plugin_statedata_all(h, yspec, nsc, xpath, xret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    /* Add default state to config if present */
    if (xml_default_recurse(*xret, 1) < 0)
	goto done;
    /* Add default global state */
    if (xml_global_defaults(h, *xret, nsc, xpath, yspec, 1) < 0)
	goto done;
    retval = 1; /* OK */
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (cb)
	cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Help function to filter out anything that is outside of xpath 
 *
 * Code complex to filter out anything that is outside of xpath 
 * Actually this is a safety catch, should really be done in plugins
 * and modules_state functions.
 * But it is problematic, because defaults, at least of config data, is in place
 * and we need to re-add it.
 * Note original xpath
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  yspec   Yang spec
 * @param[in]  xret    Result XML tree
 * @param[in]  xvec    xpath lookup result on xret
 * @param[in]  xlen    length of xvec
 * @param[in]  xpath   XPath point to object to get
 * @param[in]  nsc     Namespace context of xpath
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
filter_xpath_again(clicon_handle h,
		   yang_stmt    *yspec,
		   cxobj        *xret,
		   cxobj       **xvec,
		   size_t        xlen,
		   char         *xpath,
		   cvec         *nsc)
{
    int     retval = -1;
    int     i;

    if (xret == NULL){
	clicon_err(OE_PLUGIN, EINVAL, "xret is NULL");
	goto done;
    }
    /* If vectors are specified then mark the nodes found and
     * then filter out everything else,
     * otherwise return complete tree.
     */
    if (xvec != NULL){
	for (i=0; i<xlen; i++)
	    xml_flag_set(xvec[i], XML_FLAG_MARK);
    }
    /* Remove everything that is not marked */
    if (!xml_flag(xret, XML_FLAG_MARK))
	if (xml_tree_prune_flagged_sub(xret, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
    /* reset flag */
    if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Help function for NACM access and returnmessage
 *
 * @param[in]  h        Clicon handle 
 * @param[in]  xret     Result XML tree
 * @param[in]  xvec    xpath lookup result on xret
 * @param[in]  xlen    length of xvec
 * @param[in]  xpath    XPath point to object to get
 * @param[in]  nsc      Namespace context of xpath
 * @param[in]  username User name for NACM access
 * @param[in]  depth    Nr of levels to print, -1 is all, 0 is none
 * @param[out] cbret    Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     0        OK
 * @retval    -1        Error
 */
static int
get_nacm_and_reply(clicon_handle h,
		   cxobj        *xret,
		   cxobj       **xvec,
		   size_t        xlen,
		   char         *xpath,
		   cvec         *nsc,
		   char         *username,
		   int32_t       depth,
		   cbuf         *cbret)
{
    int     retval = -1;
    cxobj  *xnacm = NULL;

    /* Pre-NACM access step */
    xnacm = clicon_nacm_cache(h);
    if (xnacm != NULL){ /* Do NACM validation */
	/* NACM datanode/module read validation */
	if (nacm_datanode_read(h, xret, xvec, xlen, username, xnacm) < 0) 
	    goto done;
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);     /* OK */
    if (xret==NULL)
	cprintf(cbret, "<data/>");
    else{
	if (xml_name_set(xret, NETCONF_OUTPUT_DATA) < 0)
	    goto done;
	/* Top level is data, so add 1 to depth if significant */
	if (clixon_xml2cbuf(cbret, xret, 0, 0, depth>0?depth+1:depth, 0) < 0)
	    goto done;
    }
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    return retval;
}

/*! Help function for parsing restconf query parameter and setting netconf attribute
 *
 * If not "unbounded", parse and set a numeric value
 * @param[in]     h          Clixon handle
 * @param[in]     name       Name of attribute
 * @param[in]     defaultstr Default string which is accepted and sets value to 0
 * @param[in,out] cbret      Output buffer for internal RPC message if invalid
 * @param[out]    value      Value
 * @retval       -1          Error
 * @retval        0          Invalid, cbret set
 * @retval        1          OK
 */
static int
element2value(clicon_handle  h,
	      cxobj         *xe,
	      char          *name,
	      char          *defaultstr,
	      cbuf          *cbret,
	      uint32_t      *value)
{
    char  *valstr;
    cxobj *x;
    
    *value = 0;
    if ((x = xml_find_type(xe, NULL, name, CX_ELMNT)) != NULL &&
	(valstr = xml_body(x)) != NULL){
	return netconf_parse_uint32(name, valstr, defaultstr, 0, cbret, value);
    }
    return 1;
}

/*! Set flag on node having schema default value.
 * @param[in]   x	XML node
 * @param[in]   flag	Flag to be used
 * @retval      0    	OK

 */
static int
xml_flag_default_value(cxobj *x,
		       uint16_t flag)
{
    yang_stmt	*y;
    cg_var	*cv;
    char	*yv;
    char	*xv;

    xml_flag_reset(x, flag); /* Assume not default value */
    if ((xv = xml_body(x)) == NULL)
 	goto done;
    if ((y = xml_spec(x)) == NULL)
	goto done;
    if ((cv = yang_cv_get(y)) == NULL)
	goto done;
    if ((cv = yang_cv_get(y)) == NULL)
	goto done;
    if (cv_name_get(cv) == NULL)
	goto done;
    if ((yv = cv2str_dup(cv)) == NULL)
	goto done;
    if (strcmp(xv, yv) == 0)
	xml_flag_set(x, flag); /* Actual value same as default value */
    free(yv);

  done:
    return 0;
}

/*! Add default attribute to node with default value.
 * @param[in]   x      	XML node
 * @param[in]   flags 	Flags indicatiing default nodes
 * @retval      0    	OK
 * @retval     -1    	Error
 */
static int
xml_add_default_tag(cxobj *x,
		    uint16_t flags)
{
    int retval = -1;
    cxobj *xattr;

    if (xml_flag(x, flags)) {
	/* set default attribute */
	if ((xattr = xml_new("default", x, CX_ATTR)) == NULL)
	    goto done;
	if (xml_value_set(xattr, "true") < 0)
	    goto done;
	if (xml_prefix_set(xattr, "wd") < 0)
	    goto done;
    }
    retval = 0;
    done:
    return retval;
}

/*! Update XML return tree according to RFC6243: with-defaults
 * @param[in]   xe      Request: <rpc><xn></rpc>
 * @param[in]   xret    XML return tree to be updated
 * @retval      0    	OK
 * @retval     -1    	Error
 */

static int
with_defaults(cxobj *xe,
	      cxobj *xret)
{
    int    retval = -1;
    cxobj *xfind;
    char  *mode;

    if ((xfind = xml_find(xe, "with-defaults")) != NULL) {
	if ((mode = xml_find_value(xfind, "body")) == NULL)
	    goto done;

	if (strcmp(mode, "explicit") == 0) {
	    /* Clear marked nodes */
	    if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*) xml_flag_reset,(void*) XML_FLAG_MARK) < 0)
		goto done;
	    /* Mark state nodes */
	    if (xml_non_config_data(xret, NULL) < 0)
		goto done;
	    /* Remove default configuration nodes*/
	    if (xml_tree_prune_flags(xret, XML_FLAG_DEFAULT, XML_FLAG_MARK | XML_FLAG_DEFAULT) < 0)
		goto done;
	    /* TODO. Remove empty containers */
	    goto ok;
	}
	if (strcmp(mode, "trim") == 0) {
	    /* Remove default nodes from XML */
	    if (xml_tree_prune_flags(xret, XML_FLAG_DEFAULT, XML_FLAG_DEFAULT) < 0)
		goto done;
	    /* Mark and remove nodes having schema default values */
	    if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*) xml_flag_default_value, (void*) XML_FLAG_MARK) < 0)
		goto done;
	    if (xml_tree_prune_flags(xret, XML_FLAG_MARK, XML_FLAG_MARK)
		    < 0)
		goto done;
	    /* TODO. Remove empty containers */
	    goto ok;
	}
	if (strcmp(mode, "report-all-tagged") == 0) {
	    if (xmlns_set(xret, "wd", "urn:ietf:params:xml:ns:netconf:default:1.0") < 0)
		goto done;
	    /* Mark nodes having default schema values */
	    if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*) xml_flag_default_value, (void*) XML_FLAG_MARK) < 0)
		goto done;
	    /* Add tag attributes to default nodes */
	    if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*) xml_add_default_tag, (void*) (XML_FLAG_DEFAULT | XML_FLAG_MARK)) < 0)
		goto done;
	    goto ok;
	}
	if (strcmp(mode, "report-all") == 0) {
	    /* Accept mode, do nothing */
	    goto ok;
	}
    }
    ok:
        retval = 0;
    done:
        return retval;
}

/*! Specialized get for list-pagination
 *
 * It is specialized enough to have its own function. Specifically, extra attributes as well
 * as the list-paginaiton API
 * @param[in]  h       Clicon handle 
 * @param[in]  ce      Client entry, for locking
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  content Get config/state/both
 * @param[in]  db      Database name
 * @param[in]  xpath   XPath point to object to get
 * @param[in]  nsc     Namespace context of xpath
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     0       OK
 * @retval    -1       Error
 * @note pagination uses appending xpath with predicate, eg [position()<limit], this may not work 
 *       if there is an existing predicate
 * XXX Lots of this code (in particular at the end) is copy of get_common
 */
static int
get_list_pagination(clicon_handle        h,
		    struct client_entry *ce,
		    cxobj               *xe,
		    netconf_content      content,
		    char                *db,
		    int32_t              depth,
		    yang_stmt           *yspec,
		    char                *xpath,
		    cvec                *nsc,
		    char                *username,
		    cbuf                *cbret
		    )
{
    int             retval = -1;
    uint32_t        offset = 0;
    uint32_t        limit = 0;
    cbuf           *cbpath = NULL;
    int             list_config;
    yang_stmt      *ylist;
    cxobj          *xerr = NULL;
    cbuf           *cbmsg = NULL; /* For error msg */
    cxobj          *xret = NULL;
    char           *xpath2; /* With optional pagination predicate */
    int             ret;
    uint32_t        iddb; /* DBs lock, if any */
    int             locked;
    cbuf           *cberr = NULL; 
    cxobj         **xvec = NULL;
    size_t          xlen;
#ifdef NOTYET
    cxobj          *x;
    char           *direction = NULL;
    char           *sort = NULL;
    char           *where = NULL;
#endif

    /* Check if list/leaf-list */
    if (yang_path_arg(yspec, xpath?xpath:"/", &ylist) < 0)
	goto done;
    if (ylist == NULL){
	if ((cbmsg = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	/* error reason should be in clicon_err_reason */
	cprintf(cbmsg, "Netconf get list-pagination: \"%s\" not found", xpath);
	if (netconf_invalid_value(cbret, "application", cbuf_get(cbmsg)) < 0)
	    goto done;
	goto ok;
    }
    if (yang_keyword_get(ylist) != Y_LIST &&
	yang_keyword_get(ylist) != Y_LEAF_LIST){
	if (netconf_invalid_value(cbret, "application", "list-pagination is enabled but target is not list or leaf-list") < 0)
	    goto done;
	goto ok;
    }
    /* Sanity checks on state/config */
    if ((list_config = yang_config_ancestor(ylist)) != 0){ /* config list */
	if (content == CONTENT_NONCONFIG){
	    if (netconf_invalid_value(cbret, "application", "list-pagination targets a config list but content request is nonconfig") < 0)
		goto done;
	    goto ok;
	}
    }
    else { /* state list */
	if (content == CONTENT_CONFIG){
	    if (netconf_invalid_value(cbret, "application", "list-pagination targets a state list but content request is config") < 0)
		goto done;
	    goto ok;
	}
    }
    /* offset */
    if ((ret = element2value(h, xe, "offset", "none", cbret, &offset)) < 0)
	goto done;
    /* limit */
    if (ret && (ret = element2value(h, xe, "limit", "unbounded", cbret, &limit)) < 0)
	goto done;
#ifdef NOTYET
    /* direction */
    if (ret && (x = xml_find_type(xe, NULL, "direction", CX_ELMNT)) != NULL){
	direction = xml_body(x);
	if (strcmp(direction, "forward") != 0 && strcmp(direction, "reverse") != 0){
	    if (netconf_bad_attribute(cbret, "application",
				      "direction", "Unrecognized value of direction attribute") < 0)
		goto done;
	    goto ok;
	}
    }
    /* sort */
    if (ret && (x = xml_find_type(xe, NULL, "sort-by", CX_ELMNT)) != NULL)
	sort = xml_body(x);
    if (sort) {
	/* XXX: nothing yet */
    }
    /* where */
    if (ret && (x = xml_find_type(xe, NULL, "where", CX_ELMNT)) != NULL)
	where = xml_body(x);
#endif
    /* Read config */
    switch (content){
    case CONTENT_CONFIG:    /* config data only */
    case CONTENT_ALL:       /* both config and state */
	/* Build a "predicate" cbuf 
	 * This solution uses xpath predicates to translate "limit" and "offset" to
	 * relational operators <>.
	 */
	if ((cbpath = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	/* This uses xpath. Maybe limit should use parameters */
	if (xpath)
	    cprintf(cbpath, "%s", xpath);
	else
	    cprintf(cbpath, "/");
#ifdef NOTYET
	if (where)
	    cprintf(cbpath, "[%s]", where);
#endif
	if (offset){
	    cprintf(cbpath, "[%u <= position()", offset);
	    if (limit)
		cprintf(cbpath, " and position() < %u", limit+offset);
	    cprintf(cbpath, "]");
	}
	else if (limit)
	    cprintf(cbpath, "[position() < %u]", limit);

	/* Append predicate to original xpath and replace it */
	xpath2 = cbuf_get(cbpath);
	/* specific xpath */
	if (xmldb_get0(h, db, YB_MODULE, nsc, xpath2?xpath2:"/", 1, &xret, NULL, NULL) < 0) {
	    if ((cbmsg = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cbmsg, "Get %s datastore: %s", db, clicon_err_reason);
	    if (netconf_operation_failed(cbret, "application", cbuf_get(cbmsg)) < 0)
		goto done;
	    goto ok;
	}
	break;
    case CONTENT_NONCONFIG: /* state data only */
	if ((xret = xml_new(DATASTORE_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)/* Only top tree */
	    goto done;
	break;
    }/* switch content */

    if (list_config){
#ifdef LIST_PAGINATION_REMAINING
	/* Get total/remaining
	 * XXX: Works only for cache
	 */
	if ((xcache = xmldb_cache_get(h, db)) != NULL){
	    if (xpath_count(xcache, nsc, xpath, &total) < 0)
		goto done;
	    if (total >= (offset + limit))
		remaining = total - (offset + limit);
	}
#endif
    }
    else {/* Check if running locked (by this session) */
	if ((iddb = xmldb_islocked(h, "running")) != 0 &&
	    iddb == ce->ce_id)
	    locked = 1;
	else
	    locked = 0;
	if ((ret = clixon_pagination_cb_call(h, xpath, locked,
					     offset, limit,
					     xret)) < 0)
	    goto done;
	if (ret == 0){
	    if ((cberr = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    /* error reason should be in clicon_err_reason */
	    cprintf(cberr, "Internal error, pagination state callback invalid return : %s",
		    clicon_err_reason);
	    if (netconf_operation_failed_xml(&xerr, "application", cbuf_get(cberr)) < 0)
		goto done;
	    if (clixon_xml2cbuf(cbret, xerr, 0, 0, -1, 0) < 0)
		goto done;
	    goto ok;
	}

	/* System makes the binding */
	if ((ret = xml_bind_yang(xret, YB_MODULE, yspec, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if (clicon_debug_get() && xret)
		clicon_log_xml(LOG_DEBUG, xret, "Yang bind pagination state");
	    if (clixon_netconf_internal_error(xerr,
					      ". Internal error, state callback returned invalid XML",
					      NULL) < 0)
		goto done;
	    if (clixon_xml2cbuf(cbret, xerr, 0, 0, -1, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    if (with_defaults(xml_parent(xe), xret) < 0)
	goto done;
    if (xpath_vec(xret, nsc, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
	goto done;
    /* Help function to filter out anything that is outside of xpath */
    if (filter_xpath_again(h, yspec, xret, xvec, xlen, xpath, nsc) < 0)
	goto done;
#ifdef LIST_PAGINATION_REMAINING
    /* Add remaining attribute Sec 3.1.5: 
       Any list or leaf-list that is limited includes, on the first element in the result set, 
       a metadata value [RFC7952] called "remaining"*/
    if (limit && x1){ 
	cxobj *xa;
	cbuf  *cba = NULL;

	/* Add remaining attribute */
	if ((xa = xml_new("remaining", x1, CX_ATTR)) == NULL)
	    goto done;
	if ((cba = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cba, "%u", remaining);
	if (xml_value_set(xa, cbuf_get(cba)) < 0)
	    goto done;
	if (xml_prefix_set(xa, "cp") < 0)
	    goto done;
	if (xmlns_set(x1, "cp", "http://clicon.org/clixon-netconf-list-pagination") < 0)
	    goto done;
	if (cba)
	    cbuf_free(cba);
    }
#endif /* LIST_PAGINATION_REMAINING */
    if (get_nacm_and_reply(h, xret, xvec, xlen, xpath, nsc, username, depth, cbret) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    if (xvec)
	free(xvec);
    if (cbmsg)
	cbuf_free(cbmsg);
    if (cbpath)
	cbuf_free(cbpath);
    if (xerr)
	xml_free(xerr);
    if (cberr)
	cbuf_free(cberr);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Common get/get-config code for retrieving  configuration and state information.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  ce      Client entry, for locking
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[in]  content Get config/state/both
 * @param[in]  db      Database name
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     0       OK
 * @retval    -1       Error
 * @see from_client_get
 * @see from_client_get_config 
 */
static int
get_common(clicon_handle        h,
	   struct client_entry *ce,
	   cxobj               *xe,
	   netconf_content      content,
	   char                *db,
	   cbuf                *cbret
	   )
{
    int             retval = -1;
    cxobj          *xfilter;
    char           *xpath = NULL;
    cxobj          *xret = NULL;
    char           *username;
    cvec           *nsc0 = NULL; /* Create a netconf namespace context from filter */
    cvec           *nsc = NULL;
    char           *attr;
    int32_t         depth = -1; /* Nr of levels to print, -1 is all, 0 is none */
    yang_stmt      *yspec;
    cxobj          *xerr = NULL;
    int             ret;
    char           *reason = NULL;
    cbuf           *cbmsg = NULL; /* For error msg */
    char           *xpath0;
    cbuf           *cbreason = NULL;
    int             list_pagination = 0;
    cxobj         **xvec = NULL;
    size_t          xlen;
    cxobj          *xfind;
    
    clicon_debug(1, "%s", __FUNCTION__);
    username = clicon_username_get(h);
    if ((yspec =  clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec9");
	goto done;
    }
    if ((xfilter = xml_find(xe, "filter")) != NULL){
	if ((xpath0 = xml_find_value(xfilter, "select"))==NULL)
	    xpath0 = "/";
	/* Create namespace context for xpath from <filter>
	 *  The set of namespace declarations are those in scope on the
	 * <filter> element.
	 */
	else
	    if (xml_nsctx_node(xfilter, &nsc0) < 0)
		goto done;
	if ((ret = xpath2canonical(xpath0, nsc0, yspec, &xpath, &nsc, &cbreason)) < 0)
	    goto done;
	if (ret == 0){
	    if (netconf_bad_attribute(cbret, "application",
				      "select", cbuf_get(cbreason)) < 0)
		goto done;
	    goto ok;
	}
    }
    /* Clixon extensions: depth */
    if ((attr = xml_find_value(xe, "depth")) != NULL){
	if ((ret = parse_int32(attr, &depth, &reason)) < 0){
	    clicon_err(OE_XML, errno, "parse_int32");
	    goto done;
	}
	if (ret == 0){
	    if (netconf_bad_attribute(cbret, "application",
				      "depth", "Unrecognized value of depth attribute") < 0)
		goto done;
	    goto ok;
	}
    }
    /* Check if list pagination */
    if ((xfind = xml_find_type(xe, NULL, "list-pagination", CX_ELMNT)) != NULL)
	list_pagination = 1;
    /* Sanity check for list pagination: path must be a list/leaf-list, if it is,
     * check config/state
     */
    if (list_pagination){
	if (get_list_pagination(h, ce,
				xfind,
				content, db,
				depth, yspec, xpath, nsc, username,
				cbret) < 0)
	    goto done;
	goto ok;
    }
    /* Read configuration */
    switch (content){
    case CONTENT_CONFIG:    /* config data only */
	/* specific xpath */
	if (xmldb_get0(h, db, YB_MODULE, nsc, xpath?xpath:"/", 1, &xret, NULL, NULL) < 0) {
	    if ((cbmsg = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cbmsg, "Get %s datastore: %s", db, clicon_err_reason);
	    if (netconf_operation_failed(cbret, "application", cbuf_get(cbmsg)) < 0)
		goto done;
	    goto ok;
	}
	break;
    case CONTENT_ALL:       /* both config and state */
    case CONTENT_NONCONFIG: /* state data only */
	if (clicon_option_bool(h, "CLICON_VALIDATE_STATE_XML")){
	    /* Whole config tree, for validate debug */
	    if (xmldb_get0(h, "running", YB_MODULE, nsc, NULL, 1, &xret, NULL, NULL) < 0) {
		if ((cbmsg = cbuf_new()) == NULL){
		    clicon_err(OE_UNIX, errno, "cbuf_new");
		    goto done;
		}
		cprintf(cbmsg, "Get %s datastore: %s", db, clicon_err_reason);
		if (netconf_operation_failed(cbret, "application", cbuf_get(cbmsg)) < 0)
		    goto done;
		goto ok;
	    }
	}
	else if (content == CONTENT_ALL){
	    /* specific xpath */
	    if (xmldb_get0(h, db, YB_MODULE, nsc, xpath?xpath:"/", 1, &xret, NULL, NULL) < 0) {
		if ((cbmsg = cbuf_new()) == NULL){
		    clicon_err(OE_UNIX, errno, "cbuf_new");
		    goto done;
		}
		cprintf(cbmsg, "Get %s datastore: %s", db, clicon_err_reason);
		if (netconf_operation_failed(cbret, "application", cbuf_get(cbmsg)) < 0)
		    goto done;
		goto ok;
	    }
	}
	/* CONTENT_NONCONFIG */
	else if ((xret = xml_new(DATASTORE_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)/* Only top tree */
	    goto done;
	break;
    }/* switch content */
    /* If not only config,
     * get state data from plugins as defined by plugin_statedata(), if any 
     */
    /* Read state */
    switch (content){
    case CONTENT_CONFIG:    /* config data only */
	break;
    case CONTENT_ALL:       /* both config and state */
    case CONTENT_NONCONFIG: /* state data only */
	if ((ret = get_client_statedata(h, xpath?xpath:"/", nsc, &xret)) < 0)
	    goto done;
	if (ret == 0){ /* Error from callback (error in xret) */
	    if (clixon_xml2cbuf(cbret, xret, 0, 0, -1, 0) < 0)
		goto done;
	    goto ok;
	}
	break;
    }
    if (with_defaults(xe, xret) < 0)
	goto done;
    if (content != CONTENT_CONFIG &&
	clicon_option_bool(h, "CLICON_VALIDATE_STATE_XML")){
	/* Check XML  by validating it. return internal error with error cause 
	 * Primarily intended for user-supplied state-data.
	 * The whole config tree must be present in case the state data references config data
	 */
	if ((ret = xml_yang_validate_all_top(h, xret, &xerr)) < 0) 
	    goto done;
	if (ret > 0 &&
	    (ret = xml_yang_validate_add(h, xret, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if (clicon_debug_get())
		clicon_log_xml(LOG_DEBUG, xret, "VALIDATE_STATE");
	    if (clixon_netconf_internal_error(xerr,
					      ". Internal error, state callback returned invalid XML",
					      NULL) < 0)
		goto done;
	    if (clixon_xml2cbuf(cbret, xerr, 0, 0, -1, 0) < 0)
		goto done;
	    goto ok;
	}
    } /* CLICON_VALIDATE_STATE_XML */

    if (content == CONTENT_NONCONFIG){ /* state only, all config should be removed now */
	/* Keep state data only, remove everything that is config. Note that state data
	 * may be a sub-part in a config tree, we need to traverse to find all
	 */
	if (xml_non_config_data(xret, NULL) < 0)
	    goto done;
	if (xml_tree_prune_flagged_sub(xret, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
	if (xml_apply(xret, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	    goto done;
    }
    if (xpath_vec(xret, nsc, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
	goto done;
    if (filter_xpath_again(h, yspec, xret, xvec, xlen, xpath, nsc) < 0)
	goto done;
    if (get_nacm_and_reply(h, xret, xvec, xlen, xpath, nsc, username, depth, cbret) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xvec)
	free(xvec);
    if (xret)
	xml_free(xret);
    if (cbreason)
	cbuf_free(cbreason);
    if (nsc0)
	xml_nsctx_free(nsc0);
    if (nsc)
	xml_nsctx_free(nsc);
    if (cbmsg)
	cbuf_free(cbmsg);
    if (reason)
	free(reason);
    if (xerr)
	xml_free(xerr);
    if (xpath)
	free(xpath);
    return retval;
}

/*! Retrieve all or part of a specified configuration.
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * @see from_client_get
 */
int
from_client_get_config(clicon_handle h,
		       cxobj        *xe,
		       cbuf         *cbret,
		       void         *arg,
		       void         *regarg)
{
    int                  retval = -1;
    char                *db;
    struct client_entry *ce = (struct client_entry *)arg;

    if ((db = netconf_db_find(xe, "source")) == NULL){
	clicon_err(OE_XML, 0, "db not found");
	goto done;
    }
    retval = get_common(h, ce, xe, CONTENT_CONFIG, db, cbret);
 done:
    return retval;
}

/*! Retrieve running configuration and device state information.
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 *
 * @see from_client_get_config 
 */
int
from_client_get(clicon_handle h,
		cxobj        *xe,
		cbuf         *cbret,
		void         *arg, 
		void         *regarg)
{
    netconf_content      content = CONTENT_ALL;
    char                *attr;
    struct client_entry *ce = (struct client_entry *)arg;
    
    /* Clixon extensions: content */
    if ((attr = xml_find_value(xe, "content")) != NULL)
	content = netconf_content_str2int(attr);
    return get_common(h, ce, xe, content, "running", cbret);
}
