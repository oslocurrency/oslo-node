#include <oslo/ipc_flatbuffers_lib/generated/flatbuffers/nanoapi_generated.h>
#include <oslo/lib/errors.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/node/ipc/action_handler.hpp>
#include <oslo/node/ipc/ipc_server.hpp>
#include <oslo/node/node.hpp>

#include <iostream>

namespace
{
oslo::account parse_account (std::string const & account, bool & out_is_deprecated_format)
{
	oslo::account result (0);
	if (account.empty ())
	{
		throw oslo::error (oslo::error_common::bad_account_number);
	}
	if (result.decode_account (account))
	{
		throw oslo::error (oslo::error_common::bad_account_number);
	}
	else if (account[3] == '-' || account[4] == '-')
	{
		out_is_deprecated_format = true;
	}

	return result;
}
/** Returns the message as a Flatbuffers ObjectAPI type, managed by a unique_ptr */
template <typename T>
auto get_message (osloapi::Envelope const & envelope)
{
	auto raw (envelope.message_as<T> ()->UnPack ());
	return std::unique_ptr<typename T::NativeTableType> (raw);
}
}

/**
 * Mapping from message type to handler function.
 * @note This must be updated whenever a new message type is added to the Flatbuffers IDL.
 */
auto oslo::ipc::action_handler::handler_map () -> std::unordered_map<osloapi::Message, std::function<void(oslo::ipc::action_handler *, osloapi::Envelope const &)>, oslo::ipc::enum_hash>
{
	static std::unordered_map<osloapi::Message, std::function<void(oslo::ipc::action_handler *, osloapi::Envelope const &)>, oslo::ipc::enum_hash> handlers;
	if (handlers.empty ())
	{
		handlers.emplace (osloapi::Message::Message_IsAlive, &oslo::ipc::action_handler::on_is_alive);
		handlers.emplace (osloapi::Message::Message_TopicConfirmation, &oslo::ipc::action_handler::on_topic_confirmation);
		handlers.emplace (osloapi::Message::Message_AccountWeight, &oslo::ipc::action_handler::on_account_weight);
		handlers.emplace (osloapi::Message::Message_ServiceRegister, &oslo::ipc::action_handler::on_service_register);
		handlers.emplace (osloapi::Message::Message_ServiceStop, &oslo::ipc::action_handler::on_service_stop);
		handlers.emplace (osloapi::Message::Message_TopicServiceStop, &oslo::ipc::action_handler::on_topic_service_stop);
	}
	return handlers;
}

oslo::ipc::action_handler::action_handler (oslo::node & node_a, oslo::ipc::ipc_server & server_a, std::weak_ptr<oslo::ipc::subscriber> const & subscriber_a, std::shared_ptr<flatbuffers::FlatBufferBuilder> const & builder_a) :
flatbuffer_producer (builder_a),
node (node_a),
ipc_server (server_a),
subscriber (subscriber_a)
{
}

void oslo::ipc::action_handler::on_topic_confirmation (osloapi::Envelope const & envelope_a)
{
	auto confirmationTopic (get_message<osloapi::TopicConfirmation> (envelope_a));
	ipc_server.get_broker ().subscribe (subscriber, std::move (confirmationTopic));
	osloapi::EventAckT ack;
	create_response (ack);
}

void oslo::ipc::action_handler::on_service_register (osloapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { oslo::ipc::access_permission::api_service_register, oslo::ipc::access_permission::service });
	auto query (get_message<osloapi::ServiceRegister> (envelope_a));
	ipc_server.get_broker ().service_register (query->service_name, this->subscriber);
	osloapi::SuccessT success;
	create_response (success);
}

void oslo::ipc::action_handler::on_service_stop (osloapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { oslo::ipc::access_permission::api_service_stop, oslo::ipc::access_permission::service });
	auto query (get_message<osloapi::ServiceStop> (envelope_a));
	if (query->service_name == "node")
	{
		ipc_server.node.stop ();
	}
	else
	{
		ipc_server.get_broker ().service_stop (query->service_name);
	}
	osloapi::SuccessT success;
	create_response (success);
}

void oslo::ipc::action_handler::on_topic_service_stop (osloapi::Envelope const & envelope_a)
{
	auto topic (get_message<osloapi::TopicServiceStop> (envelope_a));
	ipc_server.get_broker ().subscribe (subscriber, std::move (topic));
	osloapi::EventAckT ack;
	create_response (ack);
}

void oslo::ipc::action_handler::on_account_weight (osloapi::Envelope const & envelope_a)
{
	require_oneof (envelope_a, { oslo::ipc::access_permission::api_account_weight, oslo::ipc::access_permission::account_query });
	bool is_deprecated_format{ false };
	auto query (get_message<osloapi::AccountWeight> (envelope_a));
	auto balance (node.weight (parse_account (query->account, is_deprecated_format)));

	osloapi::AccountWeightResponseT response;
	response.voting_weight = balance.str ();
	create_response (response);
}

void oslo::ipc::action_handler::on_is_alive (osloapi::Envelope const & envelope)
{
	osloapi::IsAliveT alive;
	create_response (alive);
}

bool oslo::ipc::action_handler::has_access (osloapi::Envelope const & envelope_a, oslo::ipc::access_permission permission_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access (credentials, permission_a);
}

bool oslo::ipc::action_handler::has_access_to_all (osloapi::Envelope const & envelope_a, std::initializer_list<oslo::ipc::access_permission> permissions_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access_to_all (credentials, permissions_a);
}

bool oslo::ipc::action_handler::has_access_to_oneof (osloapi::Envelope const & envelope_a, std::initializer_list<oslo::ipc::access_permission> permissions_a) const noexcept
{
	// If credentials are missing in the envelope, the default user is used
	std::string credentials;
	if (envelope_a.credentials () != nullptr)
	{
		credentials = envelope_a.credentials ()->str ();
	}

	return ipc_server.get_access ().has_access_to_oneof (credentials, permissions_a);
}

void oslo::ipc::action_handler::require (osloapi::Envelope const & envelope_a, oslo::ipc::access_permission permission_a) const
{
	if (!has_access (envelope_a, permission_a))
	{
		throw oslo::error (oslo::error_common::access_denied);
	}
}

void oslo::ipc::action_handler::require_all (osloapi::Envelope const & envelope_a, std::initializer_list<oslo::ipc::access_permission> permissions_a) const
{
	if (!has_access_to_all (envelope_a, permissions_a))
	{
		throw oslo::error (oslo::error_common::access_denied);
	}
}

void oslo::ipc::action_handler::require_oneof (osloapi::Envelope const & envelope_a, std::initializer_list<oslo::ipc::access_permission> permissions_a) const
{
	if (!has_access_to_oneof (envelope_a, permissions_a))
	{
		throw oslo::error (oslo::error_common::access_denied);
	}
}
