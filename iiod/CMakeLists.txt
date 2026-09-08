cmake_minimum_required(VERSION 3.10)
project(iiod C)

include(FindBISON)
include(FindFLEX)

flex_target(lexer
	${CMAKE_CURRENT_SOURCE_DIR}/lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.c)
bison_target(parser
	${CMAKE_CURRENT_SOURCE_DIR}/parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.c)
add_flex_bison_dependency(lexer parser)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES ${PTHREAD_LIBRARIES})
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
set(TMP_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "")
check_symbol_exists(pthread_setname_np "pthread.h" HAS_PTHREAD_SETNAME_NP)
set(CMAKE_C_FLAGS "${TMP_FLAGS}")
set(CMAKE_REQUIRED_LIBRARIES)
set(CMAKE_REQUIRED_DEFINITIONS)

if (CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_COMPILER_IS_GNUCC)
	# flex sometimes generates code which generate sign comparison errors
	set_source_files_properties(${FLEX_lexer_OUTPUTS} PROPERTIES COMPILE_FLAGS "-Wno-sign-compare")
	set_source_files_properties(${BISON_parser_OUTPUTS} PROPERTIES COMPILE_FLAGS "-Wno-sign-compare")
endif ()

set(IIOD_BUFFER_METADATA_PROVIDER "" CACHE FILEPATH
	"Optional C source implementing capture-associated iiOD buffer metadata")
set(IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES "" CACHE STRING
	"Additional C sources used by the optional iiOD metadata provider")
set(IIOD_BUFFER_METADATA_INCLUDE_DIRS "" CACHE STRING
	"Include directories for the optional iiOD metadata provider")
set(IIOD_BUFFER_METADATA_LIBRARIES "" CACHE STRING
	"Libraries for the optional iiOD metadata provider")
set(IIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER "" CACHE FILEPATH
	"Platform source providing atomic persistent-hop plan/device evidence")
set(IIOD_HAS_BUFFER_PERSISTENT_HOP OFF)
if (IIOD_BUFFER_METADATA_PROVIDER)
	if (IIOD_BUFFER_METADATA_PROVIDER MATCHES "spf-buffer-metadata\\.c$")
		set(SPF_TEMPERATURE_CACHE_SOURCE
			${CMAKE_CURRENT_SOURCE_DIR}/spf-temperature-cache.c)
		set(SPF_BUFFER_LAYOUT_SOURCE
			${CMAKE_CURRENT_SOURCE_DIR}/spf-buffer-layout.c)
		set(SPF_DDR_BURST_REQUEST_SOURCE
			${CMAKE_CURRENT_SOURCE_DIR}/spf-ddr-burst-request.c)
		set(SPF_DDR_RING_SOURCES
			${CMAKE_CURRENT_SOURCE_DIR}/spf-ddr-ring-request.c)
		set(SPF_SAMPLER_COVERAGE_SOURCE
			${CMAKE_CURRENT_SOURCE_DIR}/spf-sampler-coverage.c)
		list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
			${SPF_TEMPERATURE_CACHE_SOURCE} SPF_TEMPERATURE_CACHE_INDEX)
		if (SPF_TEMPERATURE_CACHE_INDEX EQUAL -1)
			list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
				${SPF_TEMPERATURE_CACHE_SOURCE})
		endif()
		list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
			${SPF_BUFFER_LAYOUT_SOURCE} SPF_BUFFER_LAYOUT_INDEX)
		if (SPF_BUFFER_LAYOUT_INDEX EQUAL -1)
			list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
				${SPF_BUFFER_LAYOUT_SOURCE})
		endif()
		list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
			${SPF_DDR_BURST_REQUEST_SOURCE} SPF_DDR_BURST_REQUEST_INDEX)
		if (SPF_DDR_BURST_REQUEST_INDEX EQUAL -1)
			list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
				${SPF_DDR_BURST_REQUEST_SOURCE})
		endif()
		foreach(SPF_DDR_RING_SOURCE ${SPF_DDR_RING_SOURCES})
			list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
				${SPF_DDR_RING_SOURCE} SPF_DDR_RING_SOURCE_INDEX)
			if (SPF_DDR_RING_SOURCE_INDEX EQUAL -1)
				list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
					${SPF_DDR_RING_SOURCE})
			endif()
		endforeach()
		list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
			${SPF_SAMPLER_COVERAGE_SOURCE} SPF_SAMPLER_COVERAGE_INDEX)
		if (SPF_SAMPLER_COVERAGE_INDEX EQUAL -1)
			list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
				${SPF_SAMPLER_COVERAGE_SOURCE})
		endif()
		if (IIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER)
			set(SPF_HOP_SOURCES
				${CMAKE_CURRENT_SOURCE_DIR}/spf-hop-protocol.c
				${CMAKE_CURRENT_SOURCE_DIR}/spf-hop-session.c
				${CMAKE_CURRENT_SOURCE_DIR}/spf-hop-scheduler.c
				${IIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER})
			foreach(SPF_HOP_SOURCE ${SPF_HOP_SOURCES})
				list(FIND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
					${SPF_HOP_SOURCE} SPF_HOP_SOURCE_INDEX)
				if (SPF_HOP_SOURCE_INDEX EQUAL -1)
					list(APPEND IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES
						${SPF_HOP_SOURCE})
				endif()
			endforeach()
			set(IIOD_HAS_BUFFER_PERSISTENT_HOP ON)
		endif()
	endif()
	set(IIOD_METADATA_CFILES ${IIOD_BUFFER_METADATA_PROVIDER}
		${IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES})
