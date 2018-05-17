
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <memory.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include <string>
#include <vector>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <event2/event.h>

#include "comm_client.h"
#include "cct_proxy_client.h"
#include "cc_proxy_protocol.h"
#include "comm_client_cb_api.h"

#define PPRDFD	0
#define PPWRFD	1

static const struct timeval naught = {0,0};
static const struct timeval _50ms_ = {0,50000};
static const struct timeval _1sec_ = {1,0};

cct_proxy_client::cct_proxy_client(const char * proxy_addr, const u_int16_t proxy_port)
: m_proxy_addr(proxy_addr), m_proxy_port(proxy_port), m_peer_mask(NULL), m_mask_size(0)
, m_sockfd(-1), m_base(NULL), m_timer(NULL), m_read(NULL), m_write(NULL)
{
}

cct_proxy_client::~cct_proxy_client()
{
}

int cct_proxy_client::start(const unsigned int id, const unsigned int peer_count, const char * comm_conf_file, comm_client_cb_api * sink)
{
	m_mask_size = (peer_count + 7) / 8;
	m_peer_mask = new u_int8_t[m_mask_size];
	memset(m_peer_mask, 0, m_mask_size);

	return comm_client::start(id, peer_count, comm_conf_file, sink);
}

void cct_proxy_client::stop()
{
	comm_client::stop();

	delete m_peer_mask;
	m_peer_mask = NULL;
	m_mask_size = 0;
}

