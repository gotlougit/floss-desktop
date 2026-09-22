#!/usr/bin/env python3
"""Compile Floss's actual mSBC ring-buffer class and test fragmented framing.

Only allocator, logging and PLC bookkeeping are stubbed. No radio, codec library,
root access or network is needed. Pass the patched Bluetooth source directory.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
args = p.parse_args()
source = (args.source / 'system/stack/btm/btm_sco_hci.cc').read_text()
start = source.index('struct tBTM_MSBC_INFO {')
end = source.index('static tBTM_MSBC_INFO* msbc_info', start)
implementation = source[start:end].replace('log::', 'test_log::')
constants = '\n'.join(re.findall(r'(?:static const uint8_t btm_h2_header_frames_count|constexpr size_t btm_wbs_(?:supported_pkt_size|msbc_buffer_size))\[\] = .*?;', source))
assert len(constants.splitlines()) == 3
prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
constexpr size_t BTM_MSBC_PKT_LEN=60, BTM_MSBC_FS=120, BTM_MSBC_H2_HEADER_LEN=2;
constexpr uint8_t BTM_MSBC_H2_HEADER_0=1, BTM_MSBC_SYNC_WORD=0xad;
enum decode_buf_state { DECODE_BUF_EMPTY, DECODE_BUF_FULL, DECODE_BUF_HALFFULL };
namespace test_log { template<class... T> void warn(T...) {} template<class... T> void error(T...) {} template<class... T> void debug(T...) {} }
void* osi_calloc(size_t n) { return std::calloc(1,n); }
void osi_free(void* p) { std::free(p); }
void osi_free_and_reset(void** p) { std::free(*p); *p=nullptr; }
struct tBTM_MSBC_PLC { void init() {} void deinit() {} };
struct tBTM_SCO_PKT_STATUS { void init() {} };
'''
lookup = ('bool incomplete=false; auto* frame=b.find_msbc_pkt_head(&incomplete); if(incomplete) break;'
          if 'find_msbc_pkt_head(bool*' in implementation else 'auto* frame=b.find_msbc_pkt_head();')
test = r'''
int main() {
 unsigned failures=0;
 for(size_t chunk: {size_t(24),size_t(60),size_t(72)}) {
  for(size_t phase=0;phase<60;phase++) {
   tBTM_MSBC_INFO b{}; b.init(chunk);
   std::vector<uint8_t> stream(phase,0x55);
   for(unsigned n=0;n<80;n++) {
    std::vector<uint8_t> frame(60,0x55);
    frame[0]=1;frame[1]=btm_h2_header_frames_count[n%4];frame[2]=0xad;
    stream.insert(stream.end(),frame.begin(),frame.end());
   }
   unsigned good=0, lost=0, rejected=0;
   for(size_t i=0;i+chunk<=stream.size();i+=chunk) {
    std::vector<uint8_t> packet(stream.begin()+i,stream.begin()+i+chunk);
    if(b.write(packet)!=chunk) { rejected++;continue; }
    while(b.decode_buf_data_len()>=60) {
     LOOKUP
     if(frame) { assert(frame[0]==1 && frame[2]==0xad);good++; } else lost++;
     b.mark_pkt_decoded();
    }
   }
   if(good<78 || rejected) {
    if(failures<8) std::cerr<<"FAIL chunk="<<chunk<<" phase="<<phase<<" good="<<good<<" lost="<<lost<<" rejected="<<rejected<<"\n";
    failures++;
   }
   b.deinit();
  }
 }
 if(failures){std::cerr<<failures<<" alignment cases failed\n";return 1;}
 std::cout<<"PASS: 180 production mSBC ring-buffer alignment cases (24/60/72-byte chunks, all 60 starting offsets)\n";
}
'''.replace('LOOKUP', lookup)
with tempfile.TemporaryDirectory(prefix='floss-msbc-framing-') as d:
    unit=Path(d)/'test.cc'; binary=Path(d)/'test'
    unit.write_text(prefix+constants+'\n'+implementation+test)
    subprocess.run(shlex.split(args.cxx)+['-std=c++20','-O1','-Wall','-Wextra','-Werror',str(unit),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
