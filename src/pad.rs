//! Virtual DualShock 4 exposed by the winstadia UMDF driver (driver/winstadia.c).

use std::fmt;

pub const VENDOR_ID: u16 = 0x054C;
pub const PRODUCT_ID: u16 = 0x09CC;
/// Serial number string that tells the virtual pad apart from a real DS4.
pub const SERIAL: &str = "winstadia";

// Status feature report: [1] bridge state, [2] rumble failed,
// [3..7] last Win32 error (little endian).
const REPORT_ID_STATUS: u8 = 0xE1;
pub const STATUS_REPORT_LEN: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bridge {
    Searching,
    Connected,
    OpenFailed,
}

/// State of the driver's bridge to the physical controller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status {
    pub bridge: Bridge,
    pub rumble_failed: bool,
    pub error: u32,
}

/// Buffer to pass to `get_feature_report`.
pub fn status_request() -> [u8; STATUS_REPORT_LEN] {
    let mut report = [0u8; STATUS_REPORT_LEN];
    report[0] = REPORT_ID_STATUS;
    report
}

pub fn parse_status(report: &[u8]) -> Option<Status> {
    if report.len() < 7 || report[0] != REPORT_ID_STATUS {
        return None;
    }
    let bridge = match report[1] {
        0 => Bridge::Searching,
        1 => Bridge::Connected,
        2 => Bridge::OpenFailed,
        _ => return None,
    };
    Some(Status {
        bridge,
        rumble_failed: report[2] != 0,
        error: u32::from_le_bytes([report[3], report[4], report[5], report[6]]),
    })
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.bridge {
            Bridge::Searching => write!(f, "no Stadia controller connected")?,
            Bridge::Connected => write!(f, "Stadia controller connected")?,
            Bridge::OpenFailed => {
                write!(f, "cannot open the Stadia controller (Win32 error {})", self.error)?
            }
        }
        if self.rumble_failed {
            // Expected over Bluetooth, where Windows rejects output reports.
            write!(f, ", rumble unavailable (Win32 error {})", self.error)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_status() {
        let mut report = status_request();
        report[1] = 1;
        report[2] = 1;
        report[3] = 87;
        let status = parse_status(&report).unwrap();
        assert_eq!(status, Status { bridge: Bridge::Connected, rumble_failed: true, error: 87 });
        assert_eq!(
            status.to_string(),
            "Stadia controller connected, rumble unavailable (Win32 error 87)"
        );
    }

    #[test]
    fn rejects_unknown_reports() {
        assert_eq!(parse_status(&[0x01, 0, 0, 0, 0, 0, 0]), None);
        assert_eq!(parse_status(&[REPORT_ID_STATUS, 9, 0, 0, 0, 0, 0]), None);
    }
}
