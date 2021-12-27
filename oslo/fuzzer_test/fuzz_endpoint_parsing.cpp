#include <oslo/node/common.hpp>

/** Fuzz endpoint parsing */
void fuzz_endpoint_parsing (const uint8_t * Data, size_t Size)
{
	auto data (std::string (reinterpret_cast<char *> (const_cast<uint8_t *> (Data)), Size));
	oslo::endpoint endpoint;
	oslo::parse_endpoint (data, endpoint);
	oslo::tcp_endpoint tcp_endpoint;
	oslo::parse_tcp_endpoint (data, tcp_endpoint);
}

/** Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput (const uint8_t * Data, size_t Size)
{
	fuzz_endpoint_parsing (Data, Size);
	return 0;
}
