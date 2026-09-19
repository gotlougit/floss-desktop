#!/usr/bin/env python3
"""Check disconnect history with an inaccessible returned buffer (no audio server)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', required=True)
parser.add_argument('--source', type=Path, default=Path('pipewire'))
a = parser.parse_args()
source = a.source.resolve()
convert = source / 'spa/plugins/audioconvert'
text = (convert / 'audioconvert.c').read_text()
start = text.index('static void capture_state(')
end = text.index('\nstatic int do_set_port_io', start)
harness = r'''
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <spa/buffer/buffer.h>
#include "gaps-ops-c.c"
struct buffer { struct spa_buffer *buf; };
struct port {
    uint32_t id, last_buffer, n_buffers;
    bool is_dsp, ramp_start;
    struct buffer *buffers;
};
struct impl { struct gaps gaps; };
FUNCTION
int main(void) {
    float history[4] = {0}, input[8] = {1,2,3,4,5,6,7,8};
    const float *src[] = {input};
    struct gaps_state state = {.mode = GAPS_MODE_ZERO};
    struct impl impl = {.gaps = {.channels = 1, .states = {&state}}};
    spa_history_init(&state.hist, history, 4);
    assert(gaps_check_c(&impl.gaps, src, 8) == 0);
    void *returned = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(returned != MAP_FAILED);
    struct port port = {.id = 0, .is_dsp = true, .last_buffer = 0,
                       .n_buffers = 1, .buffers = returned};
    capture_state(&impl, &port);
    assert(port.ramp_start && port.last_buffer == SPA_ID_INVALID);
    assert(state.hist.fill == 4);
    for (unsigned i = 0; i < 4; ++i) assert(history[i] == input[i+4]);
    state.mode = GAPS_MODE_NORMAL;
    input[7] = 42;
    port.ramp_start = false;
    port.last_buffer = 0;
    assert(gaps_check_c(&impl.gaps, src, 8) == 0);
    capture_state(&impl, &port);
    assert(port.ramp_start && history[3] == 42);
    munmap(returned, 4096);
    return 0;
}
'''.replace('FUNCTION', text[start:end])
with tempfile.TemporaryDirectory(prefix='floss-disconnect-') as temp:
    test = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    test.write_text(harness)
    subprocess.run([a.cc, '-std=gnu11', '-Wall', '-Wextra', '-O2',
                    '-I' + str(source / 'spa/include'), '-I' + str(convert),
                    str(test), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: suspension uses copied history without accessing returned buffers')
