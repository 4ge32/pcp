/*
 * Copyright (c) 2012-2013 Red Hat.
 * Copyright (c) 1995-2000,2004 Silicon Graphics, Inc.  All Rights Reserved.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include <limits.h>
#include <assert.h>
#include "pmapi.h"
#include "impl.h"
#include "internal.h"

/* Host access control list */

typedef struct {
    char		*hostspec;	/* Host specification */
    __pmSockAddr	*hostid;	/* Partial host-id to match */
    __pmSockAddr	*hostmask;	/* Mask for wildcarding */
    int			level;		/* Level of wildcarding */
    unsigned int	specOps;	/* Mask of specified operations */
    unsigned int	denyOps;	/* Mask of disallowed operations */
    int			maxcons;	/* Max connections permitted (0 => no limit) */
    int			curcons;	/* Current # connections from matching clients */
} hostinfo;

static hostinfo	*hostlist;
static int	nhosts;
static int	szhostlist;

/* User access control list */

typedef struct {
    char		*username;	/* User specification */
    uid_t		userid;		/* User identifier to match */
    unsigned int	ngroups;	/* Count of groups to which the user belongs */
    gid_t		*groupids;	/* Names of groups to which the user belongs */
    unsigned int	specOps;	/* Mask of specified operations */
    unsigned int	denyOps;	/* Mask of disallowed operations */
    int			maxcons;	/* Max connections permitted (0 => no limit) */
    int			curcons;	/* Current # connections from matching clients */
} userinfo;

static userinfo	*userlist;
static int	nusers;
static int	szuserlist;

/* Group access control list */

typedef struct {
    char		*groupname;	/* Group specification */
    gid_t		groupid;	/* Group identifier to match */
    unsigned int	nusers;		/* Count of users in this group */
    uid_t		*userids;	/* Names of users in this group */
    unsigned int	specOps;	/* Mask of specified operations */
    unsigned int	denyOps;	/* Mask of disallowed operations */
    int			maxcons;	/* Max connections permitted (0 => no limit) */
    int			curcons;	/* Current # connections from matching clients */
} groupinfo;

static groupinfo *grouplist;
static int	ngroups;
static int	szgrouplist;

/* Mask of the operations defined by the user of the routines */
static unsigned int	all_ops;	/* mask of all operations specifiable */

/* This allows the set of valid operations to be specified.
 * Operations must be powers of 2.
 */
int
__pmAccAddOp(unsigned int op)
{
    unsigned int	i, mask;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;

    /* op must not be zero or clash with existing ops */
    if (op == 0 || (op & all_ops))
	return -EINVAL;

    /* Find the lowest bit in op that is set (WORD_BIT from limits.h is the
     * number of bits in an unsigned int)
     */
    for (i = 0; i < WORD_BIT; i++)
	if (op & (mask = 1 << i))
	    break;

    /* only one bit may be set in op */
    if (op & ~mask)
	return -EINVAL;

    all_ops |= mask;
    return 0;
}

/* Get the host id for this host.  The host id is used when translating
 * references to localhost into the host's real IP address during parsing of
 * the access control section of the config file.  It may also used when
 * checking for incoming connections from localhost.
 */

static int		gotmyhostid;
static __pmHostEnt	*myhostid;
static char		myhostname[MAXHOSTNAMELEN+1];

/*
 * Always called with __pmLock_libpcp already held, so accessing
 * gotmyhostid, myhostname, myhostid and gethostname() call are all 
 * thread-safe.
 */
static int
getmyhostid(void)
{
    if (gethostname(myhostname, MAXHOSTNAMELEN) < 0) {
	__pmNotifyErr(LOG_ERR, "gethostname failure\n");
	return -1;
    }
    myhostname[MAXHOSTNAMELEN-1] = '\0';

    if ((myhostid = __pmGetAddrInfo(myhostname)) == NULL) {
	__pmNotifyErr(LOG_ERR, "__pmGetAddrInfo(%s), %s\n",
		     myhostname, hoststrerror());
	return -1;
    }
    gotmyhostid = 1;
    return 0;
}

/* Used for saving the current state of the access lists */

enum { HOSTS_SAVED = 0x1, USERS_SAVED = 0x2, GROUPS_SAVED = 0x4 };
static int	saved;
static hostinfo	*oldhostlist;
static int	oldnhosts;
static int	oldszhostlist;
static userinfo	*olduserlist;
static int	oldnusers;
static int	oldszuserlist;
static groupinfo *oldgrouplist;
static int	oldngroups;
static int	oldszgrouplist;

/* Save the current access control lists.
 * Returns 0 for success or a negative error code on error.
 */
int
__pmAccSaveLists(void)
{
    int sts, code = 0;

    if ((sts = __pmAccSaveHosts()) < 0)
	code = sts;
    if ((sts = __pmAccSaveUsers()) < 0)
	code = sts;
    if ((sts = __pmAccSaveGroups()) < 0)
	code = sts;
    return code;
}

int
__pmAccSaveHosts(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (saved & HOSTS_SAVED)
	return PM_ERR_TOOBIG;

    saved |= HOSTS_SAVED;
    oldhostlist = hostlist;
    oldnhosts = nhosts;
    oldszhostlist = szhostlist;
    hostlist = NULL;
    nhosts = 0;
    szhostlist = 0;
    return 0;
}

int
__pmAccSaveUsers(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (saved & USERS_SAVED)
	return PM_ERR_TOOBIG;

    saved |= USERS_SAVED;
    olduserlist = userlist;
    oldnusers = nusers;
    oldszuserlist = szuserlist;
    userlist = NULL;
    nusers = 0;
    szuserlist = 0;
    return 0;
}

int
__pmAccSaveGroups(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (saved & GROUPS_SAVED)
	return PM_ERR_TOOBIG;

    saved |= GROUPS_SAVED;
    oldgrouplist = grouplist;
    oldngroups = ngroups;
    oldszgrouplist = szgrouplist;
    grouplist = NULL;
    ngroups = 0;
    szgrouplist = 0;
    return 0;
}

/* Free the current access lists.  These are done automatically by
 * __pmAccRestoreLists so there is no need for them to be globally visible.
 * A caller of these routines should never need to dispose of the access lists
 * once they has been built.
 */
static void
accfreehosts(void)
{
    int		i;

    if (szhostlist) {
	for (i = 0; i < nhosts; i++)
	    if (hostlist[i].hostspec != NULL)
		free(hostlist[i].hostspec);
	free(hostlist);
    }
    hostlist = NULL;
    nhosts = 0;
    szhostlist = 0;
}

