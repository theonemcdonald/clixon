/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Clixon XML XPATH 1.0 according to https://www.w3.org/TR/xpath-10
 *
 * Some notes on namespace extensions in Netconf/Yang
 * RFC6241 8.9.1
 * The set of namespace declarations are those in scope on the <filter> element.
 * <rpc message-id="101" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
 *    <get-config>
 *       <filter xmlns:t="http://example.com/schema/1.2/config"
 *               type="xpath"
 *               select="/t:top/t:users/t:user[t:name='fred']"/>
 *       </get-config>
 * We need to add namespace context to the cpath tree, typically in eval. How do
 * we do that? 
 * One observation is that the namespace context is static, so it can not be a part
 * of the xpath-tree, which is context-dependent. 
 * Best is to send it as a (read-only) parameter to the xp_eval family of functions
 * as an exlicit namespace context.
 * For that you need an API to get/set namespaces: clixon_xml_nscache.c?
 * Then you need to fix API functions and this is the real work:
 * - Replace all existing functions or create new?
 * - Expose explicit namespace parameter, or xml object, or default namespace?
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
#include <fnmatch.h>
#include <stdint.h>
#include <assert.h>
#include <syslog.h>
#include <fcntl.h>
#include <math.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xpath_eval.h"

/* Mapping between XPATH operator string <--> int  */
const map_str2int xpopmap[] = {
    {"and",              XO_AND},
    {"or",               XO_OR},
    {"div",              XO_DIV},
    {"mod",              XO_MOD},
    {"+",                XO_ADD},
    {"*",                XO_MULT},
    {"-",                XO_SUB},
    {"=",                XO_EQ},
    {"!=",               XO_NE},
    {">=",               XO_GE},
    {"<=",               XO_LE},
    {"<",                XO_LT},
    {">",                XO_GT},
    {"|",                XO_UNION}, 
    {NULL,               -1}
};

/*!
 * @retval   -1     Error  XXX: retval -1 not properly handled 
 * @retval    0     No match  
 * @retval    1     Match 
 */
static int
nodetest_eval_node(cxobj      *x,
		   xpath_tree *xs,
		   cvec       *nsc)
{
    int  retval = -1;
    char *name1 = xml_name(x);
    char *prefix1 = xml_prefix(x);
    char *nsxml = NULL;     /* xml body namespace */
    char *nsxpath = NULL; /* xpath context namespace */
    char *prefix2 = NULL;
    char *name2 = NULL;

    /* Namespaces is s0, name is s1 */
    if (strcmp(xs->xs_s1, "*")==0)
	return 1;
    /* get namespace of xml tree */
    if (xml2ns(x, prefix1, &nsxml) < 0)
	goto done;
    prefix2 = xs->xs_s0;
    name2 = xs->xs_s1;
    /* Before going into namespaces, check name equality and filter out noteq  */
    if (strcmp(name1, name2) != 0){
	retval = 0; /* no match */
	goto done;
    }
    /* here names are equal 
     * Now look for namespaces
     * 1) prefix1 and prefix2 point to same namespace <<-- try this first
     * 2) prefix1 is equal to prefix2 <<-- then try this
     * (1) is strict yang xml
     * (2) without yang
     */
    if (nsc != NULL) { /* solution (1) */
	nsxpath = xml_nsctx_get(nsc, prefix2);
	if (nsxml != NULL && nsxpath != NULL)
	    retval = (strcmp(nsxml, nsxpath) == 0);
	else
	    retval = (nsxml == nsxpath); /* True only if both are NULL */
    }
    else{ /* solution (2) */
	if (prefix1 == NULL && prefix2 == NULL)
	    retval = 1;
	else if (prefix1 == NULL || prefix2 == NULL)
	    retval = 0;
	else
	    retval = strcmp(prefix1, prefix2) == 0;
    }
#if 0 /* debugging */
    /* If retval == 0 here, then there is name match, but not ns match */
    if (retval == 0){
	fprintf(stderr, "%s NOMATCH xml: (%s)%s\n\t\t xpath: (%s)%s\n", __FUNCTION__,
		name1, nsxml,
		name2, nsxpath);
    }
#endif
 done:	/* retval set in preceeding statement */
    return retval;
}
    
