/*******************************************************************

    PROPRIETARY NOTICE

These coded instructions, statements, and computer programs contain
proprietary information of eComCloud Object Solutions, and they are
protected under copyright law. They may not be distributed, copied,
or used except under the provisions of the terms of the Source Code
Licence Agreement, in the file "LICENCE.md", which should have been
included with this software

    Copyright Notice

    (c) 2020-2021 eComCloud Object Solutions.
        All rights reserved.
********************************************************************/

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <cassert>
#include <unistd.h>

#include "eci/WSRPC.hh"

static int parseMessage(const char *message, ucl_object_t **res)
{
    struct ucl_parser *parser = ucl_parser_new(0);
    ucl_object_t *obj = NULL;
    const ucl_object_t *error, *result;
    int ret = 0;

    ucl_parser_add_string(parser, message, strlen(message));

    if (ucl_parser_get_error(parser))
    {
        printf("RPC error: Malformed message: %s\n",
               ucl_parser_get_error(parser));
        ret = -1;
        goto cleanup;
    }

    *res = ucl_parser_get_object(parser);

cleanup:

    if (obj)
        ucl_object_unref(obj);
    ucl_parser_free(parser);

    return ret;
}

/**
 * Is /obj a valid request object??
 */
static bool uclObjIsRequest(const ucl_object_t *obj)
{
    const ucl_object_t *method_o, *params_o;
    if (!(ucl_object_type(obj) == UCL_OBJECT))
        return false;
    if (!(method_o = ucl_object_lookup(obj, "method")) ||
        !(ucl_object_type(method_o) == UCL_STRING))
        return false; /* missing method */
    if ((params_o = ucl_object_lookup(obj, "params")) &&
        !(ucl_object_type(params_o) == UCL_OBJECT))
        return false; /* bad params */
    return true;
}

/**
 * Is /obj a response?
 * @returns 0 if not a response.
 * @returns ID if a response
 */
static int uclObjIsResponseForId(const ucl_object_t *obj)
{
    const ucl_object_t *id_o, *error_o, *result_o;

    id_o = ucl_object_lookup(obj, "id");
    error_o = ucl_object_lookup(obj, "error");
    result_o = ucl_object_lookup(obj, "result");
    bool has_result_or_error = false;

    if (!(ucl_object_type(obj) == UCL_OBJECT))
    {
        return 0; /* not an object */
    }
    if (!id_o || !(ucl_object_type(id_o) == UCL_INT))
    {
        return 0; /* response missing ID*/
    }
    if (error_o)
    {
        has_result_or_error = true;
    }
    if (result_o && has_result_or_error == true)
    {
        return 0; /* has both result and error */
    }
    else if (result_o)
        has_result_or_error = true;

    return has_result_or_error ? ucl_object_toint(id_o) : 0;
}

static ucl_object_t *makeError(int code, std::string message /*,
                                ucl_object_t *data*/)
{
    ucl_object_t *uerr = ucl_object_typed_new(UCL_OBJECT);
    ucl_object_insert_key(uerr, ucl_object_fromint(code), "code", 0, 0);
    ucl_object_insert_key(uerr, ucl_object_fromstring(message.c_str()),
                          "message", 0, 0);
    /* ucl_object_insert_key(uerr, data ? data : ucl_object_typed_new(UCL_NULL),
                          "data", 0, 0); */
    return uerr;
}

static WSRPCError uclToError(const ucl_object_t *error)
{
    WSRPCError rerror;
    const ucl_object_t *code = ucl_object_lookup(error, "code"),
                       *message = ucl_object_lookup(error, "message"),
                       *data = ucl_object_lookup(error, "data");
    rerror.errcode = (WSRPCError::Code)ucl_object_toint(code);
    rerror.errmsg = message ? ucl_object_tostring(message) : "";
    return rerror;
}

