// Copyright (c) 2019 Tim Cullen
// See LICENSE file

#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <signal.h>

#include <iostream>
#include <iomanip>
#include <chrono>
#include <ratio>
#include <thread>
#include <mutex>
#include <functional>
#include <utility>
#include <list>
#include <unordered_map>
#include <string>

#include <boost/system/system_error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/range/irange.hpp>
#include <boost/program_options.hpp>

using namespace std;
using namespace std::chrono;
using namespace boost::asio::ip;
using namespace boost::program_options;

// constants
static const int IO_GEN_NUM_ELEMS      = 16;
static const int IO_GEN_ELEM_SIZE      = 4096;
static const int IO_GEN_MAX_IN_FLIGHT  = 1;
static const int IO_GEN_RETRY_COUNT    = 4;
static const uint64_t ONE_GB           = (1024*1024*1024);
static const uint64_t ONE_GIGABIT      = ((1024*1024*1024) * (uint64_t)8);
static const uint32_t ONE_MB           = (1024*1024);
static const uint32_t ONE_MEGABIT      = ((1024*1024) * 8);
static const uint32_t ONE_KB           = 1024;
static const uint32_t ONE_KILOBIT      = 1024 * 8;

// types
typedef enum {
    PROTO_UNSUPPORTED,
    PROTO_TCP,
    PROTO_TCPv6,
    PROTO_UDP,
    PROTO_UDPv6,
    PROTO_SCTP,
    PROTO_RDP,
    PROTO_MULTICAST,
    PROTO_RAW
} proto_type_t;

struct thrd_cfg {
    // which protocol is configured for testing
    proto_type_t ptype;

    // processor core to run this thread on
    int core_id;

    // number and size of data buffers
    int n_elems;
    int elem_size;

    // max io requests outstanding at any given time
    int max_in_flight;

    // target ip address and port for clients
    string destip;
    int port;

    thrd_cfg()
        : ptype( PROTO_UNSUPPORTED ),core_id( -1 ),port( 0 ),
          n_elems( IO_GEN_NUM_ELEMS ),elem_size( IO_GEN_ELEM_SIZE ),
          max_in_flight( 0 ) {}
};
typedef struct thrd_cfg thrd_cfg_t;

struct io_stats {
    uint64_t total_bytes;
    uint32_t n_reqs;
    uint32_t n_retried_reqs;

    // start time and end time
    steady_clock::time_point start_time;
    steady_clock::time_point end_time;
};
typedef struct io_stats iostats_t;

class io_generator {
    // pre-allocated buffers for data transmit/receive
    uint8_t *m_buffer;
    list<boost::asio::mutable_buffer> m_bfl;

    // where all the magic happens
    boost::asio::io_context m_ioctx;

    // opens the door for tcp
    tcp::acceptor m_tsa;

    // config info
    thrd_cfg_t m_cfg;

    // stats info
    iostats_t stats;

    // socket(s) on which io will be generated
    list<tcp::socket *> tsl;

    union {
        udp::socket *udp_skt;
    } u;

public:
    // allocate a data buffer
    boost::asio::mutable_buffer& alloc_buffer( void )
        { boost::asio::mutable_buffer& buf = m_bfl.front();
            m_bfl.pop_front(); return buf; }

    // free a data buffer
    void free_buffer( boost::asio::mutable_buffer& buf )
        { m_bfl.push_back( buf ); }

    // get this party started
    void start_io( void );

    // wait until the party is over
    void wait( void );

    // get the current stats
    iostats_t get_stats( void ) { return stats; }

    // stats accounting
    void add_total_bytes( uint64_t bytes ) { stats.total_bytes += bytes; }
    void add_inc_nreqs( void ) { stats.n_reqs++; }
    void add_inc_retried_reqs( void ) { stats.n_retried_reqs++; }
    void set_start_time( void ) { stats.start_time = steady_clock::now(); }

    // config accessors
    int get_elem_size( void )     { return m_cfg.elem_size; }
    int get_max_in_flight( void ) { return m_cfg.max_in_flight; }
    int get_core_id( void )       { return m_cfg.core_id; }

    // socket accessors
    void set_tcp_socket( tcp::socket *ts ) { tsl.push_back( ts ); }

    void set_udp_socket( udp::socket *us ) { u.udp_skt = us; }
    udp::socket *get_udp_socket( void )    { return u.udp_skt; }

    // ctor / dtor
    io_generator( thrd_cfg_t );
    virtual ~io_generator();
};

