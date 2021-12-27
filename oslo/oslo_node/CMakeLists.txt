add_executable (oslo_node
	daemon.cpp
	daemon.hpp
	entry.cpp)

target_link_libraries (oslo_node
	node
	rpc
	secure
	argon2
	Boost::boost
	${PLATFORM_LIBS}
)

if (OSLO_STACKTRACE_BACKTRACE)
	target_link_libraries(oslo_node backtrace)
endif ()

target_compile_definitions(oslo_node
	PRIVATE
		-DTAG_VERSION_STRING=${TAG_VERSION_STRING}
		-DGIT_COMMIT_HASH=${GIT_COMMIT_HASH}
		-DBOOST_PROCESS_SUPPORTED=${BOOST_PROCESS_SUPPORTED})

set_target_properties (oslo_node
	PROPERTIES
		COMPILE_FLAGS
			"-DQT_NO_KEYWORDS -DBOOST_ASIO_HAS_STD_ARRAY=1")

add_custom_command(TARGET oslo_node
				   POST_BUILD
				   COMMAND oslo_node --generate_config node > ${PROJECT_BINARY_DIR}/config-node.toml.sample
				   COMMAND oslo_node --generate_config rpc > ${PROJECT_BINARY_DIR}/config-rpc.toml.sample)

if ((OSLO_GUI OR RAIBLOCKS_GUI) AND NOT APPLE)
	if (WIN32)
		install(TARGETS oslo_node
				RUNTIME DESTINATION .
		)
	else ()
		install(TARGETS oslo_node
				RUNTIME DESTINATION ./bin)
	endif()
endif()