else()
	set(IIOD_METADATA_CFILES buffer-metadata-none.c)
endif()

# An ordinary, opt-in userspace SDK: no detector numerics are linked into iiOD.
# Paths/digests belong to the operator's pinned build, never a network request.
set(IIOD_SCANNER_GLRT_LIBRARY "" CACHE FILEPATH "Single-RX asynchronous scanner acquisition SDK")
set(IIOD_SCANNER_GLRT_INCLUDE_DIR "" CACHE PATH "Directory containing the public scanner_glrt.h")
foreach(GLRT_SETTING WORKER_PATH TEMPLATES_2500000 TEMPLATES_5000000
		ALGORITHM_SHA256 CONFIGURATION_SHA256)
	set(IIOD_SCANNER_GLRT_${GLRT_SETTING} "" CACHE STRING "Trusted scanner GLRT ${GLRT_SETTING}")
endforeach()
if(IIOD_SCANNER_GLRT_LIBRARY)
	if(NOT IIOD_HAS_BUFFER_PERSISTENT_HOP)
		message(FATAL_ERROR "Scanner GLRT requires the SPF persistent-hop metadata provider")
	endif()
	if(NOT EXISTS "${IIOD_SCANNER_GLRT_LIBRARY}" OR
			NOT EXISTS "${IIOD_SCANNER_GLRT_INCLUDE_DIR}/scanner_glrt.h")
		message(FATAL_ERROR "Scanner GLRT requires a built SDK and its public header")
	endif()
	foreach(GLRT_PATH WORKER_PATH TEMPLATES_2500000 TEMPLATES_5000000)
		if(NOT IS_ABSOLUTE "${IIOD_SCANNER_GLRT_${GLRT_PATH}}" OR
				IIOD_SCANNER_GLRT_${GLRT_PATH} MATCHES "[\";\\\\\n\r]")
			message(FATAL_ERROR "Scanner GLRT ${GLRT_PATH} must be a literal absolute runtime path")
		endif()
	endforeach()
	foreach(GLRT_HASH ALGORITHM_SHA256 CONFIGURATION_SHA256)
		string(LENGTH "${IIOD_SCANNER_GLRT_${GLRT_HASH}}" GLRT_HASH_LENGTH)
		if(NOT GLRT_HASH_LENGTH EQUAL 64 OR
				NOT IIOD_SCANNER_GLRT_${GLRT_HASH} MATCHES "^[0-9a-f]+$" OR
				IIOD_SCANNER_GLRT_${GLRT_HASH} MATCHES "^0+$")
			message(FATAL_ERROR "Scanner GLRT ${GLRT_HASH} must be a nonzero lowercase SHA-256")
		endif()
	endforeach()
	list(APPEND IIOD_METADATA_CFILES spf-scanner-glrt.c)
endif()

function(iiod_configure_scanner_glrt target)
	target_include_directories(${target} PRIVATE ${IIOD_SCANNER_GLRT_INCLUDE_DIR})
	target_link_libraries(${target} ${IIOD_SCANNER_GLRT_LIBRARY})
	target_compile_definitions(${target} PRIVATE IIOD_HAS_SCANNER_GLRT=1)
	foreach(GLRT_SETTING WORKER_PATH TEMPLATES_2500000 TEMPLATES_5000000
			ALGORITHM_SHA256 CONFIGURATION_SHA256)
		target_compile_definitions(${target} PRIVATE
			IIOD_SCANNER_GLRT_${GLRT_SETTING}="${IIOD_SCANNER_GLRT_${GLRT_SETTING}}")
	endforeach()
