#include <unistd.h>
#include <inttypes.h>
#include <iostream>
#include <vector>
#include <forward_list>
#include <mutex>
#include <condition_variable>
#include <thread>

#include <boost/system/system_error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;

#include "tcsvc_op.h"

static mutex m;
static condition_variable cv;
static bool done = false;

static uint64_t tcsvc_establish( tcsvc_op&,io_context& );
static void io_op_handler( boost::system::error_code,size_t,tcsvc_op& );

class tcperf_thread {
    public:
        void operator()( io_context& ioctx ) {
            vector<uint8_t> buf( 4096 );

            forward_list<vector<uint8_t> > bl;
            bl.push_front( buf );

            try {
                tcsvc_op top;

                top.ptype  = tcsvc_op::TCSVC_UDPv4;
                top.op     = tcsvc_op::TCSVC_ASYNC_WRITE;
                top.addr   = "127.0.0.1";
                top.port   = 5201;

                uint64_t skt_key = tcsvc_establish( top,ioctx );
                udp::socket *udp_skt = reinterpret_cast<udp::socket *>( skt_key );

                for( auto&& buf1 : bl )
                    udp_skt->async_send( mutable_buffer( buf1.data(),buf1.size() ),
                                         boost::bind( io_op_handler,_1,_2,top ) );

                ioctx.run();

                delete udp_skt;
            }
            catch( boost::system::system_error& e ) {
                cout << e.what() << endl;
            }
        }
};

static uint64_t tcsvc_establish( tcsvc_op& top,io_context& ioctx )
{
    udp::endpoint ep( make_address( top.addr ),top.port );
    udp::socket *udp_skt = new udp::socket( ioctx );
    udp_skt->connect( ep );

    return reinterpret_cast<uint64_t>( udp_skt );
}

static void io_op_handler( boost::system::error_code ec,size_t bytes_done,
                           tcsvc_op& tcop )
{
    lock_guard<mutex> lk( m );

    cout << "io callback with ec " << ec << endl;
    done = true;
    cv.notify_one();
}

int main()
{
    boost::asio::io_context ioctx;
    int ret = 0;

    tcperf_thread tcpt;
    thread t( tcpt,ref( ioctx ) );

    unique_lock<mutex> lk( m );
    if( !done )
        cv.wait( lk,[]{ return done; } );

    t.join();

    return ret;
}