void cct_proxy_client::run()
{
	m_base = event_base_new();
	if(NULL != m_base)
	{
		m_read = event_new(m_base, -1, EV_TIMEOUT, connect_cb, this);
		if(NULL != m_read)
		{
			if(0 == event_add(m_read, &naught))
			{
				if(0 == pipe(m_out_pipe))
				{
					m_timer = event_new(m_base, -1, EV_TIMEOUT|EV_PERSIST, timer_cb, this);
					if(NULL != m_timer)
					{
						if(0 == event_add(m_read, &_50ms_))
						{
							syslog(LOG_NOTICE, "%s: starting event loop.", __FUNCTION__);
							event_base_dispatch(m_base);
							syslog(LOG_NOTICE, "%s: event loop stopped.", __FUNCTION__);

							if(NULL != m_write)
							{
								event_del(m_write);
								event_free(m_write);
								m_write = NULL;
							}

							if(-1 != m_sockfd)
							{
								close(m_sockfd);
								m_sockfd = -1;
							}
							event_del(m_timer);
						}
						else
							syslog(LOG_ERR, "%s: timer event addition failed.", __FUNCTION__);

						event_free(m_timer);
						m_timer = NULL;
					}
					else
						syslog(LOG_ERR, "%s: timer event allocation failed.", __FUNCTION__);

					close(m_out_pipe[PPRDFD]);
					m_out_pipe[PPRDFD] = -1;
					close(m_out_pipe[PPWRFD]);
					m_out_pipe[PPWRFD] = -1;
				}
				else
				{
		            int errcode = errno;
		            char errmsg[256];
		            syslog(LOG_ERR, "%s: pipe() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
				}

				event_del(m_read);
			}
			else
				syslog(LOG_ERR, "%s: connect event addition failed.", __FUNCTION__);

			event_free(m_read);
			m_read = NULL;
		}
		else
			syslog(LOG_ERR, "%s: connect event allocation failed.", __FUNCTION__);
		event_base_free(m_base);
		m_base = NULL;
	}
	else
		syslog(LOG_ERR, "%s: event base allocation failed.", __FUNCTION__);
}

void cct_proxy_client::set_syslog_name()
{
	memset(m_syslog_name, 0, 32);
	snprintf(m_syslog_name, 32, "cct-prxy-clnt-%u", m_id);
}

int cct_proxy_client::send(const unsigned int dst_id, const unsigned char * msg, const size_t size)
{
	size_t offset = dst_id/8;
	u_int8_t mask = 0x01 << (dst_id%8);
	if(0 == (m_peer_mask[offset] & mask))
	{
		syslog(LOG_WARNING, "%s: rejected a message to disconnected peer %u.", __FUNCTION__, dst_id);
		return -1;
	}

	peer_msg_t msghdr;
	msghdr.peer_id = dst_id;
	msghdr.size = size;

	struct iovec iov[2];
	iov[0].iov_base = &msghdr;
	iov[0].iov_len = sizeof(peer_msg_t);
	iov[1].iov_base = (void *)msg;
	iov[1].iov_len = size;

	ssize_t nwrit = writev(m_out_pipe[PPWRFD], iov, 2);
	if((ssize_t)(sizeof(peer_msg_t) + size) != nwrit)
	{
		if(0 > nwrit)
		{
			int errcode = errno;
			char errmsg[512];
			syslog(LOG_ERR, "%s: writev() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
	        exit(-__LINE__);
		}
		else
		{
			syslog(LOG_ERR, "%s: writev() partial write of %lu out of %lu.", __FUNCTION__, (size_t)nwrit, (sizeof(peer_msg_t) + size));
	        exit(-__LINE__);
		}
	}
	else
		syslog(LOG_DEBUG, "%s: %lu bytes written to out pipe.", __FUNCTION__, (size_t)nwrit);
	return 0;
}

void cct_proxy_client::on_connect()
{
	if(0 == make_connection())
	{
		event_free(m_read);
		m_data.clear();

		m_read = event_new(m_base, m_sockfd, EV_READ, read_cb, this);
		if(0 != event_add(m_read, NULL))
		{
			syslog(LOG_ERR, "%s: read event addition failed.", __FUNCTION__);
			exit(-__LINE__);
		}

		m_write = event_new(m_base, m_out_pipe[PPRDFD], EV_READ, write1_cb, this);
		if(0 != event_add(m_write, NULL))
		{
			syslog(LOG_ERR, "%s: write event addition failed.", __FUNCTION__);
			exit(-__LINE__);
		}
	}
	else
	{
		if(0 != event_add(m_read, &_1sec_))
		{
			syslog(LOG_ERR, "%s: connect event addition failed.", __FUNCTION__);
			exit(-__LINE__);
		}
	}
}

void cct_proxy_client::on_timer()
{
	if(!get_run_flag())
	{
		syslog(LOG_DEBUG, "%s: run flag down; breaking event loop.", __FUNCTION__);
		event_base_loopbreak(m_base);
	}
}

void cct_proxy_client::on_read()
{
	u_int8_t buffer[4096];
	ssize_t nread = read(m_sockfd, buffer, 4096);
	if(0 < nread)
	{
		m_data.insert(m_data.end(), buffer, buffer + nread);
		process_messages();
		return;
	}
	else if(0 == nread)
		syslog(LOG_NOTICE, "%s: read 0 bytes; disconnection.", __FUNCTION__);
	else
	{
		int errcode = errno;
		char errmsg[512];
        syslog(LOG_ERR, "%s: read() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
	}

	m_data.clear();
	close(m_sockfd);
	m_sockfd = -1;

	event_del(m_write);
	event_free(m_write);
	m_write = NULL;

	event_del(m_read);
	event_free(m_read);
	m_read = event_new(m_base, -1, EV_TIMEOUT, connect_cb, this);
	if(NULL == m_read)
	{
		syslog(LOG_ERR, "%s: read event allocation failed.", __FUNCTION__);
		exit(-__LINE__);
	}
	if(0 != event_add(m_read, &_1sec_))
	{
		syslog(LOG_ERR, "%s: connect event addition failed.", __FUNCTION__);
		exit(-__LINE__);
	}
}

void cct_proxy_client::on_write1()
{
	event_free(m_write);
	m_write = event_new(m_base, m_sockfd, EV_WRITE, write2_cb, this);
	if(NULL == m_write)
	{
		syslog(LOG_ERR, "%s: write event allocation failed.", __FUNCTION__);
		exit(-__LINE__);
	}
	if(0 != event_add(m_write, NULL))
	{
		syslog(LOG_ERR, "%s: write event addition failed.", __FUNCTION__);
		exit(-__LINE__);
	}
}

void cct_proxy_client::on_write2()
{
	event_free(m_write);
	m_write = NULL;

	int result = splice(m_out_pipe[PPRDFD], NULL, m_sockfd, NULL, 4096, 0);
	if(0 > result)
	{
		int errcode = errno;
		char errmsg[512];
        syslog(LOG_ERR, "%s: splice() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
    	syslog(LOG_INFO, "%s: disconnecting.", __FUNCTION__);

    	m_data.clear();
    	close(m_sockfd);
    	m_sockfd = -1;

    	event_del(m_read);
    	event_free(m_read);
    	m_read = event_new(m_base, -1, EV_TIMEOUT, connect_cb, this);
    	if(NULL == m_read)
    	{
    		syslog(LOG_ERR, "%s: read event allocation failed.", __FUNCTION__);
    		exit(-__LINE__);
    	}
    	if(0 != event_add(m_read, &_1sec_))
    	{
    		syslog(LOG_ERR, "%s: connect event addition failed.", __FUNCTION__);
    		exit(-__LINE__);
    	}
	}
	else
	{
		syslog(LOG_DEBUG, "%s: %d bytes spliced out to conn.", __FUNCTION__, result);

		m_write = event_new(m_base, m_out_pipe[PPRDFD], EV_READ, write1_cb, this);
		if(NULL == m_write)
		{
			syslog(LOG_ERR, "%s: write event allocation failed.", __FUNCTION__);
			exit(-__LINE__);
		}
		if(0 != event_add(m_write, NULL))
		{
			syslog(LOG_ERR, "%s: write event addition failed.", __FUNCTION__);
			exit(-__LINE__);
		}
	}
}

int cct_proxy_client::make_connection()
{
	if (0 <= (m_sockfd = socket(AF_INET, SOCK_STREAM, 0)))
	{
		syslog(LOG_DEBUG, "%s: socket created %d.", __FUNCTION__, m_sockfd);

		struct sockaddr_in sockaddr;
		if (0 != inet_aton(m_proxy_addr.c_str(), &sockaddr.sin_addr))
	    {
			sockaddr.sin_port = htons(m_proxy_port);
			sockaddr.sin_family = AF_INET;

			if(0 == connect(m_sockfd, (const struct sockaddr *)&sockaddr, sizeof(struct sockaddr_in)))
			{
				syslog(LOG_NOTICE, "%s: socket %d connected to [%s:%hu].", __FUNCTION__, m_sockfd, m_proxy_addr.c_str(), m_proxy_port);
				return 0;
			}
			else
			{
				int errcode = errno;
				char errmsg[512];
		        syslog(LOG_ERR, "%s: connect() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
			}
	    }
		else
			syslog(LOG_ERR, "%s: failure converting proxy IP address <%s>.", __FUNCTION__, m_proxy_addr.c_str());

		close(m_sockfd);
		m_sockfd = -1;
	}
	else
	{
		int errcode = errno;
		char errmsg[512];
        syslog(LOG_ERR, "%s: socket() failed with error %d : [%s].", __FUNCTION__, errcode, strerror_r(errcode, errmsg, 256));
	}
	return -1;
}

int cct_proxy_client::process_messages()
{
	while(!m_data.empty())
	{
		size_t data_size = m_data.size();
		if(sizeof(peer_msg_t) > data_size)
			break;

		peer_msg_t hdr = *((peer_msg_t *)m_data.data());
		hdr.ntoh();
		if((sizeof(peer_msg_t) + hdr.size) > data_size)
			break;

		this->m_sink->on_comm_message(hdr.peer_id, m_data.data() + sizeof(peer_msg_t), hdr.size);
		m_data.erase(m_data.begin(), m_data.begin() + (sizeof(peer_msg_t) + hdr.size));
	}
	return 0;
}

void cct_proxy_client::connect_cb(evutil_socket_t fd, short what, void * arg)
{
	((cct_proxy_client *)arg)->on_connect();
}

void cct_proxy_client::timer_cb(evutil_socket_t fd, short what, void * arg)
{
	((cct_proxy_client *)arg)->on_timer();
}

void cct_proxy_client::read_cb(evutil_socket_t fd, short what, void * arg)
{
	((cct_proxy_client *)arg)->on_read();
}

void cct_proxy_client::write1_cb(evutil_socket_t fd, short what, void * arg)
{
	((cct_proxy_client *)arg)->on_write1();
}

void cct_proxy_client::write2_cb(evutil_socket_t fd, short what, void * arg)
{
	((cct_proxy_client *)arg)->on_write2();
}