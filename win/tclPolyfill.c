/*
 * tclPolyfill.c --
 *
 *	This file contains routines that replace some Tcl internal functions
 *      when building as an extension. This file should not be included when
 *      building as part of Tcl core.
 *
 * Copyright (c) 2019-2020 Ashok P. Nadkarni.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */
#include "tclWinIocp.h"

/*
 *----------------------------------------------------------------------
 *
 * TclCreateSocketAddress --
 *
 *	This function initializes a sockaddr structure for a host and port.
 *
 * Results:
 *	1 if the host was valid, 0 if the host could not be converted to an IP
 *	address.
 *
 * Side effects:
 *	Fills in the *sockaddrPtr structure.
 *
 *----------------------------------------------------------------------
 */

int
TclCreateSocketAddress(
    Tcl_Interp *interp,                 /* Interpreter for querying
					 * the desired socket family */
    struct addrinfo **addrlist,		/* Socket address list */
    const char *host,			/* Host. NULL implies INADDR_ANY */
    int port,				/* Port number */
    int willBind,			/* Is this an address to bind() to or
					 * to connect() to? */
    const char **errorMsgPtr)		/* Place to store the error message
					 * detail, if available. */
{
    struct addrinfo hints;
    struct addrinfo *p;
    struct addrinfo *v4head = NULL, *v4ptr = NULL;
    struct addrinfo *v6head = NULL, *v6ptr = NULL;
    char *native = NULL, portbuf[TCL_INTEGER_SPACE], *portstring;
    const char *family = NULL;
    Tcl_DString ds;
    int result;

    if (host != NULL) {
	native = Tcl_UtfToExternalDString(NULL, host, -1, &ds);
    }

    /*
     * Workaround for OSX's apparent inability to resolve "localhost", "0"
     * when the loopback device is the only available network interface.
     */
    if (host != NULL && port == 0) {
        portstring = NULL;
    } else {
        snprintf(portbuf, RTL_NUMBER_OF(portbuf), "%d", port);
        portstring = portbuf;
    }

    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    /*
     * Magic variable to enforce a certain address family - to be superseded
     * by a TIP that adds explicit switches to [socket]
     */

    if (interp != NULL) {
        family = Tcl_GetVar(interp, "::tcl::unsupported::socketAF", 0);
        if (family != NULL) {
            if (strcmp(family, "inet") == 0) {
                hints.ai_family = AF_INET;
            } else if (strcmp(family, "inet6") == 0) {
                hints.ai_family = AF_INET6;
            }
        }
    }

    hints.ai_socktype = SOCK_STREAM;

#if 0
    /*
     * We found some problems when using AI_ADDRCONFIG, e.g. on systems that
     * have no networking besides the loopback interface and want to resolve
     * localhost. See [Bugs 3385024, 3382419, 3382431]. As the advantage of
     * using AI_ADDRCONFIG in situations where it works, is probably low,
     * we'll leave it out for now. After all, it is just an optimisation.
     *
     * Missing on: OpenBSD, NetBSD.
     * Causes failure when used on AIX 5.1 and HP-UX
     */

#if defined(AI_ADDRCONFIG) && !defined(_AIX) && !defined(__hpux)
    hints.ai_flags |= AI_ADDRCONFIG;
#endif /* AI_ADDRCONFIG && !_AIX && !__hpux */
#endif /* 0 */

    if (willBind) {
	hints.ai_flags |= AI_PASSIVE;
    }

    result = getaddrinfo(native, portstring, &hints, addrlist);

    if (host != NULL) {
	Tcl_DStringFree(&ds);
    }

    if (result != 0) {
	*errorMsgPtr =
#ifdef EAI_SYSTEM	/* Doesn't exist on Windows */
		(result == EAI_SYSTEM) ? Tcl_PosixError(interp) :
#endif /* EAI_SYSTEM */
		gai_strerrorA(result);
        return 0;
    }

    /*
     * Put IPv4 addresses before IPv6 addresses to maximize backwards
     * compatibility of [fconfigure -sockname] output.
     *
     * There might be more elegant/efficient ways to do this.
     */
    if (willBind) {
	for (p = *addrlist; p != NULL; p = p->ai_next) {
	    if (p->ai_family == AF_INET) {
		if (v4head == NULL) {
		    v4head = p;
		} else {
		    v4ptr->ai_next = p;
		}
		v4ptr = p;
	    } else {
		if (v6head == NULL) {
		    v6head = p;
		} else {
		    v6ptr->ai_next = p;
		}
		v6ptr = p;
	    }
	}
	*addrlist = NULL;
	if (v6head != NULL) {
	    *addrlist = v6head;
	    v6ptr->ai_next = NULL;
	}
	if (v4head != NULL) {
	    v4ptr->ai_next = *addrlist;
	    *addrlist = v4head;
	}
    }
    return 1;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclSockGetPort --
 *
 *	Maps from a string, which could be a service name, to a port. Used by
 *	socket creation code to get port numbers and resolve registered
 *	service names to port numbers.
 *
 * Results:
 *	A standard Tcl result. On success, the port number is returned in
 *	portPtr. On failure, an error message is left in the interp's result.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

int
TclSockGetPort(
    Tcl_Interp *interp,
    const char *string, /* Integer or service name */
    const char *proto, /* "tcp" or "udp", typically */
    int *portPtr)		/* Return port number */
{
    struct servent *sp;		/* Protocol info for named services */
    Tcl_DString ds;
    const char *native;

    if (Tcl_GetInt(NULL, string, portPtr) != TCL_OK) {
	/*
	 * Don't bother translating 'proto' to native.
	 */

	native = Tcl_UtfToExternalDString(NULL, string, -1, &ds);
	sp = getservbyname(native, proto);		/* INTL: Native. */
	Tcl_DStringFree(&ds);
	if (sp != NULL) {
	    *portPtr = ntohs((unsigned short) sp->s_port);
	    return TCL_OK;
	}
    }
    if (Tcl_GetInt(interp, string, portPtr) != TCL_OK) {
	return TCL_ERROR;
    }
    if (*portPtr > 0xFFFF) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"couldn't open socket: port number too high", -1));
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AcceptCallbackProc --
 *
 *	This callback is invoked by the TCP channel driver when it accepts a
 *	new connection from a client on a server socket.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the script does.
 *
 *----------------------------------------------------------------------
 */

