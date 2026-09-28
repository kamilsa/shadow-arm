use std::cell::RefCell;
use std::net::Ipv4Addr;

use self::codel_queue::CoDelQueue;
use crate::core::worker::Worker;
use crate::network::PacketDevice;
use crate::network::packet::PacketRc;
use crate::utility::{Magic, ObjectCounter};
mod codel_queue;

use shadow_shim_helper_rs::emulated_time::EmulatedTime;

/// A synthetic address that identifies a host's uplink router within the host's
/// internal packet routing table. It is only used as a routing-table key and is
/// never used as the destination address of a real packet.
pub const UPLINK_ROUTER_ADDR: Ipv4Addr = Ipv4Addr::new(0, 0, 0, 1);

/// A router assists with moving packets between hosts across the simulated
/// network.
pub struct Router {
    magic: Magic<Self>,
    _counter: ObjectCounter,
    address: Ipv4Addr,
    /// Packets inbound to the host from the simulated network.
    inbound_packets: RefCell<CoDelQueue>,
}

impl Router {
    /// Create a new router for a host that will help route packets between it
    /// and other hosts. The `address` must uniquely identify this router to the
    /// host that owns it.
    pub fn new(address: Ipv4Addr) -> Router {
        Router {
            magic: Magic::new(),
            address,
            _counter: ObjectCounter::new("Router"),
            inbound_packets: RefCell::new(CoDelQueue::new()),
        }
    }

    fn push_inner(&self, packet: PacketRc, now: EmulatedTime) {
        self.magic.debug_check();
        self.inbound_packets.borrow_mut().push(packet, now);
    }

    fn pop_inner(&self, now: EmulatedTime) -> Option<PacketRc> {
        self.magic.debug_check();
        self.inbound_packets.borrow_mut().pop(now)
    }

    /// Routes the packet from the source host through the virtual internet to
    /// the destination host.
    fn route_outgoing_packet(&self, packet: PacketRc) {
        // TODO: move Worker::send_packet to here?
        Worker::with_active_host(|src_host| Worker::send_packet(src_host, packet)).unwrap();
    }

    /// Routes the packet from the virtual internet into our CoDel queue, which
    /// can then be received by the destiantion host by calling pop().
    pub fn route_incoming_packet(&self, packet: PacketRc) {
        self.push_inner(packet, Worker::current_time().unwrap())
    }
}

impl PacketDevice for Router {
    fn get_address(&self) -> Ipv4Addr {
        self.address
    }

    fn pop(&self) -> Option<PacketRc> {
        // When the host calls pop, we provide the next packet from the CoDel queue.
        self.pop_inner(Worker::current_time().unwrap())
    }

    fn push(&self, packet: PacketRc) {
        // When the host calls push, we send to the virtual internet.
        self.route_outgoing_packet(packet);
    }
}

/// The mirror image of [`Router`] for packets leaving a host.
///
/// The host's processes can write to their sockets much faster than the
/// configured upload bandwidth allows. If we relied on the socket send buffer
/// to apply back pressure, the process would be stalled with `EWOULDBLOCK` and
/// the congestion controller would never observe the loss that a real
/// bottleneck would produce; it could stay in slow-start forever. Instead, this
/// device accepts packets as fast as the process produces them and buffers them
/// in a CoDel queue. A rate-limited [`Relay`](crate::network::relay::Relay)
/// then drains the queue at the upload bandwidth, dropping packets under
/// sustained overload instead of only adding latency.
///
/// Unlike the downlink [`Router`], `push()` here is the "inbound" direction
/// (from the host's interface) and `pop()` is the "outbound" direction (to the
/// internet), so that the CoDel queue sits between the socket send buffer and
/// the rate-limited relay.
pub struct UplinkRouter {
    magic: Magic<Self>,
    _counter: ObjectCounter,
    address: Ipv4Addr,
    /// Packets outbound from the host's sockets to the simulated network.
    outbound_packets: RefCell<CoDelQueue>,
}

impl UplinkRouter {
    /// Create a new uplink router. The `address` is a synthetic address that
    /// must uniquely identify this router in the owning host's routing table.
    pub fn new(address: Ipv4Addr) -> UplinkRouter {
        UplinkRouter {
            magic: Magic::new(),
            address,
            _counter: ObjectCounter::new("UplinkRouter"),
            outbound_packets: RefCell::new(CoDelQueue::new()),
        }
    }