/*! Make a nodetest
 * @param[in] xs    XPATH stack of type XP_NODE or XP_NODE_FN
 * @param[in] nsc   XML Namespace context
 * @retval   -1     Error
 * @retval    0     No match  
 * @retval    1     Match
 * - node() is true for any node of any type whatsoever.
 * - text() is true for any text node.
 */
static int
nodetest_eval(cxobj      *x,
	      xpath_tree *xs,
	      cvec       *nsc)
{
    int   retval = 0; /* NB: no match is default (not error) */
    char *fn;

    if (xs->xs_type == XP_NODE)
	retval = nodetest_eval_node(x, xs, nsc);
    else if (xs->xs_type == XP_NODE_FN){
	fn = xs->xs_s0;
	if (strcmp(fn, "node")==0)
	    retval = 1;
	else if (strcmp(fn, "text")==0)
	    retval = 1;
    }
    /* note, retval set by previous statement */
    return retval;
}

/*!
 * @param[in]  xn
 * @param[in]  nodetest  XPATH stack
 * @param[in]  node_type
 * @param[in]  flags
 * @param[in]  nsc        XML Namespace context
 * @param[out] vec0
 * @param[out] vec0len
 */
int
nodetest_recursive(cxobj      *xn, 
		   xpath_tree *nodetest,
		   int         node_type,
		   uint16_t    flags,
		   cvec       *nsc,
		   cxobj    ***vec0,
		   size_t     *vec0len)
{
    int     retval = -1;
    cxobj  *xsub; 
    cxobj **vec = *vec0;
    size_t  veclen = *vec0len;

    xsub = NULL;
    while ((xsub = xml_child_each(xn, xsub, node_type)) != NULL) {
	if (nodetest_eval(xsub, nodetest, nsc) == 1){
	    clicon_debug(2, "%s %x %x", __FUNCTION__, flags, xml_flag(xsub, flags));
	    if (flags==0x0 || xml_flag(xsub, flags))
		if (cxvec_append(xsub, &vec, &veclen) < 0)
		    goto done;
	    //	    continue; /* Dont go deeper */
	}
	if (nodetest_recursive(xsub, nodetest, node_type, flags, nsc, &vec, &veclen) < 0)
	    goto done;
    }
    retval = 0;
    *vec0 = vec;
    *vec0len = veclen;
  done:
    return retval;
}

/*! Evaluate xpath step rule of an XML tree
 *
 * @param[in]  xc0  Incoming context
 * @param[in]  xs   XPATH node tree
 * @param[in]  nsc  XML Namespace context
 * @param[out] xrp  Resulting context
 *
 * - A node test that is a QName is true if and only if the type of the node (see [5 Data Model]) 
 * is the principal node type and has an expanded-name equal to the expanded-name specified by the QName.
 * - A node test * is true for any node of the principal node type.
 * - node() is true for any node of any type whatsoever.
 * - text() is true for any text node.
 */
