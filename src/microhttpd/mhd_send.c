/*
  This file is part of libmicrohttpd
  Copyright (C) 2019 ng0 <ng0@n0.is>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

 */

/**
 * @file microhttpd/mhd_send.c
 * @brief Implementation of send() wrappers.
 * @author ng0 (N. Gillmann)
 * @author Christian Grothoff
 * @author Evgeny Grin
 */

/* Worth considering for future improvements and additions:
 * NetBSD has no sendfile or sendfile64. The way to work
 * with this seems to be to mmap the file and write(2) as
 * large a chunk as possible to the socket. Alternatively,
 * use madvise(..., MADV_SEQUENTIAL). */

/* Functions to be used in: send_param_adapter, MHD_send_
 * and every place where sendfile(), sendfile64(), setsockopt()
 * are used. */

#include "mhd_send.h"
#ifdef MHD_LINUX_SOLARIS_SENDFILE
#include <sys/sendfile.h>
#endif /* MHD_LINUX_SOLARIS_SENDFILE */
#if defined(HAVE_FREEBSD_SENDFILE) || defined(HAVE_DARWIN_SENDFILE)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif /* HAVE_FREEBSD_SENDFILE || HAVE_DARWIN_SENDFILE */
#ifdef HAVE_SYS_PARAM_H
/* For FreeBSD version identification */
#include <sys/param.h>
#endif /* HAVE_SYS_PARAM_H */
#include "mhd_assert.h"


/**
 * sendfile() chuck size
 */
#define MHD_SENFILE_CHUNK_         (0x20000)

/**
 * sendfile() chuck size for thread-per-connection
 */
#define MHD_SENFILE_CHUNK_THR_P_C_ (0x200000)

#ifdef HAVE_FREEBSD_SENDFILE
#ifdef SF_FLAGS
/**
 * FreeBSD sendfile() flags
 */
static int freebsd_sendfile_flags_;

/**
 * FreeBSD sendfile() flags for thread-per-connection
 */
static int freebsd_sendfile_flags_thd_p_c_;
#endif /* SF_FLAGS */
/**
 * Initialises static variables
 */
void
MHD_send_init_static_vars_ (void)
{
/* FreeBSD 11 and later allow to specify read-ahead size
 * and handles SF_NODISKIO differently.
 * SF_FLAGS defined only on FreeBSD 11 and later. */
#ifdef SF_FLAGS
  long sys_page_size = sysconf (_SC_PAGESIZE);
  if (0 >= sys_page_size)
  {   /* Failed to get page size. */
    freebsd_sendfile_flags_ = SF_NODISKIO;
    freebsd_sendfile_flags_thd_p_c_ = SF_NODISKIO;
  }
  else
  {
    freebsd_sendfile_flags_ =
      SF_FLAGS ((uint16_t) ((MHD_SENFILE_CHUNK_ + sys_page_size - 1)
                            / sys_page_size), SF_NODISKIO);
    freebsd_sendfile_flags_thd_p_c_ =
      SF_FLAGS ((uint16_t) ((MHD_SENFILE_CHUNK_THR_P_C_ + sys_page_size - 1)
                            / sys_page_size), SF_NODISKIO);
  }
#endif /* SF_FLAGS */
}


#endif /* HAVE_FREEBSD_SENDFILE */


/**
 * Handle pre-send setsockopt calls.
 *
 * @param connection the MHD_Connection structure
 * @param plain_send set to true if plain send() or sendmsg() will be called,
 *                   set to false if TLS socket send(), sendfile() or
 *                   writev() will be called.
 * @param push_data whether to push data to the network from buffers after
 *                  the next call of send function.
 */
