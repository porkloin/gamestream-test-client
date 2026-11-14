cmake_minimum_required(VERSION 3.16)
project(gamestream-test-client LANGUAGES C)
set(CMAKE_C_STANDARD 11)

# The commit --version names: CI and moonshine-test-headless pass it; a local build says "dev".
set(GTC_COMMIT "dev" CACHE STRING "the commit this client is built from")

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# moonlight-common-c, linked statically so the binary stands alone.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/moonlight-common-c moonlight-common-c EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(EXPAT REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)
pkg_check_modules(AV REQUIRED libavcodec libavutil libva libva-drm libnl-genl-3.0)

# Pairing and app launch over the host's HTTP(S) API, from moonlight-embedded.
set(GAMESTREAM ${THIRD_PARTY}/moonlight-embedded/libgamestream)
add_library(gamestream STATIC
  ${GAMESTREAM}/client.c
  ${GAMESTREAM}/http.c
  ${GAMESTREAM}/mkcert.c
  ${GAMESTREAM}/xml.c)
target_include_directories(gamestream PUBLIC ${GAMESTREAM} ${UUID_INCLUDE_DIRS})
target_link_libraries(gamestream PUBLIC moonlight-common-c OpenSSL::SSL OpenSSL::Crypto CURL::libcurl EXPAT::EXPAT ${UUID_LIBRARIES})

add_executable(gamestream-test-client src/main.c src/decode.c src/loss.c src/queue.c src/click.c src/record.c src/probe.c src/clock.c src/display.c src/pattern.c)
target_include_directories(gamestream-test-client PRIVATE ${AV_INCLUDE_DIRS} ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(gamestream-test-client PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(gamestream-test-client PRIVATE GTC_COMMIT="${GTC_COMMIT}")
target_link_libraries(gamestream-test-client PRIVATE gamestream moonlight-common-c ${AV_LIBRARIES} m)
# The loss injector sits between moonlight-common-c and the socket calls, and takes the
# acknowledgement flag off the frames the host flags before the library sees them.
target_link_options(gamestream-test-client PRIVATE -Wl,--wrap=recvfrom -Wl,--wrap=sendto -Wl,--wrap=connectionReceivedCompleteFrame)
install(TARGETS gamestream-test-client)

enable_testing()

# These tests stub the few moonlight-common-c symbols under test, so they need no host.
add_executable(test_loss tests/test_loss.c)
target_include_directories(test_loss PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(test_loss PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME loss COMMAND test_loss)

add_executable(test_click tests/test_click.c src/click.c)
target_include_directories(test_click PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(test_click PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_click PRIVATE m pthread)
add_test(NAME click COMMAND test_click)

# The --expect parser lives in main.c, included with its entry point renamed.
add_executable(test_expect tests/test_expect.c src/decode.c src/loss.c src/queue.c src/click.c src/record.c src/probe.c src/clock.c src/display.c src/pattern.c)
target_include_directories(test_expect PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src ${AV_INCLUDE_DIRS})
target_compile_options(test_expect PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_expect PRIVATE gamestream moonlight-common-c ${AV_LIBRARIES} m)
target_link_options(test_expect PRIVATE -Wl,--wrap=recvfrom -Wl,--wrap=sendto -Wl,--wrap=connectionReceivedCompleteFrame)
add_test(NAME expect COMMAND test_expect)

add_executable(test_display tests/test_display.c src/display.c)
target_include_directories(test_display PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(test_display PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_display PRIVATE m)
add_test(NAME display COMMAND test_display)

add_executable(test_pattern tests/test_pattern.c src/pattern.c)
target_include_directories(test_pattern PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(test_pattern PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME pattern COMMAND test_pattern)

add_executable(test_clock tests/test_clock.c src/clock.c)
target_include_directories(test_clock PRIVATE src ${THIRD_PARTY}/moonlight-common-c/src)
target_compile_options(test_clock PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME clock COMMAND test_clock)