static void
accfreeusers(void)
{
    int		i;

    if (szuserlist) {
	for (i = 1; i < nusers; i++) {
	    free(userlist[i].username);
	    if (userlist[i].ngroups)
	        free(userlist[i].groupids);
	}
	free(userlist);
    }
    userlist = NULL;
    nusers = 0;
    szuserlist = 0;
}

static void
accfreegroups(void)
{
    int		i;

    if (szgrouplist) {
	for (i = 1; i < ngroups; i++) {
	    free(grouplist[i].groupname);
	    if (grouplist[i].nusers)
		free(grouplist[i].userids);
	}
	free(grouplist);
    }
    grouplist = NULL;
    ngroups = 0;
    szgrouplist = 0;
}

/* Restore the previously saved access lists.  Any current list is freed.
 * Returns 0 for success or a negative error code on error.
 */
int
__pmAccRestoreLists(void)
{
    int sts, code = 0;

    if ((sts = __pmAccRestoreHosts()) < 0)
	code = sts;
    if ((sts = __pmAccRestoreUsers()) < 0 && !code)
	code = sts;
    if ((sts = __pmAccRestoreGroups()) < 0 && !code)
	code = sts;
    return code;
}

int
__pmAccRestoreHosts(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (!(saved & HOSTS_SAVED))
	return PM_ERR_TOOSMALL;

    accfreehosts();
    saved &= ~HOSTS_SAVED;
    hostlist = oldhostlist;
    nhosts = oldnhosts;
    szhostlist = oldszhostlist;
    return 0;
}

int
__pmAccRestoreUsers(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (!(saved & USERS_SAVED))
	return PM_ERR_TOOSMALL;

    accfreeusers();
    saved &= ~USERS_SAVED;
    userlist = olduserlist;
    nusers = oldnusers;
    szuserlist = oldszuserlist;
    return 0;
}

int
__pmAccRestoreGroups(void)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (!(saved & GROUPS_SAVED))
	return PM_ERR_TOOSMALL;

    accfreegroups();
    saved &= ~GROUPS_SAVED;
    grouplist = oldgrouplist;
    ngroups = oldngroups;
    szgrouplist = oldszgrouplist;
    return 0;
}

/* Free the previously saved access lists.  These should be called when the saved
 * access lists are no longer required (typically because the new ones supercede
 * the old, have been verified as valid and correct, etc).
 */
void
__pmAccFreeSavedLists(void)
{
    __pmAccFreeSavedHosts();
    __pmAccFreeSavedUsers();
    __pmAccFreeSavedGroups();
}

void
__pmAccFreeSavedHosts(void)
{
    int		i;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;
    if (!(saved & HOSTS_SAVED))
	return;

    if (oldszhostlist) {
	for (i = 0; i < oldnhosts; i++)
	    if (oldhostlist[i].hostspec != NULL)
		free(oldhostlist[i].hostspec);
	free(oldhostlist);
    }
    saved &= ~HOSTS_SAVED;
}

void
__pmAccFreeSavedUsers(void)
{
    int		i;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;
    if (!(saved & USERS_SAVED))
	return;

    if (oldszuserlist) {
	for (i = 1; i < oldnusers; i++) {
	    free(olduserlist[i].username);
	    if (olduserlist[i].ngroups)
		free(olduserlist[i].groupids);
	}
	free(olduserlist);
    }
    saved &= ~USERS_SAVED;
}

void
__pmAccFreeSavedGroups(void)
{
    int		i;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;
    if (!(saved & GROUPS_SAVED))
	return;

    if (oldszgrouplist) {
	for (i = 1; i < oldngroups; i++) {
	    free(oldgrouplist[i].groupname);
	    if (oldgrouplist[i].nusers)
		free(oldgrouplist[i].userids);
	}
	free(oldgrouplist);
    }
    saved &= ~GROUPS_SAVED;
}

/* Build up strings representing the ip address and the mask.
 * Compute the wildcard level as we go.
 */
static int
parseInetWildCard(const char *name, char *ip, char *mask)
{
    int level;
    int ipIx, maskIx;
    int i, n;
    const char *p;

    /* Accept "*" or ".*" as the inet full wild card spec. */
    level = 4;
    ipIx = maskIx = 0;
    p = name;
    if (*p == '.') {
        ++p;
	if (*p != '*') {
	    __pmNotifyErr(LOG_ERR, "Bad IP address wildcard, %s\n", name);
	    return -EINVAL;
	}
    }
    for (/**/; *p && *p != '*' ; p++) {
        n = (int)strtol(p, (char **)&p, 10);
	if ((*p != '.' && *p != '*') || n < 0 || n > 255) {
	    __pmNotifyErr(LOG_ERR, "Bad IP address wildcard, %s\n", name);
	    return -EINVAL;
	}
	if (ipIx != 0) {
	    ipIx += sprintf(ip + ipIx, ".");
	    maskIx += sprintf(mask + maskIx, ".");
	}
	ipIx += sprintf(ip + ipIx, "%d", n);
	maskIx += sprintf(mask + maskIx, "255");
	--level;
	/* Check the wildcard level, 0 is exact match, 4 is most general */
	if (level < 1) {
	    __pmNotifyErr(LOG_ERR, "Too many dots in host pattern \"%s\"\n", name);
	    return -EINVAL;
	}
    }
    /* Add zeroed components for the wildcarded levels. */
    for (i = 0; i < level; ++i) {
        if (ipIx != 0) {
	    ipIx += sprintf(ip + ipIx, ".");
	    maskIx += sprintf(mask + maskIx, ".");
	}
	ipIx += sprintf(ip + ipIx, "0");
	maskIx += sprintf(mask + maskIx, "0");
    }
    return level;
}