void
AcceptCallbackProc(
    ClientData callbackData,	/* The data stored when the callback was
				 * created in the call to
				 * Tcl_OpenTcpServer. */
    Tcl_Channel chan,		/* Channel for the newly accepted
				 * connection. */
    char *address,		/* Address of client that was accepted. */
    int port)			/* Port of client that was accepted. */
{
    IocpAcceptCallback *acceptCallbackPtr = callbackData;

    /*
     * Check if the callback is still valid; the interpreter may have gone
     * away, this is signalled by setting the interp field of the callback
     * data to NULL.
     */

    if (acceptCallbackPtr->interp != NULL) {
	char portBuf[TCL_INTEGER_SPACE];
	char *script = acceptCallbackPtr->script;
	Tcl_Interp *interp = acceptCallbackPtr->interp;
	int result;

	Tcl_Preserve(script);
	Tcl_Preserve(interp);

        sprintf_s(portBuf, sizeof(portBuf)/sizeof(portBuf[0]), "%d", port);
	Tcl_RegisterChannel(interp, chan);

	/*
	 * Artificially bump the refcount to protect the channel from being
	 * deleted while the script is being evaluated.
	 */

	Tcl_RegisterChannel(NULL, chan);

	result = Tcl_VarEval(interp, script, " ", Tcl_GetChannelName(chan),
		" ", address, " ", portBuf, NULL);
	if (result != TCL_OK) {
	    Tcl_BackgroundException(interp, result);
	    Tcl_UnregisterChannel(interp, chan);
	}

	/*
	 * Decrement the artificially bumped refcount. After this it is not
	 * safe anymore to use "chan", because it may now be deleted.
	 */

	Tcl_UnregisterChannel(NULL, chan);

	Tcl_Release(interp);
	Tcl_Release(script);
    } else {
	/*
	 * The interpreter has been deleted, so there is no useful way to use
	 * the client socket - just close it.
	 */

	Tcl_Close(NULL, chan);
    }
}
