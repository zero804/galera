/**
 * Default poll implementation 
 */

#include "galeracomm/poll.hpp"
#include "galeracomm/fifo.hpp"
#include "galeracomm/logger.hpp"
#include "galeracomm/exception.hpp"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <poll.h>
#include <map>
#include <iostream>
#include <algorithm>

class PollDef : public Poll {

    typedef std::map <const int, PollContext*> PollContextMap;
    typedef std::pair<const int, PollContext*> PollContextMapPair;
    typedef std::map <const int, PollContext*>::iterator PollContextMapIterator;

    PollContextMap ctx_map;
    ssize_t        n_pfds;
    struct pollfd* pfds;
    long long      last_check; // timestamp of the last timeout check

    PollDef (const PollDef&);
    void operator=(const PollDef&);

public:

    PollDef();
    ~PollDef();
    void insert(const int, PollContext *);
    void erase(const int);
    void set(const int, const PollEnum);
    void unset(const int, const PollEnum);
    int poll(const int t = Poll::DEFAULT_KA_TIMEOUT);
};



static inline int map_event_to_mask(const PollEnum e)
{
    int ret = (e & PollEvent::POLL_IN) ? POLLIN : 0;
    ret |= ((e & PollEvent::POLL_OUT) ? POLLOUT : 0);
    return ret;
}

static inline PollEnum map_mask_to_event(const int m)
{
    int ret = PollEvent::POLL_NONE;
    ret |= (m & POLLIN)   ? PollEvent::POLL_IN    : PollEvent::POLL_NONE;
    ret |= (m & POLLOUT)  ? PollEvent::POLL_OUT   : PollEvent::POLL_NONE;
    ret |= (m & POLLERR)  ? PollEvent::POLL_ERR   : PollEvent::POLL_NONE;
    ret |= (m & POLLHUP)  ? PollEvent::POLL_HUP   : PollEvent::POLL_NONE;
    ret |= (m & POLLNVAL) ? PollEvent::POLL_INVAL : PollEvent::POLL_NONE;
    return static_cast<PollEnum>(ret);
}

static struct pollfd *pfd_find(struct pollfd *pfds, const size_t n_pfds, 
			       const int fd)
{
    for (size_t i = 0; i < n_pfds; i++) {
	if (pfds[i].fd == fd)
	    return &pfds[i];
    }
    return 0;
}


void PollDef::insert(const int fd, PollContext *pctx)
{

    if (ctx_map.insert(PollContextMapPair(fd, pctx)).second ==
        false)
	throw DException("Insert");
}

void PollDef::erase(const int fd)
{
    
    unset(fd, PollEvent::POLL_ALL);

    size_t s = ctx_map.size();
    ctx_map.erase(fd);
    if (ctx_map.size() + 1 != s) {
	log_warn << "Fd didn't exist in poll set, map size: " << ctx_map.size();
    }
}

void PollDef::set(const int fd, const PollEnum e)
{
    struct pollfd *pfd = 0;
    
//   if (e == PollEvent::POLL_OUT)
//	std::cerr << "fd " << fd << " set POLL_OUT\n";
    if ((pfd = pfd_find(pfds, n_pfds, fd)) == 0) {
	++n_pfds;
	pfds = reinterpret_cast<struct pollfd *>
            (realloc(pfds, n_pfds*sizeof(struct pollfd)));
	pfd = &pfds[n_pfds - 1];
	pfd->fd = fd;
	pfd->events = 0;
	pfd->revents = 0;
    }
    pfd->events = static_cast<unsigned short>(pfd->events | map_event_to_mask(e));
}

void PollDef::unset(const int fd, const PollEnum e)
{
    struct pollfd *pfd = 0;

//    if (e == PollEvent::POLL_OUT)
//	std::cerr << "fd " << fd << " unset POLL_OUT\n";
    if ((pfd = pfd_find(pfds, n_pfds, fd)) != 0) {
	pfd->events = static_cast<unsigned short>(pfd->events & ~map_event_to_mask(e));
	if (pfd->events == 0) {
	    --n_pfds;
	    memmove (&pfd[0], &pfd[1],
                     (n_pfds - (pfd - pfds))*sizeof(struct pollfd));
	    pfds = reinterpret_cast<struct pollfd *>
                (realloc(pfds, n_pfds*sizeof(struct pollfd)));
	}
    }
}

