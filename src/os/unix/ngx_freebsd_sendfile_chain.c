
/*
 * Copyright (C) 2002-2004 Igor Sysoev, http://sysoev.ru/en/
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


/*
 * Although FreeBSD sendfile() allows to pass a header and a trailer
 * it can not send a header with a part of the file in one packet until
 * FreeBSD 5.2-STABLE.  Besides over the fast ethernet connection sendfile()
 * may send the partially filled packets, i.e. the 8 file pages may be sent
 * as the 11 full 1460-bytes packets, then one incomplete 324-bytes packet,
 * and then again the 11 full 1460-bytes packets.
 *
 * So we use the TCP_NOPUSH option (similar to Linux's TCP_CORK)
 * to postpone the sending - it not only sends a header and the first part
 * of the file in one packet but also sends file pages in the full packets.
 *
 * But until FreeBSD 4.5 the turning TCP_NOPUSH off does not flush a pending
 * data that less than MSS so that data may be sent with 5 second delay.
 * So we do not use TCP_NOPUSH on FreeBSD prior to 4.5 although it can be used
 * for non-keepalive HTTP connections.
 */


ngx_chain_t *ngx_freebsd_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in,
                                        off_t limit)
{
    int              rc;
    u_char          *prev;
    off_t            fprev, sent, send, sprev, aligned;
    size_t           hsize, fsize;
    ssize_t          size;
    ngx_uint_t       eintr, eagain, complete;
    struct iovec    *iov;
    struct sf_hdtr   hdtr;
    ngx_err_t        err;
    ngx_buf_t       *file;
    ngx_array_t      header, trailer;
    ngx_event_t     *wev;
    ngx_chain_t     *cl;

    wev = c->write;

    if (!wev->ready) {
        return in;
    }

#if (HAVE_KQUEUE)

    if ((ngx_event_flags & NGX_HAVE_KQUEUE_EVENT) && wev->pending_eof) {
        ngx_log_error(NGX_LOG_INFO, c->log, wev->kq_errno,
                      "kevent() reported about an closed connection");

        wev->error = 1;
        return NGX_CHAIN_ERROR;
    }

#endif

    send = 0;
    eagain = 0;

    for ( ;; ) {
        file = NULL;
        fsize = 0;
        hsize = 0;
        eintr = 0;
        complete = 0;
        sprev = send;

        ngx_init_array(header, c->pool, 10, sizeof(struct iovec),
                       NGX_CHAIN_ERROR);
        ngx_init_array(trailer, c->pool, 10, sizeof(struct iovec),
                       NGX_CHAIN_ERROR);

        /* create the header iovec and coalesce the neighbouring bufs */

        prev = NULL;
        iov = NULL;

        for (cl = in;
             cl && header.nelts < IOV_MAX && send < limit;
             cl = cl->next)
        {
            if (ngx_buf_special(cl->buf)) {
                continue;
            }

            if (!ngx_buf_in_memory_only(cl->buf)) {
                break;
            }

            size = cl->buf->last - cl->buf->pos;

            if (send + size > limit) {
                size = limit - send;
            }

            if (prev == cl->buf->pos) {
                iov->iov_len += size;

            } else {
                ngx_test_null(iov, ngx_push_array(&header), NGX_CHAIN_ERROR);
                iov->iov_base = (void *) cl->buf->pos;
                iov->iov_len = size;
            }

            prev = cl->buf->pos + size;
            hsize += size;
            send += size;
        }

        /* get the file buf */

        if (cl && cl->buf->in_file && send < limit) {
            file = cl->buf;
            fsize = 0;

            /* coalesce the neighbouring file bufs */

            do {
                size = (size_t) (cl->buf->file_last - cl->buf->file_pos);

                if (send + size > limit) {
                    size = limit - send;

                    aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)
                                                      & ~(ngx_pagesize - 1);

                    if (aligned <= cl->buf->file_last) {
                        size = aligned - cl->buf->file_pos;
                    }
                }

                fsize += size;
                send += size;
                fprev = cl->buf->file_pos + size;
                cl = cl->next;

            } while (cl
                     && cl->buf->in_file
                     && send < limit
                     && file->file->fd == cl->buf->file->fd
                     && fprev == cl->buf->file_pos);
        }

        if (file) {
            /* create the tailer iovec and coalesce the neighbouring bufs */

            prev = NULL;
            iov = NULL;

            for (/* void */;
                 cl && header.nelts < IOV_MAX && send < limit;
                 cl = cl->next)
            {
                if (ngx_buf_special(cl->buf)) {
                    continue;
                }

                if (!ngx_buf_in_memory_only(cl->buf)) {
                    break;
                }

                size = cl->buf->last - cl->buf->pos;

                if (send + size > limit) {
                    size = limit - send;
                }

                if (prev == cl->buf->pos) {
                    iov->iov_len += size;

                } else {
                    ngx_test_null(iov, ngx_push_array(&trailer),
                                  NGX_CHAIN_ERROR);
                    iov->iov_base = (void *) cl->buf->pos;
                    iov->iov_len = size;
                }

                prev = cl->buf->pos + size;
                send += size;
            }
        }

        if (file) {

            if (ngx_freebsd_use_tcp_nopush
                && c->tcp_nopush == NGX_TCP_NOPUSH_UNSET)
            {

                if (ngx_tcp_nopush(c->fd) == NGX_ERROR) {
                    err = ngx_errno;

                    /*
                     * there is a tiny chance to be interrupted, however
                     * we continue a processing without the TCP_NOPUSH
                     */

                    if (err != NGX_EINTR) {
                        wev->error = 1;
                        ngx_connection_error(c, err,
                                             ngx_tcp_nopush_n " failed");
                        return NGX_CHAIN_ERROR;
                    }

                } else {
                    c->tcp_nopush = NGX_TCP_NOPUSH_SET;

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                                   "tcp_nopush");
                }
            }

            hdtr.headers = (struct iovec *) header.elts;
            hdtr.hdr_cnt = header.nelts;
            hdtr.trailers = (struct iovec *) trailer.elts;
            hdtr.trl_cnt = trailer.nelts;

            /*
             * the "nbytes bug" of the old sendfile() syscall:
             * http://www.freebsd.org/cgi/query-pr.cgi?pr=33771
             */

            if (ngx_freebsd_sendfile_nbytes_bug == 0) {
                hsize = 0;
            }

            sent = 0;

            rc = sendfile(file->file->fd, c->fd, file->file_pos,
                          fsize + hsize, &hdtr, &sent, 0);

            if (rc == -1) {
                err = ngx_errno;

                if (err == NGX_EAGAIN || err == NGX_EINTR) {
                    if (err == NGX_EINTR) {
                        eintr = 1;

                    } else {
                        eagain = 1;
                    }

                    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, err,
                                   "sendfile() sent only " OFF_T_FMT " bytes",
                                   sent);

                } else {
                    wev->error = 1;
                    ngx_connection_error(c, err, "sendfile() failed");
                    return NGX_CHAIN_ERROR;
                }
            }

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "sendfile: %d, @" OFF_T_FMT " " OFF_T_FMT ":%d",
                           rc, file->file_pos, sent, fsize + hsize);

        } else {
            rc = writev(c->fd, header.elts, header.nelts);

            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "writev: %d of " SIZE_T_FMT, rc, hsize);

            if (rc == -1) {
                err = ngx_errno;

                if (err == NGX_EAGAIN || err == NGX_EINTR) {
                    if (err == NGX_EINTR) {
                        eintr = 1;
                    }

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                                   "writev() not ready");

                } else {
                    wev->error = 1;
                    ngx_connection_error(c, err, "writev() failed");
                    return NGX_CHAIN_ERROR;
                }
            }

            sent = rc > 0 ? rc : 0;
        }

        if (send - sprev == sent) {
            complete = 1;
        }

        c->sent += sent;

        for (cl = in; cl; cl = cl->next) {

            if (ngx_buf_special(cl->buf)) {
                continue;
            }

            if (sent == 0) {
                break;
            }

            size = ngx_buf_size(cl->buf);

            if (sent >= size) {
                sent -= size;

                if (ngx_buf_in_memory(cl->buf)) {
                    cl->buf->pos = cl->buf->last;
                }

                if (cl->buf->in_file) {
                    cl->buf->file_pos = cl->buf->file_last;
                }

                continue;
            }

            if (ngx_buf_in_memory(cl->buf)) {
                cl->buf->pos += sent;
            }

            if (cl->buf->in_file) {
                cl->buf->file_pos += sent;
            }

            break;
        }

        if (eagain) {

            /*
             * sendfile() can return EAGAIN even if it has sent
             * a whole file part but the successive sendfile() call would
             * return EAGAIN right away and would not send anything.
             * We use it as a hint.
             */

            wev->ready = 0;
            return cl;
        }

        if (eintr) {
            continue;
        }

        if (!complete) {
            wev->ready = 0;
            return cl;
        }

        if (send >= limit || cl == NULL) {
            return cl;
        }

        in = cl;
    }
}
