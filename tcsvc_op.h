
#ifndef INCLUDED_TCSVC_OPERATION_H
#define INCLUDED_TCSVC_OPERATION_H

#include <string>
#include <vector>
#include <list>

class tcsvc_op {
public:
    typedef vector<uint8_t> tcsvc_buffer;
    typedef void (*cbfunc_t)( tcsvc_op * );
    typedef enum { TCSVC_NOOP,TCSVC_ASYNC_READ,TCSVC_ASYNC_WRITE,TCSVC_ESTABLISH,
                   TCSVC_ACCEPT } tcsvc_io_op;
    typedef enum { TCSVC_UNSUPPORTED,TCSVC_TCPv4,TCSVC_TCPv6,TCSVC_UDPv4,
                   TCSVC_UDPv6 } tcsvc_proto;

    void operator()() {
        cbfunc( this );
    }

    // operation to perform
    tcsvc_io_op op;
    tcsvc_proto ptype;

    // address of target to connect to
    // address to listen for connections on
    // MAC, IPv4, and IPv6 addresses supported
    string   addr;
    uint16_t port;

    // connection id used to reference appropriate socket
    uint64_t skt_key;

    // pointer to applications registered callback function
    cbfunc_t cbfunc;

    // total bytes transferred
    uint64_t total_bytes_transferred;

    // list of buffers to operate on
    list<tcsvc_buffer> bl;

    tcsvc_op() : op( TCSVC_NOOP ),ptype( TCSVC_UNSUPPORTED ),port( 0 ),skt_key( 0 ),
                 total_bytes_transferred( 0 ) {}
    virtual ~tcsvc_op() {}
};

#endif
