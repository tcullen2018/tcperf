
#include <inttypes.h>
#include <signal.h>
#include <sys/epoll.h>
#include <thread>
#include <mutex>
#include <iostream>
#include <list>
#include <algorithm>
#include <cstring>
#include <liburing.h>

#include "udp.h"

// namespaces
using namespace std;

// constants
static const int EPOLL_SIZE_IGNORED = 1;
static const int MAX_IOV            = 16;
static const int MAX_IN_FLIGHT      = 16;
static const uint16_t QUEUE_DEPTH   = 1024;

// types
struct io_req {
    struct msghdr mh;
    struct iovec iov[MAX_IOV];
    int iovlen;
};

typedef list<struct io_req> io_req_queue;
typedef list<thread> thrd_list;

// prototypes
void queue_io( io_req_queue& );

// globals
static struct io_uring iour;
static uint8_t wb[4096];
static bool cont_running = true;
static io_req_queue ioq;
static mutex ioq_m;
static thrd_list tl;
static mutex tl_m;

/*
while( cont_running ) {
    // check for available completions
    // if you get -EAGAIN as ret code then the CQ is empty
    // otherwise 0 = success with *cqe_ptr pointing to avail completion
    // any other errno value is an actual error

    // submit new io reqs until SQ full or out of new reqs or max_in_flight reached

    // ^^^ this will busy wait, probably need to some way to block until SQ not full or
    // some CQ entries are avail
}
*/

static void io_loop( uint64_t not_used )
{
    struct io_uring_sqe *sqe = NULL;
    struct io_uring_cqe *cqe = NULL;
    sigset_t ss;
    int max_in_flight = 0;
    int epfd;
    int ret;

    // block all signals except SIGUSR1
    sigfillset( &ss );
//    sigdelset( &ss,SIGUSR1 );
    pthread_sigmask( SIG_BLOCK,&ss,NULL );

    // empty the sigset to specify only SIGUSR1
    sigemptyset( &ss );
    sigaddset( &ss,SIGUSR1 );

    if( (epfd = epoll_create( EPOLL_SIZE_IGNORED )) > 0 ) {
        if( !(ret = io_uring_queue_init( QUEUE_DEPTH,&iour,0 )) ) {
            try {
                udp4 udpskt( "127.0.0.1",5201 );
                struct epoll_event evt;

                evt.events = EPOLLIN | EPOLLOUT;

                if( !(epoll_ctl( epfd,EPOLL_CTL_ADD,udpskt.get_skt(),&evt )) ) {
                    while( cont_running ) {
                        if( (ret = epoll_pwait( epfd,&evt,1,-1,&ss )) >= 0 ) {
                            if( !ret ) {
                                evt.events |= EPOLLOUT;
                                continue;
                            }

                            if( evt.events & EPOLLIN ) {
                                do {
                                    if( (ret = io_uring_wait_cqe_nr( &iour,&cqe,0 )) ) {
                                        cout << "failed to dequeue CQ entry: " << strerror( ret ) << endl;
                                        cont_running = false;
                                        break;
                                    }

                                    evt.events |= EPOLLOUT;
                                    if( cqe != NULL )
                                        io_uring_cqe_seen( &iour,cqe );
                                    else
                                        break;
                                } while( !ret );
                            }

                            if( evt.events & EPOLLOUT ) {
                                if( max_in_flight < MAX_IN_FLIGHT ) {
                                    lock_guard<mutex> lg( ioq_m );
                                    for( io_req_queue::iterator it = ioq.begin(); it != ioq.end(); ) {
                                        if( (sqe = io_uring_get_sqe( &iour )) != NULL ) {
                                            io_uring_prep_sendmsg( sqe,udpskt.get_skt(),&(*it).mh,0 );
                                            ret = io_uring_submit( &iour );
                                            if( ret == -EBUSY || ret == -EAGAIN ) {
                                                evt.events &= ~(EPOLLOUT);
                                                break;
                                            }
                                            else {
                                                it = ioq.erase( it );
                                                max_in_flight++;
                                            }
                                        }
                                        else {
                                            evt.events &= ~(EPOLLOUT);
                                            break;
                                        }

                                        if( max_in_flight >= MAX_IN_FLIGHT ) {
                                            evt.events &= ~(EPOLLOUT);
                                            break;
                                        }
                                    }

                                    if( ioq.empty() )
                                        evt.events &= ~(EPOLLOUT);
                                }
                                else
                                    evt.events &= ~(EPOLLOUT);

                                evt.events |= EPOLLIN;
                            }
/*
                            if( evt.events & EPOLLRDHUP ) {
                                cout << "connection reset by peer: " << strerror( errno ) << endl;
                                cont_running = false;
                            }

                            if( evt.events & EPOLLERR ) {
                                cout << "error on the socket: " << strerror( errno ) << endl;
                                cont_running = false;
                            }

                            if( evt.events & EPOLLHUP ) {
                                cout << "hang up on the socket: " << strerror( errno ) << endl;
                                cont_running = false;
                            }
*/
                        }
                        else {
                            cout << "failed to wait for IO event notification: " << strerror( errno ) << endl;
                            cont_running = false;
                        }
                    }
                }
                else
                    cout << "failed to associate socket with epoll instance: " << strerror( errno ) << endl;
            }
            catch( runtime_error& e ) {
                cout << "failed to create socket: " << e.what() << endl;
            }

            io_uring_queue_exit( &iour );
        }
        else
            cout << "failed to initialize io_uring for asynchronous IO: " << strerror( errno ) << endl;

        close( epfd );
    }
    else
        cout << "failed to create epoll instance for IO event notification: " << strerror( errno ) << endl;
}

static void test_thrd( uint64_t ior_arg )
{
    struct io_req *ior = (struct io_req *)ior_arg;
    io_req_queue lrq;
    lrq.push_back( *ior );

    queue_io( lrq );
}

void queue_io( io_req_queue& io_req_q )
{
    pthread_t tid;

    // merge new io requests into global list
    ioq_m.lock();
    ioq.splice( ioq.end(),io_req_q );
    ioq_m.unlock();

    // round-robin io servicing threads to attempt some sort of load balancing
    tl_m.lock();
    tid = tl.front().native_handle();
    //rotate( tl.begin(),++tl.begin(),tl.end() );
    tl_m.unlock();

    pthread_kill( tid,SIGUSR1 );
}

int main( int argc,char **argv )
{
    struct io_req ior = { 0 };

    ior.iov[0].iov_base = &wb[0];
    ior.iov[0].iov_len  = sizeof( wb );
    ior.mh.msg_iov      = &ior.iov[0];
    ior.mh.msg_iovlen   = 1;

    tl.emplace_back( thread( io_loop,0 ) );
    thread tt1( test_thrd,(uint64_t)&ior );
    tt1.join();
    for( auto& t : tl )
        t.join();

    return 0;
}