/* write an object to the FD. returns true if successful. */
bool WSRPCTransport::writeObj(ucl_object_t *obj)
{
    char *s = (char *)ucl_object_emit(obj, UCL_EMIT_JSON_COMPACT);
    int32_t len = strlen(s) + 1;
    bool ret;

    if (write(fd, (char *)&len, sizeof(int32_t)) == -1)
    {
        perror("failed to write length");
        ret = false;
    }
    else if (write(fd, s, len) == -1)
    {
        perror("failed to write length");
        ret = false;
    }

    free(s);
    return true;
}

ucl_object_t *wsRPCSerialisevoid(void *in)
{
    return ucl_object_typed_new(UCL_NULL);
}

ucl_object_t *wsRPCSerialisebool(bool *in)
{
    return ucl_object_frombool(*in);
}

ucl_object_t *wsRPCSerialiseint(int *in)
{
    return ucl_object_fromint(*in);
}

ucl_object_t *wsRPCSerialisestring(std::string *in)
{
    return ucl_object_fromstring(in->c_str());
}

bool wsRPCDeserialisevoid(const ucl_object_t *obj, void *out)
{
    if (ucl_object_type(obj) != UCL_NULL)
        return false;
    return true;
}

bool wsRPCDeserialisebool(const ucl_object_t *obj, bool *out)
{
    if (ucl_object_type(obj) != UCL_BOOLEAN)
        return false;
    *out = ucl_object_toboolean(obj);
    return true;
}

bool wsRPCDeserialiseint(const ucl_object_t *obj, int *out)
{
    if (ucl_object_type(obj) != UCL_INT)
        return false;
    *out = ucl_object_toint(obj);
    return true;
}

bool wsRPCDeserialisestring(const ucl_object_t *obj, std::string *out)
{
    if (ucl_object_type(obj) != UCL_STRING)
        return false;
    *out = ucl_object_tostring(obj);
    return true;
}

WSRPCError::WSRPCError(Code code, std::string msg)
    : errcode(code), errmsg(std::move(msg))
{
}

WSRPCError WSRPCError::invalidParams()
{
    return WSRPCError(kInvalidParameters, "Invalid method parameter(s).");
}

WSRPCCompletion::WSRPCCompletion(WSRPCTransport *xprt, int id)
    : xprt(xprt), id(id)
{
}

WSRPCCompletion::~WSRPCCompletion()
{
    if (result)
        ucl_object_unref(result);
}

bool WSRPCCompletion::wait()
{
    return xprt->awaitReply(this, 2000);
}

void WSRPCCompletion::completeWith(ucl_object_t *obj)
{
    const ucl_object_t *result_o, *error_o;
    result_o = ucl_object_lookup(obj, "result");
    if (result_o)
    {
        err.errcode = WSRPCError::kSuccess;
        err.errmsg = "";
        result = ucl_object_ref(result_o);
    }
    else
        err = uclToError(ucl_object_lookup(obj, "error"));
    if (delegate)
        fnDelegateInvoker(delegate, this);
    ucl_object_unref(obj);
}

void WSRPCTransport::sendError(int id, WSRPCError &err)
{
    ucl_object_t *response;

    if (!id)
        return;

    response = ucl_object_typed_new(UCL_OBJECT);

    ucl_object_insert_key(response, ucl_object_fromstring("2.0"), "jsonrpc", 0,
                          0);
    ucl_object_insert_key(response, makeError(err.errcode, err.errmsg), "error",
                          0, 0);
    ucl_object_insert_key(response, ucl_object_fromint(id), "id", 0, 0);

    writeObj(response);

    ucl_object_unref(response);
}

void WSRPCTransport::sendReply(int id, ucl_object_t *obj)
{
    ucl_object_t *response = ucl_object_typed_new(UCL_OBJECT);

    ucl_object_insert_key(response, ucl_object_fromstring("2.0"), "jsonrpc", 0,
                          1);
    ucl_object_insert_key(response, obj ? obj : ucl_object_typed_new(UCL_NULL),
                          "result", 0, 1);
    ucl_object_insert_key(response, ucl_object_fromint(id), "id", 0, 1);

    writeObj(response);
    ucl_object_unref(response);
}

