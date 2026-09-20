add_executable(WeReadProtocolTest
  WeReadProtocolTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadProtocol.cpp
)

add_executable(WeReadTimeLedgerTest WeReadTimeLedgerStandaloneTest.cpp)
add_executable(WeReadServiceJournalTest WeReadServiceJournalStandaloneTest.cpp)
target_include_directories(WeReadServiceJournalTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
add_executable(WeReadServiceClientTest WeReadServiceClientStandaloneTest.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadServiceClient.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp)
target_include_directories(WeReadServiceClientTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/time_cloud_stubs ${CMAKE_CURRENT_SOURCE_DIR}/time_storage_stubs
  ${REPO_ROOT}/lib/WeReadWebApi/src ${REPO_ROOT}/lib/JsonParser)
target_compile_definitions(WeReadServiceClientTest PRIVATE ENABLE_CHINESE_VERSION)
foreach(service_target WeReadServiceJournalTest WeReadServiceClientTest)
  target_compile_features(${service_target} PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(${service_target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${service_target} PRIVATE -UNDEBUG -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${service_target} COMMAND ${service_target})
endforeach()
add_executable(WeReadTimeDiagnosticTest WeReadTimeDiagnosticStandaloneTest.cpp)
target_include_directories(WeReadTimeDiagnosticTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadTimeDiagnosticTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeDiagnosticTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeDiagnosticTest COMMAND WeReadTimeDiagnosticTest)
add_executable(WeReadTimeDataLayoutTest WeReadTimeDataLayoutStandaloneTest.cpp)
target_include_directories(WeReadTimeDataLayoutTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadTimeDataLayoutTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeDataLayoutTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeDataLayoutTest COMMAND WeReadTimeDataLayoutTest)
add_executable(WeReadTimeStorageTest WeReadTimeStorageStandaloneTest.cpp)
target_include_directories(WeReadTimeStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/time_storage_stubs ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadTimeStorageTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeStorageTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeStorageTest COMMAND WeReadTimeStorageTest)
add_executable(WeReadHandoverManifestTest WeReadHandoverManifestStandaloneTest.cpp)
target_include_directories(WeReadHandoverManifestTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadHandoverManifestTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadHandoverManifestTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadHandoverManifestTest COMMAND WeReadHandoverManifestTest)
add_executable(WeReadTimeTransactionTest WeReadTimeTransactionStandaloneTest.cpp)
add_executable(WeReadTimeQueueTest WeReadTimeQueueStandaloneTest.cpp)
target_include_directories(WeReadTimeQueueTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadTimeQueueTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeQueueTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeQueueTest COMMAND WeReadTimeQueueTest)
target_include_directories(WeReadTimeTransactionTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadTimeTransactionTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeTransactionTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeTransactionTest COMMAND WeReadTimeTransactionTest)
add_executable(WeReadPacedTimeTest WeReadPacedTimeStandaloneTest.cpp)
target_include_directories(WeReadPacedTimeTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadPacedTimeTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadPacedTimeTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadPacedTimeTest COMMAND WeReadPacedTimeTest)
add_executable(WeReadTimeCloudTest WeReadTimeCloudStandaloneTest.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadTimeResponse.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp)
target_include_directories(WeReadTimeCloudTest PRIVATE
  ${REPO_ROOT}/lib/WeReadWebApi/src ${REPO_ROOT}/lib/JsonParser)
if(MSVC)
  target_compile_options(WeReadTimeCloudTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeCloudTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeCloudTest COMMAND WeReadTimeCloudTest)
add_executable(WeReadTimeQueryTest WeReadTimeQueryStandaloneTest.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadTimeCloud.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadTimeResponse.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp)
target_include_directories(WeReadTimeQueryTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/time_cloud_stubs
  ${REPO_ROOT}/lib/WeReadWebApi/src ${REPO_ROOT}/lib/JsonParser)
if(MSVC)
  target_compile_options(WeReadTimeQueryTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeQueryTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeQueryTest COMMAND WeReadTimeQueryTest)
add_executable(WeReadTimeReplayTest WeReadTimeReplayStandaloneTest.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadTimeCloud.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadTimeResponse.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp)
target_include_directories(WeReadTimeReplayTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/time_cloud_stubs
  ${REPO_ROOT}/lib/WeReadWebApi/src ${REPO_ROOT}/lib/JsonParser)
if(MSVC)
  target_compile_options(WeReadTimeReplayTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeReplayTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeReplayTest COMMAND WeReadTimeReplayTest)

find_package(Threads REQUIRED)
add_executable(WeReadTimeAsyncTest
  WeReadTimeAsyncStandaloneTest.cpp
  ${REPO_ROOT}/src/activities/apps/weread/webapi/WeReadTimeSync.cpp)
target_compile_features(WeReadTimeAsyncTest PRIVATE cxx_std_20)
target_compile_definitions(WeReadTimeAsyncTest PRIVATE ENABLE_CHINESE_VERSION=1)
target_include_directories(WeReadTimeAsyncTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/time_async_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/time_storage_stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/WeReadWebApi/src
  ${REPO_ROOT}/src/activities/apps/weread/webapi)
target_link_libraries(WeReadTimeAsyncTest PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(WeReadTimeAsyncTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeAsyncTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeAsyncTest COMMAND WeReadTimeAsyncTest)
set_tests_properties(WeReadTimeAsyncTest PROPERTIES TIMEOUT 30)
add_executable(WeReadExternalTimeTest WeReadExternalTimeStandaloneTest.cpp)
target_include_directories(WeReadExternalTimeTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
if(MSVC)
  target_compile_options(WeReadExternalTimeTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadExternalTimeTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadExternalTimeTest COMMAND WeReadExternalTimeTest)
target_include_directories(WeReadTimeLedgerTest PRIVATE ${REPO_ROOT}/lib/WeReadWebApi/src)
# The standalone regression harness deliberately uses assertions in Release too.
if(MSVC)
  target_compile_options(WeReadTimeLedgerTest PRIVATE /UNDEBUG)
else()
  target_compile_options(WeReadTimeLedgerTest PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME WeReadTimeLedgerTest COMMAND WeReadTimeLedgerTest)

target_include_directories(WeReadProtocolTest BEFORE PRIVATE
  ${crosspoint_simulator_SOURCE_DIR}/src
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/WeReadWebApi/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(WeReadProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WeReadProtocolTest)

add_executable(WeReadTouchGeometryTest
  WeReadTouchGeometryTest.cpp
)

target_include_directories(WeReadTouchGeometryTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(WeReadTouchGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WeReadTouchGeometryTest)

add_executable(WeReadStoreTest
  WeReadStoreTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadBrowse.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadProtocol.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadStore.cpp
  ${REPO_ROOT}/lib/WeReadWebApi/src/WeReadXhtmlCodec.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${crosspoint_simulator_SOURCE_DIR}/src/HalStorage.cpp
)

target_include_directories(WeReadStoreTest BEFORE PRIVATE
  ${crosspoint_simulator_SOURCE_DIR}/src
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/WeReadWebApi/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(WeReadStoreTest PRIVATE
  CROSSPOINT_EMULATED=1
  DESTRUCTOR_CLOSES_FILE=1
  XML_CONTEXT_BYTES=1024
  XML_GE=0
)

target_link_libraries(WeReadStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WeReadStoreTest PROPERTIES RUN_SERIAL TRUE)