static int
xp_eval_step(xp_ctx     *xc0,
	     xpath_tree *xs,
	     cvec       *nsc,
	     xp_ctx    **xrp)
{
    int         retval = -1;
    int         i;
    cxobj      *x;
    cxobj      *xv;
    cxobj      *xp;
    cxobj     **vec = NULL;
    size_t      veclen = 0;
    xpath_tree *nodetest = xs->xs_c0;
    xp_ctx     *xc = NULL;
    
    /* Create new xc */
    if ((xc = ctx_dup(xc0)) == NULL)
	goto done;
    switch (xs->xs_int){
    case A_ANCESTOR:
	break;
    case A_ANCESTOR_OR_SELF:
	break;
    case A_ATTRIBUTE: /* principal node type is attribute */
	break;
    case A_CHILD:
	if (xc->xc_descendant){
	    for (i=0; i<xc->xc_size; i++){
		xv = xc->xc_nodeset[i];
		if (nodetest_recursive(xv, nodetest, CX_ELMNT, 0x0, nsc, &vec, &veclen) < 0)
		    goto done;
	    }
	    xc->xc_descendant = 0;
	}
	else{
	    if (nodetest->xs_type==XP_NODE_FN &&
		nodetest->xs_s0 &&
		strcmp(nodetest->xs_s0,"current")==0){
		if (cxvec_append(xc->xc_initial, &vec, &veclen) < 0)
		    goto done;
	    }
	    else for (i=0; i<xc->xc_size; i++){
		xv = xc->xc_nodeset[i];
		x = NULL;
		while ((x = xml_child_each(xv, x, CX_ELMNT)) != NULL) {
		    /* xs->xs_c0 is nodetest */
		    if (nodetest == NULL || nodetest_eval(x, nodetest, nsc) == 1)
			if (cxvec_append(x, &vec, &veclen) < 0)
			    goto done;
		}
	    }
	}
	ctx_nodeset_replace(xc, vec, veclen);
	break;
    case A_DESCENDANT:
    case A_DESCENDANT_OR_SELF:
	for (i=0; i<xc->xc_size; i++){
	    xv = xc->xc_nodeset[i];
	    if (nodetest_recursive(xv, xs->xs_c0, CX_ELMNT, 0x0, nsc, &vec, &veclen) < 0)
		goto done;
	}
	ctx_nodeset_replace(xc, vec, veclen);
	break;
    case A_FOLLOWING:
	break;
    case A_FOLLOWING_SIBLING:
	break;
    case A_NAMESPACE: /* principal node type is namespace */
	break;
    case A_PARENT:
	veclen = xc->xc_size;
	vec = xc->xc_nodeset;
	xc->xc_size = 0;
	xc->xc_nodeset = NULL;
	for (i=0; i<veclen; i++){
	    x = vec[i];
	    if ((xp = xml_parent(x)) != NULL)
		if (cxvec_append(xp, &xc->xc_nodeset, &xc->xc_size) < 0)
		    goto done;
	}
	if (vec){
	    free(vec);
	    vec = NULL;
	}
	break;
    case A_PRECEEDING:
	break;
    case A_PRECEEDING_SIBLING:
	break;
    case A_SELF:
	break;
    default:
	clicon_err(OE_XML, 0, "No such axisname: %d", xs->xs_int);
	goto done;
	break;
    }
    if (xs->xs_c1){
	if (xp_eval(xc, xs->xs_c1, nsc, xrp) < 0)
	    goto done;
    }
    else{
	*xrp = xc;
	xc = NULL;
    }
    assert(*xrp);
    retval = 0;
 done:
    if (xc)
	ctx_free(xc);
    return retval;
}

/*! Evaluate xpath predicates rule
 *
 * pred -> pred expr
 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPATH node tree
 * @param[in]  nsc  XML Namespace context
 * @param[out] xrp  Resulting context
 *
 * A predicate filters a node-set with respect to an axis to produce a new 
 * node-set. For each node in the node-set to be filtered, the PredicateExpr is
 * evaluated with that node as the context node, with the number of nodes in
 * the node-set as the context size, and with the proximity position of the node
 * in the node-set with respect to the axis as the context position; if 
 * PredicateExpr evaluates to true for that node, the node is included in the
 * new node-set; otherwise, it is not included.
 * A PredicateExpr is evaluated by evaluating the Expr and converting the result
 * to a boolean. If the result is a
 * - number, the result will be converted to true if the number is equal to the 
 *   context position and will be converted to false otherwise; 
 * - if the result is not a number, then the result will be converted as if by a
 *   call to the boolean function. 
 * Thus a location path para[3] is equivalent to para[position()=3].
 */
