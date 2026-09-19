//! DualShock 4 presented by the winstadia UMDF driver (driver/winstadia.c).

use std::fmt;

pub const VENDOR_ID: u16 = 0x054C;
pub const PRODUCT_ID: u16 = 0x09CC;
/// Serial number string that tells the pad apart from a real DS4.
pub const SERIAL: &str = "winstadia";

// Status feature report: [1] rumble failed, [2..6] last error NTSTATUS
// (little endian), [6] length of the last raw Stadia input report, [7..] that
// report.
const REPORT_ID_STATUS: u8 = 0xE1;
pub const STATUS_REPORT_LEN: usize = 64;
const RAW_REPORT_OFFSET: usize = 7;

// Output report: [1] flags, [4] weak motor, [5] strong motor.
const REPORT_ID_OUTPUT: u8 = 0x05;
const OUTPUT_REPORT_LEN: usize = 32;
const OUTPUT_FLAG_RUMBLE: u8 = 0x01;

/// State of the driver's link to the controller.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub rumble_failed: bool,
    pub error: u32,
    /// Last input report received from the controller, empty before the first.
    pub raw_report: Vec<u8>,
}

/// Buffer to pass to `get_feature_report`.
pub fn status_request() -> [u8; STATUS_REPORT_LEN] {
    let mut report = [0u8; STATUS_REPORT_LEN];
    report[0] = REPORT_ID_STATUS;
    report
}

pub fn parse_status(report: &[u8]) -> Option<Status> {
    if report.len() < RAW_REPORT_OFFSET || report[0] != REPORT_ID_STATUS {
        return None;
    }
    let raw_report = report.get(RAW_REPORT_OFFSET..RAW_REPORT_OFFSET + report[6] as usize)?;
    Some(Status {
        rumble_failed: report[1] != 0,
        error: u32::from_le_bytes([report[2], report[3], report[4], report[5]]),
        raw_report: raw_report.to_vec(),
    })
}

/// Builds the output report that sets the motor speeds (0..=255).
pub fn rumble_report(strong: u8, weak: u8) -> [u8; OUTPUT_REPORT_LEN] {
    let mut report = [0u8; OUTPUT_REPORT_LEN];
    report[0] = REPORT_ID_OUTPUT;
    report[1] = OUTPUT_FLAG_RUMBLE;
    report[4] = weak;
    report[5] = strong;
    report
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.raw_report.is_empty() {
            write!(f, "running, no input received yet")?;
        } else {
            write!(f, "running, receiving input")?;
        }
        if self.rumble_failed {
            write!(f, ", last rumble write failed")?;
        }
        if self.error != 0 {
            write!(f, ", last error 0x{:08X}", self.error)?;
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
        report[2..6].copy_from_slice(&0xC000_009Du32.to_le_bytes());
        report[6] = 2;
        report[7..9].copy_from_slice(&[0x03, 0x08]);
        let status = parse_status(&report).unwrap();
        assert_eq!(
            status,
            Status { rumble_failed: true, error: 0xC000_009D, raw_report: vec![0x03, 0x08] }
        );
        assert_eq!(
            status.to_string(),
            "running, receiving input, last rumble write failed, last error 0xC000009D"
        );
    }

    #[test]
    fn rejects_unknown_reports() {
        assert_eq!(parse_status(&[0x01, 0, 0, 0, 0, 0, 0]), None);
        // Raw report length pointing past the end.
        assert_eq!(parse_status(&[REPORT_ID_STATUS, 0, 0, 0, 0, 0, 9, 1]), None);
    }

    #[test]
    fn rumble_report_layout() {
        let report = rumble_report(255, 1);
        assert_eq!(report[..6], [0x05, 0x01, 0, 0, 1, 255]);
    }
}
