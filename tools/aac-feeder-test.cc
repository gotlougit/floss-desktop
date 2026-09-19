// SPDX-License-Identifier: Apache-2.0
// Compile the production feeder functions with deterministic I/O boundaries.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace bluetooth::log {
template <typename... Args> void warn(const char*, Args...) {}
template <typename... Args> void info(const char*, Args...) {}
template <typename... Args> void assert_that(bool value, const char*, Args...) { assert(value); }
}
using namespace bluetooth;
constexpr int BT_DEFAULT_BUFFER_SIZE = 8192;
constexpr int AVDT_MEDIA_OFFSET = 16;
struct BT_HDR { uint16_t event, len, offset, layer_specific; };
static void* osi_calloc(size_t n) { return std::calloc(1, n); }
static void osi_free(void* p) { std::free(p); }
struct {
  uint32_t (*read_callback)(uint8_t*, uint32_t);
  bool (*enqueue_callback)(BT_HDR*, size_t, uint32_t);
  struct { uint32_t channel_count = 2, bits_per_sample = 16; } feeding_params;
  struct { float counter = 0; } aac_feeding_state;
  uint32_t pcm_samples_per_frame = 1024, timestamp = 0;
  struct {
    uint64_t media_read_total_expected_packets = 0;
    uint64_t media_read_total_expected_reads_count = 0;
    uint64_t media_read_total_expected_read_bytes = 0;
    uint64_t media_read_total_actual_read_bytes = 0;
    uint64_t media_read_total_actual_reads_count = 0;
    uint64_t media_read_total_dropped_packets = 0;
  } stats;
} a2dp_aac_encoder_cb;
static uint32_t available;
static unsigned encoded, enqueued;
static int codec_socket = -1;
static struct {
  int encode_pcm(uint8_t* pcm, int length, uint8_t* output, int capacity) {
    // Same strict input length contract as the real MMC AAC encoder.
    assert(length == 4096);
    assert(capacity >= 32);
    for (int i = 0; i < length; ++i) assert(pcm[i] == (i < int(available) ? 0x5a : 0));
    if (codec_socket >= 0) {
      assert(send(codec_socket, pcm, length, MSG_NOSIGNAL) == length);
      uint8_t packet[8192];
      assert(recv(codec_socket, packet, sizeof(packet), 0) > 0);
    }
    std::memset(output, 0, 32);
    ++encoded;
    return 32;
  }
} codec_intf;

static bool a2dp_aac_read_feeding(uint8_t*, uint32_t*);
// Generated directly from the checkout; neither function is reimplemented.
#include "aac-feeder-production.inc"

int main(int argc, char** argv) {
  assert(argc == 1 || argc == 2);
  if (argc == 2) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    assert(std::strlen(argv[1]) < sizeof(address.sun_path));
    std::strcpy(address.sun_path, argv[1]);
    codec_socket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(codec_socket >= 0);
    timeval timeout{3, 0};
    assert(setsockopt(codec_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(connect(codec_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  }
  a2dp_aac_encoder_cb.read_callback = [](uint8_t* buffer, uint32_t size) {
    assert(size == 4096);
    std::memset(buffer, 0x5a, available);
    return available;
  };
  a2dp_aac_encoder_cb.enqueue_callback = [](BT_HDR* buffer, size_t frames, uint32_t consumed) {
    assert(frames == 1 && buffer->len == 32 && buffer->layer_specific == 1);
    assert(consumed == available); // Padding must not inflate consumption accounting.
    ++enqueued;
    osi_free(buffer);
    return true;
  };
  for (uint32_t count : {0u, 1u, 1764u, 4057u, 4095u, 4096u}) {
    available = count;
    encoded = enqueued = 0;
    a2dp_aac_encoder_cb.stats = {};
    a2dp_aac_encoder_cb.aac_feeding_state.counter = 0;
    const auto timestamp = a2dp_aac_encoder_cb.timestamp;
    a2dp_aac_encode_frames(3);
    assert(encoded == (count ? 3u : 0u));
    assert(enqueued == encoded);
    assert(a2dp_aac_encoder_cb.timestamp == timestamp + encoded * 1024);
    assert(a2dp_aac_encoder_cb.stats.media_read_total_actual_read_bytes == count * encoded);
    assert(a2dp_aac_encoder_cb.stats.media_read_total_dropped_packets == 0);
    if (!count) assert(a2dp_aac_encoder_cb.aac_feeding_state.counter == 3 * 4096);
    std::cout << "PCM read " << count << ": PASS\n";
  }
  if (codec_socket >= 0) close(codec_socket);
}