struct op_context {
    proto_type_t ptype;

    uint32_t total_io_len;
    uint32_t cur_off;

    // number of times to retry performing the same io request
    int retries;

    union {
        tcp::socket *tcp_skt;
        udp::socket *udp_skt;
    } u;

    // data buffer this io requests is using
    boost::asio::mutable_buffer buf;

    // reference to io_generator object this io is being performed within
    io_generator *io_gen;

    op_context()
        : ptype( PROTO_UNSUPPORTED ),total_io_len( 0 ),cur_off( 0 ),
            retries( IO_GEN_RETRY_COUNT ),io_gen( NULL ) {}
};
typedef struct op_context opctx_t;

typedef unordered_map<string,function<void ( thrd_cfg_t& )> > proto_map_t;
typedef duration<double,ratio<1,1000> > ms_duration;

// globals
static bool continue_running = true;
static list<iostats_t> stats_list;
static mutex g_stats_mutex;

// prototypes
static pid_t tcperf_gettid( void );

static void read_handler( boost::system::error_code ec,size_t bytes_done,
                          opctx_t& opctx )
{
    static bool first_time = true;
    bool fake_success = false;

    if( first_time ) {
        opctx.io_gen->set_start_time();
        first_time = false;
    }

    if( !ec ) {
retry:
        if( continue_running ) {
            if( !fake_success )
                opctx.retries = IO_GEN_RETRY_COUNT;

            switch( opctx.ptype ) {
                case PROTO_TCP:
                    boost::asio::async_read( *opctx.u.tcp_skt,opctx.buf,
                        boost::bind( read_handler,_1,_2,opctx ) );
                    break;
                case PROTO_UDP:
                    opctx.u.udp_skt->async_receive( opctx.buf,
                        boost::bind( read_handler,_1,_2,opctx ) );
                    break;
                default:
                    throw runtime_error( "operation context is corrupt" );
                    break;
            }

            // accounting
            // Shakespeare said "First thing we do is shoot all the accountants"
            opctx.io_gen->add_total_bytes( bytes_done );
            opctx.io_gen->add_inc_nreqs();
        }
    }
    else {
        if( boost::system::errc::connection_refused == ec ) {
            if( --opctx.retries > 0 ) {
                fake_success = true;
                goto retry;
            }
            else
                throw runtime_error( "operation retry count exceeded" );
        }
        else
            throw boost::system::system_error( ec );
    }
}

static void tcp_accept_handler( boost::system::error_code ec,tcp::socket tcp_skt,
                                io_generator *io_gen )
{
    opctx_t opctx;

    if( ec )
        throw boost::system::system_error( ec );

    opctx.u.tcp_skt = new tcp::socket( move( tcp_skt ) );
    io_gen->set_tcp_socket( opctx.u.tcp_skt );

    opctx.ptype        = PROTO_TCP;
    opctx.total_io_len = io_gen->get_elem_size();
    opctx.cur_off      = 0;
    opctx.buf          = io_gen->alloc_buffer();
    opctx.io_gen       = io_gen;

    for( int i : boost::irange( 0,io_gen->get_max_in_flight() ) )
        boost::asio::async_read( *opctx.u.tcp_skt,
            opctx.buf,boost::bind( read_handler,_1,_2,opctx ) );
}

void io_generator::start_io( void )
{
    opctx_t opctx;

    opctx.ptype        = m_cfg.ptype;
    opctx.total_io_len = m_cfg.elem_size;
    opctx.cur_off      = 0;
    opctx.buf          = alloc_buffer();
    opctx.io_gen       = this;

    switch( m_cfg.ptype ) {
        case PROTO_TCP:
            {
                tcp::endpoint ep( make_address_v4( m_cfg.destip ),m_cfg.port + m_cfg.core_id );
                boost::asio::socket_base::reuse_address opt( true );
                m_tsa.open( ep.protocol() );
                m_tsa.set_option( opt );
                m_tsa.bind( ep );
                m_tsa.listen();
                m_tsa.async_accept( boost::bind( tcp_accept_handler,_1,_2,this ) );
            }
            break;
        case PROTO_UDP:
            opctx.u.udp_skt = u.udp_skt;
            for( int i : boost::irange( 0,m_cfg.max_in_flight ) )
                opctx.u.udp_skt->async_receive( opctx.buf,
                    boost::bind( read_handler,_1,_2,opctx ) );
            break;
        default:
            throw logic_error( "Error: Thread config structure is corrupt" );
            break;
    }
}

