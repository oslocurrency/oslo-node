#include <oslo/boost/asio/ip/address_v6.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/lib/tomlconfig.hpp>
#include <oslo/node/websocketconfig.hpp>

oslo::websocket::config::config () :
port (network_constants.default_websocket_port),
address (boost::asio::ip::address_v6::loopback ().to_string ())
{
}

oslo::error oslo::websocket::config::serialize_toml (oslo::tomlconfig & toml) const
{
	toml.put ("enable", enabled, "Enable or disable WebSocket server.\ntype:bool");
	toml.put ("address", address, "WebSocket server bind address.\ntype:string,ip");
	toml.put ("port", port, "WebSocket server listening port.\ntype:uint16");
	return toml.get_error ();
}

oslo::error oslo::websocket::config::deserialize_toml (oslo::tomlconfig & toml)
{
	toml.get<bool> ("enable", enabled);
	boost::asio::ip::address_v6 address_l;
	toml.get_optional<boost::asio::ip::address_v6> ("address", address_l, boost::asio::ip::address_v6::loopback ());
	address = address_l.to_string ();
	toml.get<uint16_t> ("port", port);
	return toml.get_error ();
}

oslo::error oslo::websocket::config::serialize_json (oslo::jsonconfig & json) const
{
	json.put ("enable", enabled);
	json.put ("address", address);
	json.put ("port", port);
	return json.get_error ();
}

oslo::error oslo::websocket::config::deserialize_json (oslo::jsonconfig & json)
{
	json.get<bool> ("enable", enabled);
	boost::asio::ip::address_v6 address_l;
	json.get_required<boost::asio::ip::address_v6> ("address", address_l, boost::asio::ip::address_v6::loopback ());
	address = address_l.to_string ();
	json.get<uint16_t> ("port", port);
	return json.get_error ();
}
