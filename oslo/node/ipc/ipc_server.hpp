#pragma once

#include <oslo/ipc_flatbuffers_lib/generated/flatbuffers/nanoapi_generated.h>
#include <oslo/lib/errors.hpp>
#include <oslo/lib/ipc.hpp>
#include <oslo/node/ipc/ipc_access_config.hpp>
#include <oslo/node/ipc/ipc_broker.hpp>
#include <oslo/node/node_rpc_config.hpp>

#include <atomic>
#include <mutex>

namespace flatbuffers
{
class Parser;
}
namespace oslo
{
class node;
class error;
namespace ipc
{
	class access;
	/** The IPC server accepts connections on one or more configured transports */
	class ipc_server final
	{
	public:
		ipc_server (oslo::node & node, oslo::node_rpc_config const & node_rpc_config);
		~ipc_server ();
		void stop ();

		oslo::node & node;
		oslo::node_rpc_config const & node_rpc_config;

		/** Unique counter/id shared across sessions */
		std::atomic<uint64_t> id_dispenser{ 1 };
		oslo::ipc::broker & get_broker ();
		oslo::ipc::access & get_access ();
		oslo::error reload_access_config ();

	private:
		void setup_callbacks ();
		oslo::ipc::broker broker;
		oslo::ipc::access access;
		std::unique_ptr<dsock_file_remover> file_remover;
		std::vector<std::shared_ptr<oslo::ipc::transport>> transports;
	};
}
}