static int
parseIPv6WildCard(const char *name, char *ip, char *mask)
{
    int level;
    int ipIx, maskIx;
    int emptyRegion;
    int n;
    const char *p;

    /* Accept ":*" as the IPv6 full wild card spec. Otherwise,
       if the string starts with ':', then the second character must also be a ':'
       which would form a region of zeroes of unspecified length. */
    level = 8;
    emptyRegion = 0;
    ipIx = maskIx = 0;
    p = name;
    if (*p == ':') {
        ++p;
	if (*p != '*') {
	    if (*p != ':') {
		__pmNotifyErr(LOG_ERR, "Bad IP address wildcard, %s\n", name);
		return -EINVAL;
	    }
	    ipIx = sprintf(ip, ":");
	    maskIx = sprintf(mask, ":");
	    /* The second colon will be detected in the loop below. */
	}
    }

    for (/**/; *p && *p != '*' ; p++) {
        /* Check for an empty region. There can only be one. */
        if (*p == ':') {
	    if (emptyRegion) {
	        __pmNotifyErr(LOG_ERR, "Too many empty regions in host pattern \"%s\"\n", name);
		return -EINVAL;
	    }
	    emptyRegion = 1;
	    ipIx += sprintf(ip + ipIx, ":");
	    maskIx += sprintf(mask + maskIx, ":");
	}
	else {
	    n = (int)strtol(p, (char **)&p, 16);
	    if ((*p != ':' && *p != '*') || n < 0 || n > 0xffff) {
	        __pmNotifyErr(LOG_ERR, "Bad IP address wildcard, %s\n", name);
		return -EINVAL;
	    }
	    if (ipIx != 0) {
	        ipIx += sprintf(ip + ipIx, ":");
		maskIx += sprintf(mask + maskIx, ":");
	    }
	    ipIx += sprintf(ip + ipIx, "%x", n);
	    maskIx += sprintf(mask + maskIx, "ffff");
	}
	--level;
	/* Check the wildcard level, 0 is exact match, 8 is most general */
	if (level < 1) {
	    __pmNotifyErr(LOG_ERR, "Too many colons in host pattern \"%s\"\n", name);
	    return -EINVAL;
	}
    }
    /* Add zeroed components for the wildcarded levels.
       If the entire address is wildcarded then return the zero address. */
    if (level == 8 || (level == 7 && emptyRegion)) {
        /* ":*" or "::*" */
        strcpy(ip, "::");
        strcpy(mask, "::");
	level = 8;
    }
    else if (emptyRegion) {
        /* If there was an empty region, then we assume that the wildcard represents the final
	   segment of the spec only. */
        sprintf(ip + ipIx, ":0");
	sprintf(mask + maskIx, ":0");
    }
    else {
        /* no empty region, so use one to finish off the address and the mask */
        sprintf(ip + ipIx, "::");
	sprintf(mask + maskIx, "::");
    }
    return level;
}

static int
parseWildCard(const char *name, char *ip, char *mask)
{
    /* Names containing ':' are IPv6. The IPv6 full wildcard spec is ":*". */
    if (strchr(name, ':') != NULL)
        return parseIPv6WildCard(name, ip, mask);

    /* Names containing '.' are inet. The inet full wildcard spec ".*". */
    if (strchr(name, '.') != NULL)
        return parseInetWildCard(name, ip, mask);

    __pmNotifyErr(LOG_ERR, "Bad IP address wildcard, %s\n", name);
    return -EINVAL;
}

/* Information representing an access specification. */
struct accessSpec {
    char		*name;
    __pmSockAddr	*hostid;
    __pmSockAddr	*hostmask;
    int			level;
};

/* Construct the proper spec for the given wildcard. */
static int
getWildCardSpec(const char *name, struct accessSpec *spec)
{
    char ip[INET6_ADDRSTRLEN];
    char mask[INET6_ADDRSTRLEN];

    /* Build up strings representing the ip address and the mask. Compute the wildcard
       level as we go. */
    spec->level = parseWildCard(name, ip, mask);
    if (spec->level < 0)
	return spec->level;

    /* Now create socket addresses for the ip address and mask. */
    spec->hostid = __pmStringToSockAddr(ip);
    if (spec->hostid == NULL) {
	__pmNotifyErr(LOG_ERR, "__pmStringToSockAddr failure\n");
	return -ENOMEM;
    }
    spec->hostmask = __pmStringToSockAddr(mask);
    if (spec->hostmask == NULL) {
	__pmNotifyErr(LOG_ERR, "__pmStringToSockAddr failure\n");
	__pmSockAddrFree(spec->hostid);
	return -ENOMEM;
    }

    /* Do this last since a valid name indicates a valid spec. */
    spec->name = strdup(name);
    return 0;
}

/* Determine all of the access specs which result from the given name. */
static struct accessSpec *
getHostAccessSpecs(const char *name, int *sts)
{
    struct accessSpec	*specs;
    size_t		specSize;
    size_t		specIx;
    size_t		ix;
    size_t		need;
    __pmSockAddr	*myAddr;
    __pmHostEnt		*servInfo;
    void		*enumIx;
    int			family;
    const char		*realname;
    const char		*p;

    /* If the general wildcard ("*") is specified, then generate individual wildcards for
       inet and, if supported, IPv6. */
    *sts = 0;
    if (strcmp(name, "*") == 0) {
	const char *ipv6 = __pmGetAPIConfig("ipv6");

	/* Use calloc so that the final entry is zeroed. */
	specs = calloc(3, sizeof(*specs));
	if (specs == NULL)
	    __pmNoMem("Access Spec List", 3 * sizeof(*specs), PM_FATAL_ERR);
	getWildCardSpec(".*", &specs[0]);
	if (ipv6 != NULL && strcmp(ipv6, "true") == 0)
	    getWildCardSpec(":*", &specs[1]); /* Guaranteed to succeed. */
	return specs;
    }

    /* If any other wildcard is specified, then our list will contain that single item. */
    if ((p = strchr(name, '*')) != NULL) {
	if (p[1] != '\0') {
	    __pmNotifyErr(LOG_ERR,
			  "Wildcard in host pattern \"%s\" is not at the end\n",
			  name);
	    *sts = -EINVAL;
	    return NULL;
	}
	/* Use calloc so that the final entry is zeroed. */
	specs = calloc(2, sizeof(*specs));
	if (specs == NULL)
	    __pmNoMem("Access Spec List", 2 * sizeof(*specs), PM_FATAL_ERR);
	*sts = getWildCardSpec(name, &specs[0]);
	return specs;
    }

    /* Assume we have a host name or address. Resolve it and contruct a list containing all of the
       resolved addresses. If the name is "localhost", then resolve using the actual host name. */
    if (strcasecmp(name, "localhost") == 0) {
	if (!gotmyhostid) {
	    if (getmyhostid() < 0) {
		__pmNotifyErr(LOG_ERR, "Can't get host name/IP address, giving up\n");
		*sts = -EHOSTDOWN;
		return NULL;	/* should never happen! */
	    }
	}
	realname = myhostname;
    }
    else
	realname = name;

    *sts = -EHOSTUNREACH;
    specs = NULL;
    specSize = 0;
    specIx = 0;
    if ((servInfo = __pmGetAddrInfo(realname)) != NULL) {
	/* Collect all of the resolved addresses. Check for the end of the list within the
	   loop since we need to add an empty entry and the code to grow the list is within the
	   loop. */
	enumIx = NULL;
	for (myAddr = __pmHostEntGetSockAddr(servInfo, &enumIx);
	     /**/;
	     myAddr = __pmHostEntGetSockAddr(servInfo, &enumIx)) {
	    if (specIx == specSize) {
		specSize = specSize == 0 ? 4 : specSize * 2;
		need = specSize * sizeof(*specs);
		specs = realloc(specs, need);
		if (specs == NULL) {
		    __pmNoMem("Access Spec List", need, PM_FATAL_ERR);
		}
	    }
	    /* No more addresses? */
	    if (myAddr == NULL) {
		specs[specIx].name = NULL;
		break;
	    }
	    /* Don't add any duplicate entries. It causes false permission clashes. */
	    for (ix = 0; ix < specIx; ++ix) {
		if (__pmSockAddrCompare(myAddr, specs[ix].hostid) == 0)
		    break;
	    }
	    if (ix < specIx){
		__pmSockAddrFree(myAddr);
		continue;
	    }
	    /* Add the new address and its corrsponding mask. */
	    family = __pmSockAddrGetFamily(myAddr);
	    if (family == AF_INET)
		specs[specIx].hostmask = __pmStringToSockAddr("255.255.255.255");
	    else if (family == AF_INET6)
		specs[specIx].hostmask = __pmStringToSockAddr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
	    else {
		__pmNotifyErr(LOG_ERR, "Unsupported socket address family: %d\n", family);
		__pmSockAddrFree(myAddr);
		continue;
	    }
	    specs[specIx].hostid = myAddr;
	    specs[specIx].level = 0;
	    specs[specIx].name = strdup(name);
	    *sts = 0;
	    ++specIx;
	}
	__pmHostEntFree(servInfo);
    }
    else {
	__pmNotifyErr(LOG_ERR, "__pmGetAddrInfo(%s), %s\n",
		      realname, hoststrerror());
    }

    /* Return NULL if nothing was discovered. *sts is already set. */
    if (specIx == 0 && specs != NULL) {
	free(specs);
	specs = NULL;
    }
    return specs;
}

