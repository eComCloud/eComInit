/*******************************************************************

    PROPRIETARY NOTICE

These coded instructions, statements, and computer programs contain
proprietary information of eComCloud Object Solutions, and they are
protected under copyright law. They may not be distributed, copied,
or used except under the provisions of the terms of the Source Code
Licence Agreement, in the file "LICENCE.md", which should have been
included with this software

    Copyright Notice

    (c) 2021 eComCloud Object Solutions.
        All rights reserved.
********************************************************************/

#pragma once

#include <list>
#include <queue>
#include <string>

#include "ucl.h"

typedef ucl_object_t *(*WSRPCSerialisationFun)(void *);
typedef bool (*WSRPCDeserialisationFun)(const ucl_object_t *obj, void *out);

/* this always returns a JSON type null object */
ucl_object_t *wsRPCSerialisevoid(void *in);
ucl_object_t *wsRPCSerialisebool(bool *in);
ucl_object_t *wsRPCSerialiseint(int *in);
ucl_object_t *wsRPCSerialisestring(std::string *in);

/* this only checks if the object is of JSON type null */
bool wsRPCDeserialisevoid(const ucl_object_t *obj, void *out);
bool wsRPCDeserialisebool(const ucl_object_t *obj, bool *out);
bool wsRPCDeserialiseint(const ucl_object_t *obj, int *out);
bool wsRPCDeserialisestring(const ucl_object_t *obj, std::string *out);

class WSRPCTransport;
class WSRPCListener;
class WSRPCTransport;

struct WSRPCError
{
    enum Code
    {
        kSuccess = 0,
        kError = 1,

        kWSREMethodNotFound = -32601,
        kInvalidParameters = -32602,

        kReplyTimeout = -32500, /* timeout waiting for server to reply */
        kLocalDeserialisationFailure = -32501,
    };

    Code errcode = kSuccess;
    std::string errmsg;

    WSRPCError() = default;
    WSRPCError(Code code, std::string msg);

    static WSRPCError invalidParams();
};

/**
 * A WSRPC request descriptor received when a method is invoked.
 */
struct WSRPCReq
{
    WSRPCTransport *xprt;
    int id;
    std::string method_name;
    const ucl_object_t *params;

    /**
     * Result UCL object. If this is set by the method implementation function,
     * then NO attempt will be made to read and serialise the rval (the pointer
     * to which was passed to the impl).
     */
    ucl_object_t *result;
    /* error to send back, if needed */
    WSRPCError err;
};

/**
 * A WSRPC method completion - this is updated when a method is completed.
 */
class WSRPCCompletion
{
  public:
    typedef void (*FnDelegateInvoker)(void *, WSRPCCompletion *);

    WSRPCTransport *xprt;
    int id;
    bool sendSucceeded;
    void *delegate;
    FnDelegateInvoker fnDelegateInvoker;

    WSRPCCompletion(WSRPCTransport *xprt, int id);
    ~WSRPCCompletion();

    bool wait(); /* wait for a reply. returns true if any reply received before
                    deadline */
    /** Complete with a response object; delegate will be invoked. Unrefs obj */
    void completeWith(ucl_object_t *obj);

    WSRPCError err;
    ucl_object_t *result = NULL;
};

class WSRPCVTable
{
    friend class WSRPCTransport;
    friend class WSRPCClient;
    friend class WSRPCListener;
    friend struct WSRPCServiceProvider;

  protected:
    /**
     * A method to examine a request and, if it is handled, then invoke the
     * right method on the vtable.
     *
     * @returns -1 if method not handled by this vtable.
     * @returns 0 if successful.
     * @returns 1 if error occured during method processing. (Error object
     * set in req.)
     */
    typedef int (*FnReqHandler)(WSRPCReq *req, WSRPCVTable *vt);

    void invalidParams(WSRPCReq *req);

  public:
};

struct WSRPCServiceProvider
{
    WSRPCVTable *vt;
    WSRPCVTable::FnReqHandler fnReqHandler;
};

class WSRPCTransport
{
    friend class WSRPCListener;
    friend class WSRPCCompletion;
    friend struct WSRPCServiceProvider;
    friend class WSRPCClient;

