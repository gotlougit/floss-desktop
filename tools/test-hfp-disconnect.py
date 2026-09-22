#!/usr/bin/env python3
"""Run production SLC cleanup with a late/absent SCO disconnect callback.

Extracts the cleanup statements and helper from bluetooth_media.rs. Native HFP,
D-Bus notifications and policy inputs are stubbed; no services are accessed.
"""
import argparse, os, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=Path('Bluetooth/system/gd/rust/linux/stack/src/bluetooth_media.rs'))
p.add_argument('--rustc',default='rustc')
p.add_argument('--cc',default='cc')
a=p.parse_args(); source=a.source.read_text()
start=source.index('                    BthfConnectionState::Disconnected =>')
end=source.index('                        self.rm_connected_profile(addr, Profile::Hfp, true);',start)
block=source[start:end]
start=block.index('                        self.uhid_destroy(&addr);')
comment=block.find('                        // SLC may disappear')
if comment>=0: start=comment
cleanup=block[start:]
helper=''
if '    fn remove_sco_synthetic_call(' in source:
 start=source.index('    fn remove_sco_synthetic_call(')
 end=source.index('\n    }',start)+6
 helper=source[start:end]
unit=r'''
#![allow(dead_code)]
use std::collections::HashMap;
type RawAddress=u8;
#[derive(Clone,Copy)] enum BthfAudioState { Connected, Disconnecting, Disconnected }
enum TelephonyEvent { CRASRemoveActiveCall }
struct Phone { num_active:u8 }
struct Media {
 hfp_audio_state:HashMap<u8,BthfAudioState>, hfp_states:HashMap<u8,bool>, hfp_cap:HashMap<u8,bool>,
 call_list:Vec<u8>, phone_state:Phone, synthetic_policy:bool, notifications:usize,
}
impl Media {
 fn should_insert_call_when_sco_start(&self,_:u8)->bool { self.synthetic_policy }
 fn phone_state_change(&mut self,_:String) { self.notifications+=1; }
 fn notify_telephony_event(&mut self,_:&u8,_:TelephonyEvent) {}
 fn uhid_destroy(&mut self,_:&u8) {}
 fn audio_session_state(&mut self,_:u8,_:bool,_:bool) {}
 HELPER
 fn disconnect(&mut self,addr:u8) { CLEANUP }
}
fn main() {
 for state in [BthfAudioState::Connected,BthfAudioState::Disconnecting] {
  for synthetic in [true,false] {
   let mut m=Media { hfp_audio_state:HashMap::from([(1,state)]),hfp_states:HashMap::from([(1,true)]),hfp_cap:HashMap::new(),call_list:vec![1],phone_state:Phone {num_active:1},synthetic_policy:synthetic,notifications:0 };
   m.disconnect(1);
   assert_eq!(m.phone_state.num_active, if synthetic {0} else {1},"SLC loss must clear synthetic calls, preserve real calls");
   assert_eq!(m.call_list.len(),if synthetic {0} else {1});
   assert_eq!(m.notifications,usize::from(synthetic));
   assert!(m.hfp_audio_state.is_empty());
   // A duplicate SLC teardown must not send another synthetic call update.
   m.disconnect(1); assert_eq!(m.notifications,usize::from(synthetic));
  }
 }
 println!("PASS: SLC-first disconnect clears synthetic calls, preserves real calls, and tolerates duplicate teardown");
}
'''.replace('HELPER',helper).replace('CLEANUP',cleanup)
with tempfile.TemporaryDirectory(prefix='floss-hfp-disconnect-') as d:
 src=Path(d)/'test.rs'; binary=Path(d)/'test';src.write_text(unit)
 subprocess.run([a.rustc,'--edition=2021','-C','linker='+a.cc,str(src),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