static int
xp_eval_predicate(xp_ctx     *xc,
		  xpath_tree *xs,
		  cvec       *nsc,
		  xp_ctx    **xrp)
{
    int      retval = -1;
    xp_ctx  *xr0 = NULL;
    xp_ctx  *xr1 = NULL;
    xp_ctx  *xrc = NULL;
    int      i;
    cxobj   *x;
    xp_ctx  *xcc;
    
    if (xs->xs_c0 == NULL){ /* empty */
	if ((xr0 = ctx_dup(xc)) == NULL)
	    goto done;
    }
    else{ /* eval previous predicates */
	if (xp_eval(xc, xs->xs_c0, nsc, &xr0) < 0) 	
	    goto done;	
    }
    if (xs->xs_c1){
	/* Loop over each node in the nodeset */
	assert (xr0->xc_type == XT_NODESET);
	if ((xr1 = malloc(sizeof(*xr1))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memset(xr1, 0, sizeof(*xr1));
	xr1->xc_type = XT_NODESET;
	xr1->xc_node = xc->xc_node;
	xr1->xc_initial = xc->xc_initial;
	for (i=0; i<xr0->xc_size; i++){
	    x = xr0->xc_nodeset[i];
	    /* Create new context */
	    if ((xcc = malloc(sizeof(*xcc))) == NULL){
		clicon_err(OE_XML, errno, "malloc");
		goto done;
	    }
	    memset(xcc, 0, sizeof(*xcc));
	    xcc->xc_type = XT_NODESET;
	    xcc->xc_initial = xc->xc_initial;
	    xcc->xc_node = x;
	    /* For each node in the node-set to be filtered, the PredicateExpr is
	     * evaluated with that node as the context node */
	    if (cxvec_append(x, &xcc->xc_nodeset, &xcc->xc_size) < 0)
		goto done;
	    if (xp_eval(xcc, xs->xs_c1, nsc, &xrc) < 0)
		goto done;
	    if (xcc)
		ctx_free(xcc);
	    if (xrc->xc_type == XT_NUMBER){
		/* If the result is a number, the result will be converted to true
		   if the number is equal to the context position */
		if ((int)xrc->xc_number == i)
		    if (cxvec_append(x, &xr1->xc_nodeset, &xr1->xc_size) < 0)
			goto done;		    
	    }
	    else {
		/* if PredicateExpr evaluates to true for that node, the node is 
		   included in the new node-set */
		if (ctx2boolean(xrc))
		    if (cxvec_append(x, &xr1->xc_nodeset, &xr1->xc_size) < 0)
			goto done;		    
	    }
	    if (xrc)
		ctx_free(xrc);
	}

    }
    assert(xr0||xr1);
    if (xr1){
	*xrp = xr1;
	xr1 = NULL;
    }
    else
	if (xr0){
	    *xrp = xr0;
	    xr0 = NULL;
	}
    retval = 0;
 done:
    if (xr0)
	ctx_free(xr0);
    if (xr1)
	ctx_free(xr1);
    return retval;
}

/*! Given two XPATH contexts, eval logical  operations: or,and
 * The logical operators convert their operands to booleans
 * @param[in]  xc1  Context of operand1
 * @param[in]  xc2  Context of operand2
 * @param[in]  op   Relational operator
 * @param[out] xrp  Result context
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
xp_logop(xp_ctx    *xc1,
	 xp_ctx    *xc2,
	 enum xp_op op,
	 xp_ctx   **xrp)
{
    int     retval = -1;
    xp_ctx *xr = NULL;
    int     b1;
    int     b2;
    
    if ((xr = malloc(sizeof(*xr))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_initial = xc1->xc_initial;
    xr->xc_type = XT_BOOL;
    if ((b1 = ctx2boolean(xc1)) < 0)
	goto done;
    if ((b2 = ctx2boolean(xc2)) < 0)
	goto done;
    switch (op){
    case XO_AND:
	xr->xc_bool = b1 && b2;
	break;
    case XO_OR:
	xr->xc_bool = b1 || b2;
	break;
    default:
	clicon_err(OE_UNIX, errno, "%s:Invalid operator %s in this context",
		   __FUNCTION__, clicon_int2str(xpopmap,op));
	goto done;
    }
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! Given two XPATH contexts, eval numeric operations: +-*,div,mod
 * The numeric operators convert their operands to numbers as if by 
 * calling the number function.
 * @param[in]  xc1  Context of operand1
 * @param[in]  xc2  Context of operand2
 * @param[in]  op   Relational operator
 * @param[out] xrp  Result context
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
xp_numop(xp_ctx    *xc1,
	 xp_ctx    *xc2,
	 enum xp_op op,
	 xp_ctx   **xrp)
{
    int     retval = -1;
    xp_ctx *xr = NULL;
    double  n1;
    double  n2;
    
    if ((xr = malloc(sizeof(*xr))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_initial = xc1->xc_initial;
    xr->xc_type = XT_NUMBER;
    if (ctx2number(xc1, &n1) < 0)
	goto done;
    if (ctx2number(xc2, &n2) < 0)
	goto done;
    if (isnan(n1) || isnan(n2))
	xr->xc_number = NAN;
    else
	switch (op){
	case XO_DIV:
	    xr->xc_number = n1/n2;
	    break;
	case XO_MOD:
	    xr->xc_number = ((int)n1)%((int)n2);
	    break;
	case XO_ADD:
	    xr->xc_number = n1+n2;
	    break;
	case XO_MULT:
	    xr->xc_number = n1*n2;
	    break;
	case XO_SUB:
	    xr->xc_number = n1-n2;
	    break;
	default:
	    clicon_err(OE_UNIX, errno, "Invalid operator %s in this context",
		       clicon_int2str(xpopmap,op));
	    goto done;
	}
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! Given two XPATH contexts, eval relational operations: <>=
 * A RelationalExpr is evaluated by comparing the objects that result from 
 * evaluating the two operands.
 * This is covered:
 * (a) Both are INTs, BOOLs, STRINGs. Result type is boolean
 * (b) Both are nodesets and one is empty. Result type is boolean.
 * (c) One is nodeset and other is INT or STRING. Result type is nodeset
 * (d) All others (eg two nodesets, BOOL+STRING) are not supported.
 * Op is = EQ
 * From XPATH 1.0 standard, the evaluation has three variants:
 * (1) comparisons that involve node-sets are defined in terms of comparisons that
 * do not involve node-sets; this is defined uniformly for =, !=, <=, <, >= and >.
 * (2) comparisons that do not involve node-sets are defined for = and !=. 
 * (3) comparisons that do not involve node-sets are defined for <=, <, >= and >.
 * @param[in]  xc1  Context of operand1
 * @param[in]  xc2  Context of operand2
 * @param[in]  op   Relational operator
 * @param[out] xrp  Result context
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
xp_relop(xp_ctx    *xc1,
	 xp_ctx    *xc2,
	 enum xp_op op,
	 xp_ctx   **xrp)
{
    int     retval = -1;
    xp_ctx *xr = NULL;
    xp_ctx *xc;
    cxobj  *x;
    int     i;
    int     j;
    int     b;
    char   *s1;
    char   *s2;
    int     reverse = 0;
    double  n1, n2;
    
    if ((xr = malloc(sizeof(*xr))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_initial = xc1->xc_initial;
    xr->xc_type = XT_BOOL;
    if (xc1->xc_type == xc2->xc_type){ /* cases (2-3) above */
	switch (xc1->xc_type){
	case XT_NODESET:
	    /* If both are node-sets, then it is true iff the string value of one 
	       node in the first node-set and one in the second node-set is true */
	    for (i=0; i<xc1->xc_size; i++){
		if ((s1 = xml_body(xc1->xc_nodeset[i])) == NULL){
		    xr->xc_bool = 0;
		    goto ok;
		}
		for (j=0; j<xc2->xc_size; j++){
		    if ((s2 = xml_body(xc2->xc_nodeset[j])) == NULL){
			xr->xc_bool = 0;
			goto ok;
		    }
		    switch(op){
		    case XO_EQ:
			xr->xc_bool = (strcmp(s1, s2)==0);
			break;
		    case XO_NE:
			xr->xc_bool = (strcmp(s1, s2)!=0);
			break;
		    case XO_GE:
			xr->xc_bool = (strcmp(s1, s2)>=0);
			break;
		    case XO_LE:
			xr->xc_bool = (strcmp(s1, s2)<=0);
			break;
		    case XO_LT:
			xr->xc_bool = (strcmp(s1, s2)<0);
			break;
		    case XO_GT:
			xr->xc_bool = (strcmp(s1, s2)>0);
			break;
		    default:
			clicon_err(OE_XML, 0, "Operator %s not supported for nodeset/nodeset comparison", clicon_int2str(xpopmap,op));
			goto done;
			break;
		    }	    
		    if (xr->xc_bool) /* enough to find a single node */
			break;
		}
		if (xr->xc_bool) /* enough to find a single node */
		    break;
	    }
	    break;
	case XT_BOOL:
	    xr->xc_bool = (xc1->xc_bool == xc2->xc_bool);
	    break;
	case XT_NUMBER:
	    xr->xc_bool = (xc1->xc_number == xc2->xc_number);
	    break;
	case XT_STRING:
	    xr->xc_bool = (strcmp(xc1->xc_string, xc2->xc_string)==0);
	    break;
	}
    }
    else if (xc1->xc_type != XT_NODESET &&
	     xc2->xc_type != XT_NODESET){
	clicon_err(OE_XML, 0, "Mixed types not supported, %d %d", xc1->xc_type, xc2->xc_type);
	goto done;
    }
    else{ /* one is nodeset, ie (1) above */
	if (xc2->xc_type == XT_NODESET){
	    xc = xc2;
	    xc2 = xc1;
	    xc1 = xc;
	    reverse++; /* reverse */
	}
	/* xc1 is nodeset 
	 * xc2 is something else */
	switch (xc2->xc_type){
	case XT_BOOL:
	    /* comparison on the boolean and the result of converting the 
	       node-set to a boolean using the boolean function is true. */
	    b = ctx2boolean(xc1);
	    switch(op){
	    case XO_EQ:
		xr->xc_bool = (b == xc2->xc_bool);
		break;
	    case XO_NE:
		xr->xc_bool = (b != xc2->xc_bool);
		break;
	    default:
		clicon_err(OE_XML, 0, "Operator %s not supported for nodeset and bool", clicon_int2str(xpopmap,op));
		goto done;
		break;
	    } /* switch op */
	    break;
	case XT_STRING:
	    /* If one object to be compared is a node-set and the
	       other is a string, then the comparison will be true if and only
	       if there is a node in the node-set such that the result of
	       performing the comparison on the string-value of the node and 
	       the other string is true.*/
	    s2 = xc2->xc_string;
	    for (i=0; i<xc1->xc_size; i++){
		x = xc1->xc_nodeset[i]; /* node in nodeset */
		s1 = xml_body(x);
		switch(op){
		case XO_EQ:
		    if (s1 == NULL || s2 == NULL)
			xr->xc_bool = (s1==NULL && s2 == NULL);
		    else
			xr->xc_bool = (strcmp(s1, s2)==0);
		    break;
		case XO_NE:
		    if (s1 == NULL || s2 == NULL)
			xr->xc_bool = !(s1==NULL && s2 == NULL);
		    else
			xr->xc_bool = (strcmp(s1, s2));
		    break;
		default:
		    clicon_err(OE_XML, 0, "Operator %s not supported for nodeset and string", clicon_int2str(xpopmap,op));
		goto done;
		    break;
		}
		if (xr->xc_bool) /* enough to find a single node */
		    break;
	    }
	    break;
	case XT_NUMBER:
	    for (i=0; i<xc1->xc_size; i++){
		x = xc1->xc_nodeset[i]; /* node in nodeset */
		if (sscanf(xml_body(x), "%lf", &n1) != 1)
		    n1 = NAN;
		n2 = xc2->xc_number;
		switch(op){
		case XO_EQ:
		    xr->xc_bool = (n1 == n2);
		    break;
		case XO_NE:
		    xr->xc_bool = (n1 != n2);
		    break;
		case XO_GE:
		    xr->xc_bool = reverse?(n2 >= n1):(n1 >= n2);
		    break;
		case XO_LE:
		    xr->xc_bool = reverse?(n2 <= n1):(n1 <= n2);
		    break;
		case XO_LT:
		    xr->xc_bool = reverse?(n2 < n1):(n1 < n2);
		    break;
		case XO_GT:
		    xr->xc_bool = reverse?(n2 > n1):(n1 > n2);
		    break;
		default:
		    clicon_err(OE_XML, 0, "Operator %s not supported for nodeset and number", clicon_int2str(xpopmap,op));
		goto done;
		    break;
		}
		if (xr->xc_bool) /* enough to find a single node */
		    break;
	    }
	    break;
	default:
	    clicon_err(OE_XML, 0, "Type %d not supported", xc2->xc_type);
	} /* switch type */
    }
 ok:
    /* Just ensure bool is 0 or 1 */
    if (xr->xc_type == XT_BOOL && xr->xc_bool != 0)
	xr->xc_bool = 1;
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! Given two XPATH contexts, eval union operation
 * Both operands must be nodesets, otherwise empty nodeset is returned
 * @param[in]  xc1  Context of operand1
 * @param[in]  xc2  Context of operand2
 * @param[in]  op   Relational operator
 * @param[out] xrp  Result context
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
xp_union(xp_ctx    *xc1,
	 xp_ctx    *xc2,
	 enum xp_op op,
	 xp_ctx   **xrp)
{
    int     retval = -1;
    xp_ctx *xr = NULL;
    int     i;
    
    if (op != XO_UNION){
	clicon_err(OE_UNIX, errno, "%s:Invalid operator %s in this context",
		   __FUNCTION__, clicon_int2str(xpopmap,op));
	goto done;
    }
    if ((xr = malloc(sizeof(*xr))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_initial = xc1->xc_initial;
    xr->xc_type = XT_NODESET;

    for (i=0; i<xc1->xc_size; i++)
	if (cxvec_append(xc1->xc_nodeset[i], &xr->xc_nodeset, &xr->xc_size) < 0)
	    goto done;
    for (i=0; i<xc2->xc_size; i++){
	if (cxvec_append(xc2->xc_nodeset[i], &xr->xc_nodeset, &xr->xc_size) < 0)
	    goto done;
    }
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! Evaluate an XPATH on an XML tree

 * The initial sequence of steps selects a set of nodes relative to a context node. 
 * Each node in that set is used as a context node for the following step.
 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPATH node tree
 * @param[in]  nsc  XML Namespace context
 * @param[out] xrp  Resulting context
 * @retval     0    OK
 * @retval    -1    Error
 */
int
xp_eval(xp_ctx     *xc,
	xpath_tree *xs,
	cvec       *nsc,
	xp_ctx    **xrp)
{
    int        retval = -1;
    cxobj     *x;
    xp_ctx    *xr0 = NULL;
    xp_ctx    *xr1 = NULL;
    xp_ctx    *xr2 = NULL;
    int        use_xr0 = 0; /* In 2nd child use transitively result of 1st child */
    
    if (debug>1){
	cbuf *cb;
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	ctx_print(cb, +2, xc, xpath_tree_int2str(xs->xs_type));
	clicon_debug(2, "%s", cbuf_get(cb));
	cbuf_free(cb);
    }
    /* Pre-actions before check first child c0
     */
    switch (xs->xs_type){
    case XP_RELLOCPATH:
	if (xs->xs_int == A_DESCENDANT_OR_SELF)
	    xc->xc_descendant = 1; /* XXX need to set to 0 in sub */
	break;
    case XP_ABSPATH:
	/* Set context node to top node, and nodeset to that node only */
	x = xc->xc_node;
	while (xml_parent(x) != NULL)
	    x = xml_parent(x);
	xc->xc_node = x;
	xc->xc_nodeset[0] = x;
	xc->xc_size=1;
	/* // is short for /descendant-or-self::node()/ */
	if (xs->xs_int == A_DESCENDANT_OR_SELF)
	    xc->xc_descendant = 1; /* XXX need to set to 0 in sub */

	break;
    case XP_STEP:    /* XP_NODE is first argument -not called explicitly */
	if (xp_eval_step(xc, xs, nsc, xrp) < 0)
	    goto done;
	goto ok;
	break;
    case XP_PRED:
	if (xp_eval_predicate(xc, xs, nsc, xrp) < 0)
	    goto done;
	goto ok;
	break;
    default:
	break;
    }
    /* Eval first child c0
     */
    if (xs->xs_c0){
	if (xp_eval(xc, xs->xs_c0, nsc, &xr0) < 0) 	
	    goto done;
    }
    /* Actions between first and second child
     */
    switch (xs->xs_type){
    case XP_EXP:
	break;
    case XP_AND:
	break;
    case XP_RELEX: /* relexpr --> addexpr | relexpr relop addexpr */
	break;
    case XP_ADD: /* combine mult and add ops */
	break;
    case XP_UNION:
	break;
    case XP_PATHEXPR:
	break;
    case XP_LOCPATH:
	break;
    case XP_ABSPATH:
	use_xr0++;
	/* Special case, no c0 or c1, single "/" */
	if (xs->xs_c0 == NULL){
	    if ((xr0 = malloc(sizeof(*xr0))) == NULL){
		clicon_err(OE_UNIX, errno, "malloc");
		goto done;
	    }
	    memset(xr0, 0, sizeof(*xr0));
	    xr0->xc_initial = xc->xc_initial;
	    xr0->xc_type = XT_NODESET;
	    x = NULL;
	    while ((x = xml_child_each(xc->xc_node, x, CX_ELMNT)) != NULL) {
		if (cxvec_append(x, &xr0->xc_nodeset, &xr0->xc_size) < 0)
		    goto done;
	    }
	}
	break;
    case XP_RELLOCPATH:
	use_xr0++;
	if (xs->xs_int == A_DESCENDANT_OR_SELF)
	    xc->xc_descendant = 1; /* XXX need to set to 0 in sub */
	break;
    case XP_NODE:
	break;
    case XP_NODE_FN:
	break;
    case XP_PRI0:
	break;
    case XP_PRIME_NR: /* primaryexpr -> [<number>] */
	if ((xr0 = malloc(sizeof(*xr0))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memset(xr0, 0, sizeof(*xr0));
	xr0->xc_initial = xc->xc_initial;
	xr0->xc_type = XT_NUMBER;
	xr0->xc_number = xs->xs_double;
	break;
    case XP_PRIME_STR:
	if ((xr0 = malloc(sizeof(*xr0))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memset(xr0, 0, sizeof(*xr0));
	xr0->xc_initial = xc->xc_initial;
	xr0->xc_type = XT_STRING;
	xr0->xc_string = xs->xs_s0?strdup(xs->xs_s0):NULL;
	break;
    case XP_PRIME_FN:
	break;
    default:
	break;
    }
    /* Eval second child c0
     * Note, some operators like locationpath, need transitive context (use_xr0)
     */
    if (xs->xs_c1)
	if (xp_eval(use_xr0?xr0:xc, xs->xs_c1, nsc, &xr1) < 0) 
	    goto done;
    /* Actions after second child
     */
    if (xs->xs_c1)
	switch (xs->xs_type){
	case XP_AND: /* combine and and or ops */
	    if (xp_logop(xr0, xr1, xs->xs_int, &xr2) < 0)
		goto done;
	    break;
	case XP_RELEX: /* relexpr --> addexpr | relexpr relop addexpr */
	    if (xp_relop(xr0, xr1, xs->xs_int, &xr2) < 0)
		goto done;
	    break;
	case XP_ADD: /* combine mult and add ops */
	    if (xp_numop(xr0, xr1, xs->xs_int, &xr2) < 0)
		goto done;
	    break;
	case XP_UNION: /* combine and and or ops */
	    if (xp_union(xr0, xr1, xs->xs_int, &xr2) < 0)
		goto done;
	default:
	    break;
	}
    xc->xc_descendant = 0;
    assert(xr0||xr1||xr2);
    if (xr2){
	*xrp = xr2;
	xr2 = NULL;
    }
    else if (xr1){
	*xrp = xr1;
	xr1 = NULL;
    }
    else
	if (xr0){
	    *xrp = xr0;
	    xr0 = NULL;
	}
 ok:
    if (debug){
	cbuf *cb;
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	ctx_print(cb, -2, *xrp, xpath_tree_int2str(xs->xs_type));
	clicon_debug(2, "%s", cbuf_get(cb));
	cbuf_free(cb);
    }
    retval = 0;
 done:
    if (xr2)
	ctx_free(xr2);
    if (xr1)
	ctx_free(xr1);
    if (xr0)
	ctx_free(xr0);
    return retval;
} /* xp_eval */

