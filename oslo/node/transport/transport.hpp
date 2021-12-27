#pragma once

#include <oslo/lib/locks.hpp>
#include <oslo/lib/rate_limiting.hpp>
#include <oslo/lib/stats.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/socket.hpp>

namespace oslo
{
class bandwidth_limiter final
{
public:
	// initialize with limit 0 = unbounded
	bandwidth_limiter (const double, const size_t);
	bool should_drop (const size_t &);

private:
	oslo::rate::token_bucket bucket;
};

namespace transport
{
	class message;
	oslo::endpoint map_endpoint_to_v6 (oslo::endpoint const &);
	oslo::endpoint map_tcp_to_endpoint (oslo::tcp_endpoint const &);
	oslo::tcp_endpoint map_endpoint_to_tcp (oslo::endpoint const &);
	// Unassigned, reserved, self
	bool reserved_address (oslo::endpoint const &, bool = false);
	static std::chrono::seconds constexpr syn_cookie_cutoff = std::chrono::seconds (5);
	enum class transport_type : uint8_t
	{
		undefined = 0,
		udp = 1,
		tcp = 2
	};
	class channel
	{
	public:
		channel (oslo::node &);
		virtual ~channel () = default;
		virtual size_t hash_code () const = 0;
		virtual bool operator== (oslo::transport::channel const &) const = 0;
		void send (oslo::message const &, std::function<void(boost::system::error_code const &, size_t)> const & = nullptr, oslo::buffer_drop_policy = oslo::buffer_drop_policy::limiter);
		virtual void send_buffer (oslo::shared_const_buffer const &, oslo::stat::detail, std::function<void(boost::system::error_code const &, size_t)> const & = nullptr, oslo::buffer_drop_policy = oslo::buffer_drop_policy::limiter) = 0;
		virtual std::function<void(boost::system::error_code const &, size_t)> callback (oslo::stat::detail, std::function<void(boost::system::error_code const &, size_t)> const & = nullptr) const = 0;
		virtual std::string to_string () const = 0;
		virtual oslo::endpoint get_endpoint () const = 0;
		virtual oslo::tcp_endpoint get_tcp_endpoint () const = 0;
		virtual oslo::transport::transport_type get_type () const = 0;

		std::chrono::steady_clock::time_point get_last_bootstrap_attempt () const
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			return last_bootstrap_attempt;
		}

		void set_last_bootstrap_attempt (std::chrono::steady_clock::time_point const time_a)
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			last_bootstrap_attempt = time_a;
		}

		std::chrono::steady_clock::time_point get_last_packet_received () const
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			return last_packet_received;
		}

		void set_last_packet_received (std::chrono::steady_clock::time_point const time_a)
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			last_packet_received = time_a;
		}

		std::chrono::steady_clock::time_point get_last_packet_sent () const
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			return last_packet_sent;
		}

		void set_last_packet_sent (std::chrono::steady_clock::time_point const time_a)
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			last_packet_sent = time_a;
		}

		boost::optional<oslo::account> get_node_id_optional () const
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			return node_id;
		}

		oslo::account get_node_id () const
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			if (node_id.is_initialized ())
			{
				return node_id.get ();
			}
			else
			{
				return 0;
			}
		}

		void set_node_id (oslo::account node_id_a)
		{
			oslo::lock_guard<std::mutex> lk (channel_mutex);
			node_id = node_id_a;
		}

		uint8_t get_network_version () const
		{
			return network_version;
		}

		void set_network_version (uint8_t network_version_a)
		{
			network_version = network_version_a;
		}

		mutable std::mutex channel_mutex;

	private:
		std::chrono::steady_clock::time_point last_bootstrap_attempt{ std::chrono::steady_clock::time_point () };
		std::chrono::steady_clock::time_point last_packet_received{ std::chrono::steady_clock::time_point () };
		std::chrono::steady_clock::time_point last_packet_sent{ std::chrono::steady_clock::time_point () };
		boost::optional<oslo::account> node_id{ boost::none };
		std::atomic<uint8_t> network_version{ 0 };

	protected:
		oslo::node & node;
	};
} // namespace transport
} // namespace oslo

namespace std
{
template <>
struct hash<::oslo::transport::channel>
{
	size_t operator() (::oslo::transport::channel const & channel_a) const
	{
		return channel_a.hash_code ();
	}
};
template <>
struct equal_to<std::reference_wrapper<::oslo::transport::channel const>>
{
	bool operator() (std::reference_wrapper<::oslo::transport::channel const> const & lhs, std::reference_wrapper<::oslo::transport::channel const> const & rhs) const
	{
		return lhs.get () == rhs.get ();
	}
};
}

namespace boost
{
template <>
struct hash<::oslo::transport::channel>
{
	size_t operator() (::oslo::transport::channel const & channel_a) const
	{
		std::hash<::oslo::transport::channel> hash;
		return hash (channel_a);
	}
};
template <>
struct hash<std::reference_wrapper<::oslo::transport::channel const>>
{
	size_t operator() (std::reference_wrapper<::oslo::transport::channel const> const & channel_a) const
	{
		std::hash<::oslo::transport::channel> hash;
		return hash (channel_a.get ());
	}
};
}