/* Routine to add a group to the group access list with a specified set of
 * permissions and a maximum connection limit.
 * specOps is a mask.  Only bits corresponding to operations specified by
 *	__pmAccAddOp have significance.  A 1 bit indicates that the
 *	corresponding bit in the denyOps mask is to be used.  A zero bit in
 *	specOps means the corresponding bit in denyOps should be ignored.
 * denyOps is a mask where a 1 bit indicates that permission to perform the
 *	corresponding operation should be denied.
 * maxcons is a maximum connection limit for individial groups.  Zero means
 *      unspecified, which will allow unlimited connections or a subsequent
 *      __pmAccAddUser call with the same group to override maxcons.
 *
 * Returns a negated system error code on failure.
 */

int
__pmAccAddGroup(const char *name, unsigned int specOps, unsigned int denyOps, int maxcons)
{
    size_t		need;
    unsigned int	nusers;
    int			i = 0, sts, wildcard, found = 0;
    char		errmsg[256];
    char		*groupname;
    uid_t		*userids;
    gid_t		groupid;
    groupinfo		*gp;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (specOps & ~all_ops)
	return -EINVAL;
    if (maxcons < 0)
	return -EINVAL;

    wildcard = (strcmp(name, "*") == 0);
    if (!wildcard) {
	if ((sts = __pmGroupID(name, &groupid)) < 0) {
	    __pmNotifyErr(LOG_ERR, "Failed to lookup group \"%s\": %s\n",
				name, sts == 0 ? "no such group exists" :
				pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	    return -EINVAL;
	}

	/* Search for a match to this group in the groups access table */
	for (i = 1; i < ngroups; i++) {
	    if (groupid == grouplist[i].groupid) {
		found = 1;
		break;
	    }
	}
    }

    /* Check and augment existing group access list entry for this groupid
     * if a match was found, otherwise insert a new entry in list.
     */
    if (found) {
	/* If the specified operations overlap, they must agree */
	gp = &grouplist[i];
	if ((gp->maxcons && maxcons && gp->maxcons != maxcons) ||
	    ((gp->specOps & specOps) &&
	     ((gp->specOps & gp->denyOps) ^ (specOps & denyOps)))) {
		__pmNotifyErr(LOG_ERR,
			  "Permission clash for group %s with earlier statement\n",
			  name);
	    return -EINVAL;
	}
	gp->specOps |= specOps;
	gp->denyOps |= (specOps & denyOps);
	if (maxcons)
	    gp->maxcons = maxcons;
    } else {
	/* Make the group access list larger if required */
	if (ngroups == szgrouplist) {
	    szgrouplist += 8;
	    need = szgrouplist * sizeof(groupinfo);
	    grouplist = (groupinfo *)realloc(grouplist, need);
	    if (grouplist == NULL)
		__pmNoMem("AddGroup enlarge", need, PM_FATAL_ERR);
	}
	/* insert a permanent initial entry for '*' group wildcard */
	if (ngroups == 0) {
	    gp = &grouplist[0];
	    memset(gp, 0, sizeof(*gp));
	    gp->groupname = "*";
	    if (!wildcard)	/* if so, we're adding two entries */
	        i = ++ngroups;
	}
	if (wildcard) {
	    i = 0;	/* always the first entry, setup constants */
	    gp = &grouplist[i];  /* for use when overwriting below */
	    groupname = gp->groupname;
	    groupid = gp->groupid;
	    userids = gp->userids;
	    nusers = gp->nusers;
	} else if ((sts = __pmGroupsUserIDs(name, &userids, &nusers)) < 0) {
	    __pmNotifyErr(LOG_ERR,
			"Failed to lookup users in group \"%s\": %s\n",
			name, pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	    return sts;
	} else if ((groupname = strdup(name)) == NULL) {
	    __pmNoMem("AddGroup name", strlen(name)+1, PM_FATAL_ERR);
	}
	gp = &grouplist[i];
	gp->groupname = groupname;
	gp->groupid = groupid;
	gp->userids = userids;
	gp->nusers = nusers;
	gp->specOps = specOps;
	gp->denyOps = specOps & denyOps;
	gp->maxcons = maxcons;
	gp->curcons = 0;
	ngroups++;
    }

    return 0;
}

/* Routine to add a user to the user access list with a specified set of
 * permissions and a maximum connection limit.
 * specOps is a mask.  Only bits corresponding to operations specified by
 *	__pmAccAddOp have significance.  A 1 bit indicates that the
 *	corresponding bit in the denyOps mask is to be used.  A zero bit in
 *	specOps means the corresponding bit in denyOps should be ignored.
 * denyOps is a mask where a 1 bit indicates that permission to perform the
 *	corresponding operation should be denied.
 * maxcons is a maximum connection limit for individial users.  Zero means
 *      unspecified, which will allow unlimited connections or a subsequent
 *      __pmAccAddUser call with the same user to override maxcons.
 *
 * Returns a negated system error code on failure.
 */

int
__pmAccAddUser(const char *name, unsigned int specOps, unsigned int denyOps, int maxcons)
{
    size_t		need;
    unsigned int	ngroups;
    int			i = 0, sts, wildcard, found = 0;
    char		errmsg[256];
    char		*username;
    uid_t		userid;
    gid_t		*groupids;
    userinfo		*up;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (specOps & ~all_ops)
	return -EINVAL;
    if (maxcons < 0)
	return -EINVAL;

    wildcard = (strcmp(name, "*") == 0);
    if (!wildcard) {
	if ((sts = __pmUserID(name, &userid)) < 0) {
	    __pmNotifyErr(LOG_ERR, "Failed to lookup user \"%s\": %s\n",
				name, sts == 0 ? "no such user exists" :
				pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	    return -EINVAL;
	}

	/* Search for a match to this user in the existing table of users. */
	for (i = 1; i < nusers; i++) {
	    if (userid == userlist[i].userid) {
		found = 1;
		break;
	    }
	}
    }

    /* Check and augment existing user access list entry for this userid if a
     * match was found otherwise insert a new entry in list.
     */
    if (found) {
	/* If the specified operations overlap, they must agree */
	up = &userlist[i];
	if ((up->maxcons && maxcons && up->maxcons != maxcons) ||
	    ((up->specOps & specOps) &&
	     ((up->specOps & up->denyOps) ^ (specOps & denyOps)))) {
		__pmNotifyErr(LOG_ERR,
			  "Permission clash for user %s with earlier statement\n",
			  name);
		return -EINVAL;
	}
	up->specOps |= specOps;
	up->denyOps |= (specOps & denyOps);
	if (maxcons)
	    up->maxcons = maxcons;
    } else {
	/* Make the user access list larger if required */
	if (nusers == szuserlist) {
	    szuserlist += 8;
	    need = szuserlist * sizeof(userinfo);
	    userlist = (userinfo *)realloc(userlist, need);
	    if (userlist == NULL) {
		__pmNoMem("AddUser enlarge", need, PM_FATAL_ERR);
	    }
	}
	/* insert a permanent initial entry for '*' user wildcard */
	if (nusers == 0) {
	    up = &userlist[0];
	    memset(up, 0, sizeof(*up));
	    up->username = "*";
	    if (!wildcard)	/* if so, we're adding two entries */
	        i = ++nusers;
	}
	if (wildcard) {
	    i = 0;	/* always the first entry, setup constants */
	    up = &userlist[i];   /* for use when overwriting below */
	    username = up->username;
	    userid = up->userid;
	    ngroups = up->ngroups;
	    groupids = up->groupids;
	} else if ((sts = __pmUsersGroupIDs(name, &groupids, &ngroups)) < 0) {
	    __pmNotifyErr(LOG_ERR,
			"Failed to lookup groups for user \"%s\": %s\n",
			name, pmErrStr_r(sts, errmsg, sizeof(errmsg)));
	    return sts;
	} else if ((username = strdup(name)) == NULL) {
	    __pmNoMem("AddUser name", strlen(name)+1, PM_FATAL_ERR);
	}
	up = &userlist[i];
	up->username = username;
	up->userid = userid;
	up->groupids = groupids;
	up->ngroups = ngroups;
	up->specOps = specOps;
	up->denyOps = specOps & denyOps;
	up->maxcons = maxcons;
	up->curcons = 0;
	nusers++;
    }

    return 0;
}

/* Routine to add a host to the host access list with a specified set of
 * permissions and a maximum connection limit.
 * specOps is a mask.  Only bits corresponding to operations specified by
 *	__pmAccAddOp have significance.  A 1 bit indicates that the
 *	corresponding bit in the denyOps mask is to be used.  A zero bit in
 *	specOps means the corresponding bit in denyOps should be ignored.
 * denyOps is a mask where a 1 bit indicates that permission to perform the
 *	corresponding operation should be denied.
 * maxcons is a maximum connection limit for clients on hosts matching the host
 *	id.  Zero means unspecified, which will allow unlimited connections or
 *	a subsequent __pmAccAddHost call with the same host to override maxcons.
 *
 * Returns a negated system error code on failure.
 */

int
__pmAccAddHost(const char *name, unsigned int specOps, unsigned int denyOps, int maxcons)
{
    size_t		need;
    int			i, sts;
    struct accessSpec	*specs;
    struct accessSpec	*spec;
    hostinfo		*hp;
    int			found;
    char		*prevHost;
    char		*prevName;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;
    if (specOps & ~all_ops)
	return -EINVAL;
    if (maxcons < 0)
	return -EINVAL;

    /* The specified name may result in more than one access specification. */
    specs = getHostAccessSpecs(name, &sts);
    if (specs == NULL)
	return sts;

    /* Search for a match to each spec in the existing table of hosts. We will either use
       or free the host id, mask and name of each spec as we go. */
    prevHost = NULL;
    prevName = NULL;
    found = 0;
    for (spec = specs; spec->name != NULL; ++spec) {
	sts = 0;
	for (i = 0; i < nhosts; i++) {
	    if (hostlist[i].level > spec->level)
		break;
	    /* hostid AND level must match.  Wildcarded IP addresses have zero in
	     * the unspecified components.  Distinguish between 155.23.6.0 and
	     * 155.23.6.* or 155.23.0.0 and 155.23.* by wildcard level.  IP
	     * addresses shouldn't have zero in last position but to deal with
	     * them just in case.
	     */
	    if (__pmSockAddrCompare(spec->hostid, hostlist[i].hostid) == 0 &&
		spec->level == hostlist[i].level) {
		sts = 1;
		break;
	    }
	}

	/* Check and augment existing host access list entry for this host id if a
	 * match was found (sts == 1) otherwise insert a new entry in list.
	 */
	if (sts == 1) {
	    __pmSockAddrFree(spec->hostid);
	    __pmSockAddrFree(spec->hostmask);

	    /* If the specified operations overlap, they must agree */
	    hp = &hostlist[i];
	    if ((hp->maxcons && maxcons && hp->maxcons != maxcons) ||
		((hp->specOps & specOps) &&
		 ((hp->specOps & hp->denyOps) ^ (specOps & denyOps)))) {
		/* Suppress duplicate messages. These can occur when a host resolves to more
		   than one address. */
		if (prevName == NULL ||
		    strcmp(prevName, spec->name) != 0 || strcmp(prevHost, hp->hostspec) != 0) {
		    __pmNotifyErr(LOG_ERR,
				  "Permission clash for %s with earlier statement for %s\n",
				  spec->name, hp->hostspec);
		    if (prevName != NULL) {
			free(prevName);
			free(prevHost);
		    }
		    prevName = strdup(spec->name);
		    prevHost = strdup(hp->hostspec);
		}
		free(spec->name);
		continue;
	    }
	    free(spec->name);
	    hp->specOps |= specOps;
	    hp->denyOps |= (specOps & denyOps);
	    if (maxcons)
		hp->maxcons = maxcons;
	}
	else {
	    /* Make the host access list larger if required */
	    if (nhosts == szhostlist) {
		szhostlist += 8;
		need = szhostlist * sizeof(hostinfo);
		hostlist = (hostinfo *)realloc(hostlist, need);
		if (hostlist == NULL) {
		    __pmNoMem("AddHost enlarge", need, PM_FATAL_ERR);
		}
	    }

	    /* Move any subsequent hosts down to make room for the new entry*/
	    hp = &hostlist[i];
	    if (i < nhosts)
		memmove(&hostlist[i+1], &hostlist[i],
			(nhosts - i) * sizeof(hostinfo));
	    hp->hostspec = spec->name;
	    hp->hostid = spec->hostid;
	    hp->hostmask = spec->hostmask;
	    hp->level = spec->level;
	    hp->specOps = specOps;
	    hp->denyOps = specOps & denyOps;
	    hp->maxcons = maxcons;
	    hp->curcons = 0;
	    nhosts++;
	}
	/* Count the found hosts. */
	++found;
    } /* loop over addresses */

    if (prevName != NULL) {
	free(prevName);
	free(prevHost);
    }
    free(specs);
    return found != 0 ? 0 : -EINVAL;
}

static __pmSockAddr **
getClientIds(const __pmSockAddr *hostid, int *sts)
{
    __pmSockAddr	**clientIds;
    __pmSockAddr	*myAddr;
    size_t		clientIx;
    size_t		clientSize;
    size_t		need;
    void		*enumIx;

    *sts = 0;

    /* If the address is not for "localhost", then return a list containing only
       the given address. */
    if (! __pmSockAddrIsLoopBack(hostid)) {
	clientIds = calloc(2, sizeof(*clientIds));
	if (clientIds == NULL)
	    __pmNoMem("Client Ids", 2 * sizeof(*clientIds), PM_FATAL_ERR);
	clientIds[0] = __pmSockAddrDup(hostid);
	return clientIds;
    }

    /* Map "localhost" to the real IP addresses.  Host access statements for
     * localhost are mapped to the "real" IP addresses so that wildcarding works
     * consistently. First get the real host address;
     */
    PM_INIT_LOCKS();
    PM_LOCK(__pmLock_libpcp);
    if (!gotmyhostid)
	getmyhostid();

    *sts = PM_ERR_PERMISSION;
    if (gotmyhostid <= 0) {
	PM_UNLOCK(__pmLock_libpcp);
	return NULL;
    }
    PM_UNLOCK(__pmLock_libpcp);

    /* Now construct a list containing each address. Check for the end of the list within the
       loop since we need to add an empty entry and the code to grow the list is within the
       loop. */
    clientIds = NULL;
    clientIx = 0;
    clientSize = 0;
    enumIx = NULL;
    for (myAddr = __pmHostEntGetSockAddr(myhostid, &enumIx);
	 /**/;
	 myAddr = __pmHostEntGetSockAddr(myhostid, &enumIx)) {
	if (clientIx == clientSize) {
	    clientSize = clientSize == 0 ? 4 : clientSize * 2;
	    need = clientSize * sizeof(*clientIds);
	    clientIds = realloc(clientIds, need);
	    if (clientIds == NULL) {
		PM_UNLOCK(__pmLock_libpcp);
		__pmNoMem("Client Ids", need, PM_FATAL_ERR);
	    }
	}
	/* No more addresses? */
	if (myAddr == NULL) {
	    clientIds[clientIx] = NULL;
	    break;
	}
	/* Add the new address and its corrsponding mask. */
	clientIds[clientIx] = myAddr;
	++clientIx;
	*sts = 0;
    }

    /* If no addresses were discovered, then return NULL. *sts is already set. */
    if (clientIx == 0 && clientIds != NULL) {
	free(clientIds);
	clientIds = NULL;
    }
    return clientIds;
}

static void
freeClientIds(__pmSockAddr **clientIds)
{
    int i;
    for (i = 0; clientIds[i] != NULL; ++i)
	free(clientIds[i]);
    free(clientIds);
}

/* Called after accepting new client's connection to check that another
 * connection from its host is permitted and to find which operations the
 * client is permitted to perform.
 * hostid is the address of the host that the client is running on
 * denyOpsResult is a pointer to return the capability vector
 */
int
__pmAccAddClient(__pmSockAddr *hostid, unsigned int *denyOpsResult)
{
    int			i;
    int			sts;
    hostinfo		*hp;
    hostinfo		*lastmatch = NULL;
    int			clientIx;
    __pmSockAddr	**clientIds;
    __pmSockAddr	*clientId;
    __pmSockAddr	*maskedId;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;

    /* There could be more than one address associated with this host.*/
    clientIds = getClientIds(hostid, &sts);
    if (clientIds == NULL)
	return sts;

    *denyOpsResult = 0;			/* deny nothing == allow all */
    if (nhosts == 0)			/* No access controls => allow all */
	return 0;

    /* Accumulate permissions for each client address. */
    for (clientIx = 0; clientIds[clientIx] != NULL; ++clientIx) {
	clientId = clientIds[clientIx];
	for (i = nhosts - 1; i >= 0; i--) {
	    hp = &hostlist[i];
	    maskedId = __pmSockAddrDup(clientId);
	    if (__pmSockAddrGetFamily(maskedId) == __pmSockAddrGetFamily(hp->hostmask) &&
		__pmSockAddrCompare(__pmSockAddrMask(maskedId, hp->hostmask), hp->hostid) == 0) {
		/* Clobber specified ops then set. Leave unspecified ops alone. */
		*denyOpsResult &= ~hp->specOps;
		*denyOpsResult |= hp->denyOps;
		lastmatch = hp;
	    }
	    __pmSockAddrFree(maskedId);
	}
	/* no matching entry in hostlist => allow all */

	/* If no operations are allowed, disallow connection */
	if (*denyOpsResult == all_ops) {
	    freeClientIds(clientIds);
	    return PM_ERR_PERMISSION;
	}

	/* Check for connection limit */
	if (lastmatch != NULL && lastmatch->maxcons &&
	    lastmatch->curcons >= lastmatch->maxcons) {

	    *denyOpsResult = all_ops;
	    freeClientIds(clientIds);
	    return PM_ERR_CONNLIMIT;
	}

	/* Increment the count of current connections for ALL host specs in the
	 * host access list that match the client's IP address.  A client may
	 * contribute to several connection counts because of wildcarding.
	 */
	for (i = 0; i < nhosts; i++) {
	    hp = &hostlist[i];
	    maskedId = __pmSockAddrDup(clientId);
	    if (__pmSockAddrGetFamily(maskedId) == __pmSockAddrGetFamily(hp->hostmask) &&
		__pmSockAddrCompare(__pmSockAddrMask(maskedId, hp->hostmask), hp->hostid) == 0)
		if (hp->maxcons)
		    hp->curcons++;
	    __pmSockAddrFree(maskedId);
	}
    }

    freeClientIds(clientIds);
    return 0;
}

void
__pmAccDelClient(__pmSockAddr *hostid)
{
    int		i;
    int		sts;
    hostinfo	*hp;
    int		clientIx;
    __pmSockAddr **clientIds;
    __pmSockAddr *clientId;
    __pmSockAddr *maskedId;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;

    /* There could be more than one address associated with this host.*/
    clientIds = getClientIds(hostid, &sts);
    if (clientIds == NULL)
	return;

    /* Decrement the count of current connections for ALL host specs in the
     * host access list that match the client's IP addresses.  A client may
     * contribute to several connection counts because of wildcarding.
     */
    for (clientIx = 0; clientIds[clientIx] != NULL; ++clientIx) {
	clientId = clientIds[clientIx];
	for (i = 0; i < nhosts; i++) {
	    hp = &hostlist[i];
	    maskedId = __pmSockAddrDup(clientId);
	    if (__pmSockAddrGetFamily(maskedId) == __pmSockAddrGetFamily(hp->hostmask) &&
		__pmSockAddrCompare(__pmSockAddrMask(maskedId, hp->hostmask), hp->hostid) == 0)
		if (hp->maxcons)
		    hp->curcons--;
	    __pmSockAddrFree(maskedId);
	}
    }
    freeClientIds(clientIds);
}

static int
findGidInUsersGroups(const userinfo *up, gid_t gid)
{
    int		i;

    for (i = 0; i < up->ngroups; i++)
	if (up->groupids[i] == gid)
	    return 1;
    return 0;
}

static int
accessCheckUsers(uid_t uid, gid_t gid, unsigned int *denyOpsResult)
{
    userinfo	*up;
    int		i;

    if (nusers) {
	up = &userlist[0];	/* global wildcard */
	if (up->maxcons && up->curcons >= up->maxcons) {
	    *denyOpsResult = all_ops;
	    return PM_ERR_CONNLIMIT;
	}
	*denyOpsResult |= up->denyOps;
    }

    for (i = 1; i < nusers; i++) {
	up = &userlist[i];
	if (up->userid == uid || findGidInUsersGroups(up, gid)) {
	    if (up->maxcons && up->curcons >= up->maxcons) {
		*denyOpsResult = all_ops;
		return PM_ERR_CONNLIMIT;
	    }
	    *denyOpsResult |= up->denyOps;
	}
    }
    return 0;
}

static int
findUidInGroupsUsers(const groupinfo *gp, uid_t uid)
{
    int		i;

    for (i = 0; i < gp->nusers; i++)
	if (gp->userids[i] == uid)
	    return 1;
    return 0;
}

static int
accessCheckGroups(uid_t uid, gid_t gid, unsigned int *denyOpsResult)
{
    groupinfo	*gp;
    int		i;

    if (ngroups) {
	gp = &grouplist[0];	/* global wildcard */
	if (gp->maxcons && gp->curcons >= gp->maxcons) {
	    *denyOpsResult = all_ops;
	    return PM_ERR_CONNLIMIT;
	}
	*denyOpsResult |= gp->denyOps;
    }

    for (i = 1; i < ngroups; i++) {
	gp = &grouplist[i];
	if (gp->groupid == gid || findUidInGroupsUsers(gp, uid)) {
	    if (gp->maxcons && gp->curcons >= gp->maxcons) {
		*denyOpsResult = all_ops;
		return PM_ERR_CONNLIMIT;
	    }
	    *denyOpsResult |= gp->denyOps;
	}
    }
    return 0;
}

static void updateGroupAccountConnections(gid_t, int, int);

static void
updateUserAccountConnections(uid_t uid, int descend, int direction)
{
    int		i, j;
    userinfo	*up;

    for (i = 1; i < nusers; i++) {
	up = &userlist[i];
	if (up->userid != uid)
	    continue;
	if (up->maxcons)
	    up->curcons += direction;	/* might be negative */
	assert(up->curcons >= 0);
	if (!descend)
	    continue;
	for (j = 0; j < up->ngroups; j++)
	    updateGroupAccountConnections(up->groupids[j], 0, direction);
    }
}

static void
updateGroupAccountConnections(gid_t gid, int descend, int direction)
{
    int		i, j;
    groupinfo	*gp;

    for (i = 1; i < ngroups; i++) {
	gp = &grouplist[i];
	if (gp->groupid != gid)
	    continue;
	if (gp->maxcons)
	    gp->curcons += direction;	/* might be negative */
	assert(gp->curcons >= 0);
	if (!descend)
	    continue;
	for (j = 0; j < gp->nusers; j++)
	    updateUserAccountConnections(gp->userids[j], 0, direction);
    }
}

/* Called after authenticating a new connection to check that another
 * connection from this account is permitted and to find which operations
 * the account is permitted to perform.
 * uid and gid identify the account, if not authenticated these will be
 * negative.  denyOpsResult is a pointer to return the capability vector
 * and WELL that it is both input (host access) and output (merged host
 * and account access).  So, do not blindly zero or overwrite existing.
 */
int
__pmAccAddAccount(int uid, int gid, unsigned int *denyOpsResult)
{
    int		sts;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return PM_ERR_THREAD;

    if (nusers == 0 && ngroups == 0)	/* No access controls => allow all */
	return 0;
    if (uid < 0 && gid < 0)		/* Nothing to check, short-circuit */
	return 0;

    if ((sts = accessCheckUsers(uid, gid, denyOpsResult)) < 0)
	return sts;
    if ((sts = accessCheckGroups(uid, gid, denyOpsResult)) < 0)
	return sts;

    /* If no operations are allowed, disallow connection */
    if (*denyOpsResult == all_ops)
	return PM_ERR_PERMISSION;

    /* Increment the count of current connections for this user and group
     * in the user and groups access lists.  Must walk the supplementary
     * ID lists as well as the primary ID ACLs.
     */
    updateUserAccountConnections(uid, 1, +1);
    updateGroupAccountConnections(gid, 1, +1);
    return 0;
}

void
__pmAccDelAccount(int uid, int gid)
{
    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;

    /* Decrement the count of current connections for this user and group
     * in the user and groups access lists.  Must walk the supplementary
     * ID lists as well as the primary ID ACLs.
     */
    updateUserAccountConnections(uid, 1, -1);
    updateGroupAccountConnections(gid, 1, -1);
}

static void
getAccBits(unsigned int *maskp, int *minbitp, int *maxbitp)
{
    unsigned int	mask = all_ops;
    int			i, minbit = -1;

    for (i = 0; mask; i++) {
	if (mask % 2)
	    if (minbit < 0)
		minbit = i;
	mask = mask >> 1;
    }

    *minbitp = minbit;
    *maxbitp = i - 1;
    *maskp = mask;
}

#define NAME_WIDTH	39	/* sufficient for a full IPv6 address */
#define ID_WIDTH	7	/* sufficient for large group/user ID */

void
__pmAccDumpHosts(FILE *stream)
{
    int			h, i;
    int			minbit, maxbit;
    unsigned int	mask;
    hostinfo		*hp;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;

    if (nhosts == 0) {
	fprintf(stream, "Host access list empty: host-based access control turned off\n");
	return;
    }

    getAccBits(&mask, &minbit, &maxbit);
    fprintf(stream, "Host access list:\n");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fprintf(stream, "%02d ", i);
    fprintf(stream, "Cur/MaxCons %-*s %-*s lvl host-name\n",
	    NAME_WIDTH, "host-spec", NAME_WIDTH, "host-mask");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fputs("== ", stream);
    fprintf(stream, "=========== ");
    for (i = 0; i < 2; i++) {
        for (h = 0; h < NAME_WIDTH; h++)
	    fprintf(stream, "=");
	fprintf(stream, " ");
    }
    fprintf(stream, "=== ==============\n");

    for (h = 0; h < nhosts; h++) {
	hp = &hostlist[h];

	for (i = minbit; i <= maxbit; i++) {
	    if (all_ops & (mask = 1 << i)) {
		if (hp->specOps & mask) {
		    fputs((hp->denyOps & mask) ? " n " : " y ", stream);
		}
		else {
		    fputs("   ", stream);
		}
	    }
	}
	fprintf(stream, "%5d %5d %-*s %-*s %3d %s\n", hp->curcons, hp->maxcons,
		NAME_WIDTH, __pmSockAddrToString(hp->hostid),
		NAME_WIDTH, __pmSockAddrToString(hp->hostmask),
		hp->level, hp->hostspec);
    }
}

void
__pmAccDumpUsers(FILE *stream)
{
    int			u, i;
    int			minbit, maxbit;
    char		buf[64];
    unsigned int	mask;
    userinfo		*up;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;

    if (nusers == 0) {
	fprintf(stream, "User access list empty: user-based access control turned off\n");
	return;
    }

    getAccBits(&mask, &minbit, &maxbit);
    fprintf(stream, "User access list:\n");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fprintf(stream, "%02d ", i);
    fprintf(stream, "Cur/MaxCons    uid %-*s %s\n",
	    NAME_WIDTH - ID_WIDTH, "user-name", "group-list");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fputs("== ", stream);
    fprintf(stream, "=========== ====== ");
    for (i = 0; i < NAME_WIDTH - ID_WIDTH; i++)	/* user-name */
	fprintf(stream, "=");
    fprintf(stream, " ");
    for (i = 0; i < NAME_WIDTH + 19; i++)	/* group-list */
	fprintf(stream, "=");
    fprintf(stream, "\n");

    for (u = nusers-1; u >= 0; u--) {
	up = &userlist[u];

	for (i = minbit; i <= maxbit; i++) {
	    if (all_ops & (mask = 1 << i)) {
		if (up->specOps & mask) {
		    fputs((up->denyOps & mask) ? " n " : " y ", stream);
		}
		else {
		    fputs("   ", stream);
		}
	    }
	}
	snprintf(buf, sizeof(buf), u ? "%6d" : "     *", up->userid);
	fprintf(stream, "%5d %5d %s %-*s", up->curcons, up->maxcons,
			buf, NAME_WIDTH - ID_WIDTH, up->username);
	for (i = 0; i < up->ngroups; i++)
	    fprintf(stream, "%c%u(%s)", i ? ',' : ' ', up->groupids[i],
		    __pmGroupname(up->groupids[i], buf, sizeof(buf)));
	fprintf(stream, "\n");
    }
}

void
__pmAccDumpGroups(FILE *stream)
{
    int			g, i;
    int			minbit, maxbit;
    char		buf[64];
    unsigned int	mask;
    groupinfo		*gp;

    if (PM_MULTIPLE_THREADS(PM_SCOPE_ACL))
	return;

    if (ngroups == 0) {
	fprintf(stream, "Group access list empty: group-based access control turned off\n");
	return;
    }

    getAccBits(&mask, &minbit, &maxbit);
    fprintf(stream, "Group access list:\n");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fprintf(stream, "%02d ", i);
    fprintf(stream, "Cur/MaxCons    gid %-*s %s\n",
	    NAME_WIDTH - ID_WIDTH, "group-name", "user-list");

    for (i = minbit; i <= maxbit; i++)
	if (all_ops & (1 << i))
	    fputs("== ", stream);
    fprintf(stream, "=========== ====== ");
    for (i = 0; i < NAME_WIDTH - ID_WIDTH; i++)	/* group-name */
	fprintf(stream, "=");
    fprintf(stream, " ");
    for (i = 0; i < NAME_WIDTH + 19; i++)	/* user-list */
	fprintf(stream, "=");
    fprintf(stream, "\n");

    for (g = ngroups-1; g >= 0; g--) {
	gp = &grouplist[g];

	for (i = minbit; i <= maxbit; i++) {
	    if (all_ops & (mask = 1 << i)) {
		if (gp->specOps & mask) {
		    fputs((gp->denyOps & mask) ? " n " : " y ", stream);
		}
		else {
		    fputs("   ", stream);
		}
	    }
	}
	snprintf(buf, sizeof(buf), g ? "%6d" : "     *", gp->groupid);
	fprintf(stream, "%5d %5d %s %-*s", gp->curcons, gp->maxcons,
			buf, NAME_WIDTH - ID_WIDTH, gp->groupname);
	for (i = 0; i < gp->nusers; i++)
	    fprintf(stream, "%c%u(%s)", i ? ',' : ' ', gp->userids[i],
		    __pmUsername(gp->userids[i], buf, sizeof(buf)));
	fprintf(stream, "\n");
    }
}

void
__pmAccDumpLists(FILE *stream)
{
    putc('\n', stream);
    __pmAccDumpHosts(stream);
    __pmAccDumpUsers(stream);
    __pmAccDumpGroups(stream);
    putc('\n', stream);
}