WSRPCTransport::WSRPCTransport(int fd, std::list<WSRPCServiceProvider> *svcs)
    : fd(fd), svcs(svcs), ownSvcs(false)
{
}

WSRPCTransport::~WSRPCTransport()
{
    if (curMsgBuf)
        free(curMsgBuf);
    while (!received.empty())
    {
        ucl_obj_unref(received.back());
        received.pop_back();
    }
}

void WSRPCTransport::attach(int aFd)
{
    fd = aFd;
}

bool WSRPCTransport::awaitReply(WSRPCCompletion *comp, int timeoutMsecs)
{
    struct pollfd pollfd;
    int pres;
    bool ret = true;

retry:
    pollfd = {.fd = fd, .events = POLLIN, .revents = 0};

    pres = poll(&pollfd, 1, timeoutMsecs);

    comp->err.errcode = WSRPCError::kReplyTimeout;
    comp->err.errmsg = "Timeout waiting for server to reply.";

    /* FIXME: what about EINTR */
    if (pres == -1)
    {
        printf("poll returned -1: %s\n", strerror(errno));
        return false;
    }
    else if (pres == 0)
    {
        fprintf(stderr, "No reply received to synchronous message.\n");
        return false;
    }
    else if (pollfd.revents & POLLIN)
    {
        int res;
        ucl_object_t *obj;
        bool didReceive = doRecv();

        if (!didReceive)
        {
            /* more to be received */
            timeoutMsecs = 0;
            goto retry;
        }

        res = parseMessage(curMsgBuf, &obj);
        free(curMsgBuf);
        curMsgBuf = 0;

        if (res)
        {
            fprintf(stderr, "Bad message received while awaiting reply.\n");
            return -1;
        }

        int id;
        if ((id = uclObjIsResponseForId(obj)) == comp->id)
            comp->completeWith(obj);
        else
        {
            /* An ugly hack to enable nested synchronous RPC. */
            processMessage(obj);
            if (received.size())
                for (auto &obj : received)
                    if (uclObjIsResponseForId(obj) == comp->id)
                    {
                        received.remove(obj);
                        comp->completeWith(obj);
                    }

            goto retry;
        }
    }
    else if (pollfd.revents & POLLHUP)
    {
        printf("Hangup on FD %d\n", fd);
        ret = false;
    }
    else
    {
        assert(!"unreached");
    }

    return ret;
}

void WSRPCTransport::addService(WSRPCServiceProvider svc)
{
    svcs->push_back(svc);
}

bool WSRPCTransport::doRecv()
{
    size_t len_to_recv;

    if (!curMsgLen)
    {
        size_t len = recv(fd, (char *)&curMsgLen, sizeof(int32_t), 0);
        /* FIXME: don't assert. and make all this stuff safe. */
        assert(len == 4);
        curMsgBuf = (char *)malloc(curMsgLen);
        curMsgOff = 0;
    }

    len_to_recv = curMsgLen - curMsgOff;
    curMsgOff += recv(fd, curMsgBuf, len_to_recv, 0);

    if (curMsgLen && (curMsgOff == curMsgLen))
    {
        /* message fully received */
        curMsgLen = 0;
        return true;
    }

    return false;
}

void WSRPCTransport::readyForRead()
{
    if (doRecv())
    {
        ucl_object_t *obj;

        int res = parseMessage(curMsgBuf, &obj);

        free(curMsgBuf);
        curMsgBuf = 0;
        if (!res)
            processMessage(obj);
    }
}