int PollDef::poll(const int timeout)
{
    int wait = timeout;
    long long begin = PollContext::get_timestamp();
    int p_ret;

    do
    {
        int p_cnt = 0;

        p_ret = ::poll(pfds, n_pfds, std::min(wait, Poll::DEFAULT_KA_INTERVAL));

        int err = errno;

        long long tstamp = PollContext::get_timestamp();
        bool check_timeout = (tstamp - last_check) >= Poll::DEFAULT_KA_INTERVAL;

        wait = timeout - static_cast<int>(tstamp - begin);

        if (p_ret == -1 && err == EINTR) p_ret = 0;

        if (p_ret == -1 && err != EINTR)
        {
            throw DException("");
        }
        else if (p_ret > 0 || check_timeout)
        {
            for (ssize_t i = 0; i < n_pfds; i++)
            {
                PollEnum e = map_mask_to_event(pfds[i].revents);

                if (e != PollEvent::POLL_NONE || check_timeout)
                {
                    PollContextMapIterator map_i;

                    if ((map_i = ctx_map.find(pfds[i].fd)) != ctx_map.end())
                    {
                        if (map_i->second == 0) throw DException("");

                        try {
                            map_i->second->handle(pfds[i].fd, e, tstamp);
                        }
                        catch (std::exception& e) {
                            throw DException (e.what());
                        }

                        p_cnt++;
                    }
                    else
                    {
                        throw FatalException("No ctx for fd found");
                    }
                }
                else
                {
                    if (pfds[i].revents) {
                        log_error << "Unhandled poll events";
                        throw FatalException("");
                    }
                }
            }

            if (!check_timeout)
            {
                if (p_ret != p_cnt)
                {
                    log_warn << "p_ret (" << p_ret << ") != p_cnt ("
                             << p_cnt << ")";
                }
            }
            else
            {
                last_check = tstamp;
                if (n_pfds > p_cnt)
                {
                    log_warn << "n_pfds (" << n_pfds << ") > p_cnt ("
                             << p_cnt << ")";
                }
            }
        }
    }
    while (!p_ret && wait > 0); // timeout

    return p_ret;
}

PollDef::PollDef() : ctx_map(), n_pfds(0), pfds(0), last_check(0) {}

PollDef::~PollDef()
{
    ctx_map.clear();
    free(pfds);
}

class FifoPoll : public Poll {

    std::map<const int, std::pair<PollContext *, PollEnum> > ctx_map;
    long long last_check; // timestamp of last timeout check (ms)

public:

    FifoPoll() : Poll(), ctx_map(), last_check(0) {}

    void insert(const int fd, PollContext *ctx) {
	if (ctx_map.insert(
		std::pair<const int, std::pair<PollContext *, PollEnum> >(
		    fd, 
		    std::pair<PollContext *, PollEnum>(
			ctx, PollEvent::POLL_NONE))).second == false)
	    throw DException("");
    }

    void erase(const int fd) {
	unset(fd, PollEvent::POLL_ALL);
	ctx_map.erase(fd);
    }

    void set(const int fd, const PollEnum e) {
	std::map<const int, std::pair<PollContext *, PollEnum> >::iterator 
	    ctxi = ctx_map.find(fd);
	if (ctxi == ctx_map.end())
	    throw DException("Invalid fd");
	if (Fifo::find(fd) == 0)
	    throw DException("Invalid fd");
	ctxi->second.second = static_cast<unsigned short>(ctxi->second.second | e);
    }

    void unset(const int fd, const PollEnum e) {
	std::map<const int, std::pair<PollContext *, PollEnum> >::iterator 
	    ctxi = ctx_map.find(fd);
	if (ctxi == ctx_map.end())
	    throw DException("Invalid fd");
	ctxi->second.second = static_cast<unsigned short>(ctxi->second.second & ~e);
    }
    
    int poll(int tout)
    {
	int n = 0;

	std::map<const int, std::pair<PollContext *, PollEnum> >::iterator i;

        long long tstamp = PollContext::get_timestamp();
        bool check_timeout = (tstamp - last_check) >= Poll::DEFAULT_KA_INTERVAL;

	for (i = ctx_map.begin(); i != ctx_map.end(); ++i)
        {
	    if ((i->second.second & (PollEvent::POLL_IN | PollEvent::POLL_OUT))
                || check_timeout)
            {
                Fifo *fifo = Fifo::find(i->first);

                if (fifo == 0) throw DException("Invalid fd");

		if ((i->second.second & PollEvent::POLL_IN) &&
                    (fifo->is_empty() == false))
                {
		    i->second.first->handle(i->first, PollEvent::POLL_IN,
                                            tstamp);
		    n++;
		}

		if ((i->second.second & PollEvent::POLL_OUT) &&
                    (fifo->is_full() == false))
                {
		    i->second.first->handle(i->first, PollEvent::POLL_OUT,
                                            tstamp);
		    n++;
		}

		if (check_timeout)
                {
		    i->second.first->handle(i->first, PollEvent::POLL_NONE,
                                            tstamp);
		    n++;
		}
	    }
	}

        if (check_timeout) last_check = tstamp;

	return n;
    }
};


Poll* Poll::create(const char *type)
{
    Poll* ret = 0;
    if (strcasecmp(type, "def") == 0) {
	ret = new PollDef();
    } else if (strcasecmp(type, "fifo") == 0) {
	ret = new FifoPoll();
    } else {
	throw DException("");
    }
    return ret;
}
