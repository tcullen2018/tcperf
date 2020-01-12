#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>

#include <iostream>
#include <vector>
#include <forward_list>
#include <mutex>
#include <condition_variable>
#include <thread>

#include <boost/system/system_error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/program_options.hpp>
#include <boost/range/irange.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using namespace boost::program_options;

#include "tcsvc_op.h"

static mutex m;
static condition_variable cv;
static bool done = false;

static uint64_t tcsvc_establish( tcsvc_op&,io_context& );
static void io_op_handler( boost::system::error_code,size_t,tcsvc_op& );

class tcperf_thread {
    string   protocol;                    // protocol used for communication (i.e. tcp,udp,sctp,etc. )
    string   destip;                      // destination ip address of server to communicate with
    int      port;                        // port number used for communication
    int      max_in_flight;               // maximum number of asynchronous io requests outstanding at any one time
    int      nthreads;                    // number of threads performing io
    int      nsecs;                       // amount of time to run test for
    int      core_id;                     // core number this paticular thread is bound to
    bool     reissue_io;                  // rather or not to continualy reissue io requests when one completes
    uint64_t skt_key;                     // address of socket object

    public:
        void operator()( io_context& ioctx ) {
            vector<uint8_t> buf( 4096 );
            pthread_t t;
            cpu_set_t cpuset;

            forward_list<vector<uint8_t> > bl;
            bl.push_front( buf );

            t = pthread_self();
            CPU_ZERO( &cpuset );
            CPU_SET( core_id,&cpuset );
            pthread_setaffinity_np( t,sizeof( cpu_set_t ),&cpuset );

            try {
                tcsvc_op top;

                top.ptype  = tcsvc_op::TCSVC_UDPv4;
                top.op     = tcsvc_op::TCSVC_ASYNC_WRITE;
                top.addr   = "127.0.0.1";
                top.port   = 5201;

                skt_key = tcsvc_establish( top,ioctx );
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

        void set_nthreads( const int nthreads ) { this->nthreads = nthreads; }
        int  get_nthreads( void )               { return nthreads; }

        void set_port( const int port ) { this->port = port; }
        int  get_port( void )           { return port; }

        void set_nsecs( const int nsecs ) { this->nsecs = nsecs; }
        int  get_nsecs( void )            { return nsecs; }

        void set_max_in_flight( const int mif ) { this->max_in_flight = mif; }
        int  get_max_in_flight( void )          { return max_in_flight; }

        void set_protocol( const string proto ) { this->protocol = proto; }
        string get_protocol( void )             { return protocol; }

        void set_destip( const string destip ) { this->destip = destip; }
        string get_destip( void )              { return destip; }

        void set_core_id( int core_id ) { this->core_id = core_id; }
        int get_core_id( void )         { return core_id; }

        void set_reissue_io( bool reissue_io ) { this->reissue_io = reissue_io; }
        bool get_reissue_io( void )            { return reissue_io; }

        void set_skt_key( uint64_t skt_key ) { this->skt_key = skt_key; }
        uint64_t get_skt_key( void )         { return skt_key; }

        tcperf_thread() : nthreads( 0 ),port( 0 ),nsecs( 0 ),max_in_flight( 1 ),
                          protocol( "not supported" ),destip( "0.0.0.0" ),reissue_io( false ),
                          skt_key( 0) {}
        virtual ~tcperf_thread() {}
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

static void sig_handler( int signum,siginfo_t *si,void *uctxt )
{
}

int main( int argc,char **argv )
{
    struct sigaction sa;
    forward_list<thread> tl;
    boost::asio::io_context ioctx;
    tcperf_thread tcpt;
    int ret = 0;

    sa.sa_sigaction = sig_handler;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset( &sa.sa_mask );
    sigaction( SIGINT,&sa,NULL );
    sigaction( SIGTERM,&sa,NULL );

    cout << "testudp version 0.1 running on:" << endl << endl;
    system( "lscpu" );

    try {
        options_description desc( "Usage" );
        variables_map vm;

        desc.add_options()
            ( "help","This Usage Message" )
            ( "protocol",value<string>(),"Specify Protocol to Use (Required)" )
            ( "destip",value<string>(),"Specify Target IP address (Required)" )
            ( "port",value<int>(),"Specify Target Port Number (Required)" )
            ( "nthreads",value<int>(),"Number of OS Level Threads to Start" )
            ( "max_in_flight",value<int>(),"Max IO Requests per Thread" )
            ( "time",value<int>(),"Time in Seconds for Test Run" );
        store( parse_command_line( argc,argv,desc ),vm );
        notify( vm );

        if( vm.count( "help" ) )
            cout << desc << endl;
        else {
            tcpt.set_protocol( vm["protocol"].as<string>() );
            tcpt.set_destip( vm["destip"].as<string>() );
            tcpt.set_port( vm["port"].as<int>() );

            tcpt.set_nthreads( thread::hardware_concurrency() );
            if( vm.count( "nthreads" ) )
                tcpt.set_nthreads( vm["nthreads"].as<int>() );

            if( vm.count( "max_in_flight" ) )
                tcpt.set_max_in_flight( vm["max_in_flight"].as<int>() );

            if( vm.count( "time" ) )
                tcpt.set_nsecs( vm["time"].as<int>() );

            for( int i : boost::irange( 0,tcpt.get_nthreads() ) ) {
                tcpt.set_core_id( i );
                tl.push_front( thread( tcpt,ref( ioctx ) ) );
            }

            unique_lock<mutex> lk( m );
            if( !done )
                cv.wait( lk,[]{ return done; } );

            for( auto&& t : tl ) t.join();
        }
    }
    catch( boost::bad_any_cast& bac ) {
        cout << "ERROR: bad_any_cast, did you forget to specify required "
                "parameters?" << endl;
        ret = -1;
    }
    // exceptions from program_options library
    catch( boost::program_options::error& e ) {
        cout << e.what() << endl;
        ret = -1;
    }

    return ret;
}