void WSRPCTransport::processMessage(ucl_object_t *obj)
{
    int resp;

    if ((resp = uclObjIsResponseForId(obj)))
    {
        for (auto comp : completions)
            if (resp == comp->id)
            {
                comp->completeWith(obj);
                return;
            }
        received.push_back((ucl_object_t *)obj);
        return;
    }
    else if (uclObjIsRequest(obj))
    {
        WSRPCReq req;
        const ucl_object_t *id = ucl_object_lookup(obj, "id");
        fflush(stdout);
        if (id && !(ucl_object_type(id) == UCL_INT))
        {
            printf("bad request: ID not an int\n");
            ucl_object_unref(obj);
            return;
        }
        req.xprt = this;
        req.id = ucl_object_toint(id);
        req.method_name = ucl_object_tostring(ucl_object_lookup(obj, "method"));
        req.params = ucl_object_lookup(obj, "params");
        req.result = NULL;
        if (svcs)
            for (auto svc : *svcs)
            {
                int res = svc.fnReqHandler(&req, svc.vt);
                if (res == -1)
                    continue;
                else if (res == 1)
                    sendError(req.id, req.err);
                else if (req.id)
                    sendReply(req.id, req.result);
                return;
            }
        req.err.errcode = WSRPCError::kWSREMethodNotFound;
        req.err.errmsg = "The method does not exist / is not available.";
        sendError(req.id, req.err);
    }
    else
    {
        char *message = (char *)ucl_object_emit(obj, UCL_EMIT_JSON_COMPACT);
        printf("Bad WSRPC message: %s", message);
        free(message);
    }
    ucl_object_unref(obj);
}

WSRPCCompletion *WSRPCTransport::sendMessage(
    std::string method, ucl_object_t *params, void *delegate,
    WSRPCCompletion::FnDelegateInvoker fnDelegateInvoker)
{
    int id = rand();
    WSRPCCompletion *comp = new WSRPCCompletion(this, id);
    ucl_object_t *msg = ucl_object_typed_new(UCL_OBJECT);
    const char *txt;

    ucl_object_insert_key(msg, ucl_object_fromstring("2.0"), "jsonrpc", 0, 1);
    ucl_object_insert_key(msg, ucl_object_fromstring(method.c_str()), "method",
                          0, 1);
    ucl_object_insert_key(msg, params, "params", 0, 1);
    ucl_object_insert_key(msg, ucl_object_fromint(id), "id", 0, 1);

    writeObj(msg);

    ucl_object_unref(msg);

    assert((!delegate && !fnDelegateInvoker) || delegate && fnDelegateInvoker);
    comp->delegate = delegate;
    comp->fnDelegateInvoker = fnDelegateInvoker;

    if (delegate)
        completions.push_back(comp);

    return comp;
}

void WSRPCListener::attach(int anFd)
{
    fd = anFd;
}

void WSRPCListener::addService(WSRPCServiceProvider svc)
{
    svcs.push_back(svc);
}

void WSRPCListener::fdEvent(int aFD, int revents)
{
    // we will check with all our clients too, for they may be getting their own
    // Requests, or indeed their own Responses.
    if (revents & POLLHUP)
    {
        for (auto it = clientXprts.begin(); it != clientXprts.end(); it++)
            if (it->fd == aFD)
            {
                printf("Dropping client %d\n", aFD);
                delegate->clientDisconnected(&*it);
                clientXprts.erase(it);
                close(aFD);
                break;
            }
    }
    else if (aFD == fd)
    {
        int clFd = accept(aFD, NULL, NULL);
        printf("Accept client %d\n", clFd);

        if (clFd == -1)
        {
            printf("Error accepting client: %s\n", strerror(errno));
            return;
        }

        clientXprts.emplace_back(std::move(WSRPCTransport(clFd, &svcs)));
        delegate->clientConnected(&clientXprts.back());
    }
    else if (revents & POLLIN)
    {
        for (auto it = clientXprts.begin(); it != clientXprts.end(); it++)
            if (it->fd == aFD)
                it->readyForRead();
    }
}