    fn push_inner(&self, packet: PacketRc, now: EmulatedTime) {
        self.magic.debug_check();
        self.outbound_packets.borrow_mut().push(packet, now);
    }

    fn pop_inner(&self, now: EmulatedTime) -> Option<PacketRc> {
        self.magic.debug_check();
        self.outbound_packets.borrow_mut().pop(now)
    }

    /// Returns the number of packets currently buffered in the CoDel queue.
    #[cfg(test)]
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.outbound_packets.borrow().len()
    }
}

impl PacketDevice for UplinkRouter {
    fn get_address(&self) -> Ipv4Addr {
        self.address
    }

    fn pop(&self) -> Option<PacketRc> {
        // A rate-limited relay calls pop() to drain our CoDel queue at the
        // upload bandwidth.
        self.pop_inner(Worker::current_time().unwrap())
    }

    fn push(&self, packet: PacketRc) {
        // Accept the packet immediately and buffer it for later forwarding.
        // Note that we never reject a packet here; applying back pressure is
        // the CoDel queue's job (via dropping), not the socket's.
        self.push_inner(packet, Worker::current_time().unwrap());

        // Wake the rate-limited relay that drains this queue so that the
        // packet is forwarded at the upload bandwidth.
        Worker::with_active_host(|host| host.notify_uplink_router_has_packets()).unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::network::tests::mock_time_millis;
    use shadow_shim_helper_rs::simulation_time::SimulationTime;

    #[test]
    fn empty() {
        let now = mock_time_millis(1000);
        let router = Router::new(Ipv4Addr::UNSPECIFIED);
        assert!(router.inbound_packets.borrow().peek().is_none());
        assert!(router.pop_inner(now).is_none());
    }

    #[test]
    // Ignore in miri for use of c::packet* functions.
    #[cfg_attr(miri, ignore)]
    fn push_pop_simple() {
        let now = mock_time_millis(1000);
        let router = Router::new(Ipv4Addr::UNSPECIFIED);

        const N: usize = 10;

        for _ in 1..=N {
            router.push_inner(PacketRc::new_ipv4_udp_mock(), now);
            assert!(router.inbound_packets.borrow().peek().is_some());
        }
        for _ in 1..=N {
            assert!(router.inbound_packets.borrow().peek().is_some());
            assert!(router.pop_inner(now).is_some());
        }

        assert!(router.inbound_packets.borrow().peek().is_none());
        assert!(router.pop_inner(now).is_none());
    }

    #[test]
    // Ignore in miri for use of c::packet* functions.
    #[cfg_attr(miri, ignore)]
    fn uplink_accepts_all_packets() {
        let now = mock_time_millis(1000);
        let router = UplinkRouter::new(UPLINK_ROUTER_ADDR);

        const N: usize = 1000;

        // Unlike the socket send buffer, the uplink router must accept packets
        // as fast as they are produced rather than rejecting them.
        for _ in 0..N {
            router.push_inner(PacketRc::new_ipv4_udp_mock(), now);
        }
        assert_eq!(router.len(), N);

        for _ in 0..N {
            assert!(router.pop_inner(now).is_some());
        }
        assert!(router.pop_inner(now).is_none());
    }

    #[test]
    // Ignore in miri for use of c::packet* functions.
    #[cfg_attr(miri, ignore)]
    fn uplink_drops_under_sustained_overload() {
        // These match CoDel's TARGET and INTERVAL in `codel_queue.rs`.
        let target = SimulationTime::from_millis(10);
        let interval = SimulationTime::from_millis(100);

        let start = mock_time_millis(1000);
        let router = UplinkRouter::new(UPLINK_ROUTER_ADDR);

        const N: usize = 20;
        for _ in 0..N {
            router.push_inner(PacketRc::new_ipv4_udp_mock(), start);
        }
        assert_eq!(router.len(), N);

        // Observing the standing delay at the target starts CoDel's interval.
        assert!(router.pop_inner(start + target).is_some());
        assert_eq!(router.len(), N - 1);

        // The queue has been above the target for a full interval, so CoDel
        // enters drop mode and drops at least one packet instead of forwarding
        // all of them.
        assert!(router.pop_inner(start + target + interval).is_some());
        assert!(
            router.len() < N - 2,
            "expected CoDel to drop a packet under sustained overload"
        );
    }
}
