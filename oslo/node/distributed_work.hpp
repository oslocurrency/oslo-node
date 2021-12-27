#pragma once

#include <oslo/boost/asio/ip/tcp.hpp>
#include <oslo/boost/asio/strand.hpp>
#include <oslo/boost/beast/core/flat_buffer.hpp>
#include <oslo/boost/beast/http/string_body.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/timer.hpp>
#include <oslo/lib/work.hpp>
#include <oslo/node/common.hpp>

#include <boost/optional.hpp>

#include <mutex>

using request_type = boost::beast::http::request<boost::beast::http::string_body>;

namespace boost
{
namespace asio
{
	class io_context;
}
}

namespace oslo
{
class node;

struct work_request final
{
	oslo::work_version version;
	oslo::root root;
	uint64_t difficulty;
	boost::optional<oslo::account> const account;
	std::function<void(boost::optional<uint64_t>)> callback;
	std::vector<std::pair<std::string, uint16_t>> const peers;
};

/**
 * distributed_work cancels local and peer work requests when going out of scope
 */
class distributed_work final : public std::enable_shared_from_this<oslo::distributed_work>
{
	enum class work_generation_status
	{
		ongoing,
		success,
		cancelled,
		failure_local,
		failure_peers
	};

	class peer_request final
	{
	public:
		peer_request (boost::asio::io_context & io_ctx_a, oslo::tcp_endpoint const & endpoint_a) :
		endpoint (endpoint_a),
		socket (io_ctx_a)
		{
		}
		std::shared_ptr<request_type> get_prepared_json_request (std::string const &) const;
		oslo::tcp_endpoint const endpoint;
		boost::beast::flat_buffer buffer;
		boost::beast::http::response<boost::beast::http::string_body> response;
		boost::asio::ip::tcp::socket socket;
	};

public:
	distributed_work (oslo::node &, oslo::work_request const &, std::chrono::seconds const &);
	~distributed_work ();
	void start ();
	void cancel ();

private:
	void start_local ();
	/** Send a work_generate message to \p endpoint_a and handle a response */
	void do_request (oslo::tcp_endpoint const & endpoint_a);
	/** Send a work_cancel message using a new connection to \p endpoint_a */
	void do_cancel (oslo::tcp_endpoint const & endpoint_a);
	/** Called on a successful peer response, validates the reply */
	void success (std::string const &, oslo::tcp_endpoint const &);
	/** Send a work_cancel message to all remaining connections */
	void stop_once (bool const);
	void set_once (uint64_t const, std::string const & source_a = "local");
	void failure ();
	void handle_failure ();
	void add_bad_peer (oslo::tcp_endpoint const &);

	oslo::node & node;
	// Only used in destructor, as the node reference can become invalid before distributed_work objects go out of scope
	std::weak_ptr<oslo::node> node_w;
	oslo::work_request request;

	std::chrono::seconds backoff;
	boost::asio::strand<boost::asio::io_context::executor_type> strand;
	std::vector<std::pair<std::string, uint16_t>> const need_resolve;
	std::vector<std::weak_ptr<peer_request>> connections; // protected by the mutex

	work_generation_status status{ work_generation_status::ongoing };
	uint64_t work_result{ 0 };

	oslo::timer<std::chrono::milliseconds> elapsed; // logging only
	std::vector<std::string> bad_peers; // websocket
	std::string winner; // websocket

	std::mutex mutex;
	std::atomic<unsigned> resolved_extra{ 0 };
	std::atomic<unsigned> failures{ 0 };
	std::atomic<bool> finished{ false };
	std::atomic<bool> stopped{ false };
	std::atomic<bool> local_generation_started{ false };
};
}