void io_generator::wait( void )
{
    while( continue_running ) {
        m_ioctx.run_for( seconds( 1 ) );
        m_ioctx.restart();
    }

    //stats.end_time = steady_clock::now();
    m_ioctx.stop();
}

io_generator::io_generator( thrd_cfg_t cfg )
    : m_buffer( NULL ),m_tsa( m_ioctx )
{
    size_t offset = 0;

    // save our config
    m_cfg = cfg;

    // initialize stats
    memset( &stats,0,sizeof( stats ) );

    // pre-allocate io buffers
    m_buffer = new uint8_t[cfg.n_elems * cfg.elem_size];
    for( int i : boost::irange( 0,cfg.n_elems ) ) {
        m_bfl.emplace_front( m_buffer + offset,cfg.elem_size );
        offset += cfg.elem_size;
    }

    // examine cfg.proto_type and create the correct socket
    switch( cfg.ptype ) {
        case PROTO_TCP:
            break;
        case PROTO_UDP:
            {
                udp::endpoint ep( make_address_v4( cfg.destip ),cfg.port + cfg.core_id );
                u.udp_skt = new udp::socket( m_ioctx,ep );
            }
            break;
        case PROTO_UNSUPPORTED:
        default:
            throw logic_error( "Error: Thread config structure is corrupt" );
            break;
    }
}

io_generator::~io_generator()
{
    stats.end_time = steady_clock::now();

    switch( m_cfg.ptype ) {
        case PROTO_TCP:
            for( auto&& ts : tsl ) { ts->shutdown( tcp::socket::shutdown_both );
                                     delete ts; }
            break;
        case PROTO_UDP:
            u.udp_skt->shutdown( udp::socket::shutdown_both );
            delete u.udp_skt;
            break;
        default:
            break;
    }

    delete [] m_buffer;

    lock_guard<mutex> stats_lock( g_stats_mutex );
    stats_list.push_back( get_stats() );
}

// believe it or not glibc provides no wrapper for the 'get thread id' system
// call provided by the kernel so we provide our own
static pid_t tcperf_gettid( void )
{
    return syscall( SYS_gettid );
}

static void start_io_generator( thrd_cfg_t cfg )
{
    pthread_t t;
    cpu_set_t cpuset;

    // pin each thread to a separate core
    t = pthread_self();
    CPU_ZERO( &cpuset );
    CPU_SET( cfg.core_id,&cpuset );

    pthread_setaffinity_np( t,sizeof( cpu_set_t ),&cpuset );

    // flood the network with traffic
    try {
        io_generator io_gen( cfg );
        io_gen.start_io();
        io_gen.wait();
    }
    catch( boost::system::system_error& e ) {
        cout << e.what() << " - Thread Terminating" << endl;
    }
    catch( runtime_error& rte ) {
        cout << rte.what() << " - Thread Terminating" << endl;
    }
    catch( bad_alloc& ba ) {
        cout << ba.what() << " - Thread Terminating" << endl;
    }
    catch( logic_error& le ) {
        cout << le.what() << " - Thread Terminating" << endl;
    }
}

static void summarize_system( thrd_cfg_t& cfg )
{
    // app version number
    // running linux kernel version
    // running linux distro
    // processor type/info
    // number of cores available
    // number of threads
    // number of threads per core being used
    // number of network interfaces available
    // info on network interface being used
    // total size of data buffer
    // size of elements in buffer
    // number of io reqs in flight
    // protocol in use
}

