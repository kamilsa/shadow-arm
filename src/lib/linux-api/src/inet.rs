use crate::bindings;

pub use bindings::linux_sockaddr_in;
pub use bindings::{
    LINUX_IP_MTU_DISCOVER, LINUX_IP_PKTINFO, LINUX_IP_PMTUDISC_DO, LINUX_IP_PMTUDISC_DONT,
    LINUX_IP_PMTUDISC_INTERFACE, LINUX_IP_PMTUDISC_OMIT, LINUX_IP_PMTUDISC_PROBE,
    LINUX_IP_PMTUDISC_WANT, LINUX_IP_RECVTOS,
};
#[allow(non_camel_case_types)]
pub type sockaddr_in = linux_sockaddr_in;
unsafe impl shadow_pod::Pod for sockaddr_in {}
