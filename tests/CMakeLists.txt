cmake_minimum_required(VERSION 3.10)

project(iio_genxml  C)
project(iio_info    C)
project(iio_attr    C)
project(iio_readdev C)
project(iio_reg     C)

if(APPLE)
	# Add relative rpath to iio library (which is in the same framework)
	set(CMAKE_INSTALL_RPATH "@executable_path/../..")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

if (MSVC)
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../deps/wingetopt/src)
	set(GETOPT_C_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/wingetopt/src/getopt.c)
	set_source_files_properties(${GETOPT_C_FILE} PROPERTIES COMPILE_FLAGS -D_CRT_SECURE_NO_WARNINGS=1)
endif (MSVC)

if (WIN32)
	set(LIBIIO_ORIGINAL_FILENAME )
	set(LIBIIO_RC ${CMAKE_CURRENT_BINARY_DIR}/properties.rc)
	configure_file(../properties.rc.cmakein ${LIBIIO_RC} @ONLY)
endif()

if (CMAKE_COMPILER_IS_GNUCC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DIIO_CHECK_RET")
endif()
add_library(iio_tests_helper STATIC iio_common.c gen_code.c)
target_link_libraries(iio_tests_helper iio)

add_executable(iio_genxml  iio_genxml.c   ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(iio_info    iio_info.c     ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(iio_attr    iio_attr.c     ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(iio_readdev iio_readdev.c  ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(iio_reg     iio_reg.c      ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(iio_writedev iio_writedev.c  ${GETOPT_C_FILE} ${LIBIIO_RC})
add_executable(test_buffer_metadata_e2e test_buffer_metadata_e2e.c)
target_link_libraries(test_buffer_metadata_e2e iio)
add_executable(test_iiod_command_batch test_iiod_command_batch.c
	../iiod-command-batch.c)
target_include_directories(test_iiod_command_batch PRIVATE ..)
add_executable(test_metadata_batch_core test_metadata_batch_core.c)
target_include_directories(test_metadata_batch_core PRIVATE ..)
target_link_libraries(test_metadata_batch_core iio)
if((PTHREAD_LIBRARIES OR ANDROID) AND NOT WIN32 AND NOT NO_THREADS)
	target_compile_definitions(test_metadata_batch_core PRIVATE
		METADATA_BATCH_TEST_PTHREAD=1)
	target_link_libraries(test_metadata_batch_core ${PTHREAD_LIBRARIES})
endif()

if (MSVC)
	target_link_options(iio_tests_helper PUBLIC /DEBUG)
	target_link_options(iio_genxml   PUBLIC /DEBUG)
	target_link_options(iio_info     PUBLIC /DEBUG)
	target_link_options(iio_attr     PUBLIC /DEBUG)
	target_link_options(iio_readdev  PUBLIC /DEBUG)
	target_link_options(iio_reg      PUBLIC /DEBUG)
	target_link_options(iio_writedev PUBLIC /DEBUG)
endif()

target_link_libraries(iio_genxml  iio iio_tests_helper)
target_link_libraries(iio_info    iio iio_tests_helper)
target_link_libraries(iio_attr    iio iio_tests_helper)
target_link_libraries(iio_readdev iio iio_tests_helper)
target_link_libraries(iio_reg     iio iio_tests_helper)
target_link_libraries(iio_writedev iio iio_tests_helper)

set(IIO_TESTS_TARGETS iio_genxml iio_info iio_attr iio_readdev iio_reg
	iio_writedev)

if(IIOD_BUFFER_METADATA_PROVIDER MATCHES "spf-buffer-metadata\\.c$")
	set(SPF_RADIO_FRAME_V3_SOURCE "")
	set(SPF_GAIN_TIMELINE_SOURCE "")
	set(SPF_THREAD_JOIN_SOURCE "")
	foreach(SPF_PROVIDER_SOURCE ${IIOD_BUFFER_METADATA_PROVIDER_EXTRA_SOURCES})
		if(SPF_PROVIDER_SOURCE MATCHES "spf_radio_frame_v3\\.c$")
			set(SPF_RADIO_FRAME_V3_SOURCE ${SPF_PROVIDER_SOURCE})
		endif()
		if(SPF_PROVIDER_SOURCE MATCHES "spf_gain_timeline\\.c$")
			set(SPF_GAIN_TIMELINE_SOURCE ${SPF_PROVIDER_SOURCE})
		endif()
		if(SPF_PROVIDER_SOURCE MATCHES "spf_thread_join\\.c$")
			set(SPF_THREAD_JOIN_SOURCE ${SPF_PROVIDER_SOURCE})
		endif()
	endforeach()
	if(NOT SPF_RADIO_FRAME_V3_SOURCE)
		message(FATAL_ERROR "SPF tandem tests require spf_radio_frame_v3.c")
	endif()
	if(NOT SPF_GAIN_TIMELINE_SOURCE)
		message(FATAL_ERROR "SPF tandem tests require spf_gain_timeline.c")
	endif()
	if(NOT SPF_THREAD_JOIN_SOURCE)
		message(FATAL_ERROR "SPF temperature tests require spf_thread_join.c")
	endif()
	add_executable(test_spf_tandem_session test_spf_tandem_session.c
		../iiod/spf-tandem-session.c ${SPF_GAIN_TIMELINE_SOURCE}
		${SPF_RADIO_FRAME_V3_SOURCE})
	add_executable(test_spf_tandem_metadata test_spf_tandem_metadata.c
		../iiod/spf-tandem-metadata.c ${SPF_RADIO_FRAME_V3_SOURCE})
	add_executable(test_spf_temperature_cache test_spf_temperature_cache.c
		../iiod/spf-temperature-cache.c ${SPF_THREAD_JOIN_SOURCE})
	add_executable(test_spf_buffer_layout test_spf_buffer_layout.c
		../iiod/spf-buffer-layout.c)
	add_executable(test_spf_ddr_burst_request test_spf_ddr_burst_request.c
		../iiod/spf-ddr-burst-request.c)
	add_executable(test_spf_ddr_ring_request test_spf_ddr_ring_request.c
		../iiod/spf-ddr-ring-request.c)
	add_executable(test_spf_ddr_ring_status test_spf_ddr_ring_status.c
		../iiod/spf-ddr-ring-status.c)
	add_executable(test_spf_sampler_coverage test_spf_sampler_coverage.c
		../iiod/spf-sampler-coverage.c)
	add_executable(test_spf_metadata_request test_spf_metadata_request.c
		../iiod/spf-metadata-request.c)
	add_executable(test_ddr_ring_core test_ddr_ring_core.c
		../iiod/ddr-ring-core.c)
	target_include_directories(test_spf_tandem_session PRIVATE ../iiod
		${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
	target_include_directories(test_spf_tandem_metadata PRIVATE ../iiod
		${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
	target_include_directories(test_spf_temperature_cache PRIVATE ../iiod
		${IIOD_BUFFER_METADATA_INCLUDE_DIRS})
	target_include_directories(test_spf_buffer_layout PRIVATE ../iiod)
	target_include_directories(test_spf_ddr_burst_request PRIVATE ../iiod)
	target_include_directories(test_spf_ddr_ring_request PRIVATE ../iiod)
	target_include_directories(test_spf_ddr_ring_status PRIVATE ../iiod)
	target_include_directories(test_spf_sampler_coverage PRIVATE ../iiod)
	target_include_directories(test_spf_metadata_request PRIVATE ../iiod)
	target_include_directories(test_ddr_ring_core PRIVATE ../iiod)
	target_link_libraries(test_spf_temperature_cache iio ${PTHREAD_LIBRARIES})
	set(IIO_TESTS_TARGETS ${IIO_TESTS_TARGETS}
		test_spf_tandem_session test_spf_tandem_metadata
		test_spf_temperature_cache test_spf_buffer_layout)
	list(APPEND IIO_TESTS_TARGETS test_spf_ddr_burst_request)
	list(APPEND IIO_TESTS_TARGETS test_spf_ddr_ring_request
		test_spf_ddr_ring_status test_spf_sampler_coverage
		test_spf_metadata_request test_ddr_ring_core)
endif()

if(PTHREAD_LIBRARIES OR ANDROID)
	project(iio_stresstest C)
	add_executable(iio_stresstest iio_stresstest.c)
	target_link_libraries(iio_stresstest iio iio_tests_helper ${PTHREAD_LIBRARIES})
	set(IIO_TESTS_TARGETS ${IIO_TESTS_TARGETS} iio_stresstest)

	target_link_libraries(iio_readdev  ${PTHREAD_LIBRARIES})
	target_link_libraries(iio_writedev ${PTHREAD_LIBRARIES})
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux" AND
		(PTHREAD_LIBRARIES OR ANDROID) AND NOT NO_THREADS)
	add_executable(test_thread_pool_affinity test_thread_pool_affinity.c
		../iiod/thread-pool.c)
	target_include_directories(test_thread_pool_affinity PRIVATE ../iiod)
	target_compile_definitions(test_thread_pool_affinity PRIVATE _GNU_SOURCE=1)
	target_link_libraries(test_thread_pool_affinity ${PTHREAD_LIBRARIES})
	set_target_properties(test_thread_pool_affinity PROPERTIES
		C_STANDARD 99
		C_STANDARD_REQUIRED ON
		C_EXTENSIONS OFF)
endif()

set_target_properties(${IIO_TESTS_TARGETS} iio_tests_helper PROPERTIES
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)
set_target_properties(test_iiod_command_batch test_metadata_batch_core PROPERTIES
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)

if(NOT SKIP_INSTALL_ALL)
	if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin" AND OSX_FRAMEWORK)
		install(TARGETS ${IIO_TESTS_TARGETS} RUNTIME DESTINATION ${OSX_INSTALL_FRAMEWORKSDIR}/iio.framework/Tools)
	else()
		install(TARGETS ${IIO_TESTS_TARGETS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
	endif()
endif()

# Make the test targets available to the main CMakeLists.txt
set(IIO_TESTS_TARGETS ${IIO_TESTS_TARGETS} PARENT_SCOPE)