static void summarize_stats( void )
{
    iostats_t cs = { 0 };
    ms_duration long_dur,cur_dur;
    double secs;
    double bps;

    for( auto&& sle : stats_list ) {
        cs.total_bytes    += sle.total_bytes;
        cs.n_reqs         += sle.n_reqs;
        cs.n_retried_reqs += sle.n_retried_reqs;

        cur_dur = duration_cast<ms_duration>(sle.end_time - sle.start_time);
        if( cur_dur > long_dur )
            long_dur = cur_dur;
    }

    secs = long_dur.count() / 1000;
    bps  = (cs.total_bytes * 8) / secs;

    cout << endl << "Test Ran For\t\t: " << secs << " seconds" << endl;
    cout << "Data Transferred\t: ";
    if( cs.total_bytes > ONE_GB )
        cout << (double)cs.total_bytes / ONE_GB << " Gigabytes" << endl;
    else if( cs.total_bytes > ONE_MB )
        cout << (double)cs.total_bytes / ONE_MB << " Megabytes" << endl;
    else if( cs.total_bytes > ONE_KB )
        cout << (double)cs.total_bytes / ONE_KB << " Kilobytes" << endl;
    else
        cout << cs.total_bytes << " Bytes" << endl;

    cout << "                \t: ";
    if( bps > ONE_GIGABIT )
        cout << fixed << setprecision( 2 ) << bps / ONE_GIGABIT <<
            " Gb/s" << endl;
    else if( bps > ONE_MEGABIT )
        cout << fixed << setprecision( 2 ) << bps / ONE_MEGABIT <<
            " Mb/s" << endl;
    else if( bps > ONE_KILOBIT )
        cout << fixed << setprecision( 2 ) << bps / ONE_KILOBIT <<
            " Kb/s" << endl;
    else
        cout << bps << " Bits/s" << endl;

    cout << "Number of Requests\t: " << cs.n_reqs << endl;
    //    "Num Retried Requests\t: " << cs.n_retried_reqs << endl << endl;
}

static void tcp_test( thrd_cfg_t& cfg )
{
    cfg.ptype = PROTO_TCP;
}

static void udp_test( thrd_cfg_t& cfg )
{
    cfg.ptype = PROTO_UDP;
}

static void sig_handler( int signum,siginfo_t *si,void *uctxt )
{
    continue_running = false;
}

int main( int argc,char **argv )
{
    struct sigaction sa;
    list<thread> tl;
    thrd_cfg_t cfg;
    function<void( thrd_cfg_t )> func = []( thrd_cfg_t cfg )
        { start_io_generator( cfg ); };
    proto_map_t proto_map;
    int n_threads = 0;
    int ret       = 0;

    sa.sa_sigaction = sig_handler;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset( &sa.sa_mask );
    sigaction( SIGINT,&sa,NULL );
    sigaction( SIGTERM,&sa,NULL );

    try {
        // parse command line parameters
        options_description desc( "Usage" );
        variables_map vm;

        desc.add_options()
            ( "help","This Usage Message" )
            ( "protocol",value<string>(),"Specify Protocol to Use (Required)" )
            ( "listenip",value<string>(),"Specify IP Address to Listen On(Required)" )
            ( "port",value<int>(),"Specify Target Port Number (Required)" )
            ( "nthreads",value<int>(),"Number of OS Level Threads to Start" )
            ( "max_in_flight",value<int>(),"Max IO Requests per Thread" );
        store( parse_command_line( argc,argv,desc ),vm );
        notify( vm );

        proto_map.insert( pair<string,function<void ( thrd_cfg_t& )> >
                ( "tcp",tcp_test ) );

        proto_map.insert( pair<string,function<void ( thrd_cfg_t& )> >
                ( "udp",udp_test ) );

        if( vm.count( "help" ) )
            cout << desc << endl;
        else {
            // if any of this group of parameters is not specified an exception
            // is thrown by variables_map
            string proto = vm["protocol"].as<string>();
            cfg.destip   = vm["listenip"].as<string>();
            cfg.port     = vm["port"].as<int>();

            n_threads = thread::hardware_concurrency();
            if( vm.count( "nthreads" ) )
                n_threads = vm["nthreads"].as<int>();

            cfg.max_in_flight = IO_GEN_MAX_IN_FLIGHT;
            if( vm.count( "max_in_flight" ) )
                cfg.max_in_flight = vm["max_in_flight"].as<int>();

            proto_map_t::iterator it;
            if( (it = proto_map.find( proto )) != proto_map.end() ) {
                function<void ( thrd_cfg_t& )> func = it->second;
                func( cfg );
            }
            else
                throw error( proto + " is not a supported protocol" );

            summarize_system( cfg );

            // create set of io_generator threads
            for( int i : boost::irange( 0,n_threads ) ) {
                cfg.core_id = i;
                tl.emplace( tl.begin(),thread( func,cfg ) );
            }

            // wait for all io_generator threads to terminate
            for( auto&& t : tl ) t.join();

            summarize_stats();
        }
    }
    catch( boost::bad_any_cast& bac ) {
        cout << "ERROR: bad_any_cast, did you forget to specify required "
                "parameters?" << endl;
        ret = -1;
    }
    // exceptions from program_options library
    catch( error& e ) {
        cout << e.what() << endl;
        ret = -1;
    }

    return ret;
}
