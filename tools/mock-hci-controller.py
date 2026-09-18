#!/usr/bin/env python3
"""Small scripted HCI startup controller, not a Bluetooth protocol emulator.
Only explicit opcodes receive success; unknown commands get Unknown HCI Command.
Private AF_UNIX/SOCK_SEQPACKET endpoints are consumed by mock-hci-socket.c.
"""
import argparse
import os
from pathlib import Path
import selectors
import socket
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
parser.add_argument('--syslog', type=Path)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)
selector = selectors.DefaultSelector()
for name in ('mgmt', 'controller'):
    server=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
    server.bind(str(args.directory/name)); server.listen()
    selector.register(server, selectors.EVENT_READ, ('listen',name))
if args.syslog:
    log_socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    log_socket.bind(str(args.syslog))
    selector.register(log_socket, selectors.EVENT_READ, ('log', 'syslog'))
print('ready', flush=True)
# Return parameter bytes exclude status; Bluetooth byte order is little endian.
# Floss requires SSP even for controller initialization. These feature bits
# only permit startup; this fixture still cannot perform pairing or LE links.
features = struct.pack('<Q', (1 << 51) | (1 << 38))
replies={
  0x080f:b'',                            # Write default link policy
  0x0c03:b'',                              # Reset
  0x0c01:b'', 0x0c63:b'', 0x0c6d:b'',      # Event masks, LE host support
  0x0c14:b'Floss simulated controller'.ljust(248,b'\0'),
  0x1001:struct.pack('<BHBHH',9,1,9,0xffff,1),
  0x1002:bytes(64),                        # No optional commands advertised
  0x1003:features,                         # Local features
  0x1005:struct.pack('<HBHH',1021,60,10,10), # ACL/SCO buffer sizes
  0x1009:bytes.fromhex('665544332211'),      # Controller address
  0x2002:struct.pack('<HB',27,10),           # LE buffer size
  0x2003:bytes(8), 0x201c:bytes(8),         # LE features and states
  0x2005:b'', 0x2010:b'',                # Set random address / clear accept list
  0x0c26:b'',                            # Write voice setting
  0x2007:b'\x00',                         # LE advertising transmit power (0 dBm)
  0x200f:b'\x08', 0x2001:b'',              # Accept list size / event mask
  0x0c13:b'', 0x0c24:b'', 0x0c1a:b'',      # Name/class/scan enable
  0x0c45:b'', 0x0c56:b'', 0x0c7a:b'',      # Inquiry/SSP/SC modes
  0x0c05:b'', 0x0c16:b'',                  # Event filter / connection accept timeout
  0x0c18:b'', 0x0c1c:b'', 0x0c1e:b'',      # Page timeout / scan activities
  0x0c47:b'', 0x0c43:b'', 0x0c1b:struct.pack('<HH',0x800,0x12),
  0x0c1d:struct.pack('<HH',0x1000,0x12),
}
while True:
    for key,_ in selector.select():
        connection=key.fileobj; kind,name=key.data
        if kind == 'log':
            print('syslog: ' + connection.recv(65536).decode(errors='replace'), flush=True)
            continue
        if kind=='listen':
            client,_=connection.accept();selector.register(client,selectors.EVENT_READ,('client',name));continue
        packet=connection.recv(4096)
        if not packet:
            selector.unregister(connection);connection.close();continue
        if name=='mgmt':
            if len(packet)<6: raise RuntimeError('short mgmt packet')
            opcode,index,size=struct.unpack('<HHH',packet[:6])
            if size != len(packet)-6: raise RuntimeError('mgmt length mismatch')
            print(f'mgmt 0x{opcode:04x} index={index}',flush=True)
            if opcode==3: status=0; payload=struct.pack('<HH',1,0)
            elif opcode==1: status=0; payload=struct.pack('<BH',1,23)
            else: status=1;payload=b''
            response=struct.pack('<HB',opcode,status)+payload
            connection.send(struct.pack('<HHH',1,index,len(response))+response)
            continue
        if len(packet)<4 or packet[0]!=1: raise RuntimeError(f'unsupported H4 packet {packet.hex()}')
        opcode=struct.unpack('<H',packet[1:3])[0]
        if packet[3]!=len(packet)-4: raise RuntimeError('HCI length mismatch')
        print(f'hci 0x{opcode:04x} {packet[4:].hex()}',flush=True)
        if opcode == 0x2018: status=0; payload=os.urandom(8)  # LE Rand
        elif opcode==0x1004: status=0; payload=packet[4:5]+b'\x00'+(features if packet[4] == 0 else bytes(8))
        elif opcode in replies: status=0;payload=replies[opcode]
        else: status=1;payload=b'';print(f'unsupported 0x{opcode:04x}',flush=True)
        event=b'\x01'+struct.pack('<H',opcode)+bytes([status])+payload
        connection.send(bytes([4,0x0e,len(event)])+event)
