// SPDX-License-Identifier: MIT
//! HCI framing and SCO state; independent of USB and privileged device access.
const MAX_FRAME: usize = 4096;
#[derive(Debug, PartialEq, Eq)]
pub enum Action {
    Host(Vec<u8>),
    Usb(Vec<u8>),
    Sco(u8),
}
#[derive(Default)]
struct Framer {
    bytes: Vec<u8>,
}
impl Framer {
    fn push(&mut self, kind: u8, bytes: &[u8]) -> Result<Vec<Vec<u8>>, String> {
        let mut result = Vec::new();
        // Parse incrementally, retaining at most one bounded incomplete frame.
        for byte in bytes {
            self.bytes.push(*byte);
            let header = match kind {
                2 => 4,
                3 => 3,
                4 => 2,
                _ => return Err("unsupported USB packet type".into()),
            };
            if self.bytes.len() < header {
                continue;
            }
            let payload = match kind {
                2 => u16::from_le_bytes([self.bytes[2], self.bytes[3]]) as usize,
                3 => self.bytes[2] as usize,
                _ => self.bytes[1] as usize,
            };
            let total = header + payload;
            if total > MAX_FRAME {
                self.bytes.clear();
                return Err("oversized HCI frame".into());
            }
            if self.bytes.len() == total {
                let mut frame = vec![kind];
                frame.append(&mut self.bytes);
                result.push(frame);
            }
        }
        Ok(result)
    }
}
#[derive(Default)]
pub struct Transport {
    event: Framer,
    acl: Framer,
    sco: Framer,
    link: Option<(u16, u8)>,
    pub sco_rx_bytes: u64,
    pub sco_tx_bytes: u64,
}
fn handle(bytes: &[u8]) -> u16 {
    u16::from_le_bytes([bytes[0], bytes[1]]) & 0x0fff
}
impl Transport {
    pub fn usb(&mut self, kind: u8, bytes: &[u8]) -> Result<Vec<Action>, String> {
        if kind == 3 && self.link.is_none() {
            self.sco.bytes.clear();
            return Ok(vec![]);
        }
        let frames = match kind {
            2 => self.acl.push(kind, bytes),
            3 => self.sco.push(kind, bytes),
            4 => self.event.push(kind, bytes),
            _ => Err("invalid USB receive type".into()),
        }?;
        let mut actions = Vec::new();
        for packet in frames {
            if kind == 4 {
                let event = packet[1];
                let p = &packet[3..];
                let connection = match event {
                    0x2c if p.len() == 17 && p[0] == 0 && (p[9] == 0 || p[9] == 2) => {
                        Some((handle(&p[1..]), p[16]))
                    }
                    0x03 if p.len() == 11 && p[0] == 0 && p[9] == 0 => Some((handle(&p[1..]), 2)),
                    _ => None,
                };
                if let Some((id, mode)) = connection {
                    if id > 0x0eff || (mode != 2 && mode != 3) {
                        return Err("unsupported SCO connection parameters".into());
                    }
                    if self.link.is_some() && self.link != Some((id, mode)) {
                        return Err("multiple SCO connections are unsupported".into());
                    }
                    if self.link.is_none() {
                        self.link = Some((id, mode));
                        self.sco.bytes.clear();
                        self.sco_rx_bytes = 0;
                        self.sco_tx_bytes = 0;
                        // Configure endpoints BEFORE Floss sees connection success.
                        actions.push(Action::Sco(mode));
                    }
                }
                if event == 0x05
                    && p.len() == 4
                    && p[0] == 0
                    && self.link.map(|l| l.0) == Some(handle(&p[1..]))
                {
                    self.link = None;
                    self.sco.bytes.clear();
                    actions.push(Action::Sco(0));
                }
            }
            if kind == 3 {
                if self.link.map(|l| l.0) != Some(handle(&packet[1..])) {
                    continue;
                }
                self.sco_rx_bytes += (packet.len() - 4) as u64;
            }
            actions.push(Action::Host(packet));
        }
        Ok(actions)
    }
    pub fn host(&mut self, packet: &[u8]) -> Result<Vec<Action>, String> {
        let Some(&kind) = packet.first() else {
            return Err("empty host packet".into());
        };
        let header = match kind {
            1 | 3 => 3,
            2 => 4,
            _ => return Err("unsupported host packet type (LE ISO is not implemented)".into()),
        };
        if packet.len() < header + 1 {
            return Err("short host header".into());
        }
        let payload = if kind == 2 {
            u16::from_le_bytes([packet[3], packet[4]]) as usize
        } else {
            packet[3] as usize
        };
        if packet.len() != 1 + header + payload || packet.len() > MAX_FRAME + 1 {
            return Err("invalid host HCI packet length".into());
        }
        let mut actions = Vec::new();
        if kind == 1 && packet[1..3] == [3, 12] && payload == 0 {
            self.link = None;
            self.sco.bytes.clear();
            self.acl.bytes.clear();
            actions.push(Action::Sco(0));
        }
        if kind == 3 {
            if self.link.map(|l| l.0) != Some(handle(&packet[1..])) {
                return Ok(actions);
            }
            self.sco_tx_bytes += payload as u64;
        }
        actions.push(Action::Usb(packet.to_vec()));
        Ok(actions)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn connected(mode: u8, id: u16) -> Vec<u8> {
        let mut event = vec![0x2c, 17, 0];
        event.extend(id.to_le_bytes());
        event.extend([1, 2, 3, 4, 5, 6]);
        event.extend([2, 6, 2, 24, 0, 24, 0, mode]);
        event
    }
    fn disconnect(id: u16) -> [u8; 6] {
        [5, 4, 0, id as u8, (id >> 8) as u8, 0x13]
    }
    #[test]
    fn full_duplex_cvsd_and_msbc_on_fragmented_usb() {
        for mode in [2, 3] {
            let mut t = Transport::default();
            let event = connected(mode, 42);
            let mut actions = vec![];
            for byte in &event {
                actions.extend(t.usb(4, &[*byte]).unwrap());
            }
            assert_eq!(actions[0], Action::Sco(mode));
            assert_eq!(actions[1], Action::Host([vec![4], event].concat()));
            let hci = vec![3, 42, 0, 6, 1, 2, 3, 4, 5, 6];
            assert_eq!(t.host(&hci).unwrap(), vec![Action::Usb(hci.clone())]);
            let mut incoming = vec![];
            for fragment in hci[1..].chunks(2) {
                incoming.extend(t.usb(3, fragment).unwrap());
            }
            assert_eq!(incoming, vec![Action::Host(hci)]);
            assert_eq!((t.sco_rx_bytes, t.sco_tx_bytes), (6, 6));
            let end = t.usb(4, &disconnect(42)).unwrap();
            assert_eq!(end[0], Action::Sco(0));
        }
    }
    #[test]
    fn duplicates_and_unrelated_disconnects_do_not_change_sco() {
        let mut t = Transport::default();
        let event = connected(3, 42);
        t.usb(4, &event).unwrap();
        assert_eq!(t.usb(4, &event).unwrap().len(), 1);
        assert_eq!(t.usb(4, &disconnect(7)).unwrap().len(), 1);
        assert!(t.usb(4, &connected(2, 7)).is_err());
        assert!(matches!(
            t.host(&[3, 42, 0, 1, 0]).unwrap()[0],
            Action::Usb(_)
        ));
        assert!(t.host(&[3, 7, 0, 1, 0]).unwrap().is_empty());
    }
    #[test]
    fn command_acl_forwarding_and_reset() {
        let mut t = Transport::default();
        let acl = [2, 1, 0, 3, 0, 9, 8, 7];
        assert_eq!(t.host(&acl).unwrap(), vec![Action::Usb(acl.to_vec())]);
        assert_eq!(
            t.usb(2, &acl[1..]).unwrap(),
            vec![Action::Host(acl.to_vec())]
        );
        t.usb(4, &connected(2, 42)).unwrap();
        assert_eq!(
            t.host(&[1, 3, 12, 0]).unwrap(),
            vec![Action::Sco(0), Action::Usb(vec![1, 3, 12, 0])]
        );
        assert!(t.usb(3, &[42, 0, 1, 0]).unwrap().is_empty());
    }
    #[test]
    fn malformed_and_failed_connections_are_bounded() {
        let mut t = Transport::default();
        for p in [&[][..], &[1, 0], &[3, 1, 0, 2, 1], &[5, 0, 0, 0, 0]] {
            assert!(t.host(p).is_err());
        }
        assert!(t.usb(2, &[1, 0, 0xff, 0xff]).is_err());
        let mut failed = connected(3, 42);
        failed[2] = 0x0c;
        assert_eq!(t.usb(4, &failed).unwrap().len(), 1);
        assert!(t.link.is_none());
        assert!(t.usb(4, &connected(1, 42)).is_err());
    }
    #[test]
    fn several_packets_and_partial_tail() {
        let mut t = Transport::default();
        let packet = [0x0e, 4, 1, 3, 12, 0];
        let bytes = [&packet[..], &packet[..], &packet[..3]].concat();
        assert_eq!(t.usb(4, &bytes).unwrap().len(), 2);
        assert_eq!(
            t.usb(4, &packet[3..]).unwrap(),
            vec![Action::Host([vec![4], packet.to_vec()].concat())]
        );
    }
}