    /**
     * Services we handle. Not necessarily present for clients. Not owned by us;
     * the WSRPCListener (or whoever added services to a client) owns the list.
     */
    std::list<WSRPCServiceProvider> *svcs;

    /**
     * Completions we are awaiting to complete.
     */
    std::list<WSRPCCompletion *> completions;

    /* Whether we own the svcs list. */
    bool ownSvcs;

    /* Every message begins with 4 bytes representing the length of the message.
     * When we receive those 4 bytes, we allocate a buffer to hold the rest of
     * the message. */
    int32_t curMsgLen = 0;
    /* Offset into the current message being received. */
    int curMsgOff = 0;
    /* Message buffer. */
    char *curMsgBuf = NULL;
    /* When synchronously waiting on a result, we enqueue any other messages we
     * receive here for later processing. */
    std::list<ucl_object_t *> received;

    /**
     * receive some data.
     * Returns true if a complete message has been received.
     */
    bool doRecv();
    /**
     * Dispatch a received message. Either response or request. Deletes it
     * afterwards, or stores it into the received buffer if it's a response.
     */
    void processMessage(ucl_object_t *obj);

  protected:
    /* Creates a WSRPCTransport that doesn't own its svcs list. */
    WSRPCTransport(int fd, std::list<WSRPCServiceProvider> *);

    /**
     * Synchronously waits a reply for the given completion, up to /timeoutMsecs
     * milliseconds. Returns true if such a reply arrives. Completion is
     * correspondingly completed. Any other messages received are appropriately
     * queued for dispatch afterwards.
     */
    bool awaitReply(WSRPCCompletion *comp, int timeoutMsecs);

    /* Send an object.. */
    bool writeObj(ucl_object_t *obj);

    void sendError(int id, WSRPCError &err);
    void sendReply(int id, ucl_object_t *obj);

  public:
    int fd = -1;

    /**
     * The "client" constructor - use when a transport is to serve as a client
     * to a server.
     *
     * It creates its own svcs list, which it owns.
     */
    WSRPCTransport() : ownSvcs(true)
    {
        svcs = new std::list<WSRPCServiceProvider>;
    };
    ~WSRPCTransport();

    /**
     * Attach a transport to an existing socket.
     */
    void attach(int fd);

    /* Notify transport that its FD is ready for reading. */
    void readyForRead();

    /* Adds a service. Clients can call this to become bidirectional. */
    void addService(WSRPCServiceProvider aSvc);

    /**
     * Send a message.
     * @param method Method name.
     * @param params Parameter dictionary
     * @param delegate Userdata for the \p fnDelegateInvoker function. If
     * specified, so must be \p fnDelegateInvoker, and vice versa.
     * @param fnDelegateInvoker Function to invoke with arguments of this
     * completion and the value of \p delegate when the completion is completed.
     */
    WSRPCCompletion *sendMessage(
        std::string method, ucl_object_t *params, void *delegate,
        WSRPCCompletion::FnDelegateInvoker fnDelegateInvoker);
};

struct WSRPCListenerDelegate
{
    /** Client connection callback - listen for events on xprt's fd. */
    virtual void clientConnected(WSRPCTransport *xprt) = 0;
    /** Client disconnect callback - stop listening for events on xprt's fd. */
    virtual void clientDisconnected(WSRPCTransport *xprt) = 0;
};

class WSRPCListener
{
  public:
  protected:
    int fd = -1;

    /**
     * Our list of services. The WSRPCService* members of the providers are not
     * owned by us, so if you delete them, you must also delete them from this
     * listener.
     */
    std::list<WSRPCServiceProvider> svcs;
    /* Our client transports. They keep pointers to our svcs list. */
    std::list<WSRPCTransport> clientXprts;

    WSRPCListenerDelegate *delegate;

  public:
    WSRPCListener(WSRPCListenerDelegate *delegate) : delegate(delegate){};

    /* Attach to a given listening socket. */
    void attach(int fd);

    /* Add a service to the set handled by this listener.  */
    void addService(WSRPCServiceProvider svc);

    /* Call when an event occurs on an FD which may relate to the listener. */
    void fdEvent(int fd, int revents);
};