endfunction()

set(IIOD_CFILES iiod.c ops.c thread-pool.c ddr-ring-core.c
	spf-ddr-ring-status.c
	${IIOD_METADATA_CFILES}
	${BISON_parser_OUTPUTS} ${FLEX_lexer_OUTPUTS})

option(WITH_AIO "Build IIOD with async. I/O support" ON)
if (WITH_AIO)
	find_library(LIBAIO_LIBRARIES aio)
	find_path(LIBAIO_INCLUDE_DIR libaio.h)

	if (NOT LIBAIO_LIBRARIES OR NOT LIBAIO_INCLUDE_DIR)
		message(SEND_ERROR "Unable to find libaio dependency.\n"
			"If you want to disable async. I/O support, set WITH_AIO=OFF.")
	endif ()

	option(WITH_IIOD_USBD "Add support for USB through FunctionFS within IIOD" ON)
	if (WITH_IIOD_USBD)
		include(CheckTypeSize)
		set(CMAKE_EXTRA_INCLUDE_FILES linux/usb/functionfs.h)
		check_type_size("struct usb_functionfs_descs_head_v2" FUNCTIONFS_V2)
		set(CMAKE_EXTRA_INCLUDE_FILES)

		if (NOT HAVE_FUNCTIONFS_V2)
			message(SEND_ERROR "Linux kernel headers are too old.\n"
				"If you want to disable USB support, set WITH_IIOD_USBD=OFF.")
		endif()

		set(IIOD_CFILES ${IIOD_CFILES} usbd.c)
	endif()
endif()

option(WITH_IIOD_SERIAL "Add serial (UART) support" ON)
if (WITH_IIOD_SERIAL)
	set(IIOD_CFILES ${IIOD_CFILES} serial.c)
endif()

add_executable(iiod ${IIOD_CFILES})
if(IIOD_SCANNER_GLRT_LIBRARY)
	iiod_configure_scanner_glrt(iiod)
endif()
if (IIOD_HAS_BUFFER_PERSISTENT_HOP)
	target_compile_definitions(iiod PRIVATE IIOD_HAS_BUFFER_PERSISTENT_HOP=1)
	if (IIOD_BUFFER_PERSISTENT_HOP_DEVICE_PROVIDER MATCHES
			"spf-hop-device-local\\.c$")
		target_compile_definitions(iiod PRIVATE
			IIOD_HAS_KERNEL_PERSISTENT_HOP=1)
	endif()
