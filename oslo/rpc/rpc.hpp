#pragma once

#include <oslo/boost/asio/ip/tcp.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/rpc_handler_interface.hpp>
#include <oslo/lib/rpcconfig.hpp>

namespace boost
{
namespace asio
{
	class io_context;
}
}

namespace oslo
{
class rpc_handler_interface;

class rpc
{
public:
	rpc (boost::asio::io_context & io_ctx_a, oslo::rpc_config const & config_a, oslo::rpc_handler_interface & rpc_handler_interface_a);
	virtual ~rpc ();
	void start ();
	virtual void accept ();
	void stop ();

	oslo::rpc_config config;
	boost::asio::ip::tcp::acceptor acceptor;
	oslo::logger_mt logger;
	boost::asio::io_context & io_ctx;
	oslo::rpc_handler_interface & rpc_handler_interface;
	bool stopped{ false };
};

/** Returns the correct RPC implementation based on TLS configuration */
std::unique_ptr<oslo::rpc> get_rpc (boost::asio::io_context & io_ctx_a, oslo::rpc_config const & config_a, oslo::rpc_handler_interface & rpc_handler_interface_a);
}