static void
pre_send_setopt (struct MHD_Connection *connection,
                 bool plain_send,
                 bool push_data)
{
  int ret;
  /* Try to buffer data if not sending the final piece.
   * Final piece is indicated by push_data == true. */
  const bool buffer_data = (! push_data);

#ifdef MHD_USE_MSG_MORE
  if (plain_send)
  {
    /* MSG_MORE is used, no need for extra syscalls! */
    return;
  }
#else  /* ! MHD_USE_MSG_MORE */
  (void) plain_send; /* Mute compiler warning. */
#endif /* ! MHD_USE_MSG_MORE */

#if defined(MHD_TCP_CORK_NOPUSH)
  /* If connection is already in required corked state, do nothing. */
  if (connection->sk_corked == buffer_data)
    return;
  if (push_data)
    return; /* nothing to do *pre* syscall! BUG: to be fixed */
  ret = MHD_socket_cork_ (connection->socket_fd,
                          buffer_data);
  if (0 != ret)
  {
    connection->sk_corked = buffer_data;
    return;
  }
  switch (errno)
  {
  case ENOTSOCK:
    /* FIXME: Could be we are talking to a pipe, maybe remember this
       and avoid all setsockopt() in the future? */
    break;
  case EBADF:
    /* FIXME: should we die hard here? */
    break;
  case EINVAL:
    /* FIXME: optlen invalid, should at least log this, maybe die */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("optlen invalid: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  case EFAULT:
    /* wopsie, should at least log this, FIXME: maybe die */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ (
                "The address pointed to by optval is not a valid part of the process address space: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  case ENOPROTOOPT:
    /* optlen unknown, should at least log this */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("The option is unknown: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  default:
    /* any others? man page does not list more... */
    break;
  }
#else
  /* CORK/NOPUSH do not exist on this platform,
     Turning on/off of Naggle's algorithm
     (otherwise we keep it always off) */
  if (connection->sk_nodelay == push_data)
  {
    /* nothing to do, success! */
    return;
  }
  if (0 == MHD_socket_set_nodelay_ (connection->socket_fd,
                                    (push_data)))
    connection->sk_nodelay = push_data;
#endif
}


/**
 * Handle post-send setsockopt calls.
 *
 * @param connection the MHD_Connection structure
 * @param plain_send set to true if plain send() or sendmsg() have been
 *                   called,
 *                   set to false if TLS socket send(), sendfile() or
 *                   writev() has been called.
 * @param push_data whether to push data to the network from buffers
 */
static void
post_send_setopt (struct MHD_Connection *connection,
                  bool plain_send,
                  bool push_data)
{
  int ret;
  /* Try to buffer data if not sending the final piece.
   * Final piece is indicated by push_data == true. */
  const bool buffer_data = (! push_data);

#ifdef MHD_USE_MSG_MORE
  if (plain_send)
  {
    /* MSG_MORE is used, no need for extra syscalls! */
    return;
  }
#else  /* ! MHD_USE_MSG_MORE */
  (void) plain_send; /* Mute compiler warning. */
#endif /* ! MHD_USE_MSG_MORE */

#if defined(MHD_TCP_CORK_NOPUSH)
  /* If connection is already in required corked state, do nothing. */
  if (connection->sk_corked == buffer_data)
    return;
  if (buffer_data)
    return; /* nothing to do *post* syscall (in fact, we should never
               get here, as sk_corked should have succeeded in the
               pre-syscall) */
  ret = MHD_socket_cork_ (connection->socket_fd,
                          buffer_data);
  if (0 != ret)
  {
    connection->sk_corked = buffer_data;
    return;
  }
  switch (errno)
  {
  case ENOTSOCK:
    /* FIXME: Could be we are talking to a pipe, maybe remember this
       and avoid all setsockopt() in the future? */
    break;
  case EBADF:
    /* FIXME: should we die hard here? */
    break;
  case EINVAL:
    /* FIXME: optlen invalid, should at least log this, maybe die */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("optlen invalid: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  case EFAULT:
    /* wopsie, should at least log this, FIXME: maybe die */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ (
                "The address pointed to by optval is not a valid part of the process address space: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  case ENOPROTOOPT:
    /* optlen unknown, should at least log this */
#ifdef HAVE_MESSAGES
    MHD_DLOG (connection->daemon,
              _ ("The option is unknown: %s\n"),
              MHD_socket_last_strerr_ ());
#endif
    break;
  default:
    /* any others? man page does not list more... */
    break;
  }
#endif
}


/**
 * Send buffer on connection, and remember the current state of
 * the socket options; only call setsockopt when absolutely
 * necessary.
 *
 * @param connection the MHD_Connection structure
 * @param buffer content of the buffer to send
 * @param buffer_size the size of the buffer (in bytes)
 * @param options the #MHD_SendSocketOptions enum,
 *         #MHD_SSO_NO_CORK: definitely no corking (use NODELAY, or explicitly disable cork),
 *         #MHD_SSO_MAY_CORK: should enable corking (use MSG_MORE, or explicitly enable cork),
 *         #MHD_SSO_HDR_CORK: consider tcpi_snd_mss and consider not corking for the header
 *         part if the size of the header is close to the MSS.
 *         Only used if we are NOT doing 100 Continue and are still sending the
 *         header (provided in full as the buffer to #MHD_send_on_connection_ or as
 *         the header to #MHD_send_on_connection2_).
 * @return sum of the number of bytes sent from both buffers or
 *         -1 on error
 */
ssize_t
MHD_send_on_connection_ (struct MHD_Connection *connection,
                         const char *buffer,
                         size_t buffer_size,
                         enum MHD_SendSocketOptions options)
{
  bool push_data;
  MHD_socket s = connection->socket_fd;
  ssize_t ret;
#ifdef HTTPS_SUPPORT
  const bool tls_conn = (connection->daemon->options & MHD_USE_TLS);
#else  /* ! HTTPS_SUPPORT */
  const bool tls_conn = false;
#endif /* ! HTTPS_SUPPORT */

  /* error handling from send_param_adapter() */
  if ( (MHD_INVALID_SOCKET == s) ||
       (MHD_CONNECTION_CLOSED == connection->state) )
  {
    return MHD_ERR_NOTCONN_;
  }

  /* Get socket options, change/set options if necessary. */
  switch (options)
  {
  /* No corking */
  case MHD_SSO_PUSH_DATA:
    push_data = true;
    break;
  /* Do corking, consider MSG_MORE instead if available. */
  case MHD_SSO_PREFER_BUFF:
    push_data = false;
    break;
  /* Cork the header. */
  case MHD_SSO_HDR_CORK:
    push_data = (buffer_size > 1024);
    break;
  }

  pre_send_setopt (connection, (! tls_conn), push_data);
  if (tls_conn)
  {
#ifdef HTTPS_SUPPORT
    if (buffer_size > SSIZE_MAX)
      buffer_size = SSIZE_MAX;
    ret = gnutls_record_send (connection->tls_session,
                              buffer,
                              buffer_size);
    if ( (GNUTLS_E_AGAIN == ret) ||
         (GNUTLS_E_INTERRUPTED == ret) )
    {
#ifdef EPOLL_SUPPORT
      if (GNUTLS_E_AGAIN == ret)
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif
      return MHD_ERR_AGAIN_;
    }
    if (ret < 0)
    {
      /* Likely 'GNUTLS_E_INVALID_SESSION' (client communication
         disrupted); interpret as a hard error */
      return MHD_ERR_NOTCONN_;
    }
#ifdef EPOLL_SUPPORT
    /* Unlike non-TLS connections, do not reset "write-ready" if
     * sent amount smaller than provided amount, as TLS
     * connections may break data into smaller parts for sending. */
#endif /* EPOLL_SUPPORT */
#endif /* HTTPS_SUPPORT  */
    (void) 0; /* Mute compiler warning for non-TLS builds. */
  }
  else
  {
    /* plaintext transmission */
    if (buffer_size > MHD_SCKT_SEND_MAX_SIZE_)
      buffer_size = MHD_SCKT_SEND_MAX_SIZE_; /* return value limit */

#ifdef MHD_USE_MSG_MORE
    ret = MHD_send4_ (s,
                      buffer,
                      buffer_size,
                      push_data ? 0 : MSG_MORE);
#else
    ret = MHD_send4_ (s,
                      buffer,
                      buffer_size,
                      0);
#endif

    if (0 > ret)
    {
      const int err = MHD_socket_get_error_ ();

      if (MHD_SCKT_ERR_IS_EAGAIN_ (err))
      {
#if EPOLL_SUPPORT
        /* EAGAIN, no longer write-ready */
        connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
        return MHD_ERR_AGAIN_;
      }
      if (MHD_SCKT_ERR_IS_EINTR_ (err))
        return MHD_ERR_AGAIN_;
      if (MHD_SCKT_ERR_IS_ (err, MHD_SCKT_ECONNRESET_))
        return MHD_ERR_CONNRESET_;
      /* Treat any other error as hard error. */
      return MHD_ERR_NOTCONN_;
    }
#if EPOLL_SUPPORT
    else if (buffer_size > (size_t) ret)
      connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
  }
  post_send_setopt (connection, tls_conn,
                    (push_data && (buffer_size == (size_t) ret)) );

  return ret;
}


/**
 * Send header followed by buffer on connection.
 * Uses writev if possible to send both at once
 * and returns the sum of the number of bytes sent from
 * both buffers, or -1 on error;
 * if writev is unavailable, this call MUST only send from 'header'
 * (as we cannot handle the case that the first write
 * succeeds and the 2nd fails!).
 *
 * @param connection the MHD_Connection structure
 * @param header content of header to send
 * @param header_size the size of the header (in bytes)
 * @param buffer content of the buffer to send
 * @param buffer_size the size of the buffer (in bytes)
 * @return sum of the number of bytes sent from both buffers or
 *         -1 on error
 */
ssize_t
MHD_send_on_connection2_ (struct MHD_Connection *connection,
                          const char *header,
                          size_t header_size,
                          const char *buffer,
                          size_t buffer_size)
{
  MHD_socket s = connection->socket_fd;
  ssize_t ret;
  struct iovec vector[2];
#ifdef HTTPS_SUPPORT
  const bool tls_conn = (connection->daemon->options & MHD_USE_TLS);
#else  /* ! HTTPS_SUPPORT */
  const bool tls_conn = false;
#endif /* ! HTTPS_SUPPORT */

#ifdef HTTPS_SUPPORT
  if (tls_conn)
  {
    ssize_t ret;

    ret = MHD_send_on_connection_ (connection,
                                   header,
                                   header_size,
                                   MHD_SSO_HDR_CORK);
    return ret;
  }
#endif
#if defined(HAVE_SENDMSG) || defined(HAVE_WRITEV)
  /* Since we generally give the fully answer, we do not want
     corking to happen */
  pre_send_setopt (connection, (! tls_conn), true);

  vector[0].iov_base = (void *) header;
  vector[0].iov_len = header_size;
  vector[1].iov_base = (void *) buffer;
  vector[1].iov_len = buffer_size;

#if HAVE_SENDMSG
  {
    struct msghdr msg;

    memset (&msg, 0, sizeof(struct msghdr));
    msg.msg_iov = vector;
    msg.msg_iovlen = 2;

    ret = sendmsg (s, &msg, MSG_NOSIGNAL_OR_ZERO);
    if ( (-1 == ret) &&
         (EAGAIN == errno) )
      return MHD_ERR_AGAIN_;
  }
#elif HAVE_WRITEV
  {
    int iovcnt;

    iovcnt = sizeof (vector) / sizeof (struct iovec);
    ret = writev (s, vector, iovcnt);
    if ( (-1 == ret) &&
         (EAGAIN == errno) )
      return MHD_ERR_AGAIN_;
  }
#endif

  /* Only if we succeeded sending the full buffer, we need to make sure that
     the OS flushes at the end */
  post_send_setopt (connection, (! tls_conn),
                    (header_size + buffer_size == (size_t) ret));

  return ret;

#else
  return MHD_send_on_connection_ (connection,
                                  header,
                                  header_size,
                                  MHD_SSO_HDR_CORK);
#endif
}


#if defined(_MHD_HAVE_SENDFILE)
/**
 * Function for sending responses backed by file FD.
 *
 * @param connection the MHD connection structure
 * @return actual number of bytes sent
 */
ssize_t
MHD_send_sendfile_ (struct MHD_Connection *connection)
{
  ssize_t ret;
  const int file_fd = connection->response->fd;
  uint64_t left;
  uint64_t offsetu64;
#ifndef HAVE_SENDFILE64
  const uint64_t max_off_t = (uint64_t) OFF_T_MAX;
#else  /* HAVE_SENDFILE64 */
  const uint64_t max_off_t = (uint64_t) OFF64_T_MAX;
#endif /* HAVE_SENDFILE64 */
#ifdef MHD_LINUX_SOLARIS_SENDFILE
#ifndef HAVE_SENDFILE64
  off_t offset;
#else  /* HAVE_SENDFILE64 */
  off64_t offset;
#endif /* HAVE_SENDFILE64 */
#endif /* MHD_LINUX_SOLARIS_SENDFILE */
#ifdef HAVE_FREEBSD_SENDFILE
  off_t sent_bytes;
  int flags = 0;
#endif
#ifdef HAVE_DARWIN_SENDFILE
  off_t len;
#endif /* HAVE_DARWIN_SENDFILE */
  const bool used_thr_p_c = (0 != (connection->daemon->options
                                   & MHD_USE_THREAD_PER_CONNECTION));
  const size_t chunk_size = used_thr_p_c ? MHD_SENFILE_CHUNK_THR_P_C_ :
                            MHD_SENFILE_CHUNK_;
  size_t send_size = 0;
  mhd_assert (MHD_resp_sender_sendfile == connection->resp_sender);
  mhd_assert (0 == (connection->daemon->options & MHD_USE_TLS));

  pre_send_setopt (connection, false, true);

  offsetu64 = connection->response_write_position
              + connection->response->fd_off;
  left = connection->response->total_size - connection->response_write_position;
  /* Do not allow system to stick sending on single fast connection:
   * use 128KiB chunks (2MiB for thread-per-connection). */
  send_size = (left > chunk_size) ? chunk_size : (size_t) left;
  if (max_off_t < offsetu64)
  {   /* Retry to send with standard 'send()'. */
    connection->resp_sender = MHD_resp_sender_std;
    return MHD_ERR_AGAIN_;
  }
#ifdef MHD_LINUX_SOLARIS_SENDFILE
#ifndef HAVE_SENDFILE64
  offset = (off_t) offsetu64;
  ret = sendfile (connection->socket_fd,
                  file_fd,
                  &offset,
                  send_size);
#else  /* HAVE_SENDFILE64 */
  offset = (off64_t) offsetu64;
  ret = sendfile64 (connection->socket_fd,
                    file_fd,
                    &offset,
                    send_size);
#endif /* HAVE_SENDFILE64 */
  if (0 > ret)
  {
    const int err = MHD_socket_get_error_ ();
    if (MHD_SCKT_ERR_IS_EAGAIN_ (err))
    {
#ifdef EPOLL_SUPPORT
      /* EAGAIN --- no longer write-ready */
      connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
      return MHD_ERR_AGAIN_;
    }
    if (MHD_SCKT_ERR_IS_EINTR_ (err))
      return MHD_ERR_AGAIN_;
#ifdef HAVE_LINUX_SENDFILE
    if (MHD_SCKT_ERR_IS_ (err,
                          MHD_SCKT_EBADF_))
      return MHD_ERR_BADF_;
    /* sendfile() failed with EINVAL if mmap()-like operations are not
       supported for FD or other 'unusual' errors occurred, so we should try
       to fall back to 'SEND'; see also this thread for info on
       odd libc/Linux behavior with sendfile:
       http://lists.gnu.org/archive/html/libmicrohttpd/2011-02/msg00015.html */
    connection->resp_sender = MHD_resp_sender_std;
    return MHD_ERR_AGAIN_;
#else  /* HAVE_SOLARIS_SENDFILE */
    if ( (EAFNOSUPPORT == err) ||
         (EINVAL == err) ||
         (EOPNOTSUPP == err) )
    {     /* Retry with standard file reader. */
      connection->resp_sender = MHD_resp_sender_std;
      return MHD_ERR_AGAIN_;
    }
    if ( (ENOTCONN == err) ||
         (EPIPE == err) )
    {
      return MHD_ERR_CONNRESET_;
    }
    return MHD_ERR_BADF_;   /* Fail hard */
#endif /* HAVE_SOLARIS_SENDFILE */
  }
#ifdef EPOLL_SUPPORT
  else if (send_size > (size_t) ret)
    connection->epoll_state &= ~MHD_EPOLL_STATE_WRITE_READY;
#endif /* EPOLL_SUPPORT */
#elif defined(HAVE_FREEBSD_SENDFILE)
#ifdef SF_FLAGS
  flags = used_thr_p_c ?
          freebsd_sendfile_flags_thd_p_c_ : freebsd_sendfile_flags_;
#endif /* SF_FLAGS */
  if (0 != sendfile (file_fd,
                     connection->socket_fd,
                     (off_t) offsetu64,
                     send_size,
                     NULL,
                     &sent_bytes,
                     flags))
  {
    const int err = MHD_socket_get_error_ ();
    if (MHD_SCKT_ERR_IS_EAGAIN_ (err) ||
        MHD_SCKT_ERR_IS_EINTR_ (err) ||
        (EBUSY == err) )
    {
      mhd_assert (SSIZE_MAX >= sent_bytes);
      if (0 != sent_bytes)
        return (ssize_t) sent_bytes;

      return MHD_ERR_AGAIN_;
    }
    /* Some unrecoverable error. Possibly file FD is not suitable
     * for sendfile(). Retry with standard send(). */
    connection->resp_sender = MHD_resp_sender_std;
    return MHD_ERR_AGAIN_;
  }
  mhd_assert (0 < sent_bytes);
  mhd_assert (SSIZE_MAX >= sent_bytes);
  ret = (ssize_t) sent_bytes;
#elif defined(HAVE_DARWIN_SENDFILE)
  len = (off_t) send_size; /* chunk always fit */
  if (0 != sendfile (file_fd,
                     connection->socket_fd,
                     (off_t) offsetu64,
                     &len,
                     NULL,
                     0))
  {
    const int err = MHD_socket_get_error_ ();
    if (MHD_SCKT_ERR_IS_EAGAIN_ (err) ||
        MHD_SCKT_ERR_IS_EINTR_ (err))
    {
      mhd_assert (0 <= len);
      mhd_assert (SSIZE_MAX >= len);
      mhd_assert (send_size >= (size_t) len);
      if (0 != len)
        return (ssize_t) len;

      return MHD_ERR_AGAIN_;
    }
    if ((ENOTCONN == err) ||
        (EPIPE == err) )
      return MHD_ERR_CONNRESET_;
    if ((ENOTSUP == err) ||
        (EOPNOTSUPP == err) )
    {     /* This file FD is not suitable for sendfile().
           * Retry with standard send(). */
      connection->resp_sender = MHD_resp_sender_std;
      return MHD_ERR_AGAIN_;
    }
    return MHD_ERR_BADF_;   /* Return hard error. */
  }
  mhd_assert (0 <= len);
  mhd_assert (SSIZE_MAX >= len);
  mhd_assert (send_size >= (size_t) len);
  ret = (ssize_t) len;
#endif /* HAVE_FREEBSD_SENDFILE */

  /* Make sure we send the data without delay ONLY if we
     provided the complete response (not on partial write) */
  post_send_setopt (connection, false, (left == (uint64_t) ret));

  return ret;
}


#endif /* _MHD_HAVE_SENDFILE */