endif()
set_target_properties(iiod PROPERTIES
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)
target_link_libraries(iiod iio ${PTHREAD_LIBRARIES} ${AVAHI_LIBRARIES})
if (IIOD_BUFFER_METADATA_PROVIDER)
	target_include_directories(iiod PRIVATE ${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
	target_link_libraries(iiod ${IIOD_BUFFER_METADATA_LIBRARIES})
	target_compile_definitions(iiod PRIVATE IIOD_HAS_BUFFER_METADATA=1)
endif()

if (WITH_AIO)
	include_directories(${LIBAIO_INCLUDE_DIR})
	target_link_libraries(iiod ${LIBAIO_LIBRARIES})
endif ()

if (WITH_ZSTD)
	include_directories(${LIBZSTD_INCLUDE_DIR})
	target_link_libraries(iiod ${LIBZSTD_LIBRARIES})
endif()

add_definitions(-D_GNU_SOURCE=1)

if(WITH_TESTS AND WITH_XML_BACKEND)
	# Hardware-free, XML-only session fixture exercising the actual protocol.
	add_executable(test_metadata_drain_server
		../tests/test_metadata_drain_server.c ../tests/metadata-drain-client.c
		thread-pool.c ddr-ring-core.c
		spf-ddr-ring-status.c buffer-metadata-none.c ../iiod-client.c
		../iiod-command-batch.c ../lock.c ../utilities.c
		${BISON_parser_OUTPUTS} ${FLEX_lexer_OUTPUTS})
	target_include_directories(test_metadata_drain_server PRIVATE
		${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
	# Both targets use the generated parser; finish its first build before the
	# fixture's compilation to avoid duplicate concurrent flex/bison recipes.
	add_dependencies(test_metadata_drain_server iiod)
	target_link_libraries(test_metadata_drain_server iio ${PTHREAD_LIBRARIES})
	set_target_properties(test_metadata_drain_server PROPERTIES
		C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
	if(WITH_AIO)
		target_link_libraries(test_metadata_drain_server ${LIBAIO_LIBRARIES})
	endif()
	if(WITH_ZSTD)
		target_link_libraries(test_metadata_drain_server ${LIBZSTD_LIBRARIES})
	endif()
endif()

if(WITH_TESTS AND IIOD_SCANNER_GLRT_LIBRARY)
	set(GLRT_FIXTURE_SOURCES ${IIOD_METADATA_CFILES})
	list(REMOVE_ITEM GLRT_FIXTURE_SOURCES ${IIOD_BUFFER_METADATA_PROVIDER})
	add_executable(test_scanner_glrt_provider ../tests/test_scanner_glrt_provider.c
		${GLRT_FIXTURE_SOURCES})
	target_include_directories(test_scanner_glrt_provider PRIVATE ${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
	target_compile_definitions(test_scanner_glrt_provider PRIVATE IIOD_HAS_BUFFER_PERSISTENT_HOP=1)
	target_link_libraries(test_scanner_glrt_provider iio ${PTHREAD_LIBRARIES} ${IIOD_BUFFER_METADATA_LIBRARIES})
	iiod_configure_scanner_glrt(test_scanner_glrt_provider)
	set_target_properties(test_scanner_glrt_provider PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
	if(WITH_XML_BACKEND)
		add_executable(test_scanner_glrt_network_server
			../tests/test_scanner_glrt_network_server.c ../tests/test_scanner_glrt_provider.c
			thread-pool.c ddr-ring-core.c spf-ddr-ring-status.c
			${GLRT_FIXTURE_SOURCES} ${BISON_parser_OUTPUTS} ${FLEX_lexer_OUTPUTS})
		add_dependencies(test_scanner_glrt_network_server iiod)
		target_include_directories(test_scanner_glrt_network_server PRIVATE
			${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
		target_compile_definitions(test_scanner_glrt_network_server PRIVATE
			IIOD_HAS_BUFFER_PERSISTENT_HOP=1 SPF_GLRT_NETWORK_FIXTURE=1)
		target_link_libraries(test_scanner_glrt_network_server iio
			${PTHREAD_LIBRARIES} ${IIOD_BUFFER_METADATA_LIBRARIES})
		iiod_configure_scanner_glrt(test_scanner_glrt_network_server)
		set_target_properties(test_scanner_glrt_network_server PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
		if(WITH_AIO)
			target_link_libraries(test_scanner_glrt_network_server ${LIBAIO_LIBRARIES})
		endif()
		if(WITH_ZSTD)
			target_link_libraries(test_scanner_glrt_network_server ${LIBZSTD_LIBRARIES})
		endif()
	endif()
endif()

if (HAVE_AVAHI)
	target_sources(iiod PRIVATE dns-sd.c)
endif()

if(NOT SKIP_INSTALL_ALL)
	install(TARGETS iiod RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

if (WITH_SYSTEMD)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/init/iiod.service.cmakein ${PROJECT_BINARY_DIR}/init/iiod.service)
	install(FILES ${PROJECT_BINARY_DIR}/init/iiod.service DESTINATION ${SYSTEMD_UNIT_INSTALL_DIR})
endif()

if (WITH_SYSVINIT)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/init/iiod.init.cmakein ${PROJECT_BINARY_DIR}/init/iiod)
	install(FILES ${PROJECT_BINARY_DIR}/init/iiod
	        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
	        DESTINATION ${SYSVINIT_INSTALL_DIR})
endif()

if (WITH_UPSTART)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/init/iiod.conf.cmakein ${PROJECT_BINARY_DIR}/init/iiod.conf)
	install(FILES ${PROJECT_BINARY_DIR}/init/iiod.conf DESTINATION ${UPSTART_CONF_INSTALL_DIR})
endif()

toggle_iio_feature("${WITH_IIOD_SERIAL}" iiod-serial)
toggle_iio_feature("${WITH_AIO}" iiod-aio)
toggle_iio_feature("${WITH_IIOD_USBD}" iiod-usb)
toggle_iio_feature("${WITH_SYSTEMD}" iiod-systemd)
toggle_iio_feature("${WITH_SYSVINIT}" iiod-sysvinit)
toggle_iio_feature("${WITH_UPSTART}" iiod-upstart)

set(IIOD_FEATURES_ON "${IIO_FEATURES_ON}" PARENT_SCOPE)
set(IIOD_FEATURES_OFF "${IIO_FEATURES_OFF}" PARENT_SCOPE)
