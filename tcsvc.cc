
#include <unistd.h>
#include <sys/types.h>

#include <thread>
#include <string>
//#include <chrono>
#include <ratio>

#include <boost/system/system_error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/range/irange.hpp>

using namespace std;
//using namespace std::chrono;
using namespace boost::asio;
using namespace boost::asio::ip;

#include "tcsvc_op.h"

// globals
static boost::asio::io_context io_ctx;

static void io_op_handler( boost::system::error_code ec,size_t bytes_done,
                           tcsvc_op& tcop )
{
    tcop.total_bytes_transferred += bytes_done;
    tcop();
}

void tcsvc_submit_io( tcsvc_op& tcop )
{
    switch( tcop.op ) {
        case tcop.TCSVC_ASYNC_READ:
            switch( tcop.ptype ) {
                case tcop.TCSVC_TCPv4:
                case tcop.TCSVC_TCPv6:
                    {
                        tcp::socket *tcp_skt = reinterpret_cast<tcp::socket *>( tcop.skt_key );
                        for_each( tcop.bl.begin(),tcop.bl.end(),[tcp_skt,tcop]( tcsvc_op::tcsvc_buffer& buf ) {
                            boost::asio::async_read( *tcp_skt,mutable_buffer( buf.data(),buf.size() ),
                                                     boost::bind( io_op_handler,_1,_2,tcop ) );
                        } );
                    }
                    break;
                case tcop.TCSVC_UDPv4:
                case tcop.TCSVC_UDPv6:
                    {
                        udp::socket *udp_skt = reinterpret_cast<udp::socket *>( tcop.skt_key );
                        for_each( tcop.bl.begin(),tcop.bl.end(),[udp_skt,tcop]( tcsvc_op::tcsvc_buffer& buf ) {
                            udp_skt->async_receive( mutable_buffer( buf.data(),buf.size() ),
                                                    boost::bind( io_op_handler,_1,_2,tcop ) );
                        } );
                    }
                    break;
            }
            break;
        case tcop.TCSVC_ASYNC_WRITE:
            switch( tcop.ptype ) {
                case tcop.TCSVC_TCPv4:
                case tcop.TCSVC_TCPv6:
                    {
                        tcp::socket *tcp_skt = reinterpret_cast<tcp::socket *>( tcop.skt_key );
                        for_each( tcop.bl.begin(),tcop.bl.end(),[tcp_skt,tcop]( tcsvc_op::tcsvc_buffer& buf ) {
                            boost::asio::async_write( *tcp_skt,mutable_buffer( buf.data(),buf.size() ),
                                                      boost::bind( io_op_handler,_1,_2,tcop ) );
                        } );
                    }
                    break;
                case tcop.TCSVC_UDPv4:
                case tcop.TCSVC_UDPv6:
                    {
                        udp::socket *udp_skt = reinterpret_cast<udp::socket *>( tcop.skt_key );
                        for_each( tcop.bl.begin(),tcop.bl.end(),[udp_skt,tcop]( tcsvc_op::tcsvc_buffer& buf ) {
                            udp_skt->async_send( mutable_buffer( buf.data(),buf.size() ),
                                                 boost::bind( io_op_handler,_1,_2,tcop ) );
                        } );
                    }
                    break;
            }
            break;
    }
}

uint64_t tcsvc_establish( const string addr,const uint16_t port )
{
    tcp::socket *tcp_skt = new tcp::socket( io_ctx );
    tcp::endpoint ep( make_address( addr ),port );
    tcp_skt->connect( ep );

    return reinterpret_cast<uint64_t>( tcp_skt );
}

void tcsvc_async_establish( tcsvc_op& tcop )
{
}

static void tcp_accept_handler( boost::system::error_code ec,tcp::socket tcp_skt,
                                tcsvc_op& tcop )
{
    tcp::socket *ts = new tcp::socket( move( tcp_skt ) );
    tcop.skt_key = reinterpret_cast<uint64_t>( ts );

    // inform application a new connection has been established
    tcop();
}

void tcsvc_async_accept( tcsvc_op& tcop )
{
    tcp::endpoint ep;
    boost::asio::socket_base::reuse_address opt( true );

    ep.address( make_address( tcop.addr ) );
    ep.port( tcop.port );

    /*
    tcop.tsa.open( ep.protocol() );
    tcop.tsa.set_option( opt );
    tcop.tsa.bind( ep );
    tcop.tsa.listen();
    tcop.tsa.async_accept( boost::bind( tcp_accept_handler,_1,_2,tcop ) );
    */
}

static void start_io_handler( int core_id )
{
    pthread_t t;
    cpu_set_t cpuset;

    t = pthread_self();
    CPU_ZERO( &cpuset );
    CPU_SET( core_id,&cpuset );

    pthread_setaffinity_np( t,sizeof( cpu_set_t ),&cpuset );

    io_ctx.run();
}

int tcsvc_init( int nthreads )
{
    list<thread> tl;
    function<void( int )> func = []( int core_id )
        { start_io_handler( core_id ); };

    if( !nthreads )
        nthreads = thread::hardware_concurrency();

    for( int i : boost::irange( 0,nthreads ) ) 
        tl.emplace( tl.begin(),thread( func,i ) );
}

void tcsvc_term()
{
    io_ctx.stop();
    for( auto&& t : tl ) t.join();
}
